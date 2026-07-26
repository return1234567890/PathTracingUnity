using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

/// <summary>
/// URP 路径追踪渲染特性：在 URP 管线中调度 PathTrace.compute 的单 kernel
/// 多弹射路径追踪，并把结果经时域累积 + 空域滤波后 Blit 到相机颜色目标。
///
/// 四阶段流水线：
///   1. PathTrace kernel        → OutputRT(radiance) + GBuffer0(pos+depth) + GBuffer1(normal+rough)
///   2. TemporalAccumulate kernel→ AccumRT（EMA 累积历史帧）
///   3. SpatialFilter kernel    → FilteredRT（à-trous G-buffer 引导滤波，多 pass）
///   4. Blit → 相机颜色目标；ping-pong history ← accum
///
/// 用法：
///   1. 场景中存在挂载 ModelManager 的 GameObject（负责构建 9 数组）；
///   2. 场景中存在挂载 LightManager 的 GameObject（负责收集光源）；
///   3. 在 URP Renderer 资产的 Renderer Features 列表中添加本特性；
///   4. 将 PathTrace.compute 资产拖入 pathTraceShader 字段；
///   5. 播放后画面应显示多弹射路径追踪结果（直接照明 + 阴影 + 间接光）。
/// </summary>
public class PathTraceRendererFeature : ScriptableRendererFeature
{
    [Header("路径追踪")]
    public ComputeShader pathTraceShader;

    [Tooltip("输出中间 RenderTexture 的格式（需支持 UAV 写入）")]
    public RenderTextureFormat outputFormat = RenderTextureFormat.ARGBHalf;

    [Tooltip("线程组大小（每组的像素数 = groupXY²）")]
    public int groupXY = 8;

    [Tooltip("最大弹射深度（光线反弹次数）")]
    [Range(1, 32)] public int maxDepth = 8;

    [Tooltip("未命中时的环境光颜色")]
    public Color ambientColor = new Color(0.02f, 0.02f, 0.02f, 1f);

    [Header("降噪总开关")]
    [Tooltip("关闭后跳过时域累积与空域滤波，直接输出原始路径追踪结果（1 spp 噪点）")]
    public bool denoisingEnabled = true;

    [Header("时域降噪（方案A）")]
    [Tooltip("启用时域累积：静态相机时多帧 EMA 累积，有效 spp 随帧增长")]
    public bool temporalEnabled = true;

    [Header("空域降噪（方案B）")]
    [Tooltip("启用 G-buffer 引导空域滤波（à-trous 3x3 方形核）")]
    public bool spatialEnabled = true;

    [Tooltip("空域滤波 pass 数（步长 1/2/4/8...，0=禁用）")]
    [Range(0, 6)] public int spatialPasses = 3;

    private PathTraceRenderPass _pass;

    public override void Create()
    {
        _pass = new PathTraceRenderPass(this)
        {
            renderPassEvent = RenderPassEvent.AfterRendering
        };
    }

    public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
    {
        if (pathTraceShader == null || ModelManager.Instance == null)
            return;
        _pass.Renderer = renderer;
        renderer.EnqueuePass(_pass);
    }

    protected override void Dispose(bool disposing)
    {
        _pass?.Dispose();
    }
}

/// <summary>
/// 路径追踪渲染 Pass：每帧 Sync 数据 -> 绑定 -> 四阶段 Dispatch -> Blit。
/// </summary>
public class PathTraceRenderPass : ScriptableRenderPass
{
    private readonly PathTraceRendererFeature _feature;

    private ComputeShader _resolvedShader;
    private int _kPathTrace;
    private int _kTemporal;
    private int _kSpatial;

    private RenderBufferManager _bufferMgr;

    // ── Render Targets（按相机分辨率维护） ──
    private RenderTexture _outputRT;     // PathTrace radiance 输出
    private RenderTexture _gbuffer0;     // worldPos.xyz + depth (ARGBFloat)
    private RenderTexture _gbuffer1;     // normal.xyz + roughness (ARGBHalf)
    private RenderTexture _accumRT;      // 本帧累积结果（ping-pong A）
    private RenderTexture _historyRT;    // 上帧累积结果（ping-pong B）
    private RenderTexture _filterInRT;   // 空域滤波临时 RT A
    private RenderTexture _filterOutRT;  // 空域滤波临时 RT B

    private int _cachedW = -1;
    private int _cachedH = -1;

    // ── 时域累积状态 ──
    private Matrix4x4 _prevView;   // 上一帧 camera.worldToCameraMatrix
    private Matrix4x4 _prevProj;   // 上一帧 camera.projectionMatrix
    private int _accumFrame;       // 静态累积帧数（相机移动时重置为 0）
    private bool _hasPrev;         // 是否已记录上一帧矩阵

