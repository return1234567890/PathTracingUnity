using System.Collections.Generic;
using UnityEngine;

/// <summary>
/// 纹理数组管理器：收集场景材质引用的 Texture2D，按4类分组打包为
/// 统一分辨率的 Texture2DArray，返回 slice 索引供 PathMaterial 引用。
///
/// 四类纹理数组：
///   1. BaseColorArray      — _BaseMap / _MainTex
///   2. MetallicSmoothArray — _MetallicGlossMap（R=metallic, A=smoothness）
///   3. NormalArray         — _BumpMap（CPU侧预解包 DXT5nm/BC5 → 标准 xyz）
///   4. EmissiveArray       — _EmissionMap
///
/// 设计要点：
///   - Dictionary 去重：同 Texture2D 引用返回相同 slice 索引
///   - 占位机制：无纹理的类别创建 1-slice 占位数组（常规白色(1,1,1,1)，
///     法线用(0.5,0.5,1.0,1.0)），满足 HLSL 资源绑定要求
///   - 法线贴图 CPU 预解包：DXT5nm（x=alpha, y=green）→ 标准 [0,1] xyz
/// </summary>
public class TextureArrayManager : System.IDisposable
{
    // ── 纹理类型枚举 ──
    public enum TexType : uint
    {
        BaseColor = 0,
        MetallicSmooth = 1,
        Normal = 2,
        Emissive = 3
    }

    // ── 4 个输出 Texture2DArray ──
    public Texture2DArray BaseColorArray { get; private set; }
    public Texture2DArray MetallicSmoothArray { get; private set; }
    public Texture2DArray NormalArray { get; private set; }
    public Texture2DArray EmissiveArray { get; private set; }

    // ── 去重注册表（Texture2D → slice 索引） ──
    private readonly Dictionary<Texture2D, uint> _baseColorMap = new Dictionary<Texture2D, uint>();
    private readonly Dictionary<Texture2D, uint> _metallicSmoothMap = new Dictionary<Texture2D, uint>();
    private readonly Dictionary<Texture2D, uint> _normalMap = new Dictionary<Texture2D, uint>();
    private readonly Dictionary<Texture2D, uint> _emissiveMap = new Dictionary<Texture2D, uint>();

    // ── 按类型收集的有序纹理列表（构建时遍历） ──
    private readonly List<Texture2D> _baseColorList = new List<Texture2D>();
    private readonly List<Texture2D> _metallicSmoothList = new List<Texture2D>();
    private readonly List<Texture2D> _normalList = new List<Texture2D>();
    private readonly List<Texture2D> _emissiveList = new List<Texture2D>();

    private int _resolution = 1024;
    private bool _disposed;

    // ── 常量 ──
    public const uint TEX_NONE = 0xFFFFFFFFu;

    /// <summary>注册 BaseColor 纹理，返回 slice 索引（去重）。</summary>
    public uint RegisterBaseColor(Texture2D tex)
    {
        if (tex == null) return TEX_NONE;
        if (_baseColorMap.TryGetValue(tex, out uint idx)) return idx;
        idx = (uint)_baseColorList.Count;
        _baseColorMap[tex] = idx;
        _baseColorList.Add(tex);
        return idx;
    }

    /// <summary>注册 MetallicSmoothness 纹理，返回 slice 索引（去重）。</summary>
    public uint RegisterMetallicSmoothness(Texture2D tex)
    {
        if (tex == null) return TEX_NONE;
        if (_metallicSmoothMap.TryGetValue(tex, out uint idx)) return idx;
        idx = (uint)_metallicSmoothList.Count;
        _metallicSmoothMap[tex] = idx;
        _metallicSmoothList.Add(tex);
        return idx;
    }

    /// <summary>注册 Normal 纹理，返回 slice 索引（去重）。</summary>
    public uint RegisterNormal(Texture2D tex)
    {
        if (tex == null) return TEX_NONE;
        if (_normalMap.TryGetValue(tex, out uint idx)) return idx;
        idx = (uint)_normalList.Count;
        _normalMap[tex] = idx;
        _normalList.Add(tex);
        return idx;
    }

