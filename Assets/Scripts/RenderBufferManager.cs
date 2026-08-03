using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

/// <summary>
/// GPU 数据适配层：把 ModelManager 输出的 9 个 CPU 数组
/// 转换为 HLSL 友好、字节对齐的 ComputeBuffer，并统一绑定给计算着色器。
///
/// 职责（见实现方案章节3）：
///  1. 创建并维护 9 个 ComputeBuffer（new/SetData/Release）；
///  2. CPU->GPU 布局重打包（核心价值）：
///     - InstanceRecord 136B -> InstanceGpuData 144B（16B 对齐）
///     - PathTriangle      12B -> 16B（uint4 布局）
///     - PathVertex        64B -> VertexGpuData 80B（HLSL 友好 16B 对齐）
///  3. dirty/版本管理：仅在 ModelManager 数组引用变化时刷新；
///  4. Bind(...)：一次性 SetBuffer 全部 9 个输入缓冲。
///
/// 其余天然对齐的结构（BVHNode32=32B、BLASDescriptor=16B、PathMaterial=96B）
/// 与 uint primIdx(4B) 直接 SetData，无需重打包。
/// </summary>
public class RenderBufferManager : IDisposable
{
    // ── 重打包后的 CPU 侧结构（与 HLSL 严格对齐） ─────────

    /// <summary>InstanceRecord 重打包结构（144B，16B 对齐）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct InstanceGpuData
    {
        public uint materialID;
        public uint blasID;
        public uint pad0;
        public uint pad1;
        public Vector4 tRow0; // transform 行0-3（行主序，每行 = Vector4）
        public Vector4 tRow1;
        public Vector4 tRow2;
        public Vector4 tRow3;
        public Vector4 iRow0; // invTransform 行0-3
        public Vector4 iRow1;
        public Vector4 iRow2;
        public Vector4 iRow3;
        // 4*4 + 8*16 = 16 + 128 = 144B
    }

    /// <summary>PathVertex 重打包结构（80B，HLSL 友好 16B 对齐）。</summary>
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

    /// <summary>PathTriangle 重打包结构（16B）。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TriGpuData
    {
        public uint v0;
        public uint v1;
        public uint v2;
        public uint pad;
    }

    // ── Stride 常量（与 PathTrace.compute 中 HLSL 结构一致） ──
    public const int StrideBVHNode    = 32;
    public const int StrideUint       = 4;
    public const int StrideBLASDesc   = 16;
    public const int StrideInstance   = 144;
    public const int StrideTri        = 16;
    public const int StrideVertex     = 80;
    public const int StrideMaterial   = 96;   // PathMaterial: albedo(16)+metallic(4)+roughness(4)+bumpScale(4)+flags(4)+emissionColor(16)+4xTexID(16)+uvScaleOffset(16)+transmission(4)+ior(4)+diffuseTransmission(4)+thinWalled(4)
    public const int StrideLight     = 32;   // PathLight: positionOrDir(12)+range(4)+color(12)+type(4)

    // ── 9 个 ComputeBuffer ──
    public ComputeBuffer TlasNodes        { get; private set; }
    public ComputeBuffer TlasPrimIdx      { get; private set; }
    public ComputeBuffer GlobalBlasNodes  { get; private set; }
    public ComputeBuffer GlobalBlasPrimIdx { get; private set; }
    public ComputeBuffer BlasDescriptors  { get; private set; }
    public ComputeBuffer InstanceTableBuf { get; private set; }
    public ComputeBuffer GlobalTrianglesBuf { get; private set; }
    public ComputeBuffer GlobalVerticesBuf  { get; private set; }
    public ComputeBuffer MaterialsBuf       { get; private set; }
    public ComputeBuffer LightsBuf           { get; private set; }

    // ── 4 个 Texture2DArray 引用（不创建，由 TextureArrayManager 提供）──
    private Texture2DArray _baseColorArray;
    private Texture2DArray _metallicSmoothArray;
    private Texture2DArray _normalArray;
    private Texture2DArray _emissiveArray;

    // ── 缓存上次同步的数组引用，用于 dirty 检测 ──
    private BVHNode32[]                _lastTlasNodes;
    private uint[]                     _lastTlasPrimIdx;
    private BVHNode32[]                _lastGlobalBlasNodes;
    private uint[]                     _lastGlobalBlasPrimIdx;
    private TinyBVHNative.BLASDescriptor[] _lastBlasDesc;
    private TinyBVHNative.InstanceRecord[] _lastInstance;
    private PathTriangle[]             _lastTris;
    private PathVertex[]               _lastVerts;
    private PathMaterial[]             _lastMats;
    private PathLight[]                _lastLights;

    private bool _disposed;

    // ── Sync：按需重建/上传 ─────────────────────────────