    /// <summary>当前 URP renderer 引用，由 feature 在 AddRenderPasses 中注入。</summary>
    public ScriptableRenderer Renderer { get; set; }

    public PathTraceRenderPass(PathTraceRendererFeature feature)
    {
        _feature = feature;
    }

    /// <summary>pathTraceShader 引用变化时重新解析 kernel 索引。</summary>
    private ComputeShader ResolveKernels()
    {
        ComputeShader cs = _feature.pathTraceShader;
        if (cs == null) return null;
        if (_resolvedShader != cs)
        {
            _resolvedShader = cs;
            _kPathTrace = cs.FindKernel("PathTrace");
            _kTemporal   = cs.FindKernel("TemporalAccumulate");
            _kSpatial    = cs.FindKernel("SpatialFilter");
        }
        return cs;
    }

    public override void OnCameraSetup(CommandBuffer cmd, ref RenderingData renderingData)
    {
    }

    public override void Execute(ScriptableRenderContext context, ref RenderingData renderingData)
    {
        ComputeShader cs = ResolveKernels();
        if (cs == null) return;

        ModelManager mm = ModelManager.Instance;
        if (mm == null || mm.TlasNodes == null || mm.TlasNodes.Length == 0)
            return;

        // ── 1. 同步 9 大数组 + 光源数组到 ComputeBuffer ──
        if (_bufferMgr == null)
            _bufferMgr = new RenderBufferManager();
        _bufferMgr.Sync(mm);

        // ── 2. 相机参数 ──
        Camera camera = renderingData.cameraData.camera;
        int w = camera.pixelWidth;
        int h = camera.pixelHeight;
        if (w <= 0 || h <= 0) return;

        Matrix4x4 camToWorld = camera.cameraToWorldMatrix;
        Matrix4x4 invProj = camera.projectionMatrix.inverse;
        Matrix4x4 curView = camera.worldToCameraMatrix;
        Matrix4x4 curProj = camera.projectionMatrix;
        float farPlane = camera.farClipPlane;

        // ── 时域累积：相机移动检测 ──
        bool moved = !_hasPrev || _prevView != curView || _prevProj != curProj;
        if (moved)
            _accumFrame = 0;
        else
            _accumFrame = Mathf.Min(_accumFrame + 1, 1000);

        // 上一帧 viewProj（用于重投影；静态时 == 当前 viewProj → 恒等映射）
        Matrix4x4 prevViewProj = _prevProj * _prevView;

        CommandBuffer cmd = CommandBufferPool.Get("PathTrace");

        // ── PathTraceCB（全局 cbuffer，三个 kernel 共用 Width/Height） ──
        cmd.SetComputeMatrixParam(cs, "CamToWorld", camToWorld);
        cmd.SetComputeMatrixParam(cs, "InvProjection", invProj);
        cmd.SetComputeIntParam(cs, "Width", w);
        cmd.SetComputeIntParam(cs, "Height", h);
        cmd.SetComputeFloatParam(cs, "FarPlane", farPlane);

        int lightCount = (LightManager.Instance != null && LightManager.Instance.Lights != null)
            ? LightManager.Instance.Lights.Length : 0;
        cmd.SetComputeIntParam(cs, "LightCount", lightCount);
        cmd.SetComputeIntParam(cs, "FrameCount", Time.frameCount);
        cmd.SetComputeIntParam(cs, "MaxDepth", Mathf.Max(1, _feature.maxDepth));
        cmd.SetComputeVectorParam(cs, "AmbientColor", _feature.ambientColor);

        // ── TemporalCB ──
        cmd.SetComputeMatrixParam(cs, "PrevViewProj", prevViewProj);
        cmd.SetComputeIntParam(cs, "AccumFrame", _accumFrame);
        cmd.SetComputeIntParam(cs, "EnableTemporal", _feature.temporalEnabled ? 1 : 0);
        cmd.SetComputeIntParam(cs, "SpatialPasses", _feature.spatialEnabled ? _feature.spatialPasses : 0);

        // ── 3. 维护所有 RT ──
        EnsureRTs(w, h);

        int gx = (w + _feature.groupXY - 1) / _feature.groupXY;
        int gy = (h + _feature.groupXY - 1) / _feature.groupXY;

        // ═══ 阶段 1：PathTrace ═══
        cmd.SetComputeTextureParam(cs, _kPathTrace, "Output", _outputRT);
        cmd.SetComputeTextureParam(cs, _kPathTrace, "GBufferPosDepth", _gbuffer0);
        cmd.SetComputeTextureParam(cs, _kPathTrace, "GBufferNormalRough", _gbuffer1);
        _bufferMgr.Bind(cmd, cs, _kPathTrace);
        cmd.DispatchCompute(cs, _kPathTrace, gx, gy, 1);

        // ═══ 阶段 4：Blit → 相机颜色目标 ═══
        RTHandle colorTarget = Renderer.cameraColorTargetHandle;

        // ── 降噪总开关关闭：跳过时域+空域，直接 Blit 原始输出 ──
        if (!_feature.denoisingEnabled)
        {
            cmd.Blit(_outputRT, colorTarget);
            context.ExecuteCommandBuffer(cmd);
            CommandBufferPool.Release(cmd);

            // 仍更新上一帧矩阵，保证重新开启时能正确检测相机移动
            _prevView = curView;
            _prevProj = curProj;
            _hasPrev = true;
            return;
        }

        // ═══ 阶段 2：TemporalAccumulate ═══
        // 读：History(_historyRT) + CurSample(_outputRT) + GBufferPosDepth(_gbuffer0)
        // 写：Accum(_accumRT)
        cmd.SetComputeTextureParam(cs, _kTemporal, "History", _historyRT);
        cmd.SetComputeTextureParam(cs, _kTemporal, "CurSample", _outputRT);
        cmd.SetComputeTextureParam(cs, _kTemporal, "GBufferPosDepth", _gbuffer0);
        cmd.SetComputeTextureParam(cs, _kTemporal, "Accum", _accumRT);
        cmd.DispatchCompute(cs, _kTemporal, gx, gy, 1);

        // ═══ 阶段 3：SpatialFilter（à-trous 多 pass） ═══
        RenderTexture finalRT;
        if (_feature.spatialEnabled && _feature.spatialPasses > 0)
        {
            cmd.SetComputeTextureParam(cs, _kSpatial, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kSpatial, "GBufferNormalRough", _gbuffer1);

            RenderTexture src = _accumRT;
            RenderTexture dst = _filterInRT;
            for (int p = 0; p < _feature.spatialPasses; p++)
            {
                cmd.SetComputeTextureParam(cs, _kSpatial, "FilterInput", src);
                cmd.SetComputeTextureParam(cs, _kSpatial, "FilterOutput", dst);
                cmd.SetComputeIntParam(cs, "PassIndex", p);
                cmd.DispatchCompute(cs, _kSpatial, gx, gy, 1);
                // 下一 pass：读本次输出（= dst），写另一张临时 RT
                src = dst;
                dst = (dst == _filterInRT) ? _filterOutRT : _filterInRT;
            }
            finalRT = src; // 最后一次写入的目标
        }
        else
        {
            finalRT = _accumRT;
        }

        // ═══ 阶段 5：Blit → 相机颜色目标 ═══
        cmd.Blit(finalRT, colorTarget);

        context.ExecuteCommandBuffer(cmd);
        CommandBufferPool.Release(cmd);

        // ── ping-pong：history ← 本帧 accum ──
        RenderTexture swap = _historyRT;
        _historyRT = _accumRT;
        _accumRT = swap;

        // ── 更新上一帧矩阵 ──
        _prevView = curView;
        _prevProj = curProj;
        _hasPrev = true;
    }

