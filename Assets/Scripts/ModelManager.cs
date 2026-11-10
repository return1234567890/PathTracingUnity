using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

/// <summary>
/// 路径追踪顶点结构体（全局顶点数组元素，C# 侧收集用，64B）。
/// 数据为 object-space，世界变换由实例表的 transform 在 shader 侧完成。
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct PathVertex
{
    public Vector3 pos;      // 12B
    public Vector3 normal;  // 12B
    public Vector4 tangent; // 16B (xyz + sign)
    public Vector2 uv;      // 8B
    public Color   color;   // 16B
                            // 合计 64B
}

/// <summary>
/// 全局三角形数组元素：三个顶点索引（object-space 局部索引，shader 侧 + vertBase）。
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct PathTriangle
{
    public uint v0, v1, v2; // 12B
}

/// <summary>
/// 顶点 GPU 布局（80B，16B 对齐），native 侧统一顶点缓冲元素（与 vpt_types.h 一致）。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct VertexGpuData
{
    public Vector3 pos;
    public float  pad0;
    public Vector3 normal;
    public float  pad1;
    public Vector4 tangent;
    public Vector2 uv;
    public Vector2 pad2;
    public Vector4 color;
    // 16 + 16 + 16 + 16 + 16 = 80B
}

/// <summary>材质数组元素（按 materialID 索引，96B）。</summary>
/// <remarks>
/// 金属度工作流：metallic 线性插值 F0；roughness = 1 - URP _Smoothness，钳制 [0.02,1]。
/// 扩展字段：emissionColor / 4个纹理slice索引 / flags / uvScaleOffset。
/// 纹理索引 0xFFFFFFFF = 无纹理（使用 uniform 值）。
/// </remarks>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct PathMaterial
{
    public Vector4 albedo;          // 16B - base color tint (rgb + alpha)
    public float   metallic;        // 4B
    public float   roughness;       // 4B
    public float   bumpScale;       // 4B - normal map strength (_BumpScale)
    public uint    flags;           // 4B - bit0:hasBaseMap, bit1:hasMetalRough, bit2:hasNormal, bit3:hasEmissive
    // -- 32B --
    public Vector4 emissionColor;   // 16B - emissive tint (_EmissionColor rgb + intensity)
    public uint    baseTexID;       // 4B - BaseColorArray slice (0xFFFFFFFF=none)
    public uint    metalRoughTexID; // 4B - MetallicSmoothnessArray slice
    public uint    normalTexID;     // 4B - NormalArray slice
    public uint    emissiveTexID;   // 4B - EmissiveArray slice
    // -- 32B --
    public Vector4 uvScaleOffset;   // 16B - xy=scale, zw=offset
    // -- 16B --
    // ── 新增 16B 透射参数 ──
    public float   transmission;        // 4B - 镜面透射因子 [0,1]，0=不透明
    public float   ior;                 // 4B - 折射率（默认1.5玻璃，1.0空气）
    public float   diffuseTransmission; // 4B - 漫透射因子 [0,1]（纸张/布料等）
    public uint    thinWalled;          // 4B - 1=薄壁（无体积衰减），0=实体
    // -- 16B --
    // 合计 96B
}

/// <summary>PathMaterial flags 位掩码（与 HLSL 一致）。</summary>
public static class MaterialFlags
{
    public const uint MAT_HAS_BASE       = 1u;
    public const uint MAT_HAS_MR         = 2u;
    public const uint MAT_HAS_NORMAL     = 4u;
    public const uint MAT_HAS_EMISS      = 8u;
    public const uint MAT_IS_TRANSPARENT = 16u;  // bit4: 材质启用透射
    public const uint TEX_NONE          = 0xFFFFFFFFu;
}

/// <summary>
/// 场景模型收集与数据推送管理器。
/// 在游戏开始时收集场景中的模型物体与材质，把顶点/索引/材质/光源/实例数据
/// 推送到 native Vulkan 插件；BLAS/TLAS 的构建完全由 native 侧完成，
/// C# 侧只维护「哪个 BLAS 占用全局数组的哪段区间」这类纯簿记信息。
///
/// 说明：软件路径追踪（CPU BVH + HLSL 求交）与 tinybvh 依赖已移除。
/// </summary>
public class ModelManager : MonoBehaviour
{
    [Header("收集设置")]
    [Tooltip("只收集带此 Tag 的对象；留空(=Untagged)表示收集场景中所有 MeshFilter")]
    public string targetTag = "Untagged";

    [Tooltip("是否在构建完成后打印各数组统计信息")]
    public bool logBuildStats = true;

    [Header("骨骼动画")]
    [Tooltip("refit 质量衰减对策（阶段3）：refit 是原地包络微调，大幅变形多帧后 BVH 遍历效率下降；\n"
            + "该 BLAS 每累计 N 次 refit 后改走一次全量 rebuild（mode=BUILD）恢复质量，随后计数清零。\n"
            + "0=禁用（始终 refit）")]
    [Range(0, 10000)]
    public int blasRebuildInterval = 60;

    /// <summary>全局单例，便于路径追踪器访问。</summary>
    public static ModelManager Instance { get; private set; }

    // ════════ 场景数据（构建后可读） ════════

    /// <summary>全局三角形数组（按 triBase 偏移拼接）</summary>
    public PathTriangle[] GlobalTriangles { get; private set; }
    /// <summary>全局顶点数组（按 vertBase 偏移拼接，object-space）</summary>
    public PathVertex[]   GlobalVertices  { get; private set; }
    /// <summary>材质数组（按 materialID 索引，含纹理索引与UV变换）</summary>
    public PathMaterial[]  Materials       { get; private set; }

    /// <summary>纹理数组管理器（4个 Texture2DArray），供 native 侧采样。</summary>
    public TextureArrayManager TextureArrays { get; private set; }

    /// <summary>场景是否已收集到可渲染内容（至少一个实例）。</summary>
    public bool HasScene { get; private set; }