    /// <summary>
    /// 把 ModelManager 的 9 个数组同步到 ComputeBuffer。
    /// 仅在数组引用变化（场景重建）时重建 buffer；其余时间跳过。
    /// </summary>
    public void Sync(ModelManager mm)
    {
        if (mm == null)
        {
            Debug.LogWarning("[RenderBufferManager] ModelManager 为空，跳过同步。");
            return;
        }

        // 直传数组（天然对齐）：仅在引用变化时重建
        // 注意：属性不能作为 ref 参数传入，改为以普通参数传入并返回新 buffer
        TlasNodes        = SyncBVHNodes(TlasNodes, ref _lastTlasNodes, mm.TlasNodes, StrideBVHNode);
        GlobalBlasNodes  = SyncBVHNodes(GlobalBlasNodes, ref _lastGlobalBlasNodes, mm.GlobalBlasNodes, StrideBVHNode);
        TlasPrimIdx      = SyncUint(TlasPrimIdx, ref _lastTlasPrimIdx, mm.TlasPrimIdx);
        GlobalBlasPrimIdx= SyncUint(GlobalBlasPrimIdx, ref _lastGlobalBlasPrimIdx, mm.GlobalBlasPrimIdx);
        BlasDescriptors  = SyncDescriptors(BlasDescriptors, ref _lastBlasDesc, mm.BlasDescriptors);
        MaterialsBuf     = SyncMaterials(MaterialsBuf, ref _lastMats, mm.Materials);

        // 重打包数组
        SyncInstances(mm.InstanceTable);
        SyncTriangles(mm.GlobalTriangles);
        SyncVertices(mm.GlobalVertices);

        // 光源数组（独立于 ModelManager，来自 LightManager 单例）
        if (LightManager.Instance != null)
            SyncLights(LightManager.Instance.Lights);

        // ── 纹理数组同步（由 TextureArrayManager 提供）──
        if (mm.TextureArrays != null)
        {
            _baseColorArray     = mm.TextureArrays.BaseColorArray;
            _metallicSmoothArray = mm.TextureArrays.MetallicSmoothArray;
            _normalArray       = mm.TextureArrays.NormalArray;
            _emissiveArray     = mm.TextureArrays.EmissiveArray;
        }
    }

    // ── 绑定 ───────────────────────────────────────────

    /// <summary>把全部 9 个输入缓冲 + 4 个纹理数组一次性绑定到指定 kernel（立即绑定）。</summary>
    public void Bind(ComputeShader cs, int kernel)
    {
        if (cs == null) return;
        if (TlasNodes != null)        cs.SetBuffer(kernel, "TlasNodes", TlasNodes);
        if (TlasPrimIdx != null)      cs.SetBuffer(kernel, "TlasPrimIdx", TlasPrimIdx);
        if (GlobalBlasNodes != null)  cs.SetBuffer(kernel, "GlobalBlasNodes", GlobalBlasNodes);
        if (GlobalBlasPrimIdx != null) cs.SetBuffer(kernel, "GlobalBlasPrimIdx", GlobalBlasPrimIdx);
        if (BlasDescriptors != null)  cs.SetBuffer(kernel, "BlasDescriptors", BlasDescriptors);
        if (InstanceTableBuf != null) cs.SetBuffer(kernel, "InstanceTable", InstanceTableBuf);
        if (GlobalTrianglesBuf != null) cs.SetBuffer(kernel, "GlobalTriangles", GlobalTrianglesBuf);
        if (GlobalVerticesBuf != null) cs.SetBuffer(kernel, "GlobalVertices", GlobalVerticesBuf);
        if (MaterialsBuf != null)    cs.SetBuffer(kernel, "Materials", MaterialsBuf);
        if (LightsBuf != null)      cs.SetBuffer(kernel, "Lights", LightsBuf);
        // 纹理数组绑定
        if (_baseColorArray != null)      cs.SetTexture(kernel, "BaseColorArray", _baseColorArray);
        if (_metallicSmoothArray != null) cs.SetTexture(kernel, "MetallicSmoothArray", _metallicSmoothArray);
        if (_normalArray != null)         cs.SetTexture(kernel, "NormalArray", _normalArray);
        if (_emissiveArray != null)       cs.SetTexture(kernel, "EmissiveArray", _emissiveArray);
    }

