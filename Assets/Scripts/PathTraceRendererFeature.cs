using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

/// <summary>
/// URP 路径追踪渲染特性：把 Vulkan Ray Query 原生插件的路径追踪结果接入 URP 管线，
/// 并调度 DLSS Ray Reconstruction（RR）完成降噪与超分。
///
/// 管线（全部路径追踪计算都在 native Vulkan 插件侧）：
///   1. native Vulkan RayQuery PT → OutputRT(radiance) + GBuffer0(pos+depth) +
///                                  GBuffer1(normal+rough) + Diffuse/Specular Albedo（RR 材质分解）
///   2. MotionVector kernel       → MotionVectorRT + LinearDepthRT（DLSS RR 辅助图）
///   3. NGX DLSS RR Evaluate（native）→ 全分辨率输出
///   4. Blit → 相机颜色目标
///
/// 用法：
///   1. 场景中存在挂载 ModelManager 的 GameObject（负责收集模型/材质并推送 native）；
///   2. 场景中存在挂载 LightManager 的 GameObject（负责收集光源）；
///   3. 在 URP Renderer 资产的 Renderer Features 列表中添加本特性；
///   4. 将 DlssAux.compute 资产拖入 motionVectorShader 字段（启用 DLSS 时必需）；
///   5. 播放后画面显示 Vulkan 路径追踪 + DLSS RR 重建结果。
///
/// 说明：软件路径追踪链路（HLSL 光线求交 + 自研时域累积/空域滤波 + ReSTIR GI）与
///       tinybvh 依赖已移除；DLSS 只保留 RR（SR 与 debug 深度图分支已删除）。
/// </summary>
public class PathTraceRendererFeature : ScriptableRendererFeature
{
    [Header("路径追踪")]
    [Tooltip("DLSS 辅助数据计算着色器（只含 MotionVector kernel：G-buffer → 运动矢量 + 深度）。\n" +
             "仅在启用 DLSS 时需要；留空则关闭 DLSS 后仍可正常显示 PT 结果。")]
    public ComputeShader motionVectorShader;

    [Tooltip("输出中间 RenderTexture 的格式（native 侧写入，需支持 UAV）")]
    public RenderTextureFormat outputFormat = RenderTextureFormat.ARGBHalf;

    [Tooltip("线程组大小（每组的像素数 = groupXY²），用于 MotionVector kernel 的 dispatch")]
    public int groupXY = 8;

    [Tooltip("最大弹射深度（光线反弹次数），传给 native 侧路径追踪")]
    [Range(1, 32)] public int maxDepth = 8;

    [Tooltip("每像素采样数（每帧每像素发射的独立光线路径数，1=单路径靠 DLSS RR 重建）")]
    [Range(1, 32)] public int samplesPerPixel = 1;

    [Tooltip("环境光颜色（偏蓝天空）。双重用途：1) 光线未命中几何时返回的天空辐亮度；" +
             "2) NEE one-sample MIS 的常数环境辐亮度（无环境贴图时退化）。\n" +
             "RR 抖动修复：此值越大 → 环境分支帧间方差越大 → RR 累积越强；" +
             "但暗区会被该值点亮，需在「抖动消失」与「不过曝暗区」间平衡。")]
    public Color ambientColor = new Color(0.05f, 0.10f, 0.20f, 1f);

    [Tooltip("HDR 环境光贴图（equirectangular）。赋值后原生插件创建 RGBA32F VkImage +" +
             " 构建 alias-method importance 采样表，NEE 环境分支用 importance 采样替代常数 ambient。" +
             " 消除 RR 亮区残余抖动，同时获得真实天空照明/IBL 反射/软阴影半影。" +
             " 空时回退到常数 ambientColor。")]
    public Texture2D hdrEnvironment;

    [Tooltip("HDR 环境光整体强度倍率")]
    [Range(0f, 8f)] public float envIntensity = 1.0f;

    [Tooltip("HDR 环境贴图绕 Y 轴旋转角度（0-360）")]
    [Range(0f, 360f)] public float envRotation = 0f;

    private bool _envMapPushed = false;

    [Header("DLSS Ray Reconstruction")]
    [Tooltip("启用 DLSS RR（降噪+超分一体化）。关闭则直接输出 native 路径追踪的 1spp 原始结果。")]
    public bool dlssEnabled = true;