    // ── BLAS 几何登记（纯托管簿记：全局数组区间由 native BLAS 消费） ──
    private struct BlasRecord
    {
        public int vertBase;   // 该 BLAS 顶点在 GlobalVertices 中的起始下标
        public int vertCount;
        public int triBase;    // 该 BLAS 三角形在 GlobalTriangles 中的起始下标
        public int triCount;
    }
    private readonly List<BlasRecord> _blasRecords = new();

    // ── 实例登记（materialID + blasID + object→world 变换） ──
    private struct InstanceRecord
    {
        public Transform transform;    // 世界变换来源（动态同步逐帧重读）
        public Matrix4x4 localToWorld; // 构建时快照（Transform 失效时回退）
        public uint      materialID;
        public uint      blasID;
    }
    private readonly List<InstanceRecord> _instances = new();

    private void Awake()
    {
        if (Instance != null && Instance != this)
        {
            Debug.LogWarning("[ModelManager] 已存在实例，销毁重复实例。", this);
            Destroy(this);
            return;
        }
        Instance = this;
    }

    private void Start()
    {
        BuildScene();
    }

    private void OnDestroy()
    {
        OnDestroyNative();
        if (Instance == this)
            Instance = null;
        TextureArrays?.Dispose();
        TextureArrays = null;
    }

    // ── 构建主流程 ──────────────────────────────────────

