#pragma once

#include "vpt_types.h"
#include <vulkan/vulkan.h>

namespace VPTDispatch {

    // ── PathTrace dispatch ─────────────────────────────────
    // Multi-bounce PBR path tracing with NEE, GGX BSDF, Russian roulette.
    // Uses VK_KHR_ray_query for hardware-accelerated ray traversal.
    // Writes to 5 Unity RenderTextures (output + G-buffers + 2 albedo) via imageStore.
    //
    // 只有一条路径：Plan B 两阶段（PrepareDispatch + RenderCallback）。
    // 早期的直连路径 DispatchPathTrace（自建 cb + vkQueueSubmit + vkQueueWaitIdle）
    // 已整体删除，导出符号 VPT_DispatchPathTrace 一并移除（§7.6 / §10⑦）。

    /// Initialize PathTrace pipeline (18 bindings)
    void InitPathTracePipeline(VkDevice device, VkPhysicalDevice gpu);

    /// Cleanup PathTrace pipeline resources
    void DestroyPathTracePipeline(VkDevice device);

    /// Whether PathTrace pipeline is initialized
    bool IsPathTraceReady();

    // ── Plan B: Two-phase dispatch (eliminates vkQueueWaitIdle) ─────────
    // Phase 1 (PrepareDispatch): Called from C# before cmd.IssuePluginEvent.
    //   Accesses Unity textures (ObserveOnly), fills UBO, updates descriptor sets.
    //   Does NOT create/submit command buffers — just prepares GPU state.
    // Phase 2 (RenderCallback): Called by Unity during command buffer execution
    //   via IssuePluginEvent. Uses CommandRecordingState to get Unity's VkCommandBuffer,
    //   records barriers + dispatch into it. No vkQueueSubmit / vkQueueWaitIdle.

    /// Event ID for Unity rendering event (passed to IssuePluginEvent)
    static constexpr int EVENT_PATHTRACE = 1;

    /// Phase 1: Prepare dispatch parameters (call from C# before IssuePluginEvent)
    void PrepareDispatch(
        void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
        void* diffuseAlbedoPtr, void* specularAlbedoPtr,
        void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
        int32_t width, int32_t height,
        const VPT_CameraData& cameraData,
        uint32_t lightCount, uint32_t samplesPerPixel);

    /// Phase 2: Unity rendering event callback (called during cmd buffer execution)
    /// Records path trace dispatch into Unity's VkCommandBuffer via CommandRecordingState
    void RenderCallback(int eventID);

    /// Whether a dispatch is pending (prepared but not yet executed by callback)
    bool IsDispatchPending();

} // namespace VPTDispatch