    [Tooltip("质量模式：0=Quality(1.5x) 1=Balanced(1.7x) 2=Performance(2x) 3=UltraPerformance(3x)")]
    [Range(0, 3)] public int dlssQualityMode = 1;

    /// <summary>按质量模式计算精确渲染尺寸（输出像素 -> 渲染像素）。</summary>
    /// <remarks>RR 严格要求渲染分辨率精确匹配预设倍率（1.5/1.7/2/3x），否则 CreateFeature
    /// 返回 FAIL_InvalidParameter。整数除法避免浮点截断误差（0.58*1920=1113，
    /// 而 1.7x 的精确值是 1920*10/17=1129）。</remarks>
    public static void RenderSizeOf(int mode, int outW, int outH, out int w, out int h)
    {
        switch (mode)
        {
            case 0:  w = outW * 2 / 3;   h = outH * 2 / 3;   break; // Quality 1.5x
            case 1:  w = outW * 10 / 17; h = outH * 10 / 17; break; // Balanced 1.7x
            case 2:  w = outW / 2;       h = outH / 2;       break; // Performance 2x
            default: w = outW / 3;       h = outH / 3;       break; // Ultra Performance 3x
        }
        w = Mathf.Max(1, w);
        h = Mathf.Max(1, h);
    }

    // ── Halton 序列抖动生成器（8 相位循环，用于 DLSS 时域抖动） ──
    // 返回 [-0.5, 0.5] 范围的亚像素偏移（渲染像素空间）
    private static float Halton(int index, int haltonBase)
    {
        float f = 1f, r = 0f;
        for (int i = index; i > 0; i /= haltonBase)
        {
            f /= haltonBase;
            r += f * (i % haltonBase);
        }
        return r - 0.5f;
    }

    /// <summary>获取当前帧的 Halton(2,3) 抖动偏移（8 相位循环）</summary>
    public static Vector2 GetJitter(int frameCount)
    {
        int phase = (frameCount % 8) + 1;
        return new Vector2(Halton(phase, 2), Halton(phase, 3));
    }

    private PathTraceRenderPass _pass;
    private bool _nativeInitialized;

    /// <summary>Native 插件是否已就绪（供 Pass 检查）</summary>
    public bool IsNativeReady => _nativeInitialized;

    public override void Create()
    {
        // 注入点必须用 AfterRenderingPostProcessing(600) 而非 AfterRendering(1000)：
        // URP 14 只把 event==AfterRenderingPostProcessing 的 pass 视为"后处理后的 pass"
        // （hasPassesAfterPostProcessing）。若用 AfterRendering，相机的 Post Processing 开启时
        // 后处理栈会直接 blit 到屏幕并跳过 FinalBlitPass，本 pass 的 Blit 落在离屏
        // _CameraColorAttachment 上，永远不会上屏（Game 视图只见光栅）。挂在 600 上：
        // 后处理(550)写入交换缓冲 → 本 pass blit PT 结果 → FinalBlit(1000+1) 呈现上屏。
        _pass = new PathTraceRenderPass(this)
        {
            renderPassEvent = RenderPassEvent.AfterRenderingPostProcessing
        };
    }

    public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
    {
        if (ModelManager.Instance == null)
            return;

        // 一次性初始化 Native Vulkan 插件
        if (!_nativeInitialized)
        {
            _nativeInitialized = NativeBridge.TryInitialize();
            if (_nativeInitialized && NativeBridge.IsRayQuerySupported())
                Debug.Log("[PathTrace] Native Vulkan plugin ready, RayQuery supported.");
        }

        // 如果插件刚就绪且有待推送数据，重新推送
        if (_nativeInitialized && ModelManager.Instance.NativePushPending)
        {
            Debug.Log("[PathTrace] Retrying deferred PushToNative.");
            ModelManager.Instance.PushToNative();
        }

        // 推送 HDR 环境贴图（一次性，插件就绪且贴图赋值且未推送时）
        if (_nativeInitialized && hdrEnvironment != null && !_envMapPushed)
        {
            // 确保贴图已读取像素（Read/Write enabled）
            if (!hdrEnvironment.isReadable)
            {
                Debug.LogWarning("[PathTrace] hdrEnvironment is not Read/Write enabled in import settings. " +
                                  "HDR env map upload will fail. Enable Read/Write in the texture import settings.");
            }
            else
            {
                bool ok = NativeBridge.SetEnvironmentMap(hdrEnvironment);
                if (ok)
                {
                    Debug.Log("[PathTrace] HDR environment map uploaded to native plugin.");
                    _envMapPushed = true;
                }
                else
                {
                    Debug.LogWarning("[PathTrace] HDR environment map upload failed. Check import format (need uncompressed RGBA float).");
                }
            }
        }
        else if (hdrEnvironment == null)
        {
            _envMapPushed = false;  // 贴图被清空时重置标志，允许重新赋值后重新推送
        }

        _pass.Renderer = renderer;
        renderer.EnqueuePass(_pass);
    }