    /// <summary>
    /// 收集场景模型与材质，登记 BLAS 区间与实例，并推送数据到 native 插件。
    /// 在 Start 中自动调用；场景变更后可手动调用重建。
    /// </summary>
    public void BuildScene()
    {
        // 释放旧纹理数组
        TextureArrays?.Dispose();
        TextureArrays = new TextureArrayManager();

        // 临时容器
        var globalVerts  = new List<PathVertex>();
        var globalTris   = new List<PathTriangle>();
        var materials    = new List<PathMaterial>();
        var matToId      = new Dictionary<Material, int>();
        var meshToBlas   = new Dictionary<Mesh, int>();
        _blasRecords.Clear();
        _instances.Clear();
        CleanupSkinnedTrackers();
        HasScene = false;

        // 1. 收集场景中的 MeshFilter
        MeshFilter[] filters = FindObjectsOfType<MeshFilter>();
        int collectedInstanceCount = 0;

        foreach (MeshFilter mf in filters)
        {
            if (mf == null) continue;
            Mesh mesh = mf.sharedMesh;
            if (mesh == null) mesh = mf.mesh;
            if (mesh == null) continue;

            // Tag 过滤
            if (!string.IsNullOrEmpty(targetTag) && targetTag != "Untagged")
            {
                if (!mf.CompareTag(targetTag))
                    continue;
            }

            GameObject go = mf.gameObject;
            if (!go.activeInHierarchy) continue;
            var renderer = go.GetComponent<MeshRenderer>();
            if (renderer == null || !renderer.enabled) continue;

            // —— 1a. 为每个唯一 mesh 登记 BLAS 区间（去重）——
            if (!meshToBlas.TryGetValue(mesh, out int blasId))
            {
                // 几何追加到全局数组（object-space），并登记该 BLAS 占用的区间
                int vertBase = globalVerts.Count;
                int triBase  = globalTris.Count;
                AppendVertices(globalVerts, mesh);
                int triCount = AppendTriangles(globalTris, mesh);
                blasId = RegisterBlas(vertBase, mesh.vertexCount, triBase, triCount);
                meshToBlas[mesh] = blasId;
            }

            // —— 1b. 收集材质（去重，取 renderer 的首材质，单材质约束）——
            Material mat = renderer.sharedMaterial;
            if (mat == null) mat = renderer.material;
            if (!matToId.TryGetValue(mat, out int materialId))
            {
                materialId = materials.Count;
                matToId[mat] = materialId;
                materials.Add(GetMaterial(mat, TextureArrays));
            }

            // —— 1c. 登记实例 ——
            AddInstance(go.transform, (uint)materialId, (uint)blasId);
            collectedInstanceCount++;
        }

        // —— 1.5 收集 SkinnedMeshRenderer（骨骼动画：GPU 蒙皮）——
        // 多材质支持：Vulkan RT 的材质为 per-instance（instanceCustomIndex 携带 materialID），
        // 故每个 subMesh（材质槽）拆为独立 BLAS + 同 transform 实例；
        // 顶点按 subMesh 重映射为连续区段，统一顶点缓冲不膨胀、蒙皮计算不重复。
        // 关节矩阵 scratch 由同一 SMR 的所有 subMesh tracker 共享（每帧只计算一次）。
        int skinnedInstanceCount = 0;
        foreach (SkinnedMeshRenderer smr in FindObjectsOfType<SkinnedMeshRenderer>())
        {
            if (smr == null) continue;
            Mesh mesh = smr.sharedMesh;
            if (mesh == null) continue;

            if (!string.IsNullOrEmpty(targetTag) && targetTag != "Untagged")
            {
                if (!smr.CompareTag(targetTag))
                    continue;
            }

            GameObject go = smr.gameObject;
            if (!go.activeInHierarchy || !smr.enabled) continue;
            if (!mesh.isReadable)
            {
                Debug.LogWarning($"[ModelManager] SkinnedMesh '{go.name}' 的 mesh 未开启 Read/Write，跳过蒙皮路径追踪。", go);
                continue;
            }

            // —— 几何/蒙皮属性一次性读取（各 subMesh 重映射共用）——
            Vector3[] verts  = mesh.vertices;
            Vector3[] norms  = mesh.normals;
            Vector4[] tangs  = mesh.tangents;
            Vector2[] uvs    = mesh.uv;
            Color[]   colors = mesh.colors;
            bool hasNormal  = norms  != null && norms.Length  == verts.Length;
            bool hasTangent = tangs  != null && tangs.Length  == verts.Length;
            bool hasUv      = uvs    != null && uvs.Length     == verts.Length;
            bool hasColor   = colors != null && colors.Length == verts.Length;
            Transform[] bones = smr.bones;
            Matrix4x4[] bindposes = mesh.bindposes;
            int jointCount = Mathf.Min(bones != null ? bones.Length : 0,
                                       bindposes != null ? bindposes.Length : 0);
            Material[] smrMats = smr.sharedMaterials;

            // 关节矩阵共享 scratch（同一 SMR 的 subMesh tracker 复用，首个 tracker 持有 pin）
            var jointScratch = new float[Mathf.Max(jointCount, 1) * 16];
            var jointPin = GCHandle.Alloc(jointScratch, GCHandleType.Pinned);
            bool jointPinOwned = false;

            for (int s = 0; s < mesh.subMeshCount; s++)
            {
                int[] subTris = mesh.GetTriangles(s);
                if (subTris == null || subTris.Length < 3) continue;

                // —— 顶点重映射：subMesh 引用的顶点压缩为连续局部索引 ——
                var remap = new Dictionary<int, int>(subTris.Length);
                var remappedTris = new int[subTris.Length];
                int next = 0;
                for (int i = 0; i < subTris.Length; i++)
                {
                    int old = subTris[i];
                    if (!remap.TryGetValue(old, out int local))
                    {
                        local = next++;
                        remap[old] = local;
                    }
                    remappedTris[i] = local;
                }
                int subVertCount = next;

                // 局部顶点数据（object/bind-pose 空间）+ 原顶点索引表（蒙皮权重重映射用）
                var origIndices = new int[subVertCount];
                var cache  = new VertexGpuData[subVertCount];
                foreach (var kv in remap)
                {
                    int old = kv.Key, local = kv.Value;
                    origIndices[local] = old;
                    cache[local] = new VertexGpuData
                    {
                        pos     = verts[old],
                        normal  = hasNormal  ? norms[old]  : Vector3.zero,
                        tangent = hasTangent ? tangs[old]  : Vector4.zero,
                        uv      = hasUv      ? uvs[old]    : Vector2.zero,
                        color   = hasColor   ? colors[old] : Color.white
                    };
                }

                // —— bind-pose 几何登记 BLAS 区间 + 全局顶点/三角形数组追加 ——
                int vertBase = globalVerts.Count;
                int triBase  = globalTris.Count;
                for (int i = 0; i < subVertCount; i++)
                    globalVerts.Add(new PathVertex
                    {
                        pos     = cache[i].pos,
                        normal  = cache[i].normal,
                        tangent = cache[i].tangent,
                        uv      = cache[i].uv,
                        color   = cache[i].color
                    });
                for (int i = 0; i < remappedTris.Length; i += 3)
                    globalTris.Add(new PathTriangle
                    {
                        v0 = (uint)remappedTris[i],
                        v1 = (uint)remappedTris[i + 1],
                        v2 = (uint)remappedTris[i + 2]
                    });
                int skinnedBlasId = RegisterBlas(vertBase, subVertCount,
                                                 triBase, remappedTris.Length / 3);

                // —— 材质（per-subMesh 材质槽，去重；槽位缺失时回退首材质）——
                Material mat = s < smrMats.Length ? smrMats[s] : null;
                if (mat == null) mat = smr.sharedMaterial;
                if (mat == null) mat = smr.material;
                if (!matToId.TryGetValue(mat, out int skinnedMaterialId))
                {
                    skinnedMaterialId = materials.Count;
                    matToId[mat] = skinnedMaterialId;
                    materials.Add(GetMaterial(mat, TextureArrays));
                }

                // —— 实例：transform 用 SMR 自身（蒙皮关节矩阵把顶点拉回 SMR 局部空间）——
                AddInstance(smr.transform, (uint)skinnedMaterialId, (uint)skinnedBlasId);
                collectedInstanceCount++;

                // 骨骼动画追踪（GPU 蒙皮：每帧只推送关节矩阵）
                var tracker = new NativeSkinnedTracker
                {
                    smr         = smr,
                    mesh        = mesh,
                    blasID      = (uint)skinnedBlasId,
                    vertexCount = subVertCount,
                    origVertexIndices = origIndices,
                    bones       = bones,
                    bindposes   = bindposes,
                    jointCount  = jointCount,
                    // 至少 1 个关节避免零长数组（jointCount=0 的 mesh 不会注册/同步）
                    jointMatrixScratch = jointScratch,
                    jointMatPin = jointPin,
                    // 同一 SMR 多 subMesh 共享关节矩阵 scratch：首个 tracker 持有 pin（Cleanup 只 free 一次）
                    ownsJointMatrices = !jointPinOwned
                };
                if (!jointPinOwned) jointPinOwned = true;
                _skinnedTracker.Add(tracker);
                _skinnedBlasIds.Add((uint)skinnedBlasId);
                skinnedInstanceCount++;
            }

            // 未成功添加任何 subMesh tracker（subMesh 全空等）→ 释放共享 pin
            if (!jointPinOwned && jointPin.IsAllocated) jointPin.Free();
        }

        // 2. 提交数组
        GlobalVertices  = globalVerts.ToArray();
        GlobalTriangles = globalTris.ToArray();
        Materials       = materials.ToArray();
        HasScene        = _instances.Count > 0;

        // 3. 构建纹理数组（所有材质纹理注册完毕后打包）
        TextureArrays.Build(1024);

        // 4. 统计
        if (logBuildStats)
        {
            Debug.Log(
                "[ModelManager] 构建完成：" +
                $"BLAS={_blasRecords.Count}, 实例={collectedInstanceCount}, 材质={Materials.Length}, " +
                $"蒙皮实例={skinnedInstanceCount}, " +
                $"三角形={GlobalTriangles.Length}, 顶点={GlobalVertices.Length}");
        }

        // 5. 推送场景数据到 Native Vulkan 插件（BLAS/TLAS 构建）
        PushToNative();
    }

    // ── BLAS / 实例登记 helper ───────────────────────────