    /// <summary>注册 Emissive 纹理，返回 slice 索引（去重）。</summary>
    public uint RegisterEmissive(Texture2D tex)
    {
        if (tex == null) return TEX_NONE;
        if (_emissiveMap.TryGetValue(tex, out uint idx)) return idx;
        idx = (uint)_emissiveList.Count;
        _emissiveMap[tex] = idx;
        _emissiveList.Add(tex);
        return idx;
    }

    /// <summary>
    /// 构建全部 4 个 Texture2DArray（注册完毕后调用）。
    /// 空列表创建 1-slice 占位数组以满足 HLSL 资源绑定要求。
    /// </summary>
    /// <param name="resolution">每个 slice 的统一分辨率（默认 1024）</param>
    public void Build(int resolution = 1024)
    {
        _resolution = resolution;

        BaseColorArray = BuildArray(_baseColorList, TextureFormat.RGBA32, false);
        MetallicSmoothArray = BuildArray(_metallicSmoothList, TextureFormat.RGBA32, false);
        NormalArray = BuildArray(_normalList, TextureFormat.RGBA32, true);  // 法线需预解包
        EmissiveArray = BuildArray(_emissiveList, TextureFormat.RGBA32, false);
    }

    /// <summary>
    /// 构建单个 Texture2DArray。空列表时创建 1-slice 占位数组。
    /// </summary>
    private Texture2DArray BuildArray(List<Texture2D> textures, TextureFormat format, bool isNormal)
    {
        // 空列表：创建占位数组
        if (textures.Count == 0)
        {
            // 法线占位纹理需用 linear=true，避免 SetPixels 做 linear→sRGB 转换扭曲法线值
            var placeholder = new Texture2D(_resolution, _resolution, format, isNormal);
            Color[] pixels;
            if (isNormal)
            {
                // 法线占位：(0.5, 0.5, 1.0, 1.0) → 解包后 = +Z 方向（无扰动）
                // 但由于我们存 [0,1] 范围，shader 侧 *2-1 后 = (0,0,1)
                Color c = new Color(0.5f, 0.5f, 1.0f, 1.0f);
                pixels = new Color[_resolution * _resolution];
                for (int i = 0; i < pixels.Length; i++) pixels[i] = c;
            }
            else
            {
                // 常规占位：白色 (1,1,1,1)
                Color c = Color.white;
                pixels = new Color[_resolution * _resolution];
                for (int i = 0; i < pixels.Length; i++) pixels[i] = c;
            }
            placeholder.SetPixels(pixels);
            placeholder.Apply();

            // 法线贴图是数据纹理，必须用 linear=true 避免 GPU 采样时 sRGB→linear 转换扭曲法线值
            var arr = new Texture2DArray(_resolution, _resolution, 1, format, isNormal);
            Graphics.CopyTexture(placeholder, 0, 0, arr, 0, 0);
            arr.Apply(false, true);

            // 占位源纹理可立即销毁（已拷贝到数组）
            Object.DestroyImmediate(placeholder);
            return arr;
        }

        // 非空：构建 N-slice 数组
        int sliceCount = textures.Count;
        var array = new Texture2DArray(_resolution, _resolution, sliceCount, format, false, isNormal);

        for (int i = 0; i < sliceCount; i++)
        {
            Texture2D src = textures[i];

            if (isNormal)
            {
                // 法线贴图：CPU 侧预解包 DXT5nm/BC5 → 标准 [0,1] xyz
                CopyNormalTexture(src, array, i);
            }
            else
            {
                // 常规纹理：缩放拷贝
                CopyRegularTexture(src, array, i);
            }
        }

        array.Apply(false, true);
        return array;
    }