    /// <summary>把全部 9 个输入缓冲 + 4 个纹理数组以 cmd 作用域绑定（URP Pass 推荐）。</summary>
    public void Bind(CommandBuffer cmd, ComputeShader cs, int kernel)
    {
        if (cs == null) return;
        if (TlasNodes != null)        cmd.SetComputeBufferParam(cs, kernel, "TlasNodes", TlasNodes);
        if (TlasPrimIdx != null)      cmd.SetComputeBufferParam(cs, kernel, "TlasPrimIdx", TlasPrimIdx);
        if (GlobalBlasNodes != null)  cmd.SetComputeBufferParam(cs, kernel, "GlobalBlasNodes", GlobalBlasNodes);
        if (GlobalBlasPrimIdx != null) cmd.SetComputeBufferParam(cs, kernel, "GlobalBlasPrimIdx", GlobalBlasPrimIdx);
        if (BlasDescriptors != null)  cmd.SetComputeBufferParam(cs, kernel, "BlasDescriptors", BlasDescriptors);
        if (InstanceTableBuf != null) cmd.SetComputeBufferParam(cs, kernel, "InstanceTable", InstanceTableBuf);
        if (GlobalTrianglesBuf != null) cmd.SetComputeBufferParam(cs, kernel, "GlobalTriangles", GlobalTrianglesBuf);
        if (GlobalVerticesBuf != null) cmd.SetComputeBufferParam(cs, kernel, "GlobalVertices", GlobalVerticesBuf);
        if (MaterialsBuf != null)    cmd.SetComputeBufferParam(cs, kernel, "Materials", MaterialsBuf);
        if (LightsBuf != null)      cmd.SetComputeBufferParam(cs, kernel, "Lights", LightsBuf);
        // 纹理数组绑定
        if (_baseColorArray != null)      cmd.SetComputeTextureParam(cs, kernel, "BaseColorArray", _baseColorArray);
        if (_metallicSmoothArray != null) cmd.SetComputeTextureParam(cs, kernel, "MetallicSmoothArray", _metallicSmoothArray);
        if (_normalArray != null)         cmd.SetComputeTextureParam(cs, kernel, "NormalArray", _normalArray);
        if (_emissiveArray != null)       cmd.SetComputeTextureParam(cs, kernel, "EmissiveArray", _emissiveArray);
    }

    // ── 直传同步工具 ───────────────────────────────────

    // 返回本次同步后的 buffer（引用变化时重建）；data 为空则原样返回旧 buffer
    private static ComputeBuffer SyncBVHNodes(ComputeBuffer buf, ref BVHNode32[] last, BVHNode32[] data, int stride)
    {
        if (data == null) return buf;
        if (buf == null || buf.count != data.Length || !ReferenceEquals(last, data))
        {
            buf?.Release();
            buf = new ComputeBuffer(data.Length, stride);
            last = data;
            buf.SetData(data);
        }
        return buf;
    }

    private static ComputeBuffer SyncUint(ComputeBuffer buf, ref uint[] last, uint[] data)
    {
        if (data == null) return buf;
        if (buf == null || buf.count != data.Length || !ReferenceEquals(last, data))
        {
            buf?.Release();
            buf = new ComputeBuffer(data.Length, StrideUint);
            last = data;
            buf.SetData(data);
        }
        return buf;
    }

    private static ComputeBuffer SyncDescriptors(ComputeBuffer buf,
        ref TinyBVHNative.BLASDescriptor[] last, TinyBVHNative.BLASDescriptor[] data)
    {
        if (data == null) return buf;
        if (buf == null || buf.count != data.Length || !ReferenceEquals(last, data))
        {
            buf?.Release();
            buf = new ComputeBuffer(data.Length, StrideBLASDesc);
            last = data;
            buf.SetData(data);
        }
        return buf;
    }

    private static ComputeBuffer SyncMaterials(ComputeBuffer buf, ref PathMaterial[] last, PathMaterial[] data)
    {
        if (data == null) return buf;
        if (buf == null || buf.count != data.Length || !ReferenceEquals(last, data))
        {
            buf?.Release();
            buf = new ComputeBuffer(data.Length, StrideMaterial);
            last = data;
            buf.SetData(data);
        }
        return buf;
    }

    private void SyncLights(PathLight[] data)
    {
        if (data == null) return;
        bool rebuilt = LightsBuf == null || LightsBuf.count != data.Length
            || !ReferenceEquals(_lastLights, data);
        if (rebuilt)
        {
            LightsBuf?.Release();
            LightsBuf = new ComputeBuffer(data.Length, StrideLight);
            _lastLights = data;
            LightsBuf.SetData(data);
        }
    }

    // ── 重打包同步 ─────────────────────────────────────

