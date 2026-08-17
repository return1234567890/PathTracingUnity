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

    [Tooltip("每像素采样数（每帧每像素发射的独立光线路径数，1=单路径靠时域累积降噪）")]
    [Range(1, 32)] public int samplesPerPixel = 1;

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

    [Header("NRD 降噪参数")]
    [Tooltip("遮挡检测基础阈值（默认0.02，NRD 标准）")]
    public float disocclusionThreshold = 0.02f;

    [Tooltip("法线权重参数（0=自适应随粗糙度/帧数，>0=固定值覆盖）")]
    public float normalWeightParam = 0f;

    [Tooltip("平面距离深度阈值")]
    public float depthThreshold = 0.02f;

    [Tooltip("Poisson 采样点数（8 或 16）")]
    [Range(8, 16)] public int poissonTapCount = 8;

    [Tooltip("模糊半径（像素单位，每 pass 乘以步长）")]
    public float blurRadius = 1.0f;

    [Tooltip("亮度权重系数（方差驱动时）")]
    public float phiLuminance = 1.0f;

    [Tooltip("亮度相对差异上限（防止过度滤波）")]
    public float maxLuminanceRelativeDiff = 0.1f;

    [Tooltip("启用 anti-firefly 前置 pass（3x3 RCRS 极值替换）")]
    public bool enableAntiFirefly = false;

    [Header("ReSTIR GI")]
    [Tooltip("启用 ReSTIR GI 模式（关闭则走当前时域+空域管线）")]
    public bool useReSTIRGI = false;

    [Tooltip("空域重采样邻居数（4 或 8）")]
    [Range(4, 8)] public int spatialNeighborCount = 4;

    [Tooltip("ReSTIR 输出后再做几 pass 軻量空域滤波（0=直接输出）")]
    [Range(0, 3)] public int restirSpatialPostFilter = 1;

    [Tooltip("粗糙度低于此值的表面视为镜面，走回退多弹射而非 ReSTIR 重采样")]
    [Range(0.01f, 0.5f)] public float specularThreshold = 0.3f;

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
    private int _kAntiFirefly;

    // ── ReSTIR GI kernel 索引 ──
    private int _kRestirPrimary;
    private int _kRestirInit;
    private int _kRestirTemp;
    private int _kRestirSpat;
    private int _kRestirShade;

    private RenderBufferManager _bufferMgr;

    // ── Render Targets（按相机分辨率维护） ──
    private RenderTexture _outputRT;     // PathTrace radiance 输出
    private RenderTexture _gbuffer0;     // worldPos.xyz + depth (ARGBFloat)
    private RenderTexture _gbuffer1;     // normal.xyz + roughness (ARGBHalf)
    private RenderTexture _prevGbuffer0;  // 上一帧 G-buffer pos+depth（遮挡检测）
    private RenderTexture _prevGbuffer1;  // 上一帧 G-buffer normal+roughness
    private RenderTexture _accumRT;      // 本帧累积结果（ping-pong A）
    private RenderTexture _historyRT;    // 上帧累积结果（ping-pong B）
    private RenderTexture _filterInRT;   // 空域滤波临时 RT A
    private RenderTexture _filterOutRT;  // 空域滤波临时 RT B

    // ── ReSTIR GI 专用 RT ──
    private RenderTexture _directLightRT;      // RGBA16F，直接光照
    private RenderTexture _reservoirRT0;       // RGBA16F，方向+距离
    private RenderTexture _reservoirRT1;       // RGBA16F，法线+亮度
    private RenderTexture _reservoirRT2;       // RGBA32F，W+M+pdf
    private RenderTexture _reservoirRT3;       // RGBA16F，辐射度+matID
    private RenderTexture _reservoirRT4;       // RGBA32F，BSDF
    private RenderTexture _prevReservoirRT0;   // RGBA16F，历史蓄水池
    private RenderTexture _prevReservoirRT1;
    private RenderTexture _prevReservoirRT2;
    private RenderTexture _prevReservoirRT3;
    private RenderTexture _prevReservoirRT4;
    private RenderTexture _temporalResRT0;     // 时域输出（ping-pong）
    private RenderTexture _temporalResRT1;
    private RenderTexture _temporalResRT2;
    private RenderTexture _temporalResRT3;
    private RenderTexture _temporalResRT4;
    private RenderTexture _spatialResRT0;      // 空域输出（ping-pong）
    private RenderTexture _spatialResRT1;
    private RenderTexture _spatialResRT2;
    private RenderTexture _spatialResRT3;
    private RenderTexture _spatialResRT4;

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
            _kAntiFirefly = cs.FindKernel("AntiFirefly");
            _kRestirPrimary = cs.FindKernel("ReSTIR_PrimaryRay");
            _kRestirInit   = cs.FindKernel("ReSTIR_InitReservoir");
            _kRestirTemp   = cs.FindKernel("ReSTIR_TemporalResample");
            _kRestirSpat   = cs.FindKernel("ReSTIR_SpatialResample");
            _kRestirShade  = cs.FindKernel("ReSTIR_FinalShading");
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
        cmd.SetComputeIntParam(cs, "SamplesPerPixel", Mathf.Max(1, _feature.samplesPerPixel));
        cmd.SetComputeVectorParam(cs, "AmbientColor", _feature.ambientColor);

        // ── TemporalCB ──
        cmd.SetComputeMatrixParam(cs, "PrevViewProj", prevViewProj);
        cmd.SetComputeIntParam(cs, "AccumFrame", _accumFrame);
        cmd.SetComputeIntParam(cs, "EnableTemporal", _feature.temporalEnabled ? 1 : 0);
        cmd.SetComputeIntParam(cs, "SpatialPasses", _feature.spatialEnabled ? _feature.spatialPasses : 0);

        // ── NRD 移植参数（全局 uniform） ──
        cmd.SetComputeFloatParam(cs, "DisocclusionThreshold", _feature.disocclusionThreshold);
        cmd.SetComputeFloatParam(cs, "NormalWeightParam", _feature.normalWeightParam);
        cmd.SetComputeFloatParam(cs, "DepthThreshold", _feature.depthThreshold);
        cmd.SetComputeIntParam(cs, "PoissonTapCount", _feature.poissonTapCount);
        cmd.SetComputeFloatParam(cs, "BlurRadius", _feature.blurRadius);
        cmd.SetComputeFloatParam(cs, "PhiLuminance", _feature.phiLuminance);
        cmd.SetComputeFloatParam(cs, "MaxLuminanceRelativeDiff", _feature.maxLuminanceRelativeDiff);
        cmd.SetComputeIntParam(cs, "EnableAntiFirefly", _feature.enableAntiFirefly ? 1 : 0);

        // ── ReSTIR GI cbuffer 参数 ──
        if (_feature.useReSTIRGI)
        {
            cmd.SetComputeIntParam(cs, "SpatialNeighborCount", _feature.spatialNeighborCount);
            cmd.SetComputeFloatParam(cs, "SpecularThreshold", _feature.specularThreshold);
        }

        // ── 3. 维护所有 RT ──
        EnsureRTs(w, h);

        int gx = (w + _feature.groupXY - 1) / _feature.groupXY;
        int gy = (h + _feature.groupXY - 1) / _feature.groupXY;

        RTHandle colorTarget = Renderer.cameraColorTargetHandle;

        // ══════════════════════════════════════════════════════════════
        //  ReSTIR GI 5-kernel 管线分支
        // ══════════════════════════════════════════════════════════════
        if (_feature.useReSTIRGI)
        {
            // ═══ K1: ReSTIR_PrimaryRay ═══
            // 写 G-buffer + 直接光照
            cmd.SetComputeTextureParam(cs, _kRestirPrimary, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kRestirPrimary, "GBufferNormalRough", _gbuffer1);
            cmd.SetComputeTextureParam(cs, _kRestirPrimary, "ReSTIR_DirectLighting", _directLightRT);
            _bufferMgr.Bind(cmd, cs, _kRestirPrimary);
            cmd.DispatchCompute(cs, _kRestirPrimary, gx, gy, 1);

            // ═══ K2: ReSTIR_InitReservoir ═══
            // 读 G-buffer，写初始蓄水池 → _reservoirRT0-4
            cmd.SetComputeTextureParam(cs, _kRestirInit, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kRestirInit, "GBufferNormalRough", _gbuffer1);
            BindReservoirUAVs(cmd, cs, _kRestirInit, _reservoirRT0, _reservoirRT1, _reservoirRT2, _reservoirRT3);
            cmd.SetComputeTextureParam(cs, _kRestirInit, "ReservoirRT4", _reservoirRT4);
            _bufferMgr.Bind(cmd, cs, _kRestirInit);
            cmd.DispatchCompute(cs, _kRestirInit, gx, gy, 1);

            // ═══ K3: ReSTIR_TemporalResample ═══
            // 读 G-buffer(当前+历史) + 当前蓄水池(K2) + 历史蓄水池
            // 写时域蓄水池 → _temporalResRT0-4
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "GBufferNormalRough", _gbuffer1);
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "PrevGBufferPosDepth", _prevGbuffer0);
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "PrevGBufferNormalRough", _prevGbuffer1);
            BindCurReservoirSRVs(cmd, cs, _kRestirTemp, _reservoirRT0, _reservoirRT1, _reservoirRT2, _reservoirRT3);
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "CurReservoirRT4", _reservoirRT4);
            BindPrevReservoirSRVs(cmd, cs, _kRestirTemp, _prevReservoirRT0, _prevReservoirRT1, _prevReservoirRT2, _prevReservoirRT3);
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "PrevReservoirRT4", _prevReservoirRT4);
            BindReservoirUAVs(cmd, cs, _kRestirTemp, _temporalResRT0, _temporalResRT1, _temporalResRT2, _temporalResRT3);
            cmd.SetComputeTextureParam(cs, _kRestirTemp, "ReservoirRT4", _temporalResRT4);
            cmd.DispatchCompute(cs, _kRestirTemp, gx, gy, 1);

            // ═══ K4: ReSTIR_SpatialResample ═══
            // 读 G-buffer + 时域蓄水池(K3)
            // 写空域蓄水池 → _spatialResRT0-4
            cmd.SetComputeTextureParam(cs, _kRestirSpat, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kRestirSpat, "GBufferNormalRough", _gbuffer1);
            BindCurReservoirSRVs(cmd, cs, _kRestirSpat, _temporalResRT0, _temporalResRT1, _temporalResRT2, _temporalResRT3);
            cmd.SetComputeTextureParam(cs, _kRestirSpat, "CurReservoirRT4", _temporalResRT4);
            BindReservoirUAVs(cmd, cs, _kRestirSpat, _spatialResRT0, _spatialResRT1, _spatialResRT2, _spatialResRT3);
            cmd.SetComputeTextureParam(cs, _kRestirSpat, "ReservoirRT4", _spatialResRT4);
            cmd.DispatchCompute(cs, _kRestirSpat, gx, gy, 1);

            // ═══ K5: ReSTIR_FinalShading ═══
            // 读 G-buffer + 直接光照 + 空域蓄水池(K4)
            // 写 Output
            cmd.SetComputeTextureParam(cs, _kRestirShade, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kRestirShade, "GBufferNormalRough", _gbuffer1);
            cmd.SetComputeTextureParam(cs, _kRestirShade, "ReSTIR_DirectLighting", _directLightRT);
            BindCurReservoirSRVs(cmd, cs, _kRestirShade, _spatialResRT0, _spatialResRT1, _spatialResRT2, _spatialResRT3);
            cmd.SetComputeTextureParam(cs, _kRestirShade, "CurReservoirRT4", _spatialResRT4);
            cmd.SetComputeTextureParam(cs, _kRestirShade, "Output", _outputRT);
            _bufferMgr.Bind(cmd, cs, _kRestirShade);
            cmd.DispatchCompute(cs, _kRestirShade, gx, gy, 1);

            // ═══ 可选: 軻量空域滤波后处理 ═══
            if (_feature.restirSpatialPostFilter > 0)
            {
                cmd.SetComputeTextureParam(cs, _kSpatial, "GBufferPosDepth", _gbuffer0);
                cmd.SetComputeTextureParam(cs, _kSpatial, "GBufferNormalRough", _gbuffer1);
                RenderTexture src = _outputRT;
                RenderTexture dst = _filterInRT;
                for (int p = 0; p < _feature.restirSpatialPostFilter; p++)
                {
                    cmd.SetComputeTextureParam(cs, _kSpatial, "FilterInput", src);
                    cmd.SetComputeTextureParam(cs, _kSpatial, "FilterOutput", dst);
                    cmd.SetComputeIntParam(cs, "PassIndex", p);
                    cmd.DispatchCompute(cs, _kSpatial, gx, gy, 1);
                    src = dst;
                    dst = (dst == _filterInRT) ? _filterOutRT : _filterInRT;
                }
                cmd.Blit(src, colorTarget);
            }
            else
            {
                cmd.Blit(_outputRT, colorTarget);
            }

            context.ExecuteCommandBuffer(cmd);
            CommandBufferPool.Release(cmd);

            // ═══ ping-pong: prevReservoir ← 时域蓄水池（K3 输出）═══
            // ReSTIR GI 论文规定：跨帧反馈的是 Temporal Reservoir（样本严格来自同一像素、
            // 同 domain，保证 RIS 无偏）。Spatial Reservoir 已混入邻居像素样本（经
            // Jacobian shift 跨 domain），若反馈会使空间 shift 偏差跨帧复合 + M 指数饱和。
            // 因此 prevReservoir 与 _temporalResRT ping-pong，_spatialResRT 退化为瞬态。
            Swap(ref _prevReservoirRT0, ref _temporalResRT0);
            Swap(ref _prevReservoirRT1, ref _temporalResRT1);
            Swap(ref _prevReservoirRT2, ref _temporalResRT2);
            Swap(ref _prevReservoirRT3, ref _temporalResRT3);
            Swap(ref _prevReservoirRT4, ref _temporalResRT4);

            // ═══ ping-pong: prevGBuffer ← 本帧 G-buffer ═══
            Swap(ref _prevGbuffer0, ref _gbuffer0);
            Swap(ref _prevGbuffer1, ref _gbuffer1);

            _prevView = curView;
            _prevProj = curProj;
            _hasPrev = true;
            return;
        }

        // ══════════════════════════════════════════════════════════════
        //  现有管线（非 ReSTIR GI 模式）
        // ══════════════════════════════════════════════════════════════

        // ═══ 阶段 1：PathTrace ═══
        cmd.SetComputeTextureParam(cs, _kPathTrace, "Output", _outputRT);
        cmd.SetComputeTextureParam(cs, _kPathTrace, "GBufferPosDepth", _gbuffer0);
        cmd.SetComputeTextureParam(cs, _kPathTrace, "GBufferNormalRough", _gbuffer1);
        _bufferMgr.Bind(cmd, cs, _kPathTrace);
        cmd.DispatchCompute(cs, _kPathTrace, gx, gy, 1);

        // ═══ 阶段 4：Blit → 相机颜色目标 ═══
        // colorTarget 已在上方声明

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
        // 读：History(_historyRT) + CurSample(_outputRT) + GBuffer(_gbuffer0/1) + PrevGBuffer(_prevGbuffer0/1)
        // 写：Accum(_accumRT)，.a 通道存储近似方差
        cmd.SetComputeTextureParam(cs, _kTemporal, "History", _historyRT);
        cmd.SetComputeTextureParam(cs, _kTemporal, "CurSample", _outputRT);
        cmd.SetComputeTextureParam(cs, _kTemporal, "GBufferPosDepth", _gbuffer0);
        cmd.SetComputeTextureParam(cs, _kTemporal, "GBufferNormalRough", _gbuffer1);
        cmd.SetComputeTextureParam(cs, _kTemporal, "PrevGBufferPosDepth", _prevGbuffer0);
        cmd.SetComputeTextureParam(cs, _kTemporal, "PrevGBufferNormalRough", _prevGbuffer1);
        cmd.SetComputeTextureParam(cs, _kTemporal, "Accum", _accumRT);
        cmd.DispatchCompute(cs, _kTemporal, gx, gy, 1);

        // ═══ 阶段 2.5：AntiFirefly（可选前置 pass） ═══
        // 3x3 RCRS 极值替换，消除镜面反射单像素刺眼亮点
        // 输入 _accumRT → 输出 _filterInRT，供后续 SpatialFilter 使用
        RenderTexture spatialInput = _accumRT;
        if (_feature.enableAntiFirefly)
        {
            cmd.SetComputeTextureParam(cs, _kAntiFirefly, "AntiFlyInput", _accumRT);
            cmd.SetComputeTextureParam(cs, _kAntiFirefly, "AntiFlyOutput", _filterInRT);
            cmd.SetComputeTextureParam(cs, _kAntiFirefly, "GBufferPosDepth", _gbuffer0);
            cmd.DispatchCompute(cs, _kAntiFirefly, gx, gy, 1);
            spatialInput = _filterInRT;
        }

        // ═══ 阶段 3：SpatialFilter（Poisson + à-trous 多 pass） ═══
        RenderTexture finalRT;
        if (_feature.spatialEnabled && _feature.spatialPasses > 0)
        {
            cmd.SetComputeTextureParam(cs, _kSpatial, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kSpatial, "GBufferNormalRough", _gbuffer1);

            // AntiFirefly 开启时从 _filterInRT 开始，否则从 _accumRT 开始
            RenderTexture src = spatialInput;
            // AntiFirefly 已写入 _filterInRT，故空域首个 pass 写 _filterOutRT
            RenderTexture dst = (_feature.enableAntiFirefly) ? _filterOutRT : _filterInRT;
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
            finalRT = spatialInput;
        }

        // ═══ 阶段 5：Blit → 相机颜色目标 ═══
        cmd.Blit(finalRT, colorTarget);

        context.ExecuteCommandBuffer(cmd);
        CommandBufferPool.Release(cmd);

        // ── ping-pong：history ← 本帧 accum ──
        Swap(ref _historyRT, ref _accumRT);

        // ── ping-pong：prevGBuffer ← 本帧 G-buffer ──
        Swap(ref _prevGbuffer0, ref _gbuffer0);
        Swap(ref _prevGbuffer1, ref _gbuffer1);

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

    private bool _prevUseReSTIR = false;  // 跟踪 useReSTIRGI 状态变化

    private void EnsureRTs(int w, int h)
    {
        bool restirChanged = _prevUseReSTIR != _feature.useReSTIRGI;
        if (_cachedW == w && _cachedH == h && _outputRT != null && !restirChanged)
            return;
        _prevUseReSTIR = _feature.useReSTIRGI;

        ReleaseRTs();

        _outputRT   = NewRT(w, h, _feature.outputFormat);
        _gbuffer0   = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        _gbuffer1   = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        _prevGbuffer0 = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        _prevGbuffer1 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        _accumRT    = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        // History 纹理使用 Bilinear 滤波，使 TemporalAccumulate 的重投影采样
        // 能获得亚像素精度（SampleLevel 双线性）
        _historyRT  = NewRT(w, h, RenderTextureFormat.ARGBFloat, FilterMode.Bilinear);
        _filterInRT  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        _filterOutRT = NewRT(w, h, RenderTextureFormat.ARGBHalf);

        // ── ReSTIR GI 专用 RT ──
        if (_feature.useReSTIRGI)
        {
            _directLightRT  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            // 蓄水池 4 张（当前帧 + 历史）
            _reservoirRT0   = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _reservoirRT1   = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _reservoirRT2   = NewRT(w, h, RenderTextureFormat.ARGBFloat);  // RGBA32F for W+M+pdf
            _reservoirRT3   = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _reservoirRT4   = NewRT(w, h, RenderTextureFormat.ARGBFloat);  // RGBA32F for BSDF
            _prevReservoirRT0 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _prevReservoirRT1 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _prevReservoirRT2 = NewRT(w, h, RenderTextureFormat.ARGBFloat);
            _prevReservoirRT3 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _prevReservoirRT4 = NewRT(w, h, RenderTextureFormat.ARGBFloat);
            // 时域 / 空域输出（格式同蓄水池）
            _temporalResRT0 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _temporalResRT1 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _temporalResRT2 = NewRT(w, h, RenderTextureFormat.ARGBFloat);
            _temporalResRT3 = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _temporalResRT4 = NewRT(w, h, RenderTextureFormat.ARGBFloat);
            _spatialResRT0  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _spatialResRT1  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _spatialResRT2  = NewRT(w, h, RenderTextureFormat.ARGBFloat);
            _spatialResRT3  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
            _spatialResRT4  = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        }

        _cachedW = w;
        _cachedH = h;
    }

    private static RenderTexture NewRT(int w, int h, RenderTextureFormat fmt,
                                       FilterMode filter = FilterMode.Point)
    {
        var rt = new RenderTexture(w, h, 0, fmt)
        {
            enableRandomWrite = true,
            antiAliasing = 1,
            filterMode = filter,
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
        Release(ref _prevGbuffer0);
        Release(ref _prevGbuffer1);
        Release(ref _accumRT);
        Release(ref _historyRT);
        Release(ref _filterInRT);
        Release(ref _filterOutRT);
        // ── ReSTIR GI ──
        Release(ref _directLightRT);
        Release(ref _reservoirRT0); Release(ref _reservoirRT1);
        Release(ref _reservoirRT2); Release(ref _reservoirRT3);
        Release(ref _reservoirRT4);
        Release(ref _prevReservoirRT0); Release(ref _prevReservoirRT1);
        Release(ref _prevReservoirRT2); Release(ref _prevReservoirRT3);
        Release(ref _prevReservoirRT4);
        Release(ref _temporalResRT0); Release(ref _temporalResRT1);
        Release(ref _temporalResRT2); Release(ref _temporalResRT3);
        Release(ref _temporalResRT4);
        Release(ref _spatialResRT0); Release(ref _spatialResRT1);
        Release(ref _spatialResRT2); Release(ref _spatialResRT3);
        Release(ref _spatialResRT4);
        _cachedW = -1;
        _cachedH = -1;
    }

    private static void Release(ref RenderTexture rt)
    {
        if (rt != null) { rt.Release(); rt = null; }
    }

    // ── ReSTIR GI 蓄水池纹理绑定辅助方法 ──

    private static void BindReservoirUAVs(CommandBuffer cmd, ComputeShader cs, int kernel,
        RenderTexture rt0, RenderTexture rt1, RenderTexture rt2, RenderTexture rt3)
    {
        cmd.SetComputeTextureParam(cs, kernel, "ReservoirRT0", rt0);
        cmd.SetComputeTextureParam(cs, kernel, "ReservoirRT1", rt1);
        cmd.SetComputeTextureParam(cs, kernel, "ReservoirRT2", rt2);
        cmd.SetComputeTextureParam(cs, kernel, "ReservoirRT3", rt3);
    }

    private static void BindCurReservoirSRVs(CommandBuffer cmd, ComputeShader cs, int kernel,
        RenderTexture rt0, RenderTexture rt1, RenderTexture rt2, RenderTexture rt3)
    {
        cmd.SetComputeTextureParam(cs, kernel, "CurReservoirRT0", rt0);
        cmd.SetComputeTextureParam(cs, kernel, "CurReservoirRT1", rt1);
        cmd.SetComputeTextureParam(cs, kernel, "CurReservoirRT2", rt2);
        cmd.SetComputeTextureParam(cs, kernel, "CurReservoirRT3", rt3);
    }

    private static void BindPrevReservoirSRVs(CommandBuffer cmd, ComputeShader cs, int kernel,
        RenderTexture rt0, RenderTexture rt1, RenderTexture rt2, RenderTexture rt3)
    {
        cmd.SetComputeTextureParam(cs, kernel, "PrevReservoirRT0", rt0);
        cmd.SetComputeTextureParam(cs, kernel, "PrevReservoirRT1", rt1);
        cmd.SetComputeTextureParam(cs, kernel, "PrevReservoirRT2", rt2);
        cmd.SetComputeTextureParam(cs, kernel, "PrevReservoirRT3", rt3);
    }

    private static void Swap(ref RenderTexture a, ref RenderTexture b)
    {
        RenderTexture tmp = a;
        a = b;
        b = tmp;
    }
}
