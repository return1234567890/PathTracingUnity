using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

/// <summary>
/// 路径追踪顶点结构体（全局顶点数组元素）。
/// 数据为 object-space，世界变换由实例表的 transform/invTransform 在 shader 侧完成。
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

/// <summary>材质数组元素（按 materialID 索引，80B）。</summary>
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
/// BVH 节点（32 字节，Wald 布局），与 tinybvh BVH::BVHNode 一致。
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct BVHNode32
{
    public Vector3 aabbMin;  // 12B
    public uint    leftFirst;// 4B  内部节点=左子节点索引；叶子=primIdx 起始
    public Vector3 aabbMax;  // 12B
    public uint    triCount; // 4B  内部节点=0；叶子>0
}

/// <summary>
/// 场景模型收集与加速结构构建管理器。
/// 在游戏开始时收集场景中的模型物体，通过 <see cref="TinyScene"/> 构建两层级加速结构，
/// 并组装 <c>tinybvh封装使用示例.md</c> 中定义的 9 个路径追踪数据数组。
///
/// 9 个数组：
///  1. TLAS 节点数组          → <see cref="TlasNodes"/>
///  2. TLAS primIdx           → <see cref="TlasPrimIdx"/>
///  3. 全局 BLAS 节点数组     → <see cref="GlobalBlasNodes"/>
///  4. 全局 BLAS primIdx      → <see cref="GlobalBlasPrimIdx"/>
///  5. BLAS 描述表            → <see cref="BlasDescriptors"/>
///  6. 实例表                  → <see cref="InstanceTable"/>
///  7. 全局三角形数组          → <see cref="GlobalTriangles"/>
///  8. 全局顶点数组            → <see cref="GlobalVertices"/>
///  9. 材质颜色数组            → <see cref="Materials"/>
/// </summary>
public class ModelManager : MonoBehaviour
{
    [Header("收集设置")]
    [Tooltip("只收集带此 Tag 的对象；留空(=Untagged)表示收集场景中所有 MeshFilter")]
    public string targetTag = "Untagged";

    [Tooltip("是否在构建完成后打印各数组统计信息")]
    public bool logBuildStats = true;

    /// <summary>全局单例，便于路径追踪器访问。</summary>
    public static ModelManager Instance { get; private set; }

    // ── TinyScene 句柄 ──────────────────────────────────
    public TinyScene Scene { get; private set; }

    // ════════ 9 个数据数组（构建后可读） ════════

    // —— 来自 TinyScene 原生导出（已拷贝为托管数组，供 CPU 路径追踪）——
    /// <summary>1. TLAS 节点数组（32B/节点）</summary>
    public BVHNode32[]  TlasNodes        { get; private set; }
    /// <summary>2. TLAS primIdx（实例索引映射）</summary>
    public uint[]       TlasPrimIdx      { get; private set; }
    /// <summary>3. 全局 BLAS 节点数组（子节点索引已重定位）</summary>
    public BVHNode32[]  GlobalBlasNodes  { get; private set; }
    /// <summary>4. 全局 BLAS primIdx（三角形索引映射）</summary>
    public uint[]       GlobalBlasPrimIdx { get; private set; }
    /// <summary>5. BLAS 描述表（按 blasID 索引）</summary>
    public TinyBVHNative.BLASDescriptor[] BlasDescriptors { get; private set; }
    /// <summary>6. 实例表（按实例 ID 索引）</summary>
    public TinyBVHNative.InstanceRecord[] InstanceTable  { get; private set; }

    // —— 外部维护数组 ——
    /// <summary>7. 全局三角形数组（按 triBase 偏移拼接）</summary>
    public PathTriangle[] GlobalTriangles { get; private set; }
    /// <summary>8. 全局顶点数组（按 vertBase 偏移拼接，object-space）</summary>
    public PathVertex[]   GlobalVertices  { get; private set; }
    /// <summary>9. 材质数组（按 materialID 索引，含纹理索引与UV变换）</summary>
    public PathMaterial[]  Materials       { get; private set; }

