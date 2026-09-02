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
///   3. PathTraceRendererFeature 每帧调用 VPT_DispatchPathTrace()
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
        RayQueryNotSupported = 2,
    }

    /// <summary>初始化 Native 路径追踪插件</summary>
    /// <returns>0=成功, 1=Vulkan设备获取失败, 2=RT Core扩展不支持</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_Initialize();

    /// <summary>销毁插件资源</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_Destroy();

    /// <summary>查询设备是否支持 VK_KHR_ray_query</summary>
    /// <returns>1=支持, 0=不支持</returns>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern int VPT_QueryRayQuerySupport();

    /// <summary>更新场景数据</summary>
    /// <param name="sceneUpdate">指向 VPT_SceneUpdate 结构的指针</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_UpdateScene(IntPtr sceneUpdate);

    /// <summary>执行路径追踪 dispatch（阶段4）</summary>
    /// <param name="outputPtr">输出 RT 原生指针</param>
    /// <param name="gbuf0Ptr">G-Buffer0 (pos+depth) RT 原生指针</param>
    /// <param name="gbuf1Ptr">G-Buffer1 (normal+rough) RT 原生指针</param>
    /// <param name="baseColorPtr">BaseColor Texture2DArray 原生指针</param>
    /// <param name="metallicRoughPtr">MetallicSmooth Texture2DArray 原生指针</param>
    /// <param name="normalPtr">Normal Texture2DArray 原生指针</param>
    /// <param name="emissivePtr">Emissive Texture2DArray 原生指针</param>
    /// <param name="width">渲染宽度</param>
    /// <param name="height">渲染高度</param>
    /// <param name="cameraData">指向 VPT_CameraData 结构的指针</param>
    /// <param name="lightCount">场景光源数量</param>
    /// <param name="samplesPerPixel">每帧采样数</param>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_DispatchPathTrace(
        IntPtr outputPtr, IntPtr gbuf0Ptr, IntPtr gbuf1Ptr,
        IntPtr baseColorPtr, IntPtr metallicRoughPtr, IntPtr normalPtr, IntPtr emissivePtr,
        int width, int height,
        IntPtr cameraData,
        int lightCount, int samplesPerPixel);

    /// <summary>执行 debug 深度图 dispatch（直接写入 Unity RenderTexture 的 VkImage）</summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_DispatchDebugDepth(
        IntPtr nativeTexturePtr, int width, int height, IntPtr cameraData);

    // ── Plan B: Two-phase dispatch (eliminates vkQueueWaitIdle) ──────────

    /// <summary>渲染事件 ID：路径追踪 dispatch</summary>
    public const int EventPathTrace = 1;

    /// <summary>
    /// Phase 1: 准备路径追踪 dispatch（在 IssuePluginEvent 之前调用）。
    /// 访问 Unity 纹理、填充 UBO、更新描述符集，不提交命令缓冲。
    /// 参数与 VPT_DispatchPathTrace 完全一致。
    /// </summary>
    [DllImport(PluginPath, CallingConvention = CallingConvention.StdCall)]
    public static extern void VPT_PrepareDispatch(
        IntPtr outputPtr, IntPtr gbuf0Ptr, IntPtr gbuf1Ptr,
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

    /// <summary>
    /// 执行路径追踪 dispatch，直接写入 3 张 Unity RenderTexture 的 VkImage。
    /// 同时绑定 4 张 Texture2DArray 供 shader 采样。
    /// 无 CPU 回读 — compute shader 通过 imageStore 直接写入 GPU 显存。
    /// </summary>
    /// <param name="outputRT">输出 RT（ARGBHalf）</param>
    /// <param name="gbuf0RT">G-Buffer0 RT（ARGBFloat, pos+depth）</param>
    /// <param name="gbuf1RT">G-Buffer1 RT（ARGBHalf, normal+rough）</param>
    /// <param name="baseColorArr">BaseColor Texture2DArray</param>
    /// <param name="metallicRoughArr">MetallicSmooth Texture2DArray</param>
    /// <param name="normalArr">Normal Texture2DArray</param>
    /// <param name="emissiveArr">Emissive Texture2DArray</param>
    /// <param name="width">渲染宽度</param>
    /// <param name="height">渲染高度</param>
    /// <param name="cameraData">已填充好的相机数据</param>
    /// <param name="lightCount">场景光源数量</param>
    /// <param name="samplesPerPixel">每帧采样数（SPP）</param>
    public static void DispatchPathTrace(
        UnityEngine.RenderTexture outputRT, UnityEngine.RenderTexture gbuf0RT, UnityEngine.RenderTexture gbuf1RT,
        UnityEngine.Texture2DArray baseColorArr, UnityEngine.Texture2DArray metallicRoughArr,
        UnityEngine.Texture2DArray normalArr, UnityEngine.Texture2DArray emissiveArr,
        int width, int height, ref VPT_CameraData cameraData,
        int lightCount, int samplesPerPixel)
    {
        IntPtr outputPtr = outputRT.GetNativeTexturePtr();
        IntPtr gbuf0Ptr  = gbuf0RT.GetNativeTexturePtr();
        IntPtr gbuf1Ptr  = gbuf1RT.GetNativeTexturePtr();

        IntPtr baseColorPtr    = baseColorArr    != null ? baseColorArr.GetNativeTexturePtr()    : IntPtr.Zero;
        IntPtr metallicRoughPtr = metallicRoughArr != null ? metallicRoughArr.GetNativeTexturePtr() : IntPtr.Zero;
        IntPtr normalPtr       = normalArr       != null ? normalArr.GetNativeTexturePtr()       : IntPtr.Zero;
        IntPtr emissivePtr     = emissiveArr     != null ? emissiveArr.GetNativeTexturePtr()     : IntPtr.Zero;

        int camSize = Marshal.SizeOf<VPT_CameraData>();
        IntPtr camPtr = Marshal.AllocHGlobal(camSize);
        try
        {
            Marshal.StructureToPtr(cameraData, camPtr, false);
            VPT_DispatchPathTrace(outputPtr, gbuf0Ptr, gbuf1Ptr,
                baseColorPtr, metallicRoughPtr, normalPtr, emissivePtr,
                width, height, camPtr, lightCount, samplesPerPixel);
        }
        finally
        {
            Marshal.FreeHGlobal(camPtr);
        }
    }

    /// <summary>
    /// Plan B: 准备路径追踪 dispatch 并返回渲染回调指针。
    /// 调用方随后使用 cmd.IssuePluginEvent(callbackPtr, EventPathTrace)
    /// 将原生 dispatch 录制进 Unity 的命令缓冲，消除 vkQueueWaitIdle。
    /// </summary>
    /// <returns>渲染回调函数指针，传给 CommandBuffer.IssuePluginEvent</returns>
    public static IntPtr PrepareDispatch(
        UnityEngine.RenderTexture outputRT, UnityEngine.RenderTexture gbuf0RT, UnityEngine.RenderTexture gbuf1RT,
        UnityEngine.Texture2DArray baseColorArr, UnityEngine.Texture2DArray metallicRoughArr,
        UnityEngine.Texture2DArray normalArr, UnityEngine.Texture2DArray emissiveArr,
        int width, int height, ref VPT_CameraData cameraData,
        int lightCount, int samplesPerPixel)
    {
        IntPtr outputPtr = outputRT.GetNativeTexturePtr();
        IntPtr gbuf0Ptr  = gbuf0RT.GetNativeTexturePtr();
        IntPtr gbuf1Ptr  = gbuf1RT.GetNativeTexturePtr();

        IntPtr baseColorPtr    = baseColorArr    != null ? baseColorArr.GetNativeTexturePtr()    : IntPtr.Zero;
        IntPtr metallicRoughPtr = metallicRoughArr != null ? metallicRoughArr.GetNativeTexturePtr() : IntPtr.Zero;
        IntPtr normalPtr       = normalArr       != null ? normalArr.GetNativeTexturePtr()       : IntPtr.Zero;
        IntPtr emissivePtr     = emissiveArr     != null ? emissiveArr.GetNativeTexturePtr()     : IntPtr.Zero;

        int camSize = Marshal.SizeOf<VPT_CameraData>();
        IntPtr camPtr = Marshal.AllocHGlobal(camSize);
        try
        {
            Marshal.StructureToPtr(cameraData, camPtr, false);
            VPT_PrepareDispatch(outputPtr, gbuf0Ptr, gbuf1Ptr,
                baseColorPtr, metallicRoughPtr, normalPtr, emissivePtr,
                width, height, camPtr, lightCount, samplesPerPixel);
        }
        finally
        {
            Marshal.FreeHGlobal(camPtr);
        }

        return VPT_GetRenderCallback();
    }

    /// <summary>
    /// 执行 debug 深度图 dispatch，直接写入 Unity RenderTexture 的 VkImage。
    /// 无 CPU 回读 — compute shader 通过 imageStore 直接写入 GPU 显存。
    /// </summary>
    /// <param name="nativeTexturePtr">RenderTexture.GetNativeTexturePtr() 返回的指针</param>
    /// <param name="width">渲染宽度</param>
    /// <param name="height">渲染高度</param>
    /// <param name="cameraData">已填充好的相机数据</param>
    public static void DispatchDebugDepth(IntPtr nativeTexturePtr, int width, int height, ref VPT_CameraData cameraData)
    {
        int camSize = System.Runtime.InteropServices.Marshal.SizeOf<VPT_CameraData>();
        IntPtr camPtr = System.Runtime.InteropServices.Marshal.AllocHGlobal(camSize);
        try
        {
            System.Runtime.InteropServices.Marshal.StructureToPtr(cameraData, camPtr, false);
            VPT_DispatchDebugDepth(nativeTexturePtr, width, height, camPtr);
        }
        finally
        {
            System.Runtime.InteropServices.Marshal.FreeHGlobal(camPtr);
        }
    }

    /// <summary>安全初始化：自动检查 DLL 加载和 RT Core 支持</summary>
    /// <returns>true 表示初始化成功且支持 RayQuery</returns>
    public static bool TryInitialize()
    {
        try
        {
            int result = VPT_Initialize();
            switch ((InitResult)result)
            {
                case InitResult.Success:
                    Debug.Log("[NativeBridge] VPT_Initialize succeeded. RayQuery ready.");
                    return true;
                case InitResult.VulkanDeviceNotFound:
                    Debug.LogError("[NativeBridge] Vulkan device not found. " +
                                   "Switch Graphics API to Vulkan in Project Settings.");
                    return false;
                case InitResult.RayQueryNotSupported:
                    Debug.LogWarning("[NativeBridge] RT Core extensions not supported. " +
                                      "Hardware ray tracing unavailable on this GPU.");
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

    /// <summary>检查 GPU 是否支持硬件光线追踪</summary>
    public static bool IsRayQuerySupported()
    {
        try
        {
            return VPT_QueryRayQuerySupport() == 1;
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
    }

    /// <summary>场景更新打包结构（P/Invoke 传输用）</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct VPT_SceneUpdate
    {
        public IntPtr meshes;          // 指向 VPT_MeshData 数组
        public int   meshCount;
        public IntPtr materials;       // 指向 PathMaterial 数组（96B）
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
}
