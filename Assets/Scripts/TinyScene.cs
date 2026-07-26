using System;
using UnityEngine;

/// <summary>
/// tinybvh 两层级加速结构（TLAS + BLAS）的 C# 友好封装。
/// 典型流程：构造 → AddBLAS(每个 mesh) → AddInstance(每个实例+变换) →
///           BuildTLAS → Raycast / RaycastBatch → Dispose
/// 使用示例：
///   var scene = new TinyScene();
///   int blas0 = scene.AddBLAS(meshA);
///   scene.AddInstance(blas0, transformA);
///   scene.BuildTLAS();
///   var hit = scene.Raycast(origin, dir, 1000f);
///   scene.Dispose();
/// </summary>
public class TinyScene : IDisposable
{
    private IntPtr _scene;
    private bool   _disposed;

    /// <summary>TLAS 射线命中结果</summary>
    public struct SceneRayHit
    {
        public bool  hit;             // 是否命中
        public int   instanceIndex;  // 命中的实例索引（TLAS 模式）
        public int   triangleIndex;  // BLAS 内的三角形索引
        public float distance;        // 射线参数 t（距离）
        public float u, v;            // 重心坐标
    }

    public TinyScene()
    {
        _scene = TinyBVHNative.tinybvh_scene_create();
    }

    // ── BLAS 管理 ─────────────────────────────────────────

    /// <summary>从 Unity Mesh 构建一个 BLAS，返回其索引</summary>
    public int AddBLAS(Mesh mesh)
    {
        return AddBLAS(mesh.vertices, mesh.triangles);
    }

    /// <summary>从顶点 + 三角形索引构建一个 BLAS，返回其索引</summary>
    /// <param name="vertBase">该 BLAS 顶点在全局顶点数组中的基地址（路径追踪用，默认 0）</param>
    public int AddBLAS(Vector3[] vertices, int[] triangles, int vertBase = 0)
    {
        float[] data = FlattenBvhVec4(vertices, triangles, out int triCount);
        return TinyBVHNative.tinybvh_scene_add_blas(_scene, data, triCount, vertBase);
    }

    /// <summary>
    /// 使用 indexed mesh 构建 BLAS（支持共享顶点）。
    /// vertices 为顶点位置数组，triangles 为顶点索引数组（每三角形 3 个）。
    /// vertBase 为该 BLAS 顶点在全局顶点结构数组中的基地址。
    /// </summary>
    public int AddBLASIndexed(Vector3[] vertices, int[] triangles, int vertBase)
    {
        // 转为 bvhvec4 数组（xyz + pad）
        int vertCount = vertices.Length;
        float[] vertData = new float[vertCount * 4];
        for (int i = 0; i < vertCount; i++)
        {
            vertData[i * 4 + 0] = vertices[i].x;
            vertData[i * 4 + 1] = vertices[i].y;
            vertData[i * 4 + 2] = vertices[i].z;
            vertData[i * 4 + 3] = 0f;
        }
        // 转为 uint32 索引数组
        uint[] indices = new uint[triangles.Length];
        for (int i = 0; i < triangles.Length; i++)
            indices[i] = (uint)triangles[i];
        int triCount = triangles.Length / 3;
        return TinyBVHNative.tinybvh_scene_add_blas_indexed(
            _scene, vertData, indices, triCount, vertBase);
    }

    /// <summary>更新指定 BLAS 的几何（用于变形 mesh），需在 BuildTLAS 前调用</summary>
    public void UpdateBLAS(int blasIdx, Mesh mesh)
    {
        UpdateBLAS(blasIdx, mesh.vertices, mesh.triangles);
    }

    /// <summary>更新指定 BLAS 的几何（用于变形 mesh），需在 BuildTLAS 前调用</summary>
    public void UpdateBLAS(int blasIdx, Vector3[] vertices, int[] triangles)
    {
        float[] data = FlattenBvhVec4(vertices, triangles, out int triCount);
        TinyBVHNative.tinybvh_scene_update_blas(_scene, blasIdx, data, triCount);
    }

    // ── Instance 管理 ────────────────────────────────────