    /// <summary>登记一个 BLAS 占用的全局顶点/三角形区间，返回其 blasID（= 登记顺序）。</summary>
    private int RegisterBlas(int vertBase, int vertCount, int triBase, int triCount)
    {
        _blasRecords.Add(new BlasRecord
        {
            vertBase  = vertBase,
            vertCount = vertCount,
            triBase   = triBase,
            triCount  = triCount
        });
        return _blasRecords.Count - 1;
    }

    /// <summary>登记一个实例（object→world 变换 + 材质 + BLAS）。</summary>
    private void AddInstance(Transform transform, uint materialID, uint blasID)
    {
        _instances.Add(new InstanceRecord
        {
            transform    = transform,
            localToWorld = transform != null ? transform.localToWorldMatrix : Matrix4x4.identity,
            materialID   = materialID,
            blasID       = blasID
        });
    }

    // ── 材质参数提取（albedo + metallic + roughness） ──────

    /// <summary>
    /// 从材质提取 PBR 参数与纹理引用。优先 URP Lit 的 _BaseColor/_Metallic/_Smoothness，
    /// 其次 _Color/_Glossiness（旧版/内置），最后 fallback。
    /// roughness 钳制到 [0.02, 1] 防止镜面退化导致除零。
    /// 纹理提取：通过 texMgr 注册 Texture2D 获得 slice 索引，写入 PathMaterial。
    /// </summary>
    private static PathMaterial GetMaterial(Material mat, TextureArrayManager texMgr)
    {
        PathMaterial pm;
        pm.albedo = Vector4.one;
        pm.metallic = 0f;
        pm.roughness = 0.5f;
        pm.bumpScale = 1.0f;
        pm.flags = 0u;
        pm.emissionColor = Vector4.zero;
        pm.baseTexID = MaterialFlags.TEX_NONE;
        pm.metalRoughTexID = MaterialFlags.TEX_NONE;
        pm.normalTexID = MaterialFlags.TEX_NONE;
        pm.emissiveTexID = MaterialFlags.TEX_NONE;
        pm.uvScaleOffset = new Vector4(1f, 1f, 0f, 0f);
        // ── 透射参数初始化 ──
        pm.transmission = 0f;
        pm.ior = 1.5f;
        pm.diffuseTransmission = 0f;
        pm.thinWalled = 0u;

        if (mat == null) return pm;

        // ── uniform 标量（保持原有逻辑）──
        // albedo
        if (mat.HasProperty("_BaseColor"))
            pm.albedo = mat.GetVector("_BaseColor");
        else if (mat.HasProperty("_Color"))
            pm.albedo = mat.GetColor("_Color");
        else
            pm.albedo = mat.color;

        // metallic
        if (mat.HasProperty("_Metallic"))
            pm.metallic = mat.GetFloat("_Metallic");
        else if (mat.HasProperty("_MetallicGlossMap"))
            pm.metallic = 0f;

        // roughness = 1 - smoothness（URP 金属度工作流）
        if (mat.HasProperty("_Smoothness"))
            pm.roughness = Mathf.Clamp01(1f - mat.GetFloat("_Smoothness"));
        else if (mat.HasProperty("_Glossiness"))
            pm.roughness = Mathf.Clamp01(1f - mat.GetFloat("_Glossiness"));

        // 钳制到 [0.02, 1] 防止 GGX 除零与镜面退化
        pm.roughness = Mathf.Clamp(pm.roughness, 0.02f, 1f);
        pm.metallic = Mathf.Clamp01(pm.metallic);

        // ── 新增：纹理提取 ──
        uint flags = 0u;

        // BaseColor / Main texture
        Texture2D baseTex = null;
        if (mat.HasProperty("_BaseMap"))
            baseTex = mat.GetTexture("_BaseMap") as Texture2D;
        if (baseTex == null && mat.HasProperty("_MainTex"))
            baseTex = mat.GetTexture("_MainTex") as Texture2D;
        if (baseTex != null)
        {
            pm.baseTexID = texMgr.RegisterBaseColor(baseTex);
            flags |= MaterialFlags.MAT_HAS_BASE;
        }

        // Metallic-Gloss map
        Texture2D metalGlossTex = null;
        if (mat.HasProperty("_MetallicGlossMap"))
            metalGlossTex = mat.GetTexture("_MetallicGlossMap") as Texture2D;
        if (metalGlossTex != null)
        {
            pm.metalRoughTexID = texMgr.RegisterMetallicSmoothness(metalGlossTex);
            flags |= MaterialFlags.MAT_HAS_MR;
        }

        // Normal / Bump map
        Texture2D bumpTex = null;
        if (mat.HasProperty("_BumpMap"))
            bumpTex = mat.GetTexture("_BumpMap") as Texture2D;
        if (bumpTex != null)
        {
            pm.normalTexID = texMgr.RegisterNormal(bumpTex);
            flags |= MaterialFlags.MAT_HAS_NORMAL;
        }

        // Emission map
        Texture2D emissionTex = null;
        if (mat.HasProperty("_EmissionMap"))
            emissionTex = mat.GetTexture("_EmissionMap") as Texture2D;
        if (emissionTex != null)
        {
            pm.emissiveTexID = texMgr.RegisterEmissive(emissionTex);
            flags |= MaterialFlags.MAT_HAS_EMISS;
        }

        // ── emissive color：与光栅 URP Lit 自发光语义对齐 ──
        // 光栅端 SampleEmission（SurfaceInput.hlsl）仅受 _EMISSION keyword 门控，
        // 贴图未绑定采样默认纹理 → 仅设置 HDR _EmissionColor（无贴图）即自发光。
        // 原实现仅在 _EmissionMap 存在时打 MAT_HAS_EMISS，导致"只设颜色"的材质
        // 在路径追踪中丢失自发光，故补齐颜色非黑判定。
        // 不采用 EmissiveIsBlack 作否定判据：资产实证 New Material 1.mat
        // m_LightmapFlags=4 且 HDR 颜色、光栅下自发光正常，该 flag 与"GUI 关闭
        // 后残留颜色"的状态不可区分，会误杀在用的发光材质。
        if (mat.HasProperty("_EmissionColor"))
        {
            Color ec = mat.GetColor("_EmissionColor");
            // HDR 强度等比钳制：防 RGBA16F（规划中的 emissiveRT）half 溢出与极端
            // firefly（New Material 2.mat 曾出现 float.MaxValue）
            const float MaxEmission = 1e4f;
            float maxComp = Mathf.Max(ec.r, Mathf.Max(ec.g, ec.b));
            if (maxComp > MaxEmission)
            {
                float scale = MaxEmission / maxComp;
                ec.r *= scale; ec.g *= scale; ec.b *= scale;
            }
            pm.emissionColor = ec;
            if (maxComp > 1e-5f)
                flags |= MaterialFlags.MAT_HAS_EMISS;
        }
        pm.flags = flags;

        // ── 新增：bump scale ──
        if (mat.HasProperty("_BumpScale"))
            pm.bumpScale = mat.GetFloat("_BumpScale");
        else
            pm.bumpScale = 1.0f;

        // ── 新增：UV 变换（取 _BaseMap 的 tiling/offset，回退 _MainTex）──
        string uvProp = mat.HasProperty("_BaseMap") ? "_BaseMap" : (mat.HasProperty("_MainTex") ? "_MainTex" : null);
        if (uvProp != null)
        {
            pm.uvScaleOffset = new Vector4(
                mat.GetTextureScale(uvProp).x,
                mat.GetTextureScale(uvProp).y,
                mat.GetTextureOffset(uvProp).x,
                mat.GetTextureOffset(uvProp).y
            );
        }
        else
            pm.uvScaleOffset = new Vector4(1f, 1f, 0f, 0f);

        // ── 透射参数：基于 URP Surface Type + albedo alpha ──
        // Surface Type = Transparent 时启用半透明 BSDF
        if (mat.HasProperty("_Surface") && mat.GetFloat("_Surface") > 0.5f)
        {
            flags |= MaterialFlags.MAT_IS_TRANSPARENT;
            pm.thinWalled = 1u; // 薄壁模式（默认，无体积吸收）
        }
        // transmission = 1 - albedo.a（HLSL 侧 applyMaterialTextures 后用最终 alpha 覆盖）
        pm.transmission = 1.0f - pm.albedo.w;
        pm.ior = 1.5f;    // 玻璃默认折射率
        pm.diffuseTransmission = 0f;

        pm.flags = flags;

        return pm;
    }

