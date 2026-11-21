// VulkanPathTracePlugin — 导出 API 实现
//
// 阶段1: 初始化/销毁 + 扩展查询
// 阶段2: VPT_UpdateScene → vpt_scene 场景数据上传 + BLAS/TLAS
// 阶段3: 路径追踪只走 Plan B（VPT_PrepareDispatch + VPT_RenderCallback）

#include "vpt_api.h"
#include "vpt_internal.h"
#include "vpt_scene.h"
#include "vpt_dispatch.h"
#include "vpt_env.h"
#include "vpt_dlss.h"
#include "vpt_gpu_timer.h"
#include "vpt_trace.h"

#include <cstdio>

// ── VPT_Initialize ────────────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_Initialize()
{
    VPTTrace::Init();
    VPTTrace::Stage("VPT_Initialize:enter");
    VPTTrace::Beat();
    {
        static int s_SessionNo = 0;
        VPTTrace::Line("VPT_Initialize: enter (session #%d)", ++s_SessionNo);
    }

    if (VPT_IsInitialized())
    {
        fprintf(stderr, "[VPT] Already initialized.\n");
        VPTTrace::Line("VPT_Initialize: already initialized");
        return 0;
    }

    // 刷新实例（预加载模式下 UnityPluginLoad 时设备可能未就绪）
    VPT_RefreshInstance();
    // 每次（重新）初始化都认为是一块新设备：清掉上一次的 device lost 标志
    VPT_ResetDeviceLost();

    if (VPT_GetUnityVulkan() == nullptr)
    {
        fprintf(stderr, "[VPT] ERROR: IUnityGraphicsVulkan not available.\n");
        VPTTrace::Line("VPT_Initialize: FAILED, IUnityGraphicsVulkan null");
        return 1;
    }

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VPTTrace::Line("VPT_Initialize: device=%p physical=%p queueFamily=%u",
                   (void*)inst.device, (void*)inst.physicalDevice, inst.queueFamilyIndex);
    if (inst.device == VK_NULL_HANDLE || inst.physicalDevice == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] ERROR: VkDevice/VkPhysicalDevice is null.\n");
        VPTTrace::Line("VPT_Initialize: FAILED, device/physical null");
        return 1;
    }

    if (!VPT_IsRayTracingPipelineSupported())
    {
        // D5 = A：初始化失败 + 明确报错，PT 关闭（不回退光栅、不回退 ray query）
        fprintf(stderr,
                "[VPT] ERROR: RT pipeline path tracing unsupported on this GPU.\n"
                "  Required: VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline\n"
                "  and the rayTracingPipeline feature. Needs an RTX-class GPU + recent driver.\n");
        VPTTrace::Line("VPT_Initialize: FAILED, VK_KHR_ray_tracing_pipeline unsupported");
        return 2;
    }
    if (!VPT_IsAccelStructSupported())
    {
        fprintf(stderr, "[VPT] ERROR: accelerationStructure feature missing.\n");
        VPTTrace::Line("VPT_Initialize: FAILED, accelerationStructure unsupported");
        return 2;
    }

    // 加载 Vulkan 扩展函数指针（使用 vkGetDeviceProcAddr，检查扩展是否已启用）
    VPTTrace::Stage("VPT_Initialize:load-extensions");
    VPTScene::LoadExtensionFunctions(inst.getInstanceProcAddr, inst.instance, inst.device);

    if (!VPTScene::AreExtensionsReady())
    {
        fprintf(stderr, "[VPT] VPT_Initialize: extension functions not ready.\n"
                        "  This means VK_KHR_acceleration_structure and related extensions\n"
                        "  were NOT enabled on Unity's VkDevice.\n"
                        "  Fix: set isPreloaded=1 in the plugin .meta file so the\n"
                        "  InterceptInitialization callback can inject RT extensions.\n");
        fflush(stderr);
        VPTTrace::Line("VPT_Initialize: FAILED, RT extension functions not loadable");
        VPT_SetInitialized(false);
        return 2;
    }

    VPT_SetInitialized(true);

    // 环境光占位资源（1×1 黑色 + 全零 alias 表）先建好，保证 binding 16/17 永远可绑
    VPTTrace::Stage("VPT_Initialize:env-fallback");
    VPTEnv::InitFallback(inst.device, inst.physicalDevice);
    VPTTrace::Line("VPT_Initialize: env fallback created (ready=%d)", (int)VPTEnv::HasPendingUploads());

    // RT pipeline + SBT（同步创建；失败是确定性失败，不再每帧重试）
    VPTTrace::Stage("VPT_Initialize:pathtrace-pipeline");
    VPTDispatch::InitPathTracePipeline(inst.device, inst.physicalDevice);
    VPTTrace::Line("VPT_Initialize: RT pipeline created (ready=%d)", (int)VPTDispatch::IsPathTraceReady());
    if (!VPTDispatch::IsPathTraceReady())
    {
        fprintf(stderr, "[VPT] ERROR: RT pipeline creation failed: %s\n", VPTDispatch::PathTraceError());
        VPTTrace::Line("VPT_Initialize: FAILED, RT pipeline creation: %s", VPTDispatch::PathTraceError());
        // 释放半成品（layout/pool/UBO/SBT），避免重进 Play 时泄漏
        VPTDispatch::DestroyPathTracePipeline(inst.device);
        VPT_SetInitialized(false);
        return 3;
    }

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
        VPTTrace::Stage("VPT_Initialize:configure-event");
        VPTTrace::Line("VPT_Initialize: ConfigureEvent(PATHTRACE) ...");
        vulkan->ConfigureEvent(VPTDispatch::EVENT_PATHTRACE, &eventCfg);
        VPTTrace::Line("VPT_Initialize: ConfigureEvent(DLSS) ...");
        // DLSS event uses the same config
        vulkan->ConfigureEvent(EVENT_DLSS, &eventCfg);
        fprintf(stderr, "[VPT] ConfigureEvent: PATHTRACE + DLSS events configured (EnsureOutside + QueueAccess_DontCare).\n");
    }

    // GPU 分段计时：查询池建在 Unity 的 VkDevice 上（不支持时静默关闭）
    VPTTrace::Stage("VPT_Initialize:gpu-timer");
    VPTGpuTimer::Init(inst.device, inst.physicalDevice, inst.queueFamilyIndex);

    fprintf(stderr, "[VPT] VPT_Initialize succeeded. Device ready for RayTracingPipeline.\n");
    fflush(stderr);
    VPTTrace::Stage("VPT_Initialize:done");
    VPTTrace::Line("VPT_Initialize: OK");
    VPTTrace::SetActive(true);
    VPTTrace::Beat();
    return 0;
}

