// VulkanPathTracePlugin — 导出 API 实现
//
// 阶段1: 初始化/销毁 + 扩展查询
// 阶段2: VPT_UpdateScene → vpt_scene 场景数据上传 + BLAS/TLAS
// 阶段3: VPT_DispatchPathTrace → Slang compute pipeline

#include "vpt_api.h"
#include "vpt_internal.h"
#include "vpt_scene.h"
#include "vpt_dispatch.h"
#include "vpt_dlss.h"

#include <cstdio>

// ── VPT_Initialize ────────────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_Initialize()
{
    if (VPT_IsInitialized())
    {
        fprintf(stderr, "[VPT] Already initialized.\n");
        return 0;
    }

    // 刷新实例（预加载模式下 UnityPluginLoad 时设备可能未就绪）
    VPT_RefreshInstance();

    if (VPT_GetUnityVulkan() == nullptr)
    {
        fprintf(stderr, "[VPT] ERROR: IUnityGraphicsVulkan not available.\n");
        return 1;
    }

    const UnityVulkanInstance& inst = VPT_GetInstance();
    if (inst.device == VK_NULL_HANDLE || inst.physicalDevice == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] ERROR: VkDevice/VkPhysicalDevice is null.\n");
        return 1;
    }

    if (!VPT_IsRayQuerySupported() || !VPT_IsAccelStructSupported())
    {
        fprintf(stderr, "[VPT] WARNING: RT Core extensions not supported.\n");
        return 2;
    }

    // 加载 Vulkan 扩展函数指针（使用 vkGetDeviceProcAddr，检查扩展是否已启用）
    VPTScene::LoadExtensionFunctions(inst.getInstanceProcAddr, inst.instance, inst.device);

    if (!VPTScene::AreExtensionsReady())
    {
        fprintf(stderr, "[VPT] VPT_Initialize: extension functions not ready.\n"
                        "  This means VK_KHR_acceleration_structure and related extensions\n"
                        "  were NOT enabled on Unity's VkDevice.\n"
                        "  Fix: set isPreloaded=1 in the plugin .meta file so the\n"
                        "  InterceptInitialization callback can inject RT extensions.\n");
        fflush(stderr);
        VPT_SetInitialized(false);
        return 2;
    }

    VPT_SetInitialized(true);
    VPTDispatch::InitPipeline(inst.device, inst.physicalDevice);
    VPTDispatch::InitPathTracePipeline(inst.device, inst.physicalDevice);

    // ── Configure rendering event for Plan B (CommandRecordingState) ──
    // Event must be outside render pass (compute dispatch) and allow queue access
    // so that CommandRecordingState returns a valid Unity VkCommandBuffer.
    {
        IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
        UnityVulkanPluginEventConfig eventCfg{};
        eventCfg.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
        // DontCare: plugin records into Unity's current VkCommandBuffer (no direct queue submit)
        // Allow would disable access to Unity's command buffer, causing null return
        eventCfg.graphicsQueueAccess    = kUnityVulkanGraphicsQueueAccess_DontCare;
        eventCfg.flags                  = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission
                                       | kUnityVulkanEventConfigFlag_ModifiesCommandBuffersState;
        vulkan->ConfigureEvent(VPTDispatch::EVENT_PATHTRACE, &eventCfg);
        // DLSS event uses the same config
        vulkan->ConfigureEvent(EVENT_DLSS, &eventCfg);
        fprintf(stderr, "[VPT] ConfigureEvent: PATHTRACE + DLSS events configured (EnsureOutside + QueueAccess_DontCare).\n");
    }

    fprintf(stderr, "[VPT] VPT_Initialize succeeded. Device ready for RayQuery.\n");
    fflush(stderr);
    return 0;
}

// ── VPT_Destroy ───────────────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_Destroy()
{
    if (!VPT_IsInitialized())
        return;

    VPTDispatch::DestroyPathTracePipeline(VPT_GetInstance().device);
    VPTDispatch::DestroyPipeline(VPT_GetInstance().device);
    VPT_DLSS_Destroy_Internal();
    VPTScene::DestroyResources();
    VPT_SetInitialized(false);
    fprintf(stderr, "[VPT] VPT_Destroy: resources released.\n");
}

// ── VPT_UpdateScene ───────────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_UpdateScene(const VPT_SceneUpdate* sceneUpdate)
{
    if (!VPT_IsInitialized() || sceneUpdate == nullptr)
        return;

    VPTScene::UpdateScene(*sceneUpdate);
}