    /// <summary>添加一个实例（使用 Transform 的 localToWorldMatrix）</summary>
    public int AddInstance(int blasIdx, Transform transform, int materialID = 0)
    {
        return AddInstance(blasIdx, transform.localToWorldMatrix, materialID);
    }

    /// <summary>
    /// 添加一个实例，使用给定的 object→world 变换矩阵。
    /// 注意：传入的是 localToWorldMatrix，内部会转换为 tinybvh 所需的行主序布局。
    /// materialID 为该实例的材质索引（路径追踪着色用，默认 0）。
    /// </summary>
    public int AddInstance(int blasIdx, Matrix4x4 localToWorld, int materialID = 0)
    {
        float[] t16 = ToRowMajor(localToWorld);
        return TinyBVHNative.tinybvh_scene_add_instance(_scene, blasIdx, t16, materialID);
    }

    /// <summary>更新指定实例的变换（使用 Transform 的 localToWorldMatrix）</summary>
    public void UpdateInstance(int instIdx, Transform transform)
    {
        UpdateInstance(instIdx, transform.localToWorldMatrix);
    }

    /// <summary>更新指定实例的 object→world 变换矩阵</summary>
    public void UpdateInstance(int instIdx, Matrix4x4 localToWorld)
    {
        float[] t16 = ToRowMajor(localToWorld);
        TinyBVHNative.tinybvh_scene_update_instance(_scene, instIdx, t16);
    }

    /// <summary>
    /// 删除指定实例。采用 swap-remove：O(1) 但会使原末尾实例挪到被删位置，
    /// 因此外部缓存的实例索引在删除后可能失效。删除后需重新 BuildTLAS。
    /// </summary>
    public void RemoveInstance(int instIdx)
    {
        TinyBVHNative.tinybvh_scene_remove_instance(_scene, instIdx);
    }

    // ── 构建 TLAS ────────────────────────────────────────

    /// <summary>
    /// 构建/重建顶层加速结构。任何 BLAS 或 Instance 的变动后都需要重新调用。
    /// </summary>
    public void BuildTLAS()
    {
        TinyBVHNative.tinybvh_scene_build_tlas(_scene);
    }

    // ── 路径追踪数据导出 ────────────────────────────────

    /// <summary>
    /// 组装全局 BLAS 数据数组（节点拼接+重定位、primIdx拼接、描述表）。
    /// 在所有 BLAS 构建完成且 BuildTLAS 之后调用。
    /// </summary>
    public void AssembleGlobals()
    {
        TinyBVHNative.tinybvh_scene_assemble_globals(_scene);
    }

    /// <summary>获取 TLAS 节点数组指针（32B/节点，TLAS rebuild 后失效）</summary>
    public IntPtr GetTLASNodes(out int count)
    {
        return TinyBVHNative.tinybvh_scene_get_tlas_nodes(_scene, out count);
    }

    /// <summary>获取 TLAS primIdx 数组指针（实例索引，TLAS rebuild 后失效）</summary>
    public IntPtr GetTLASPrimIdx(out int count)
    {
        return TinyBVHNative.tinybvh_scene_get_tlas_primidx(_scene, out count);
    }

    /// <summary>获取全局 BLAS 节点数组指针（已重定位，assemble 后有效）</summary>
    public IntPtr GetGlobalBLASNodes(out int count)
    {
        return TinyBVHNative.tinybvh_scene_get_global_blas_nodes(_scene, out count);
    }

    /// <summary>获取全局 BLAS primIdx 数组指针（assemble 后有效）</summary>
    public IntPtr GetGlobalBLASPrimIdx(out int count)
    {
        return TinyBVHNative.tinybvh_scene_get_global_blas_primidx(_scene, out count);
    }

    /// <summary>获取 BLAS 描述表指针（按 blasID 索引，assemble 后有效）</summary>
    public IntPtr GetBLASDescriptors(out int count)
    {
        return TinyBVHNative.tinybvh_scene_get_blas_descriptors(_scene, out count);
    }

    /// <summary>获取实例表指针（按实例 ID 索引，含 materialID/blasID/transform/invTransform）</summary>
    public IntPtr GetInstanceTable(out int count)
    {
        return TinyBVHNative.tinybvh_scene_get_instance_table(_scene, out count);
    }