    /// <summary>
    /// 常规纹理缩放拷贝到 Texture2DArray 的指定 slice。
    /// 统一转换为 RGBA32 格式的临时纹理后用 CopyTexture 写入，
    /// 避免源格式（DXT1/RGB24等）与目标 RGBA32 不匹配的问题。
    /// </summary>
    private void CopyRegularTexture(Texture2D src, Texture2DArray dst, int slice)
    {
        Texture2D readable = GetReadable(src);
        if (readable == null) return;

        // 创建 RGBA32 临时纹理，统一格式与分辨率
        var tempTex = new Texture2D(_resolution, _resolution, TextureFormat.RGBA32, false);

        if (readable.width == _resolution && readable.height == _resolution)
        {
            // 尺寸匹配：直接取像素
            tempTex.SetPixels(readable.GetPixels());
        }
        else
        {
            // 尺寸不匹配：缩放
            var scaled = ScaleTexture(readable, _resolution, _resolution);
            tempTex.SetPixels(scaled.GetPixels());
            if (scaled != readable)
                Object.DestroyImmediate(scaled);
        }
        tempTex.Apply();

        // 临时纹理与数组同为 RGBA32，CopyTexture 格式兼容
        Graphics.CopyTexture(tempTex, 0, 0, dst, slice, 0);
        Object.DestroyImmediate(tempTex);

        // 清理临时可读纹理（若创建了副本）
        if (readable != src)
            Object.DestroyImmediate(readable);
    }

    /// <summary>
    /// 法线贴图预解包：DXT5nm（x=alpha*2-1, y=green*2-1, z=sqrt(1-x²-y²)）
    /// 或 BC5（x=red, y=green）→ 标准 [0,1] xyz 存储。
    /// shader 侧统一 *2-1 重映射。
    /// </summary>
    private void CopyNormalTexture(Texture2D src, Texture2DArray dst, int slice)
    {
        Texture2D readable = GetReadable(src);
        if (readable == null) return;

        // 缩放到目标分辨率
        Texture2D scaled;
        if (readable.width != _resolution || readable.height != _resolution)
        {
            scaled = ScaleTexture(readable, _resolution, _resolution);
            // 仅销毁 GetReadable 创建的副本，不销毁原始 src
            if (readable != src)
                Object.DestroyImmediate(readable);
        }
        else
        {
            scaled = readable;
        }

        // 读取像素并解包
        Color[] pixels = scaled.GetPixels();
        bool isDXT5nm = src.format == TextureFormat.DXT5 || src.format == TextureFormat.DXT5Crunched;
        bool isBC5 = src.format == TextureFormat.BC5;

        for (int i = 0; i < pixels.Length; i++)
        {
            Color c = pixels[i];
            float nx, ny;
            if (isDXT5nm)
            {
                // DXT5nm: 法线 x 存在 alpha 通道，y 存在 green 通道
                nx = c.a * 2.0f - 1.0f;
                ny = c.g * 2.0f - 1.0f;
            }
            else if (isBC5)
            {
                // BC5: x=red, y=green
                nx = c.r * 2.0f - 1.0f;
                ny = c.g * 2.0f - 1.0f;
            }
            else
            {
                // 已是标准 RGB 法线贴图
                nx = c.r * 2.0f - 1.0f;
                ny = c.g * 2.0f - 1.0f;
            }

            // Z 重建：与 Unity URP UnpackNormal 完全一致
            // z = sqrt(1 - saturate(x²+y²))，saturate 防止压缩量化导致的负值
            float nz = Mathf.Sqrt(Mathf.Max(0.0f, 1.0f - nx * nx - ny * ny));

            // 存为 [0,1] 范围（shader 侧 *2-1 重映射）
            pixels[i] = new Color(nx * 0.5f + 0.5f, ny * 0.5f + 0.5f, nz * 0.5f + 0.5f, 1.0f);
            // pixels[i] = new Color(0.735f, 0.735f, 1.0f, 1.0f);
        }

        // 写入临时 Texture2D 再 CopyTexture 到数组
        // linear=true：法线是数据纹理，SetPixels 不能做 linear→sRGB 转换
        var tempTex = new Texture2D(_resolution, _resolution, TextureFormat.RGBA32, false, true);
        tempTex.SetPixels(pixels);
        tempTex.Apply();
        Graphics.CopyTexture(tempTex, 0, 0, dst, slice, 0);

        // 清理临时纹理
        Object.DestroyImmediate(tempTex);
        if (scaled != src)
            Object.DestroyImmediate(scaled);
    }