    protected override void Dispose(bool disposing)
    {
        _pass?.Dispose();
        if (_nativeInitialized)
        {
            NativeBridge.VPT_Destroy();
            _nativeInitialized = false;
            _envMapPushed = false;
        }
    }
}

/// <summary>
/// 路径追踪渲染 Pass：每帧 同步场景 → native dispatch PT → MotionVector → DLSS/Blit。
/// </summary>
public class PathTraceRenderPass : ScriptableRenderPass
{
    private readonly PathTraceRendererFeature _feature;

    // ── DLSS 辅助 kernel ──
    private ComputeShader _resolvedShader;
    private int _kMotionVector;

    // ── Render Targets（按渲染分辨率维护） ──
    private RenderTexture _outputRT;          // native PT radiance 输出
    private RenderTexture _gbuffer0;          // worldPos.xyz + depth (ARGBFloat)
    private RenderTexture _gbuffer1;          // normal.xyz + roughness (ARGBHalf)
    // ── DLSS RR albedo G-buffer（无条件创建：native binding 14/15 descriptor 必须始终有效）──
    private RenderTexture _diffuseAlbedoRT;   // 低分辨率 ARGBHalf，albedo*(1-metallic)
    private RenderTexture _specularAlbedoRT;  // 低分辨率 ARGBHalf，EnvBRDFApprox(F0, roughness², NoV)

    // ── DLSS 专用 RT ──
    private RenderTexture _motionVectorRT;    // 低分辨率 RGFloat（运动矢量）
    private RenderTexture _linearDepthRT;     // 低分辨率 RFloat（视空间线性距离，RR 用）
    private RenderTexture _dlssOutputRT;      // 全分辨率 ARGBHalf（NGX DLSS 输出）

    private int _cachedW = -1;
    private int _cachedH = -1;

    private bool _hasPrev;         // 是否已渲染过上一帧（DLSS reset 标志）
    private bool _warnedNoMotionVector;  // motionVectorShader 缺失只告警一次

    /// <summary>当前 URP renderer 引用，由 feature 在 AddRenderPasses 中注入。</summary>
    public ScriptableRenderer Renderer { get; set; }

    public PathTraceRenderPass(PathTraceRendererFeature feature)
    {
        _feature = feature;
    }

    /// <summary>motionVectorShader 引用变化时重新解析 kernel 索引。</summary>
    private ComputeShader ResolveKernels()
    {
        ComputeShader cs = _feature.motionVectorShader;
        if (cs == null) return null;
        if (_resolvedShader != cs)
        {
            _resolvedShader = cs;
            _kMotionVector = cs.FindKernel("MotionVector");
        }
        return cs;
    }

    public override void OnCameraSetup(CommandBuffer cmd, ref RenderingData renderingData)
    {
    }