    // ── 射线求交 ─────────────────────────────────────────

    /// <summary>单条射线与整个 TLAS 求交（两层级遍历）</summary>
    public SceneRayHit Raycast(Vector3 origin, Vector3 direction, float maxDistance = 1e30f)
    {
        var r = TinyBVHNative.tinybvh_scene_intersect(
            _scene,
            origin.x, origin.y, origin.z,
            direction.x, direction.y, direction.z,
            maxDistance);
        return ToSceneRayHit(r);
    }

    /// <summary>批量射线求交（减少 P/Invoke 跨边界开销，路径追踪推荐）</summary>
    public SceneRayHit[] RaycastBatch(Vector3[] origins, Vector3[] directions, float maxDistance = 1e30f)
    {
        int count = origins.Length;
        float[] oriFlat = new float[count * 3];
        float[] dirFlat = new float[count * 3];
        for (int i = 0; i < count; i++)
        {
            oriFlat[i * 3 + 0] = origins[i].x;
            oriFlat[i * 3 + 1] = origins[i].y;
            oriFlat[i * 3 + 2] = origins[i].z;
            dirFlat[i * 3 + 0] = directions[i].x;
            dirFlat[i * 3 + 1] = directions[i].y;
            dirFlat[i * 3 + 2] = directions[i].z;
        }

        var native = new TinyBVHNative.HitResult[count];
        TinyBVHNative.tinybvh_scene_intersect_batch(
            _scene, oriFlat, dirFlat, native, count, maxDistance);

        var results = new SceneRayHit[count];
        for (int i = 0; i < count; i++)
            results[i] = ToSceneRayHit(native[i]);
        return results;
    }

    // ── 生命周期 ─────────────────────────────────────────

    public void Dispose()
    {
        if (!_disposed && _scene != IntPtr.Zero)
        {
            TinyBVHNative.tinybvh_scene_destroy(_scene);
            _scene   = IntPtr.Zero;
            _disposed = true;
        }
    }

    ~TinyScene() { Dispose(); }

    // ── 内部工具 ─────────────────────────────────────────

    private static SceneRayHit ToSceneRayHit(TinyBVHNative.HitResult r) => new SceneRayHit
    {
        hit           = r.hit != 0,
        instanceIndex = r.instIdx,
        triangleIndex = r.primIdx,
        distance      = r.t,
        u             = r.u,
        v             = r.v
    };

    /// <summary>
    /// 把 Unity 顶点 + 三角形索引展平为 tinybvh 所需的 bvhvec4 数组：
    /// 每顶点 4 个 float (x,y,z,pad)，按三角形顺序排列，共 triCount*3 个顶点。
    /// </summary>
    private static float[] FlattenBvhVec4(Vector3[] vertices, int[] triangles, out int triCount)
    {
        triCount = triangles.Length / 3;
        float[] data = new float[triCount * 3 * 4];
        for (int i = 0; i < triCount; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                Vector3 v = vertices[triangles[i * 3 + j]];
                int b = (i * 3 + j) * 4;
                data[b + 0] = v.x;
                data[b + 1] = v.y;
                data[b + 2] = v.z;
                data[b + 3] = 0f; // pad
            }
        }
        return data;
    }

    /// <summary>
    /// Unity Matrix4x4 → tinybvh bvhmat4(行主序 float[16]) 转换。
    ///
    /// 约定说明（已核对 tiny_bvh.h 源码）：
    ///   - tinybvh bvhmat4.cell[16] 为行主序：cell[row*4+col]，平移在 [3],[7],[11]。
    ///   - tinybvh_transform_point(v, T) 计算 result = M·[v;1]（列向量约定），
    ///     其中 M 即 BLASInstance.transform，语义为 object→world。
    ///   - Unity Matrix4x4 索引器 this[row,col] 返回数学元素 M[row,col]，
    ///     localToWorldMatrix 同样是 object→world、列向量约定。
    ///   - 因此直接 t[row*4+col] = m[row,col]，无需转置。
    /// </summary>
    private static float[] ToRowMajor(Matrix4x4 m)
    {
        float[] t = new float[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                t[r * 4 + c] = m[r, c];
        return t;
    }
}