    // ── mesh 几何追加 helper（静态/蒙皮两路共用）─────────

    /// <summary>把 mesh 的顶点属性（object/bind-pose 空间）追加到全局顶点数组。</summary>
    private static void AppendVertices(List<PathVertex> globalVerts, Mesh mesh)
    {
        Vector3[] verts = mesh.vertices;
        Vector3[] norms = mesh.normals;
        Vector4[] tangs = mesh.tangents;
        Vector2[] uvs   = mesh.uv;
        Color[]   colors = mesh.colors;
        bool hasNormal  = norms  != null && norms.Length  == verts.Length;
        bool hasTangent = tangs  != null && tangs.Length  == verts.Length;
        bool hasUv      = uvs    != null && uvs.Length     == verts.Length;
        bool hasColor   = colors != null && colors.Length == verts.Length;

        for (int i = 0; i < verts.Length; i++)
        {
            globalVerts.Add(new PathVertex
            {
                pos     = verts[i],
                normal  = hasNormal  ? norms[i]  : Vector3.zero,
                tangent = hasTangent ? tangs[i]  : Vector4.zero,
                uv      = hasUv      ? uvs[i]    : Vector2.zero,
                color   = hasColor   ? colors[i] : Color.white
            });
        }
    }

    /// <summary>
    /// 把 mesh 的三角形索引追加到全局三角形数组（保持原始顺序，值=object 局部顶点索引），
    /// 返回追加的三角形数量。
    /// </summary>
    private static int AppendTriangles(List<PathTriangle> globalTris, Mesh mesh)
    {
        int[] tris = mesh.triangles;
        for (int i = 0; i < tris.Length; i += 3)
        {
            globalTris.Add(new PathTriangle
            {
                v0 = (uint)tris[i],
                v1 = (uint)tris[i + 1],
                v2 = (uint)tris[i + 2]
            });
        }
        return tris.Length / 3;
    }

    // ═══════════════════════════════════════════════════════════
    //  Native Vulkan 插件数据推送
    // ═══════════════════════════════════════════════════════════

    // GCHandle 列表（场景重建前 Free）
    private System.Collections.Generic.List<GCHandle> _nativePins = new();
    // 非托管内存分配列表（场景重建前 FreeHGlobal）
    private System.Collections.Generic.List<IntPtr> _nativeAllocs = new();

    // 每帧复用的非托管实例数组缓冲（56B/元素，避免逐帧分配/封送）
    private IntPtr _instanceNativeBuffer = IntPtr.Zero;
    private int _instanceNativeCapacity = 0;
    private readonly float[] _transformScratch = new float[12]; // 3x4 行主序