    public override void Execute(ScriptableRenderContext context, ref RenderingData renderingData)
    {
        // native Vulkan 插件是唯一路径追踪实现，未就绪则直接跳过
        if (!_feature.IsNativeReady)
            return;

        ModelManager mm = ModelManager.Instance;
        if (mm == null || !mm.HasScene)
            return;

        // ── 1. 相机参数 ──
        Camera camera = renderingData.cameraData.camera;
        // ── DLSS 低分辨率渲染：dispatch / RT / uniform 均用低分辨率，
        //    最终 Blit 到全分辨率 colorTarget 时由 Bilinear 上采样 ──
        // 尺寸精确匹配 DLSS 预设倍率（RR 严格校验；SR 同样兼容）
        int w, h;
        if (_feature.dlssEnabled)
            PathTraceRendererFeature.RenderSizeOf(_feature.dlssQualityMode,
                camera.pixelWidth, camera.pixelHeight, out w, out h);
        else
        {
            w = camera.pixelWidth;
            h = camera.pixelHeight;
        }
        if (w <= 0 || h <= 0) return;

        Matrix4x4 camToWorld = camera.cameraToWorldMatrix;
        Matrix4x4 invProj = camera.projectionMatrix.inverse;
        Matrix4x4 curView = camera.worldToCameraMatrix;
        Matrix4x4 curProj = camera.projectionMatrix;

        // ── DLSS 抖动：Halton(2,3) 8 相位循环，渲染像素空间 [-0.5,0.5] ──
        // NGX 要求传出的 jitter 与渲染 jitter 数值一致且单位为输出像素
        Vector2 jitter = _feature.dlssEnabled
            ? PathTraceRendererFeature.GetJitter(Time.frameCount)
            : Vector2.zero;

        // ── 当前帧 viewProj（用于重投影；静态时 == 上一帧 viewProj → 恒等映射）──
        Matrix4x4 prevViewProj = _prevProj * _prevView;

        // ── 维护所有 RT ──
        EnsureRTs(w, h, camera.pixelWidth, camera.pixelHeight);

        // ── DLSS RR 初始化/重建 ──
        // 当 DLSS 开关、质量模式、分辨率变化时触发重建
        if (_feature.dlssEnabled)
        {
            bool needReinit = !_dlssInitialized
                || _dlssRenderW != w || _dlssRenderH != h
                || _dlssOutW != camera.pixelWidth || _dlssOutH != camera.pixelHeight
                || _dlssQuality != _feature.dlssQualityMode;
            if (needReinit)
            {
                if (_dlssInitialized)
                    NativeBridge.VPT_DLSS_Destroy();

                int result = NativeBridge.VPT_DLSS_Init(
                    w, h, camera.pixelWidth, camera.pixelHeight, _feature.dlssQualityMode);
                if (result == 0)
                {
                    _dlssInitialized = true;
                    _dlssRenderW = w;
                    _dlssRenderH = h;
                    _dlssOutW = camera.pixelWidth;
                    _dlssOutH = camera.pixelHeight;
                    _dlssQuality = _feature.dlssQualityMode;
                    Debug.Log($"[PathTrace] DLSS RR initialized: {w}x{h} -> {camera.pixelWidth}x{camera.pixelHeight} (quality={_feature.dlssQualityMode})");
                }
                else if (result == -2)
                {
                    // RR 不支持（需 RTX 显卡 + 新版驱动）
                    Debug.LogWarning("[PathTrace] DLSS RR not supported on this GPU/driver, falling back to plain Blit.");
                    _dlssInitialized = false;
                }
                else
                {
                    Debug.LogWarning("[PathTrace] DLSS RR init failed, falling back to plain Blit.");
                    _dlssInitialized = false;
                }
            }
        }
        else if (_dlssInitialized)
        {
            // DLSS 被关闭时销毁
            NativeBridge.VPT_DLSS_Destroy();
            _dlssInitialized = false;
        }

        // ── 动态场景：同步实例变换 → native 每帧重建 TLAS（必须在 PrepareDispatch 之前，
        //    使 descriptor 指向本帧槽位）──
        mm.SyncInstancesNative();

        // ── 骨骼动画：关节矩阵 → native 蒙皮 compute + BLAS refit（同样必须
        //    先于 PrepareDispatch，refit/TLAS 重建均在该帧命令流内录制）──
        mm.SyncSkinnedMeshesNative();

        int lightCount = (LightManager.Instance != null && LightManager.Instance.Lights != null)
            ? LightManager.Instance.Lights.Length : 0;

        CommandBuffer cmd = CommandBufferPool.Get("PathTrace");

        RTHandle colorTarget = Renderer.cameraColorTargetHandle;

        // ══════════════════════════════════════════════════════════════
        //  阶段 1：Vulkan RayQuery 路径追踪 → _outputRT + G-buffer + albedo
        // ══════════════════════════════════════════════════════════════
        var camData = new NativeBridge.VPT_CameraData
        {
            viewInv = MatrixToFloat16(camToWorld),
            projInv = MatrixToFloat16(invProj),
            position = new float[]
            {
                camera.transform.position.x,
                camera.transform.position.y,
                camera.transform.position.z
            },
            fov = camera.fieldOfView,
            jitter = new float[] { jitter.x, jitter.y },
            nearPlane = camera.nearClipPlane,
            farPlane = camera.farClipPlane,
            frameCount = Time.frameCount,
            maxDepth = Mathf.Max(1, _feature.maxDepth),
            ambientColor = new float[]
            {
                _feature.ambientColor.r,
                _feature.ambientColor.g,
                _feature.ambientColor.b
            },
            pad = 0f,
            envIntensity = _feature.envIntensity,
            envRotation = _feature.envRotation * Mathf.Deg2Rad
        };

        // Plan B: 两阶段 dispatch（消除 vkQueueWaitIdle）
        // Phase 1: 准备 dispatch 参数（AccessTexture + UBO + DescriptorSet）
        // Phase 2: IssuePluginEvent 将 dispatch 录制进 Unity 的命令缓冲
        TextureArrayManager texArr = mm.TextureArrays;
        IntPtr callbackPtr = NativeBridge.PrepareDispatch(_outputRT, _gbuffer0, _gbuffer1,
            _diffuseAlbedoRT, _specularAlbedoRT,
            texArr != null ? texArr.BaseColorArray : null,
            texArr != null ? texArr.MetallicSmoothArray : null,
            texArr != null ? texArr.NormalArray : null,
            texArr != null ? texArr.EmissiveArray : null,
            w, h, ref camData,
            lightCount, Mathf.Max(1, _feature.samplesPerPixel));
        cmd.IssuePluginEvent(callbackPtr, NativeBridge.EventPathTrace);

        // ═══ 阶段 1.5：MotionVector + 线性深度（DLSS RR 专用） ═══
        // 读当前帧 GBufferPosDepth + PrevViewProj，输出运动矢量 + 视空间线性深度
        ComputeShader cs = _feature.dlssEnabled ? ResolveKernels() : null;
        if (cs == null && _feature.dlssEnabled && !_warnedNoMotionVector)
        {
            _warnedNoMotionVector = true;
            Debug.LogWarning("[PathTrace] DLSS 已启用但 motionVectorShader 未赋值（或缺少 MotionVector kernel），" +
                              "将退化为 Bilinear 上采样。");
        }

        if (cs != null)
        {
            int gx = (w + _feature.groupXY - 1) / _feature.groupXY;
            int gy = (h + _feature.groupXY - 1) / _feature.groupXY;

            cmd.SetComputeIntParam(cs, "Width", w);
            cmd.SetComputeIntParam(cs, "Height", h);
            cmd.SetComputeFloatParam(cs, "FarPlane", camera.farClipPlane);
            cmd.SetComputeMatrixParam(cs, "PrevViewProj", prevViewProj);
            cmd.SetComputeMatrixParam(cs, "CurViewProj", curProj * curView);
            cmd.SetComputeTextureParam(cs, _kMotionVector, "GBufferPosDepth", _gbuffer0);
            cmd.SetComputeTextureParam(cs, _kMotionVector, "MotionVectorOut", _motionVectorRT);
            cmd.SetComputeTextureParam(cs, _kMotionVector, "LinearDepthOut", _linearDepthRT);
            cmd.DispatchCompute(cs, _kMotionVector, gx, gy, 1);
        }

        // ══════════════════════════════════════════════════════════════
        //  阶段 2：NGX DLSS RR 重建，或直接 Blit
        // ══════════════════════════════════════════════════════════════
        if (_feature.dlssEnabled && _dlssInitialized && cs != null)
        {
            // RR: 1spp 噪声 color + linearDepth + normalRough + albedo + 相机矩阵
            float[] viewMat = MatrixToFloat16(curView);
            float[] projMat = MatrixToFloat16(curProj);
            IntPtr dlssPtr = NativeBridge.PrepareDLSSRRDispatch(
                _outputRT, _motionVectorRT, _linearDepthRT, _dlssOutputRT,
                _gbuffer1, _diffuseAlbedoRT, _specularAlbedoRT,
                w, h, camera.pixelWidth, camera.pixelHeight,
                jitter.x, jitter.y, _hasPrev ? 0 : 1,
                viewMat, projMat);
            cmd.IssuePluginEvent(dlssPtr, NativeBridge.EventDlss);
            cmd.Blit(_dlssOutputRT, colorTarget);
        }
        else
        {
            // 未启用 DLSS（或初始化失败/辅助 shader 缺失）：直接输出 PT 结果
            cmd.Blit(_outputRT, colorTarget);
        }

        context.ExecuteCommandBuffer(cmd);
        CommandBufferPool.Release(cmd);

        // ── 更新上一帧矩阵 ──
        _prevView = curView;
        _prevProj = curProj;
        _hasPrev = true;
    }

