using System;
using System.Runtime.InteropServices;

public static class TinyBVHNative
{
    private const string DllName = "TinyBVHNative";

    [StructLayout(LayoutKind.Sequential)]
    public struct HitResult
    {
        public int   hit;
        public int   instIdx;  // 命中的实例索引（TLAS 模式），单 BLAS 模式恒为 0
        public int   primIdx;  // 命中的三角形索引（BLAS 内）
        public float t;
        public float u, v;
    }

    // ── 路径追踪数据导出结构体 ─────────────────────────

    /// <summary>BLAS 描述表元素（按 blasID 索引）</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct BLASDescriptor
    {
        public uint blasNodeBase;  // 该 BLAS 节点在全局节点数组中的基地址
        public uint primIdxBase;   // 该 BLAS primIdx 在全局 primIdx 数组中的基地址
        public uint triBase;       // 该 BLAS 三角形在全局三角形数组中的基地址
        public uint vertBase;      // 该 BLAS 顶点在全局顶点数组中的基地址
    }

    /// <summary>实例表元素（按实例 ID 索引）</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct InstanceRecord
    {
        public uint materialID;
        public uint blasID;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public float[] transform;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public float[] invTransform;
    }

    // ── Scene / TLAS / BLAS 接口 ──────────────────────────
    // 典型流程：scene_create → add_blas(每个 mesh) → add_instance(每个实例) →
    //          build_tlas → scene_intersect / scene_intersect_batch

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_create();

    [DllImport(DllName)]
    public static extern void tinybvh_scene_destroy(IntPtr scene);

    [DllImport(DllName)]
    public static extern int tinybvh_scene_add_blas(
        IntPtr scene,
        float[] vertices,
        int numTriangles,
        int vertBase);

    [DllImport(DllName)]
    public static extern int tinybvh_scene_add_blas_indexed(
        IntPtr scene,
        float[] vertices,
        uint[] indices,
        int numTriangles,
        int vertBase);

    [DllImport(DllName)]
    public static extern void tinybvh_scene_update_blas(
        IntPtr scene,
        int blasIdx,
        float[] vertices,
        int numTriangles);

    [DllImport(DllName)]
    public static extern int tinybvh_scene_add_instance(
        IntPtr scene,
        int blasIdx,
        float[] transform16,
        int materialID);

    [DllImport(DllName)]
    public static extern void tinybvh_scene_update_instance(
        IntPtr scene,
        int instIdx,
        float[] transform16);

    [DllImport(DllName)]
    public static extern void tinybvh_scene_remove_instance(
        IntPtr scene,
        int instIdx);

    [DllImport(DllName)]
    public static extern void tinybvh_scene_build_tlas(IntPtr scene);

    [DllImport(DllName)]
    public static extern HitResult tinybvh_scene_intersect(
        IntPtr scene,
        float ox, float oy, float oz,
        float dx, float dy, float dz,
        float tMax);

    [DllImport(DllName)]
    public static extern void tinybvh_scene_intersect_batch(
        IntPtr scene,
        float[] origins,
        float[] directions,
        HitResult[] results,
        int count,
        float tMax);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_create();

    [DllImport(DllName)]
    public static extern void tinybvh_destroy(IntPtr handle);

    [DllImport(DllName)]
    public static extern void tinybvh_build(IntPtr handle, float[] vertices, int numTriangles);

    [DllImport(DllName)]
    public static extern HitResult tinybvh_intersect(
        IntPtr handle,
        float ox, float oy, float oz,
        float dx, float dy, float dz,
        float tMax);

    [DllImport(DllName)]
    public static extern void tinybvh_intersect_batch(
        IntPtr handle,
        float[] origins,
        float[] directions,
        HitResult[] results,
        int count,
        float tMax);

    // ── 路径追踪数据导出接口 ────────────────────────────
    // 典型流程：add_blas → add_instance → build_tlas →
    //          assemble_globals → 查询各数组

    [DllImport(DllName)]
    public static extern void tinybvh_scene_assemble_globals(IntPtr scene);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_get_tlas_nodes(
        IntPtr scene, out int outCount);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_get_tlas_primidx(
        IntPtr scene, out int outCount);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_get_global_blas_nodes(
        IntPtr scene, out int outCount);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_get_global_blas_primidx(
        IntPtr scene, out int outCount);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_get_blas_descriptors(
        IntPtr scene, out int outCount);

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_scene_get_instance_table(
        IntPtr scene, out int outCount);
}