    // ── 骨骼动画：蒙皮 mesh 追踪（BuildScene 收集，每帧推送 native）──
    // GPU 蒙皮：每帧只计算关节矩阵 worldToLocal * bone.localToWorld * bindpose
    //（拉回 SMR 局部空间，使 TLAS instance transform = SMR 世界矩阵），调 VPT_UpdateSkinning，
    // native compute 读 bind-pose base 副本就地写统一顶点缓冲后 refit（对齐参考项目 AnimationVk）。
    private sealed class NativeSkinnedTracker
    {
        public SkinnedMeshRenderer smr;        // 蒙皮渲染器（变形来源）
        public Mesh mesh;                    // sharedMesh（bind-pose 顶点/骨骼数据来源）
        public uint blasID;                   // 对应 BLAS（独占，未做 mesh 去重）
        public int  vertexCount;              // subMesh 重映射后的顶点数（恒定）
        public int[] origVertexIndices;       // subMesh 局部顶点 → 原 mesh 顶点索引（蒙皮权重重映射）
        public Transform[] bones;              // 缓存 smr.bones（避免逐帧访问开销）
        public Matrix4x4[] bindposes;          // 缓存 mesh.bindposes（每帧折算进关节矩阵）
        public int jointCount;                 // 关节数 = min(bones.Length, bindposes.Length)
        public float[] jointMatrixScratch;     // jointCount*16 行主序，常驻 pinned 避免逐帧分配
        public GCHandle jointMatPin;           // jointMatrixScratch 的 pinned 句柄
        public bool ownsJointMatrices;         // 同一 SMR 多 subMesh 共享 scratch：首个 tracker 持有（Cleanup 只 free 一次）
        public bool gpuRegistered;             // PushToNative 注册成功标记（失败则该 mesh 保持 bind-pose）
    }
    private readonly List<NativeSkinnedTracker> _skinnedTracker = new();
    private readonly HashSet<uint> _skinnedBlasIds = new(); // deformable BLAS 集合（PushToNative 打 flags 用）
    // 阶段3：上次推送的 rebuild 间隔（变化时才推送；初值哨兵保证首帧必推送）
    private uint _lastPushedRebuildInterval = uint.MaxValue;

    /// <summary>是否有待推送的场景数据（插件未就绪时延迟）。</summary>
    public bool NativePushPending => _nativePushPending;

    /// <summary>插件尚未就绪时置 true，待 PathTraceRendererFeature 初始化后重试。</summary>
    private bool _nativePushPending = false;

    /// <summary>
    /// 将场景数据推送到 Native Vulkan 插件。
    /// 在 BuildScene() 完成后调用，将顶点/索引/材质/光源/实例数据
    /// 打包为 VPT_SceneUpdate 结构传入 Native。
    /// </summary>
    public void PushToNative()
    {
        // 确保插件已初始化（首次调用触发 DLL 加载 + UnityPluginLoad + VPT_Initialize）
        if (!NativeBridge.TryInitialize())
        {
            Debug.LogWarning("[ModelManager] PushToNative: plugin not ready, deferring.");
            _nativePushPending = true;
            return;
        }

        _nativePushPending = false;

        // 释放上次的 pinned handle 和非托管分配
        foreach (var h in _nativePins) if (h.IsAllocated) h.Free();
        _nativePins.Clear();
        foreach (var p in _nativeAllocs) Marshal.FreeHGlobal(p);
        _nativeAllocs.Clear();

        if (_blasRecords.Count == 0 || GlobalVertices == null || GlobalTriangles == null)
        {
            Debug.LogWarning("[ModelManager] PushToNative: scene data not ready.");
            return;
        }

        // 1. 顶点重打包: PathVertex(64B) → VertexGpuData(80B)
        var verts80 = new VertexGpuData[GlobalVertices.Length];
        for (int i = 0; i < GlobalVertices.Length; i++)
        {
            verts80[i] = new VertexGpuData
            {
                pos     = GlobalVertices[i].pos,
                normal  = GlobalVertices[i].normal,
                tangent = GlobalVertices[i].tangent,
                uv      = GlobalVertices[i].uv,
                color   = GlobalVertices[i].color
            };
        }
        var vertPin = GCHandle.Alloc(verts80, GCHandleType.Pinned);
        _nativePins.Add(vertPin);

        // 2. 索引扁平化: PathTriangle[] → uint[]
        var flatIndices = new uint[GlobalTriangles.Length * 3];
        for (int i = 0; i < GlobalTriangles.Length; i++)
        {
            flatIndices[i * 3]     = GlobalTriangles[i].v0;
            flatIndices[i * 3 + 1] = GlobalTriangles[i].v1;
            flatIndices[i * 3 + 2] = GlobalTriangles[i].v2;
        }
        var idxPin = GCHandle.Alloc(flatIndices, GCHandleType.Pinned);
        _nativePins.Add(idxPin);

        // 3. 构建 VPT_MeshData[]（每个 BLAS 一个，顺序 = blasID）
        var meshDataArr = new NativeBridge.VPT_MeshData[_blasRecords.Count];
        for (int i = 0; i < _blasRecords.Count; i++)
        {
            var desc = _blasRecords[i];
            meshDataArr[i] = new NativeBridge.VPT_MeshData
            {
                vertices    = vertPin.AddrOfPinnedObject() + desc.vertBase * 80,
                vertexCount = (uint)desc.vertCount,
                indices     = idxPin.AddrOfPinnedObject() + desc.triBase * 3 * 4,
                indexCount  = (uint)(desc.triCount * 3),
                materialID  = 0,  // materialID 由 instance 携带
                // 骨骼动画：蒙皮 BLAS 加 ALLOW_UPDATE，供每帧 GPU 蒙皮后 refit
                flags       = _skinnedBlasIds.Contains((uint)i)
                              ? NativeBridge.VPT_MESH_FLAG_DEFORMABLE : 0u
            };
        }
        var meshPin = GCHandle.Alloc(meshDataArr, GCHandleType.Pinned);
        _nativePins.Add(meshPin);

        // 4. 材质数据（PathMaterial 96B 直接兼容）
        var matPin = GCHandle.Alloc(Materials, GCHandleType.Pinned);
        _nativePins.Add(matPin);

        // 5. 光源数据
        IntPtr lightPtr = IntPtr.Zero;
        int lightCount = 0;
        if (LightManager.Instance != null && LightManager.Instance.Lights != null)
        {
            var lightPin = GCHandle.Alloc(LightManager.Instance.Lights, GCHandleType.Pinned);
            _nativePins.Add(lightPin);
            lightPtr = lightPin.AddrOfPinnedObject();
            lightCount = LightManager.Instance.Lights.Length;
        }

        // 6. 实例数据转换: InstanceRecord → VPT_InstanceData（3x4 行主序）
        var vptInstances = new NativeBridge.VPT_InstanceData[_instances.Count];
        for (int i = 0; i < _instances.Count; i++)
        {
            InstanceRecord rec = _instances[i];
            Matrix4x4 m = rec.transform != null ? rec.transform.localToWorldMatrix : rec.localToWorld;
            float[] transform3x4 = new float[12];
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++)
                    transform3x4[row * 4 + col] = m[row, col];

            vptInstances[i] = new NativeBridge.VPT_InstanceData
            {
                materialID = rec.materialID,
                blasID     = rec.blasID,
                transform  = transform3x4
            };
        }
        // VPT_InstanceData 含 float[] 字段（ByValArray），GCHandle.Pinned 使用托管布局
        // （float[] 存为 8B 引用而非 48B 内联），与 C++ sizeof=56B 步长不匹配。
        // 改用 Marshal.AllocHGlobal + StructureToPtr 逐元素封送，确保 56B/元素。
        int instSize = Marshal.SizeOf<NativeBridge.VPT_InstanceData>();
        IntPtr instPtr = Marshal.AllocHGlobal(instSize * Mathf.Max(vptInstances.Length, 1));
        for (int i = 0; i < vptInstances.Length; i++)
            Marshal.StructureToPtr(vptInstances[i], instPtr + instSize * i, false);
        _nativeAllocs.Add(instPtr);