    /// <summary>纹理数组管理器（4个 Texture2DArray），供 RenderBufferManager 访问。</summary>
    public TextureArrayManager TextureArrays { get; private set; }

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
        if (Instance == this)
            Instance = null;
        Scene?.Dispose();
        Scene = null;
        TextureArrays?.Dispose();
        TextureArrays = null;
    }

    // ── 构建主流程 ──────────────────────────────────────

    /// <summary>
    /// 收集场景模型、构建加速结构、组装 9 个数组。
    /// 在 Start 中自动调用；场景变更后可手动调用重建。
    /// </summary>
    public void BuildScene()
    {
        Scene?.Dispose();
        Scene = new TinyScene();

        // 释放旧纹理数组
        TextureArrays?.Dispose();
        TextureArrays = new TextureArrayManager();

        // 临时容器
        var globalVerts  = new List<PathVertex>();
        var globalTris   = new List<PathTriangle>();
        var materials    = new List<PathMaterial>();
        var matToId      = new Dictionary<Material, int>();
        var meshToBlas   = new Dictionary<Mesh, int>();
        var meshToVertBase = new Dictionary<Mesh, int>();
        var meshToTriBase  = new Dictionary<Mesh, int>();

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

            // —— 1a. 为每个唯一 mesh 构建 BLAS（去重）——
            if (!meshToBlas.TryGetValue(mesh, out int blasId))
            {
                int vertBase = globalVerts.Count;
                int triBase  = globalTris.Count;

                // 经由 AddBLASIndexed：vertices=object-space 顶点，triangles=顶点索引
                blasId = Scene.AddBLASIndexed(mesh.vertices, mesh.triangles, vertBase);
                meshToBlas[mesh] = blasId;
                meshToVertBase[mesh] = vertBase;
                meshToTriBase[mesh]  = triBase;

                // 追加该 mesh 的顶点到全局顶点数组（object-space）
                Vector3[] verts   = mesh.vertices;
                Vector3[] norms    = mesh.normals;
                Vector4[] tangs    = mesh.tangents;
                Vector2[] uvs      = mesh.uv;
                Color[]   colors    = mesh.colors;
                bool hasNormal = norms  != null && norms.Length  == verts.Length;
                bool hasTangent= tangs  != null && tangs.Length  == verts.Length;
                bool hasUv     = uvs    != null && uvs.Length     == verts.Length;
                bool hasColor  = colors != null && colors.Length == verts.Length;

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

                // 追加三角形索引到全局三角形数组（保持原始顺序，值=object 局部顶点索引）
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

            // —— 1c. 添加实例（object→world 变换由 TinyScene 处理）——
            Scene.AddInstance(blasId, go.transform, materialId);
            collectedInstanceCount++;
        }

        // 2. 构建 TLAS + 组装全局 BLAS 数据
        Scene.BuildTLAS();
        Scene.AssembleGlobals();

        // 3. 拷贝 6 个原生数组到托管数组
        TlasNodes         = CopyBVHNodes(Scene.GetTLASNodes(out int tlasNodeCount), tlasNodeCount);
        TlasPrimIdx       = CopyUint32(Scene.GetTLASPrimIdx(out int tlasPrimCount), tlasPrimCount);
        GlobalBlasNodes   = CopyBVHNodes(Scene.GetGlobalBLASNodes(out int blasNodeCount), blasNodeCount);
        GlobalBlasPrimIdx = CopyUint32(Scene.GetGlobalBLASPrimIdx(out int blasPrimCount), blasPrimCount);
        BlasDescriptors   = CopyBlasDescriptors(Scene.GetBLASDescriptors(out int descCount), descCount);
        InstanceTable     = CopyInstanceTable(Scene.GetInstanceTable(out int instCount), instCount);

        // 4. 提交外部维护数组
        GlobalVertices  = globalVerts.ToArray();
        GlobalTriangles = globalTris.ToArray();
        Materials       = materials.ToArray();

        // 4b. 构建纹理数组（所有材质纹理注册完毕后打包）
        TextureArrays.Build(1024);

        // 5. 统计
        if (logBuildStats)
        {
            Debug.Log(
                "[ModelManager] 构建完成：" +
                $"BLAS={meshToBlas.Count}, 实例={collectedInstanceCount}, 材质={Materials.Length}, " +
                $"TLAS节点={TlasNodes.Length}, TLASprimIdx={TlasPrimIdx.Length}, " +
                $"全局BLAS节点={GlobalBlasNodes.Length}, 全局BLASprimIdx={GlobalBlasPrimIdx.Length}, " +
                $"三角形={GlobalTriangles.Length}, 顶点={GlobalVertices.Length}");
        }
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
        pm.flags = flags;

        // ── 新增：emissive color ──
        if (mat.HasProperty("_EmissionColor"))
            pm.emissionColor = mat.GetColor("_EmissionColor");

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

    // ── 原生指针 → 托管数组 拷贝工具 ────────────────────

    private static BVHNode32[] CopyBVHNodes(IntPtr ptr, int count)
    {
        if (ptr == IntPtr.Zero || count <= 0) return Array.Empty<BVHNode32>();
        var arr = new BVHNode32[count];
        int size = Marshal.SizeOf<BVHNode32>();
        for (int i = 0; i < count; i++)
            arr[i] = Marshal.PtrToStructure<BVHNode32>(ptr + i * size);
        return arr;
    }

    private static uint[] CopyUint32(IntPtr ptr, int count)
    {
        var arr = new uint[count];
        for (int i = 0; i < count; i++)
            arr[i] = (uint)Marshal.ReadInt32(ptr, i * 4);
        return arr;
    }

    private static TinyBVHNative.BLASDescriptor[] CopyBlasDescriptors(IntPtr ptr, int count)
    {
        var arr = new TinyBVHNative.BLASDescriptor[count];
        int size = Marshal.SizeOf<TinyBVHNative.BLASDescriptor>();
        for (int i = 0; i < count; i++)
            arr[i] = Marshal.PtrToStructure<TinyBVHNative.BLASDescriptor>(ptr + i * size);
        return arr;
    }

    private static TinyBVHNative.InstanceRecord[] CopyInstanceTable(IntPtr ptr, int count)
    {
        var arr = new TinyBVHNative.InstanceRecord[count];
        int size = Marshal.SizeOf<TinyBVHNative.InstanceRecord>();
        for (int i = 0; i < count; i++)
            arr[i] = Marshal.PtrToStructure<TinyBVHNative.InstanceRecord>(ptr + i * size);
        return arr;
    }
}