    /// <summary>将 Unity Matrix4x4 转换为行主序 float[16]</summary>
    private static float[] MatrixToFloat16(Matrix4x4 m)
    {
        float[] f = new float[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                f[r * 4 + c] = m[r, c];
        return f;
    }

    public override void OnCameraCleanup(CommandBuffer cmd)
    {
    }

    public void Dispose()
    {
        // 销毁 DLSS feature
        if (_dlssInitialized)
        {
            NativeBridge.VPT_DLSS_Destroy();
            _dlssInitialized = false;
        }

        ReleaseRTs();
    }

    // ── 内部工具 ───────────────────────────────────────

    private bool _prevDlssEnabled;          // 跟踪 dlssEnabled 状态变化（切换时重建 RT）
    private int _cachedOutW = -1;            // 全分辨率宽度（DLSS 输出）
    private int _cachedOutH = -1;            // 全分辨率高度（DLSS 输出）

    // ── DLSS NGX 状态跟踪 ──
    private bool _dlssInitialized = false;
    private int _dlssRenderW = -1;
    private int _dlssRenderH = -1;
    private int _dlssOutW = -1;
    private int _dlssOutH = -1;
    private int _dlssQuality = -1;

    // ── 上一帧相机矩阵（DLSS 重投影用） ──
    private Matrix4x4 _prevView;
    private Matrix4x4 _prevProj;

    private void EnsureRTs(int w, int h, int outW, int outH)
    {
        bool dlssChanged = _prevDlssEnabled != _feature.dlssEnabled;
        bool outResChanged = _cachedOutW != outW || _cachedOutH != outH;
        if (_cachedW == w && _cachedH == h && _outputRT != null && !dlssChanged && !outResChanged)
            return;
        _prevDlssEnabled = _feature.dlssEnabled;

        ReleaseRTs();

        // DLSS 开启时，最终 Blit 源 RT 使用 Bilinear 以获得平滑上采样；
        // 关闭时使用 Point（与原始行为一致）
        FilterMode rtFilter = _feature.dlssEnabled ? FilterMode.Bilinear : FilterMode.Point;

        _outputRT   = NewRT(w, h, _feature.outputFormat, rtFilter);
        _gbuffer0   = NewRT(w, h, RenderTextureFormat.ARGBFloat);
        _gbuffer1   = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        // DLSS RR albedo G-buffer（无条件创建：native pipeline binding 14/15 的
        // descriptor 必须始终有效，即使 DLSS 关闭也不能为 null）
        _diffuseAlbedoRT  = NewRT(w, h, RenderTextureFormat.ARGBHalf);
        _specularAlbedoRT = NewRT(w, h, RenderTextureFormat.ARGBHalf);

        // ── DLSS 专用 RT ──
        if (_feature.dlssEnabled)
        {
            _motionVectorRT = NewRT(w, h, RenderTextureFormat.RGFloat);   // 低分辨率运动矢量
            _linearDepthRT  = NewRT(w, h, RenderTextureFormat.RFloat);     // 低分辨率视空间线性距离（RR 用）
            _dlssOutputRT   = NewRT(outW, outH, RenderTextureFormat.ARGBHalf, FilterMode.Bilinear);  // 全分辨率 NGX 输出
        }

        _cachedW = w;
        _cachedH = h;
        _cachedOutW = outW;
        _cachedOutH = outH;
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
        Release(ref _diffuseAlbedoRT);
        Release(ref _specularAlbedoRT);
        // ── DLSS ──
        Release(ref _motionVectorRT);
        Release(ref _linearDepthRT);
        Release(ref _dlssOutputRT);
        _cachedW = -1;
        _cachedH = -1;
        _cachedOutW = -1;
        _cachedOutH = -1;
        _prevDlssEnabled = false;
    }

    private static void Release(ref RenderTexture rt)
    {
        if (rt != null) { rt.Release(); rt = null; }
    }
}