// ── VPT_Destroy ───────────────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_Destroy()
{
    VPTTrace::Stage("VPT_Destroy:enter");
    VPTTrace::Line("VPT_Destroy: enter (initialized=%d, deviceLost=%d)",
                   (int)VPT_IsInitialized(), (int)VPT_IsDeviceLost());
    VPTTrace::SetActive(false);
    VPTTrace::Beat();

    if (!VPT_IsInitialized())
    {
        VPTTrace::Line("VPT_Destroy: skipped (not initialized)");
        return;
    }

    VPTTrace::Stage("VPT_Destroy:path-trace-pipeline");
    VPTDispatch::DestroyPathTracePipeline(VPT_GetInstance().device);
    VPTTrace::Stage("VPT_Destroy:dlss");
    VPT_DLSS_Destroy_Internal();
    VPTTrace::Stage("VPT_Destroy:gpu-timer");
    VPTGpuTimer::Shutdown();
    VPTTrace::Stage("VPT_Destroy:scene");
    VPTScene::DestroyResources();
    VPTTrace::Stage("VPT_Destroy:env");
    VPTTrace::Line("VPT_Destroy: env shutdown ...");
    VPTEnv::Shutdown();          // HDR 贴图 + 占位资源 + sampler
    VPT_SetInitialized(false);
    VPT_ResetDeviceLost();
    fprintf(stderr, "[VPT] VPT_Destroy: resources released.\n");
    VPTTrace::Stage("VPT_Destroy:done");
    VPTTrace::Line("VPT_Destroy: done");
}