    public override void OnCameraCleanup(CommandBuffer cmd)
    {
    }

    public void Dispose()
    {
        ReleaseRTs();
        _bufferMgr?.Dispose();
        _bufferMgr = null;
    }

    // ── 内部工具 ───────────────────────────────────────

    private void EnsureRTs(int w, int h)
    {
        if (_cachedW == w && _cachedH == h && _outputRT != null)
            return;

        ReleaseRTs();

        _outputRT   = NewRT(w, h, _feature.outputFormat);
        _gbuffer0   = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        _gbuffer1   = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        _accumRT    = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        _historyRT  = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        _filterInRT  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        _filterOutRT = NewRT(w, h, RenderTextureFormat.ARGBHalf);

        _cachedW = w;
        _cachedH = h;
    }

    private static RenderTexture NewRT(int w, int h, RenderTextureFormat fmt)
    {
        var rt = new RenderTexture(w, h, 0, fmt)
        {
            enableRandomWrite = true,
            antiAliasing = 1,
            filterMode = FilterMode.Point,
            useMipMap = false,
            autoGenerateMips = false
        };
        rt.Create();
        return rt;
    }

    private void ReleaseRTs()
    {
        Release(ref _outputRT);
        Release(ref _gbuffer0);
        Release(ref _gbuffer1);
        Release(ref _accumRT);
        Release(ref _historyRT);
        Release(ref _filterInRT);
        Release(ref _filterOutRT);
        _cachedW = -1;
        _cachedH = -1;
    }

    private static void Release(ref RenderTexture rt)
    {
        if (rt != null) { rt.Release(); rt = null; }
    }
}
