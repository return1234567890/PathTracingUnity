using System;
using System.Runtime.InteropServices;
using UnityEngine;

/// <summary>
/// Native Vulkan 插件 P/Invoke 桥接层。
/// 封装 VulkanPathTracePlugin.dll 的导出接口，
/// 负责初始化/销毁插件、场景数据推送、路径追踪 dispatch 调用。
///
/// 使用流程:
///   1. Unity 启动后调用 VPT_Initialize()，检查 Vulkan RT Core 支持
///   2. ModelManager 场景构建后调用 VPT_UpdateScene() 推送数据
///   3. PathTraceRendererFeature 每帧调用 PrepareDispatch() 并 IssuePluginEvent
///      （旧的一次性 VPT_DispatchPathTrace 直连路径已删除，见 §7.6/§10⑦）
///   4. 应用退出时调用 VPT_Destroy()
/// </summary>
public static class NativeBridge
{
    private const string PluginName = "VulkanPathTracePlugin";

#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
    private const string PluginPath = PluginName;
#else
    private const string PluginPath = PluginName;
#endif

    /// <summary>VPT_Initialize 返回码</summary>
    public enum InitResult
    {
        Success = 0,
        VulkanDeviceNotFound = 1,
        /// <summary>缺 VK_KHR_ray_tracing_pipeline / rayTracingPipeline feature / AS 扩展</summary>
        RayTracingPipelineNotSupported = 2,
        /// <summary>RT pipeline 创建失败（SPIR-V 被拒 / 链接失败 / SBT 创建失败）</summary>
        PipelineCreateFailed = 3,
    }

    /// <summary>初始化 Native 路径追踪插件</summary>
    /// <returns>0=成功, 1=Vulkan设备获取失败, 2=RT能力不支持, 3=RT pipeline创建失败</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_Initialize();

    /// <summary>销毁插件资源</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_Destroy();

    /// <summary>
    /// 销毁 native 管线并清掉初始化缓存：下次 TryInitialize 会重新 VPT_Initialize。
    /// 与直接调 VPT_Destroy 的区别是必须走这里，否则缓存会让重进 Play 时跳过初始化。
    /// </summary>
    public static void Shutdown()
    {
        VPT_Destroy();
        _initSucceeded = false;
    }

    /// <summary>查询设备是否支持 RT pipeline（VK_KHR_ray_tracing_pipeline）</summary>
    /// <returns>1=支持, 0=不支持</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_QueryRayTracingPipelineSupport();

    /// <summary>更新场景数据</summary>
    /// <param name="sceneUpdate">指向 VPT_SceneUpdate 结构的指针</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_UpdateScene(IntPtr sceneUpdate);

    /// <summary>
    /// 动态场景：仅更新实例变换（每帧调用）。
    /// 实例顺序必须与 VPT_UpdateScene 时一致（shader 按实例数组下标索引）。
    /// 实例数可小于初始容量，超出时 native 侧扩容（低频，会触发一次 vkDeviceWaitIdle）。
    /// </summary>
    /// <param name="instances">指向 VPT_InstanceData 数组（56B/元素）</param>
    /// <param name="instanceCount">实例数</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_UpdateInstances(IntPtr instances, int instanceCount);

