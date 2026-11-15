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

    // ── 暂停 / 恢复（供 C# 在重建场景前使用）──────────────────────────
    // 背景：C# 的 BuildScene() 会 DestroyImmediate 掉 PT shader 正在采样的
    // Texture2DArray，并重新上传贴图。这些 GPU 资源操作发生在主线程、而之前的
    // PT dispatch 还在飞行中 → 释放后仍被采样 → GPU fault → TDR。
    // 所以重建前先 RequestPause()：下一次 PT 事件里等 GPU 空闲后进入 paused，
    // C# 看到 IsPaused() 再动 GPU 资源，完事后 RequestResume()。
    void RequestPause();          // 请求暂停（下一帧事件生效）
    void RequestResume();         // 恢复
    bool IsPaused();              // 已暂停且 GPU 已空闲
    bool IsPauseRequested();

    /// 丢弃 Unity Texture2DArray 的 ImageView 缓存（场景重建后 Unity 会换新数组）
    void InvalidateTextureViews();

} // namespace VPTDispatch