        // 7. 组装 VPT_SceneUpdate 并推送
        var sceneUpdate = new NativeBridge.VPT_SceneUpdate
        {
            meshes        = meshPin.AddrOfPinnedObject(),
            meshCount     = meshDataArr.Length,
            materials     = matPin.AddrOfPinnedObject(),
            materialCount = Materials.Length,
            lights        = lightPtr,
            lightCount    = lightCount,
            instances     = instPtr,
            instanceCount = vptInstances.Length
        };

        NativeBridge.UpdateScene(ref sceneUpdate);

        // 8. 骨骼动画：GPU 蒙皮静态数据注册（一次性，须在 UpdateScene 之后：
        //    native 侧自统一顶点缓冲复制 bind-pose base 副本）。
        //    bindposes 不上传——每帧折算进关节矩阵。
        int skinRegistered = 0;
        foreach (var tr in _skinnedTracker)
        {
            if (tr.jointCount <= 0)
            {
                Debug.LogWarning($"[ModelManager] SkinnedMesh '{tr.smr?.name}' 无有效骨骼（jointCount=0），"
                               + "GPU 蒙皮不可用，该 mesh 保持 bind-pose。", tr.smr);
                continue;
            }

            BoneWeight[] bw = tr.mesh.boneWeights;
            if (bw == null || bw.Length != tr.mesh.vertexCount)
            {
                Debug.LogWarning($"[ModelManager] SkinnedMesh '{tr.smr?.name}' boneWeights 数量"
                               + $"（{bw?.Length ?? 0}）与顶点数（{tr.mesh.vertexCount}）不符，跳过 GPU 蒙皮注册。", tr.smr);
                continue;
            }

            // boneWeights → vec4 权重 + ivec4 关节索引（按 subMesh 重映射顺序，行主序展开，临时 pin）
            var weights = new float[tr.vertexCount * 4];
            var joints  = new int[tr.vertexCount * 4];
            for (int i = 0; i < tr.vertexCount; i++)
            {
                BoneWeight w = bw[tr.origVertexIndices[i]];
                weights[i * 4 + 0] = w.weight0;
                weights[i * 4 + 1] = w.weight1;
                weights[i * 4 + 2] = w.weight2;
                weights[i * 4 + 3] = w.weight3;
                joints[i * 4 + 0] = w.boneIndex0;
                joints[i * 4 + 1] = w.boneIndex1;
                joints[i * 4 + 2] = w.boneIndex2;
                joints[i * 4 + 3] = w.boneIndex3;
            }
            var wPin = GCHandle.Alloc(weights, GCHandleType.Pinned);
            var jPin = GCHandle.Alloc(joints, GCHandleType.Pinned);
            try
            {
                NativeBridge.VPT_RegisterSkinnedMesh(tr.blasID,
                    wPin.AddrOfPinnedObject(), jPin.AddrOfPinnedObject(),
                    tr.vertexCount, tr.jointCount);
                tr.gpuRegistered = true;
                skinRegistered++;
            }
            finally
            {
                wPin.Free();
                jPin.Free();
            }
        }