    /// <summary>
    /// 获取可读的 Texture2D。若源纹理不可读（isReadable=false），
    /// 通过 RenderTexture 中转创建可读副本。
    /// </summary>
    private Texture2D GetReadable(Texture2D src)
    {
        if (src == null) return null;
        if (src.isReadable) return src;

        // 不可读纹理：通过 RenderTexture 中转（使用 Linear 避免 sRGB 转换）
        var rt = RenderTexture.GetTemporary(src.width, src.height, 0,
            RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
        Graphics.Blit(src, rt);

        var readable = new Texture2D(src.width, src.height, TextureFormat.RGBA32, false);
        var oldRT = RenderTexture.active;
        RenderTexture.active = rt;
        readable.ReadPixels(new Rect(0, 0, src.width, src.height), 0, 0);
        readable.Apply();
        RenderTexture.active = oldRT;
        RenderTexture.ReleaseTemporary(rt);

        return readable;
    }

    /// <summary>缩放 Texture2D 到指定分辨率（GetPixels/SetPixels 方式，通用兼容）。</summary>
    private Texture2D ScaleTexture(Texture2D src, int dstW, int dstH)
    {
        var scaled = new Texture2D(dstW, dstH, TextureFormat.RGBA32, false);
        Color[] srcPixels = src.GetPixels();
        Color[] dstPixels = new Color[dstW * dstH];

        for (int y = 0; y < dstH; y++)
        {
            for (int x = 0; x < dstW; x++)
            {
                // 双线性插值采样
                float u = (float)x / dstW * (src.width - 1);
                float v = (float)y / dstH * (src.height - 1);
                int x0 = (int)u, y0 = (int)v;
                int x1 = Mathf.Min(x0 + 1, src.width - 1);
                int y1 = Mathf.Min(y0 + 1, src.height - 1);
                float fx = u - x0, fy = v - y0;

                Color c00 = srcPixels[y0 * src.width + x0];
                Color c10 = srcPixels[y0 * src.width + x1];
                Color c01 = srcPixels[y1 * src.width + x0];
                Color c11 = srcPixels[y1 * src.width + x1];

                Color c0 = Color.Lerp(c00, c10, fx);
                Color c1 = Color.Lerp(c01, c11, fx);
                dstPixels[y * dstW + x] = Color.Lerp(c0, c1, fy);
            }
        }

        scaled.SetPixels(dstPixels);
        scaled.Apply();
        return scaled;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        // Texture2DArray 继承自 Texture，无 Release() 方法，用 DestroyImmediate
        if (BaseColorArray != null) Object.DestroyImmediate(BaseColorArray);
        if (MetallicSmoothArray != null) Object.DestroyImmediate(MetallicSmoothArray);
        if (NormalArray != null) Object.DestroyImmediate(NormalArray);
        if (EmissiveArray != null) Object.DestroyImmediate(EmissiveArray);

        BaseColorArray = null;
        MetallicSmoothArray = null;
        NormalArray = null;
        EmissiveArray = null;

        _baseColorMap.Clear();
        _metallicSmoothMap.Clear();
        _normalMap.Clear();
        _emissiveMap.Clear();
        _baseColorList.Clear();
        _metallicSmoothList.Clear();
        _normalList.Clear();
        _emissiveList.Clear();
    }

    ~TextureArrayManager() { Dispose(); }
}