// ── VPT_UpdateScene（Plan B：只排队，渲染线程里再上传/构建）──
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_UpdateScene(const VPT_SceneUpdate* sceneUpdate)
{
    VPTTrace::Stage("VPT_UpdateScene");
    if (!VPT_IsInitialized() || sceneUpdate == nullptr)
    {
        VPTTrace::Line("VPT_UpdateScene: ignored (initialized=%d ptr=%p)",
                       (int)VPT_IsInitialized(), (const void*)sceneUpdate);
        return;
    }

    VPTTrace::Line("VPT_UpdateScene: meshes=%d mats=%d lights=%d instances=%d (queueing)",
                   sceneUpdate->meshCount, sceneUpdate->materialCount,
                   sceneUpdate->lightCount, sceneUpdate->instanceCount);
    VPTScene::PushScene(*sceneUpdate);
    VPTTrace::Line("VPT_UpdateScene: queue done");
}

// ── VPT_GetDeviceStatus（0=正常, 1=设备已丢失）──
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_GetDeviceStatus()
{
    return VPT_IsDeviceLost() ? 1 : 0;
}

// ── VPT_GetTracePath（native 诊断日志路径，供 C# 记录到 C# 日志里）──
extern "C" UNITY_INTERFACE_EXPORT const char* UNITY_INTERFACE_API
VPT_GetTracePath()
{
    return VPTTrace::Path();
}