    /// <summary>
    /// 骨骼动画（阶段2 GPU 蒙皮）：注册蒙皮 mesh 静态数据（一次性；场景重建后须重新注册）。
    /// native 侧自统一顶点缓冲复制 bind-pose base 副本（蒙皮 compute 读 base 写统一缓冲，
    /// 避免在已变形数据上反复蒙皮），上传权重/关节索引，分配关节矩阵环形缓冲。
    /// bindposes 不上传：由 C# 每帧折算进关节矩阵（worldToLocal * bone.localToWorld * bindpose）。
    /// blasID 须以 flags.deformable=1 上传，vertexCount 须与首次上传一致。
    /// </summary>
    /// <param name="blasID">BLAS 索引（与 VPT_UpdateScene 时的 mesh 顺序一致）</param>
    /// <param name="weights">vec4/顶点权重数组（boneWeights 展开，pinned）</param>
    /// <param name="joints">ivec4/顶点骨骼索引数组（pinned）</param>
    /// <param name="vertexCount">顶点数（须与首次上传一致）</param>
    /// <param name="jointCount">关节数（smr.bones.Length）</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_RegisterSkinnedMesh(
        uint blasID, IntPtr weights, IntPtr joints, int vertexCount, int jointCount);

    /// <summary>
    /// 骨骼动画（阶段2 GPU 蒙皮）：每帧上传关节矩阵（jointCount 个 mat4，行主序）。
    /// 写入 host-visible 环形槽位，dispatch 前 native 录制蒙皮 compute + BLAS refit +
    /// TLAS 重建。须先 VPT_RegisterSkinnedMesh，否则 native 侧忽略并告警。
    /// </summary>
    /// <param name="blasID">BLAS 索引</param>
    /// <param name="jointMatrices">jointCount*16 个 float（行主序，pinned）</param>
    /// <param name="jointCount">关节数（须与注册时一致）</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_UpdateSkinning(uint blasID, IntPtr jointMatrices, int jointCount);

    /// <summary>
    /// 骨骼动画（阶段3 refit 质量衰减对策）：设置变形 BLAS 全量 rebuild 间隔。
    /// refit 是原地包络微调，大幅变形多帧后 BVH 遍历效率下降；某 BLAS 累计 interval 次
    /// refit 后 native 侧改走一次 mode=BUILD 全量重建（复用 dstAS/scratch），随后计数清零。
    /// 0=禁用（始终 refit）。
    /// </summary>
    /// <param name="interval">refit 间隔次数（0=禁用）</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_SetDeformableRebuildInterval(uint interval);

    // ── Plan B: Two-phase dispatch (eliminates vkQueueWaitIdle) ──────────

    /// <summary>渲染事件 ID：路径追踪 dispatch</summary>
    public const int EventPathTrace = 1;

    /// <summary>渲染事件 ID：DLSS Evaluate dispatch</summary>
    public const int EventDlss = 2;

    /// <summary>
    /// Phase 1: 准备路径追踪 dispatch（在 IssuePluginEvent 之前调用）。
    /// 访问 Unity 纹理、填充 UBO、更新描述符集，不提交命令缓冲。
    /// </summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_PrepareDispatch(
        IntPtr outputPtr, IntPtr gbuf0Ptr, IntPtr gbuf1Ptr,
        IntPtr diffuseAlbedoPtr, IntPtr specularAlbedoPtr, IntPtr specGuidePtr,
        IntPtr baseColorPtr, IntPtr metallicRoughPtr, IntPtr normalPtr, IntPtr emissivePtr,
        int width, int height,
        IntPtr cameraData,
        int lightCount, int samplesPerPixel);

    /// <summary>
    /// 获取渲染回调函数指针（供 CommandBuffer.IssuePluginEvent 使用）。
    /// 返回的 IntPtr 指向 native 端 VPT_RenderCallback 函数。
    /// </summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern IntPtr VPT_GetRenderCallback();

    // ── 便捷方法 ──────────────────────────────────────────

    // ════════════════════════════════════════════════════════
    //  原生纹理指针缓存（BUG-004）
    //
    //  背景：每帧对 9~16 张贴图各调一次 `GetNativeTexturePtr()`，Unity（Vulkan）会为
    //  每次调用产出新的包装/资源记录，长时间运行后把 Unity 的 resource ID（20 bit，2^20）
    //  耗尽 —— 现象是 Console 被
    //    `Resource ID out of range in GetResource: 1048601 (max is 1048575)`
    //  刷屏，随后画面变黑、Editor 卡死必须强杀。
    //
    //  做法：按「贴图对象」缓存指针，只有对象换了（RT 重建 / 切分辨率 / 场景重建）或
    //  缓存超过 PtrRefreshFrames 帧才重新取。native 侧对 `AccessTexture()` 也做了同样的
    //  按指针节流，两边配合后稳态下每帧对 Unity 的资源查询降到 ~0。
    //
    //  兜底开关：环境变量 `VPT_NO_PTR_CACHE=1` 关掉本缓存（退回每帧取指针）。
    // ════════════════════════════════════════════════════════
    private const int PtrRefreshFrames = 300;

    private sealed class TexPtrSlot
    {
        public UnityEngine.Texture tex;
        public IntPtr ptr;
        public int age;
    }

    private static readonly TexPtrSlot[] s_TexPtrSlots = new TexPtrSlot[18];
    private static readonly bool s_PtrCacheEnabled =
        string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VPT_NO_PTR_CACHE"));

    /// <summary>取（带缓存的）原生纹理指针。slot 0..9 = PT，10..17 = DLSS。</summary>
    private static IntPtr CachedPtr(int slot, UnityEngine.Texture tex)
    {
        if (tex == null) return IntPtr.Zero;
        if (!s_PtrCacheEnabled) return tex.GetNativeTexturePtr();

        TexPtrSlot s = s_TexPtrSlots[slot];
        if (s == null) { s = new TexPtrSlot(); s_TexPtrSlots[slot] = s; }

        bool refresh = !ReferenceEquals(s.tex, tex) || s.ptr == IntPtr.Zero || s.age >= PtrRefreshFrames;
        if (refresh)
        {
            s.tex = tex;
            s.ptr = tex.GetNativeTexturePtr();
            s.age = 0;
        }
        else
        {
            s.age++;
        }
        return s.ptr;
    }

    /// <summary>
    /// Plan B: 准备路径追踪 dispatch 并返回渲染回调指针。
    /// 调用方随后使用 cmd.IssuePluginEvent(callbackPtr, EventPathTrace)
    /// 将原生 dispatch 录制进 Unity 的命令缓冲，消除 vkQueueWaitIdle。
    /// </summary>
    /// <returns>渲染回调函数指针，传给 CommandBuffer.IssuePluginEvent</returns>
    public static IntPtr PrepareDispatch(
        UnityEngine.RenderTexture outputRT, UnityEngine.RenderTexture gbuf0RT, UnityEngine.RenderTexture gbuf1RT,
        UnityEngine.RenderTexture diffuseAlbedoRT, UnityEngine.RenderTexture specularAlbedoRT,
        UnityEngine.RenderTexture specGuideRT,
        UnityEngine.Texture2DArray baseColorArr, UnityEngine.Texture2DArray metallicRoughArr,
        UnityEngine.Texture2DArray normalArr, UnityEngine.Texture2DArray emissiveArr,
        int width, int height, ref VPT_CameraData cameraData,
        int lightCount, int samplesPerPixel)
    {
        // 带缓存（见 CachedPtr）：每帧都调 GetNativeTexturePtr 会耗尽 Unity resource ID
        IntPtr outputPtr         = CachedPtr(0, outputRT);
        IntPtr gbuf0Ptr          = CachedPtr(1, gbuf0RT);
        IntPtr gbuf1Ptr          = CachedPtr(2, gbuf1RT);
        IntPtr diffuseAlbedoPtr  = CachedPtr(3, diffuseAlbedoRT);
        IntPtr specularAlbedoPtr = CachedPtr(4, specularAlbedoRT);
        IntPtr specGuidePtr      = CachedPtr(5, specGuideRT);

        IntPtr baseColorPtr     = CachedPtr(6, baseColorArr);
        IntPtr metallicRoughPtr = CachedPtr(7, metallicRoughArr);
        IntPtr normalPtr        = CachedPtr(8, normalArr);
        IntPtr emissivePtr      = CachedPtr(9, emissiveArr);

        int camSize = Marshal.SizeOf<VPT_CameraData>();
        IntPtr camPtr = Marshal.AllocHGlobal(camSize);
        try
        {
            Marshal.StructureToPtr(cameraData, camPtr, false);
            VPT_PrepareDispatch(outputPtr, gbuf0Ptr, gbuf1Ptr,
                diffuseAlbedoPtr, specularAlbedoPtr, specGuidePtr,
                baseColorPtr, metallicRoughPtr, normalPtr, emissivePtr,
                width, height, camPtr, lightCount, samplesPerPixel);
        }
        finally
        {
            Marshal.FreeHGlobal(camPtr);
        }

        return VPT_GetRenderCallback();
    }

    // ── DLSS NGX 接口（Ray Reconstruction） ──────────────

    /// <summary>初始化 NGX DLSS Ray Reconstruction feature</summary>
    /// <param name="renderW">渲染（低分）宽度</param>
    /// <param name="renderH">渲染（低分）高度</param>
    /// <param name="outputW">输出（全分）宽度</param>
    /// <param name="outputH">输出（全分）高度</param>
    /// <param name="qualityMode">0=Quality 1=Balanced 2=Performance 3=UltraPerf</param>
    /// <returns>0=已接受（创建排队中，用 VPT_DLSS_QueryStatus 查结果）, -1=NGX不可用, -2=RR不支持</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_DLSS_Init(
        int renderW, int renderH, int outputW, int outputH,
        int qualityMode);

    /// <summary>销毁 NGX DLSS feature</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_DLSS_Destroy();

    /// <summary>
    /// 查询 DLSS RR feature 的延迟创建结果。
    /// native 侧的 NGX CreateFeature 会录制进 Unity 的命令缓冲（渲染线程），
    /// 因此 VPT_DLSS_Init 只代表"已排队"。
    /// </summary>
    /// <returns>1=已就绪, 0=排队中/未创建, -1=创建失败</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_DLSS_QueryStatus();

    /// <summary>
    /// 查询 Vulkan 设备状态。GPU 复位（TDR / VK_ERROR_DEVICE_LOST）后会返回 1，
    /// 此时 native 已停止一切 GPU 工作，必须退出 Play 重新初始化才能恢复。
    /// </summary>
    /// <returns>0=正常, 1=设备已丢失</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_GetDeviceStatus();

    /// <summary>native 诊断日志路径（VPT-trace-native.log），用于把两份日志对照着看。</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr VPT_GetTracePath();

    // ── 暂停 / 恢复 ────────────────────────────────────────
    // 说明：运行中重建场景（体积参数热调）已经移除，所以当前没有调用方。
    // 保留这组接口是为了以后真的需要在 Play 中重建 / 大批量销毁 GPU 资源时，
    // 有一个"先停 PT、等 GPU 空闲再动资源"的安全原语：
    //   VPT_PausePT(); while (VPT_IsPaused()==0) yield return null;  // 动 GPU 资源
    //   VPT_ResumePT();
    // 背景：BuildScene 会 DestroyImmediate 掉 PT 正在采样的 Texture2DArray，
    // 与在飞帧重叠会让 GPU fault（TDR，实测 nvlddmkm 153）。

    /// <summary>请求暂停路径追踪。下一次 native 渲染事件里会等 GPU 空闲后进入 paused 状态。</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_PausePT();

    /// <summary>恢复路径追踪</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_ResumePT();

    /// <summary>1 = 已暂停且 GPU 已空闲，此时可以安全销毁/重建 GPU 资源</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_IsPaused();

    /// <summary>取 native 诊断日志路径（失败返回空串）。</summary>
    public static string GetNativeTracePath()
    {
        try
        {
            IntPtr p = VPT_GetTracePath();
            return p == IntPtr.Zero ? "" : (Marshal.PtrToStringAnsi(p) ?? "");
        }
        catch (Exception)
        {
            return "";
        }
    }

    /// <summary>
    /// Phase 1: 准备 DLSS RR Evaluate dispatch。
    /// 额外绑定 normalRough (GBuffer1) + 线性深度 + albedo G-buffer + 视图/投影矩阵。
    /// </summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_DLSS_PrepareRRDispatch(
        IntPtr colorLowPtr, IntPtr motionLowPtr, IntPtr linearDepthPtr, IntPtr outputHighPtr,
        IntPtr normalRoughPtr,
        IntPtr diffuseAlbedoPtr, IntPtr specularAlbedoPtr, IntPtr specGuidePtr,
        int renderW, int renderH, int outputW, int outputH,
        float jitterX, float jitterY, int reset,
        IntPtr viewMatrix, IntPtr projMatrix);

    /// <summary>
    /// Phase 2: DLSS 渲染回调（由 IssuePluginEvent 在命令缓冲执行期间触发）。
    /// 复用与路径追踪相同的 VPT_RenderCallback，通过 eventID 区分。
    /// </summary>

    /// <summary>
    /// 便捷方法：准备 DLSS RR dispatch 并返回回调指针。
    /// 额外传入 normalRough RT（GBuffer1）、线性深度 RT、albedo RT、视图与投影矩阵。
    /// 矩阵为 float[16] 行主序（不含 jitter），通过 GCHandle.Pinned 传入指针。
    /// </summary>
    public static IntPtr PrepareDLSSRRDispatch(
        UnityEngine.RenderTexture colorLowRT, UnityEngine.RenderTexture motionLowRT,
        UnityEngine.RenderTexture linearDepthRT, UnityEngine.RenderTexture outputHighRT,
        UnityEngine.RenderTexture normalRoughRT,
        UnityEngine.RenderTexture diffuseAlbedoRT, UnityEngine.RenderTexture specularAlbedoRT,
        UnityEngine.RenderTexture specGuideRT,
        int renderW, int renderH, int outputW, int outputH,
        float jitterX, float jitterY, int reset,
        float[] viewMatrix, float[] projMatrix)
    {
        // 带缓存（见 CachedPtr）：DLSS 这条每帧也要 8 次 GetNativeTexturePtr，
        // 同样会消耗 Unity resource ID
        IntPtr colorPtr          = CachedPtr(10, colorLowRT);
        IntPtr motionPtr         = CachedPtr(11, motionLowRT);
        IntPtr depthPtr          = CachedPtr(12, linearDepthRT);
        IntPtr outputPtr         = CachedPtr(13, outputHighRT);
        IntPtr normalPtr         = CachedPtr(14, normalRoughRT);
        IntPtr diffuseAlbedoPtr  = CachedPtr(15, diffuseAlbedoRT);
        IntPtr specularAlbedoPtr = CachedPtr(16, specularAlbedoRT);
        IntPtr specGuidePtr      = CachedPtr(17, specGuideRT);

        // 将 float[16] 矩阵 pin 到非托管内存
        GCHandle viewPin = GCHandle.Alloc(viewMatrix, GCHandleType.Pinned);
        GCHandle projPin = GCHandle.Alloc(projMatrix, GCHandleType.Pinned);
        try
        {
            VPT_DLSS_PrepareRRDispatch(colorPtr, motionPtr, depthPtr, outputPtr,
                normalPtr, diffuseAlbedoPtr, specularAlbedoPtr, specGuidePtr,
                renderW, renderH, outputW, outputH,
                jitterX, jitterY, reset,
                viewPin.AddrOfPinnedObject(), projPin.AddrOfPinnedObject());
        }
        finally
        {
            viewPin.Free();
            projPin.Free();
        }

        return VPT_GetRenderCallback();
    }

    /// <summary>
    /// 安全初始化：自动检查 DLL 加载和 RT pipeline 支持。
    /// 成功结果缓存：ModelManager / RendererFeature 每帧都会调本方法，原实现每帧进一次
    /// native 并打一条日志（实测一局 5 万+ 条、Editor.log 200MB+）。失败不缓存，保留重试机会。
    /// </summary>
    /// <returns>true 表示初始化成功且支持 RT pipeline</returns>
    private static bool _initSucceeded;

    public static bool TryInitialize()
    {
        if (_initSucceeded) return true;
        try
        {
            int result = VPT_Initialize();
            switch ((InitResult)result)
            {
                case InitResult.Success:
                    _initSucceeded = true;
                    Debug.Log("[NativeBridge] VPT_Initialize succeeded. RayTracingPipeline ready.");
                    return true;
                case InitResult.VulkanDeviceNotFound:
                    Debug.LogError("[NativeBridge] Vulkan device not found. " +
                                   "Switch Graphics API to Vulkan in Project Settings.");
                    return false;
                case InitResult.RayTracingPipelineNotSupported:
                    Debug.LogWarning("[NativeBridge] VK_KHR_ray_tracing_pipeline not supported. " +
                                     "PT 需要 RTX 20 系以上 + 较新驱动（日志里有缺哪一项）。");
                    return false;
                case InitResult.PipelineCreateFailed:
                    Debug.LogError("[NativeBridge] RT pipeline creation failed. " +
                                   "见 Player.log / Editor.log 里 [VPT] Path: 那一行（含 VkResult）。");
                    return false;
                default:
                    Debug.LogError($"[NativeBridge] VPT_Initialize returned unknown code: {result}");
                    return false;
            }
        }
        catch (DllNotFoundException)
        {
            Debug.LogError("[NativeBridge] VulkanPathTracePlugin.dll not found. " +
                           "Place it in Assets/Plugins/x86_64/.");
            return false;
        }
        catch (EntryPointNotFoundException ex)
        {
            Debug.LogError($"[NativeBridge] Export function not found: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// 设置 HDR 环境光贴图：把 Unity 导入的 HDR Texture2D 像素传给原生插件，
    /// 原生侧创建 RGBA32F VkImage + 构建 alias-method importance 采样表（SSBO）。
    /// 一次性调用（场景加载后），之后每帧 pt.slang 通过 binding 16/17 采样。
    /// </summary>
    /// <param name="pixels">RGBA32F 像素（每像素 4 float，从 Texture2D.GetPixels 获取）</param>
    /// <param name="width">贴图宽度</param>
    /// <param name="height">贴图高度</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_SetEnvironmentMap(IntPtr pixels, int width, int height);

    /// <summary>
    /// 便捷方法：从 Unity Texture2D 读取 HDR 像素并推送到原生插件。
    /// 使用 GetPixels 获取线性 RGBA 浮点数据（Unity 导入 .hdr 时自动 RGBE→float 解码）。
    /// 调用方需确保贴图为 uncompressed HDR 格式（检查 import settings）。
    /// </summary>
    /// <param name="hdr">Unity 导入的 HDR Texture2D（equirectangular）</param>
    /// <returns>true=已交给 native 排队（真正上传在渲染事件里录制）</returns>
    public static bool SetEnvironmentMap(UnityEngine.Texture2D hdr)
    {
        if (hdr == null) return false;

        // 读取线性 HDR 像素（Color = 4 float RGBA，Unity 导入 .hdr 时自动解码）
        UnityEngine.Color[] colors = hdr.GetPixels();
        if (colors == null || colors.Length == 0)
        {
            Debug.LogError("[NativeBridge] SetEnvironmentMap: GetPixels returned empty. " +
                           "Ensure HDR texture import format is uncompressed (check .meta TextureCompression=None).");
            return false;
        }

        // Color 是 4 float struct，直接 pin 数组传指针
        GCHandle pin = GCHandle.Alloc(colors, GCHandleType.Pinned);
        try
        {
            VPT_SetEnvironmentMap(pin.AddrOfPinnedObject(), hdr.width, hdr.height);
            return true;
        }
        finally
        {
            pin.Free();
        }
    }

    /// <summary>检查 GPU 是否支持硬件光线追踪（RT pipeline 后端）</summary>
    public static bool IsRayTracingPipelineSupported()
    {
        try
        {
            return VPT_QueryRayTracingPipelineSupport() == 1;
        }
        catch
        {
            return false;
        }
    }

    // ════════════════════════════════════════════════════════
    //  C# 侧共享数据结构（与 vpt_types.h 内存布局严格对齐）
    // ════════════════════════════════════════════════════════

    /// <summary>
    /// 实例数据（P/Invoke 传输用）。
    /// Transform 为 3x4 行主序矩阵，与 VkAccelerationStructureInstanceKHR 兼容。
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct VPT_InstanceData
    {
        public uint materialID;
        public uint blasID;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 12)]
        public float[] transform; // 3x4 行主序
    }

    /// <summary>网格数据描述（P/Invoke 传输用）</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct VPT_MeshData
    {
        public IntPtr vertices;     // 指向 VertexGpuData 数组（80B/vertex）
        public uint   vertexCount;
        public IntPtr indices;       // 指向 uint32 索引数组
        public uint   indexCount;
        public uint   materialID;
        public uint   flags;         // VPT_MESH_FLAG_*：bit0=deformable（骨骼动画 refit）
    }

    /// <summary>VPT_MeshData.flags 位掩码（与 vpt_types.h 对齐）</summary>
    public const uint VPT_MESH_FLAG_DEFORMABLE = 1u;

    /// <summary>场景更新打包结构（P/Invoke 传输用）</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct VPT_SceneUpdate
    {
        public IntPtr meshes;          // 指向 VPT_MeshData 数组
        public int   meshCount;
        public IntPtr materials;       // 指向 PathMaterial 数组（128B）
        public int   materialCount;
        public IntPtr lights;           // 指向 PathLight 数组（32B）
        public int   lightCount;
        public IntPtr instances;       // 指向 VPT_InstanceData 数组
        public int   instanceCount;
    }

    /// <summary>相机数据（P/Invoke 传输用）</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct VPT_CameraData
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public float[] viewInv;       // 4x4 逆视图矩阵
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public float[] projInv;       // 4x4 逆投影矩阵
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public float[] position;     // 相机世界坐标
        public float fov;            // 视场角
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
        public float[] jitter;       // 像素抖动
        public float nearPlane;
        public float farPlane;
        public int   frameCount;
        public int   maxDepth;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public float[] ambientColor;
        public float pad;
        public float envIntensity;
        public float envRotation;
    }

    // ── 便捷方法: 场景数据推送 ──────────────────────────

    /// <summary>
    /// 将场景数据推送到 Native 插件。
    /// 调用方负责 GCHandle.Alloc/pin 托管数组，传给此方法后由 Native 拷贝。
    /// </summary>
    /// <param name="sceneUpdate">已填充好的场景更新结构</param>
    public static void UpdateScene(ref VPT_SceneUpdate sceneUpdate)
    {
        int size = Marshal.SizeOf<VPT_SceneUpdate>();
        IntPtr ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(sceneUpdate, ptr, false);
            VPT_UpdateScene(ptr);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    // ── GPU 分段计时（vkCmdWriteTimestamp，调试用） ──────────

    /// <summary>GPU 分段索引（与 native VPTGpuTimer::Stage 一一对应）</summary>
    public enum GpuStage
    {
        Skin = 0,         // 开始 → GPU 蒙皮
        BlasRefit = 1,    // 蒙皮 → BLAS refit
        Tlas = 2,         // refit → TLAS 重建
        PathTrace = 3,    // TLAS → PT dispatch 结束
        MotionVector = 4, // PT → DLSS 回调开始
        Dlss = 5,         // DLSS Evaluate
        Total = 6,        // 开始 → 最后一个标记
    }

    /// <summary>
    /// 取回最近一帧完成的 GPU 分段耗时（毫秒）。
    /// native 侧结果要 1~2 帧后才可用，返回 0 表示暂无新数据（不是错误）。
    /// </summary>
    /// <param name="outMs">至少 7 个 float 的非托管缓冲（调用方 pin 住）</param>
    /// <param name="maxCount">缓冲长度</param>
    /// <param name="frameId">结果对应的帧序号（native 内部递增 ID）</param>
    /// <returns>1=有新数据，0=暂无</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_GpuTimer_GetStats(IntPtr outMs, int maxCount, out uint frameId);

    /// <summary>开关 native 侧 GPU 计时（关闭后不再写 timestamp）</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_GpuTimer_SetEnabled(int enabled);
}