// ── VPT_DispatchPathTrace（阶段3实现）─────────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_DispatchPathTrace(
    void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
    void* diffuseAlbedoPtr, void* specularAlbedoPtr,
    void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
    int32_t width, int32_t height,
    const VPT_CameraData* cameraData,
    int32_t lightCount, int32_t samplesPerPixel)
{
    if (!VPT_IsInitialized() || !cameraData || !outputPtr || !gbuf0Ptr || !gbuf1Ptr
        || !diffuseAlbedoPtr || !specularAlbedoPtr)
        return;
    if (!VPTDispatch::IsPathTraceReady())
        VPTDispatch::InitPathTracePipeline(VPT_GetInstance().device, VPT_GetInstance().physicalDevice);
    if (!VPTDispatch::IsPathTraceReady())
    {
        fprintf(stderr, "[VPT] VPT_DispatchPathTrace: pipeline not ready\n");
        return;
    }
    VPTDispatch::DispatchPathTrace(outputPtr, gbuf0Ptr, gbuf1Ptr,
        diffuseAlbedoPtr, specularAlbedoPtr,
        baseColorPtr, metallicRoughPtr, normalPtr, emissivePtr,
        width, height, *cameraData,
        (uint32_t)lightCount, (uint32_t)samplesPerPixel);
}

// ── VPT_DispatchDebugDepth（阶段2.6验证）──────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_DispatchDebugDepth(
    void* nativeTexturePtr, int32_t width, int32_t height,
    const VPT_CameraData* cameraData)
{
    if (!VPT_IsInitialized() || !cameraData || !nativeTexturePtr)
        return;
    if (!VPTDispatch::IsPipelineReady())
        VPTDispatch::InitPipeline(VPT_GetInstance().device, VPT_GetInstance().physicalDevice);
    if (!VPTDispatch::IsPipelineReady())
    {
        fprintf(stderr, "[VPT] VPT_DispatchDebugDepth: pipeline not ready\n");
        return;
    }
    VPTDispatch::DispatchDebugDepth(nativeTexturePtr, width, height, *cameraData);
}

// ── VPT_QueryRayQuerySupport ──────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_QueryRayQuerySupport()
{
    return VPT_IsRayQuerySupported() ? 1 : 0;
}

// ── Plan B: Two-phase dispatch exports ─────────────────────

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_PrepareDispatch(
    void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
    void* diffuseAlbedoPtr, void* specularAlbedoPtr,
    void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
    int32_t width, int32_t height,
    const VPT_CameraData* cameraData,
    int32_t lightCount, int32_t samplesPerPixel)
{
    if (!VPT_IsInitialized() || !cameraData || !outputPtr || !gbuf0Ptr || !gbuf1Ptr
        || !diffuseAlbedoPtr || !specularAlbedoPtr)
        return;
    if (!VPTDispatch::IsPathTraceReady())
        VPTDispatch::InitPathTracePipeline(VPT_GetInstance().device, VPT_GetInstance().physicalDevice);
    if (!VPTDispatch::IsPathTraceReady())
    {
        fprintf(stderr, "[VPT] VPT_PrepareDispatch: pipeline not ready\n");
        return;
    }
    VPTDispatch::PrepareDispatch(outputPtr, gbuf0Ptr, gbuf1Ptr,
        diffuseAlbedoPtr, specularAlbedoPtr,
        baseColorPtr, metallicRoughPtr, normalPtr, emissivePtr,
        width, height, *cameraData,
        (uint32_t)lightCount, (uint32_t)samplesPerPixel);
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_RenderCallback(int32_t eventID)
{
    // 通过 eventID 区分路径追踪 vs DLSS dispatch
    if (eventID == EVENT_DLSS)
    {
        VPT_DLSS_RenderCallback();
        return;
    }
    VPTDispatch::RenderCallback(eventID);
}

extern "C" UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
VPT_GetRenderCallback()
{
    return (void*)&VPT_RenderCallback;
}

// ══════════════════════════════════════════════════════════════
//  DLSS NGX 导出接口（阶段3 SR + 阶段4 RR）
// ══════════════════════════════════════════════════════════════

extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_DLSS_Init(int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
              int32_t qualityMode, int32_t mode)
{
    return VPT_DLSS_Init_Internal(renderW, renderH, outputW, outputH, qualityMode, mode);
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_DLSS_Destroy()
{
    VPT_DLSS_Destroy_Internal();
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_DLSS_PrepareDispatch(
    void* colorLowPtr, void* motionLowPtr, void* depthLowPtr, void* outputHighPtr,
    int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
    float jitterX, float jitterY, int32_t reset)
{
    VPT_DLSS_PrepareDispatch_Internal(colorLowPtr, motionLowPtr, depthLowPtr, outputHighPtr,
        renderW, renderH, outputW, outputH, jitterX, jitterY, reset);
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_DLSS_PrepareRRDispatch(
    void* colorLowPtr, void* motionLowPtr, void* linearDepthPtr, void* outputHighPtr,
    void* normalRoughPtr,
    void* diffuseAlbedoPtr, void* specularAlbedoPtr,
    int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
    float jitterX, float jitterY, int32_t reset,
    const float* viewMatrix, const float* projMatrix)
{
    VPT_DLSS_PrepareRRDispatch_Internal(colorLowPtr, motionLowPtr, linearDepthPtr, outputHighPtr,
        normalRoughPtr, diffuseAlbedoPtr, specularAlbedoPtr,
        renderW, renderH, outputW, outputH,
        jitterX, jitterY, reset, viewMatrix, projMatrix);
}