        Debug.Log($"[ModelManager] PushToNative: {meshDataArr.Length} meshes, " +
                  $"{Materials.Length} mats, {lightCount} lights, " +
                  $"{vptInstances.Length} instances, " +
                  $"蒙皮注册={skinRegistered}");
    }

    // ═════════════════════════════════════════════════════════
    //  动态场景：每帧实例同步（刚体运动，仅重建 TLAS，BLAS 不动）
    //  由 PathTraceRendererFeature 在 PrepareDispatch 之前调用。
    //  时域累积交由 DLSS RR 处理，无需额外重置逻辑。
    // ═════════════════════════════════════════════════════════

    /// <summary>
    /// 把实例表内活跃实例的当前 localToWorldMatrix 写入复用缓冲，
    /// 调用 VPT_UpdateInstances 触发 native 侧下一帧 TLAS 重建。
    /// 隐藏（activeInHierarchy=false）的实例跳过；实例顺序保持稳定。
    /// </summary>
    public void SyncInstancesNative()
    {
        if (_instances.Count == 0)
            return;
        if (!NativeBridge.TryInitialize())
            return;

        EnsureInstanceNativeBuffer(_instances.Count);

        int stride = Marshal.SizeOf<NativeBridge.VPT_InstanceData>(); // 56B，与 native 布局一致
        int written = 0;
        for (int i = 0; i < _instances.Count; i++)
        {
            Transform t = _instances[i].transform;
            if (t == null || !t.gameObject.activeInHierarchy)
                continue;

            // 4x4 行主序取前 3 行 → 3x4（与 VkAccelerationStructureInstanceKHR 兼容）
            Matrix4x4 m = t.localToWorldMatrix;
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++)
                    _transformScratch[row * 4 + col] = m[row, col];

            IntPtr dst = _instanceNativeBuffer + written * stride;
            Marshal.WriteInt32(dst, 0, (int)_instances[i].materialID);
            Marshal.WriteInt32(dst, 4, (int)_instances[i].blasID);
            Marshal.Copy(_transformScratch, 0, dst + 8, 12);
            written++;
        }

        if (written > 0)
            NativeBridge.VPT_UpdateInstances(_instanceNativeBuffer, written);
    }

    // ═════════════════════════════════════════════════════════
    //  骨骼动画：每帧蒙皮同步（GPU 蒙皮）
    //  由 PathTraceRendererFeature 在 PrepareDispatch 之前调用（与 SyncInstancesNative 同处）。
    //  时域累积交由 DLSS RR 处理；调用方依据返回值重置自研时域累积。
    // ═════════════════════════════════════════════════════════

    /// <summary>
    /// 对每个追踪的 SkinnedMeshRenderer 推送本帧蒙皮状态：
    /// 只算 jointCount 个关节矩阵（小数据量，参考项目 AnimationVk 的性能关键），
    /// native 蒙皮 compute 读 bind-pose base 副本就地写统一顶点缓冲后 BLAS refit。
    /// 隐藏（activeInHierarchy=false）或禁用的 SMR 跳过。
    /// 返回 true 表示本帧至少推送了一个蒙皮 mesh（几何变形中，调用方可据此重置历史）。
    /// </summary>
    public bool SyncSkinnedMeshesNative()
    {
        if (_skinnedTracker.Count == 0)
            return false;
        if (!NativeBridge.TryInitialize())
            return false;

        // 阶段3：推送 refit 全量 rebuild 间隔
        uint rebuildInterval = (uint)Mathf.Max(0, blasRebuildInterval);
        if (rebuildInterval != _lastPushedRebuildInterval)
        {
            NativeBridge.VPT_SetDeformableRebuildInterval(rebuildInterval);
            _lastPushedRebuildInterval = rebuildInterval;
        }

        bool any = false;
        for (int t = 0; t < _skinnedTracker.Count; t++)
        {
            NativeSkinnedTracker tr = _skinnedTracker[t];
            SkinnedMeshRenderer smr = tr.smr;
            if (smr == null) continue;
            if (!smr.gameObject.activeInHierarchy || !smr.enabled) continue;

            if (tr.gpuRegistered)
            {
                // 每帧只传关节矩阵（小数据），native compute 就地写顶点 + refit
                PushJointMatrices(tr);
                NativeBridge.VPT_UpdateSkinning(tr.blasID,
                    tr.jointMatPin.AddrOfPinnedObject(), tr.jointCount);
                any = true;
            }
        }
        return any;
    }

    /// <summary>
    /// 计算并写入本帧关节矩阵（行主序 4×16 float）：
    /// jointMatrix[i] = SMR.worldToLocal * bones[i].localToWorld * bindposes[i]，
    /// 把 bind-pose 顶点拉到当前姿态的 SMR 局部空间，
    /// 故 TLAS instance transform 继续用 SMR 世界矩阵（复用现有 InstanceInfo 逻辑）。
    /// </summary>
    private static void PushJointMatrices(NativeSkinnedTracker tr)
    {
        // 同一 SMR 多 subMesh 共享 scratch：由首个（owns）tracker 统一计算，其余复用
        if (!tr.ownsJointMatrices) return;

        Matrix4x4 worldToLocal = tr.smr.transform.worldToLocalMatrix;
        var scratch = tr.jointMatrixScratch;
        for (int i = 0; i < tr.jointCount; i++)
        {
            // 缺失骨骼（Unity 允许 null）→ 单位矩阵：该关节影响范围内顶点保持 bind-pose
            Matrix4x4 m = Matrix4x4.identity;
            Transform bone = tr.bones != null && i < tr.bones.Length ? tr.bones[i] : null;
            if (bone != null)
                m = worldToLocal * bone.localToWorldMatrix * tr.bindposes[i];

            int o = i * 16;
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    scratch[o + r * 4 + c] = m[r, c];
        }
    }

    /// <summary>释放蒙皮追踪资源（关节矩阵 pin）；BuildScene 重建与 OnDestroy 时调用。</summary>
    private void CleanupSkinnedTrackers()
    {
        foreach (NativeSkinnedTracker tr in _skinnedTracker)
        {
            // 关节矩阵 scratch 同一 SMR 共享：只有持有者释放，避免重复 free
            if (tr.ownsJointMatrices && tr.jointMatPin.IsAllocated) tr.jointMatPin.Free();
        }
        _skinnedTracker.Clear();
        _skinnedBlasIds.Clear();
    }

    /// <summary>确保实例非托管缓冲容量（只增不减，避免逐帧分配）</summary>
    private void EnsureInstanceNativeBuffer(int capacity)
    {
        if (capacity <= _instanceNativeCapacity && _instanceNativeBuffer != IntPtr.Zero)
            return;

        int newCapacity = Mathf.Max(capacity, _instanceNativeCapacity * 2);
        int stride = Marshal.SizeOf<NativeBridge.VPT_InstanceData>();
        if (_instanceNativeBuffer != IntPtr.Zero)
            Marshal.FreeHGlobal(_instanceNativeBuffer);
        _instanceNativeBuffer = Marshal.AllocHGlobal(newCapacity * stride);
        _instanceNativeCapacity = newCapacity;
    }

    private void OnDestroyNative()
    {
        foreach (var h in _nativePins) if (h.IsAllocated) h.Free();
        _nativePins.Clear();
        foreach (var p in _nativeAllocs) Marshal.FreeHGlobal(p);
        _nativeAllocs.Clear();
        CleanupSkinnedTrackers();
        if (_instanceNativeBuffer != IntPtr.Zero)
        {
            Marshal.FreeHGlobal(_instanceNativeBuffer);
            _instanceNativeBuffer = IntPtr.Zero;
            _instanceNativeCapacity = 0;
        }
    }
}