// ── 暂停 / 恢复（C# 重建场景前必须调用）──
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_PausePT()
{
    VPTDispatch::RequestPause();
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_ResumePT()
{
    VPTDispatch::RequestResume();
}

extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_IsPaused()
{
    return VPTDispatch::IsPaused() ? 1 : 0;
}

// ── VPT_UpdateInstances ──────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_UpdateInstances(const VPT_InstanceData* instances, int32_t instanceCount)
{
    if (!VPT_IsInitialized() || !instances || instanceCount <= 0)
        return;

    VPTScene::UpdateInstances(instances, instanceCount);
}

// ── VPT_RegisterSkinnedMesh（骨骼动画：阶段2 GPU 蒙皮静态数据注册）──
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_RegisterSkinnedMesh(uint32_t blasID, const float* weights, const int32_t* joints,
                         int32_t vertexCount, int32_t jointCount)
{
    VPTTrace::Stage("VPT_RegisterSkinnedMesh");
    if (!VPT_IsInitialized() || !weights || !joints
        || vertexCount <= 0 || jointCount <= 0)
    {
        VPTTrace::Line("VPT_RegisterSkinnedMesh: ignored (init=%d vc=%d jc=%d)",
                       (int)VPT_IsInitialized(), vertexCount, jointCount);
        return;
    }

    VPTTrace::Line("VPT_RegisterSkinnedMesh: blasID=%u verts=%d joints=%d (queueing)",
                   blasID, vertexCount, jointCount);
    VPTScene::RegisterSkinnedMesh(blasID, weights, joints,
        (uint32_t)vertexCount, (uint32_t)jointCount);
    VPTTrace::Line("VPT_RegisterSkinnedMesh: queue done");
}

// ── VPT_UpdateSkinning（骨骼动画：阶段2 每帧关节矩阵）──
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_UpdateSkinning(uint32_t blasID, const float* jointMatrices, int32_t jointCount)
{
    if (!VPT_IsInitialized() || !jointMatrices || jointCount <= 0)
        return;

    VPTScene::UpdateSkinning(blasID, jointMatrices, (uint32_t)jointCount);
}

// ── VPT_SetDeformableRebuildInterval（骨骼动画：阶段3 refit 质量衰减对策）──
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_SetDeformableRebuildInterval(uint32_t interval)
{
    if (!VPT_IsInitialized())
        return;

    VPTScene::SetDeformableRebuildInterval(interval);
}

// ── VPT_QueryRayTracingPipelineSupport ───────────────────
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_QueryRayTracingPipelineSupport()
{
    return VPT_IsRayTracingPipelineSupported() ? 1 : 0;
}

// ── VPT_SetEnvironmentMap ────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_SetEnvironmentMap(const float* pixels, int32_t width, int32_t height)
{
    VPTTrace::Stage("VPT_SetEnvironmentMap");
    if (!VPT_IsInitialized() || !pixels || width <= 0 || height <= 0)
    {
        VPTTrace::Line("VPT_SetEnvironmentMap: ignored (init=%d %dx%d)",
                       (int)VPT_IsInitialized(), width, height);
        return;
    }
    VPTTrace::Line("VPT_SetEnvironmentMap: %dx%d (CPU prep, upload deferred)", width, height);
    VPTEnv::SetEnvironmentMap(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    VPTTrace::Line("VPT_SetEnvironmentMap: queue done");
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
    VPTTrace::Beat();
    VPTTrace::Stage("VPT_PrepareDispatch");
    if (!VPT_IsInitialized() || !cameraData || !outputPtr || !gbuf0Ptr || !gbuf1Ptr
        || !diffuseAlbedoPtr || !specularAlbedoPtr)
    {
        VPTTrace::Line("VPT_PrepareDispatch: ignored (init=%d ptrs=%p/%p/%p)",
                       (int)VPT_IsInitialized(), outputPtr, gbuf0Ptr, gbuf1Ptr);
        return;
    }
    if (!VPTDispatch::IsPathTraceReady())
    {
        // 失败已判定（确定性失败），不再重试；直接返回，避免每帧重新编译刷屏
        fprintf(stderr, "[VPT] VPT_PrepareDispatch: RT pipeline not ready (%s)\n",
                VPTDispatch::PathTraceError());
        VPTTrace::Line("VPT_PrepareDispatch: RT pipeline NOT ready (%s)", VPTDispatch::PathTraceError());
        return;
    }
    VPTTrace::Line("prepare %dx%d", width, height);
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
        VPTTrace::Stage("VPT_RenderCallback:DLSS");
        VPTTrace::Beat();
        VPT_DLSS_RenderCallback();
        VPTTrace::Stage("idle");
        return;
    }
    VPTTrace::Beat();
    // 每帧只留一行心跳：卡住时最后一行就是"进到哪一帧的哪个事件"
    VPTTrace::Line("frame %u: PT event", VPTTrace::NextFrame());
    VPTDispatch::RenderCallback(eventID);
    VPTTrace::Stage("idle");
}

extern "C" UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API
VPT_GetRenderCallback()
{
    return (void*)&VPT_RenderCallback;
}

// ── GPU 分段计时 ──────────────────────────────────────────
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_GpuTimer_GetStats(float* outMs, int32_t maxCount, uint32_t* outFrameId)
{
    return VPTGpuTimer::GetStats(outMs, maxCount, outFrameId) ? 1 : 0;
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_GpuTimer_SetEnabled(int32_t enabled)
{
    VPTGpuTimer::SetEnabled(enabled != 0);
}

// ══════════════════════════════════════════════════════════════
//  DLSS NGX 导出接口（DLSS Ray Reconstruction）
// ══════════════════════════════════════════════════════════════

extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_DLSS_Init(int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
              int32_t qualityMode)
{
    VPTTrace::Beat();
    VPTTrace::Stage("VPT_DLSS_Init");
    VPTTrace::Line("VPT_DLSS_Init: enter %dx%d -> %dx%d q=%d", renderW, renderH, outputW, outputH, qualityMode);
    const int32_t r = VPT_DLSS_Init_Internal(renderW, renderH, outputW, outputH, qualityMode);
    VPTTrace::Line("VPT_DLSS_Init: ret=%d status=%d", (int)r, (int)VPT_DLSS_GetStatus());
    VPTTrace::Stage("idle");
    return r;
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
VPT_DLSS_Destroy()
{
    VPTTrace::Stage("VPT_DLSS_Destroy");
    VPTTrace::Line("VPT_DLSS_Destroy: enter");
    VPT_DLSS_Destroy_Internal();
    VPTTrace::Line("VPT_DLSS_Destroy: done");
    VPTTrace::Stage("idle");
}

// 延迟创建的查询接口：1=已就绪, 0=排队中, -1=创建失败
extern "C" UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API
VPT_DLSS_QueryStatus()
{
    return VPT_DLSS_GetStatus();
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
    VPTTrace::Beat();
    VPTTrace::Stage("VPT_DLSS_PrepareRRDispatch");
    VPT_DLSS_PrepareRRDispatch_Internal(colorLowPtr, motionLowPtr, linearDepthPtr, outputHighPtr,
        normalRoughPtr, diffuseAlbedoPtr, specularAlbedoPtr,
        renderW, renderH, outputW, outputH,
        jitterX, jitterY, reset, viewMatrix, projMatrix);
    VPTTrace::Stage("idle");
}