    private void SyncInstances(TinyBVHNative.InstanceRecord[] data)
    {
        if (data == null) return;
        bool rebuilt = InstanceTableBuf == null || InstanceTableBuf.count != data.Length
            || !ReferenceEquals(_lastInstance, data);
        if (rebuilt)
        {
            InstanceTableBuf?.Release();
            InstanceTableBuf = new ComputeBuffer(data.Length, StrideInstance);
            _lastInstance = data;
        }
        if (!rebuilt) return;

        var repacked = new InstanceGpuData[data.Length];
        for (int i = 0; i < data.Length; i++)
        {
            var src = data[i];
            repacked[i].materialID = src.materialID;
            repacked[i].blasID = src.blasID;
            repacked[i].pad0 = 0;
            repacked[i].pad1 = 0;
            // transform[16] 行主序 -> 4 个 Vector4
            repacked[i].tRow0 = RowToVec4(src.transform, 0);
            repacked[i].tRow1 = RowToVec4(src.transform, 1);
            repacked[i].tRow2 = RowToVec4(src.transform, 2);
            repacked[i].tRow3 = RowToVec4(src.transform, 3);
            repacked[i].iRow0 = RowToVec4(src.invTransform, 0);
            repacked[i].iRow1 = RowToVec4(src.invTransform, 1);
            repacked[i].iRow2 = RowToVec4(src.invTransform, 2);
            repacked[i].iRow3 = RowToVec4(src.invTransform, 3);
        }
        InstanceTableBuf.SetData(repacked);
    }

    private void SyncTriangles(PathTriangle[] data)
    {
        if (data == null) return;
        bool rebuilt = GlobalTrianglesBuf == null || GlobalTrianglesBuf.count != data.Length
            || !ReferenceEquals(_lastTris, data);
        if (rebuilt)
        {
            GlobalTrianglesBuf?.Release();
            GlobalTrianglesBuf = new ComputeBuffer(data.Length, StrideTri);
            _lastTris = data;
        }
        if (!rebuilt) return;

        var repacked = new TriGpuData[data.Length];
        for (int i = 0; i < data.Length; i++)
        {
            repacked[i].v0 = data[i].v0;
            repacked[i].v1 = data[i].v1;
            repacked[i].v2 = data[i].v2;
            repacked[i].pad = 0;
        }
        GlobalTrianglesBuf.SetData(repacked);
    }

    private void SyncVertices(PathVertex[] data)
    {
        if (data == null) return;
        bool rebuilt = GlobalVerticesBuf == null || GlobalVerticesBuf.count != data.Length
            || !ReferenceEquals(_lastVerts, data);
        if (rebuilt)
        {
            GlobalVerticesBuf?.Release();
            GlobalVerticesBuf = new ComputeBuffer(data.Length, StrideVertex);
            _lastVerts = data;
        }
        if (!rebuilt) return;

        var repacked = new VertexGpuData[data.Length];
        for (int i = 0; i < data.Length; i++)
        {
            var v = data[i];
            repacked[i].pos = v.pos;
            repacked[i].pad0 = 0f;
            repacked[i].normal = v.normal;
            repacked[i].pad1 = 0f;
            repacked[i].tangent = v.tangent;
            repacked[i].uv = v.uv;
            repacked[i].pad2 = Vector2.zero;
            repacked[i].color = v.color;
        }
        GlobalVerticesBuf.SetData(repacked);
    }

    /// <summary>从行主序 float[16] 中取出第 row 行作为 Vector4。</summary>
    private static Vector4 RowToVec4(float[] m16, int row)
    {
        int b = row * 4;
        return new Vector4(m16[b + 0], m16[b + 1], m16[b + 2], m16[b + 3]);
    }

    // ── 生命周期 ────────────────────────────────────────

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        ReleaseBuffers();
    }

    private void ReleaseBuffers()
    {
        TlasNodes?.Release();           TlasNodes = null;
        TlasPrimIdx?.Release();         TlasPrimIdx = null;
        GlobalBlasNodes?.Release();     GlobalBlasNodes = null;
        GlobalBlasPrimIdx?.Release();   GlobalBlasPrimIdx = null;
        BlasDescriptors?.Release();     BlasDescriptors = null;
        InstanceTableBuf?.Release();    InstanceTableBuf = null;
        GlobalTrianglesBuf?.Release(); GlobalTrianglesBuf = null;
        GlobalVerticesBuf?.Release();   GlobalVerticesBuf = null;
        MaterialsBuf?.Release();       MaterialsBuf = null;
        LightsBuf?.Release();           LightsBuf = null;

        _lastTlasNodes = null;
        _lastTlasPrimIdx = null;
        _lastGlobalBlasNodes = null;
        _lastGlobalBlasPrimIdx = null;
        _lastBlasDesc = null;
        _lastInstance = null;
        _lastTris = null;
        _lastVerts = null;
        _lastMats = null;
        _lastLights = null;

        // 清空纹理数组引用（由 TextureArrayManager 管理生命周期）
        _baseColorArray = null;
        _metallicSmoothArray = null;
        _normalArray = null;
        _emissiveArray = null;
    }

    ~RenderBufferManager() { Dispose(); }
}
