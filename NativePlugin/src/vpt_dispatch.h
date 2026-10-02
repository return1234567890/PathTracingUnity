#pragma once

#include "vpt_types.h"
#include <vulkan/vulkan.h>

// ── Debug depth dispatch (Phase 2.6 verification) ─────────
// Traces one ray per pixel against TLAS, writes hit distance as grayscale
// directly to Unity's RenderTexture via imageStore — no CPU readback.

namespace VPTDispatch {

    /// Initialize debug pipeline (descriptor set layout, pipeline layout, compute pipeline)
    /// Must be called after VPTScene::UpdateScene (TLAS must be ready)
    void InitPipeline(VkDevice device, VkPhysicalDevice gpu);

    /// Dispatch debug depth shader — writes directly to Unity's RenderTexture
    /// nativeTexturePtr: Unity RenderTexture native pointer (obtained via Texture2D.GetNativeTexturePtr())
    /// width/height: render dimensions
    /// cameraData: camera parameters
    void DispatchDebugDepth(
        void* nativeTexturePtr,
        int32_t width, int32_t height,
        const VPT_CameraData& cameraData);

    /// Cleanup debug pipeline resources
    void DestroyPipeline(VkDevice device);

    /// Whether debug pipeline is initialized
    bool IsPipelineReady();

    // ── PathTrace dispatch (Phase 3) ──────────────────────
    // Multi-bounce PBR path tracing with NEE, GGX BSDF, Russian roulette.
    // Uses VK_KHR_ray_query for hardware-accelerated ray traversal.
    // Writes to 5 Unity RenderTextures (output + G-buffers + 2 albedo) via imageStore.

    /// Initialize PathTrace pipeline (16 bindings: TLAS + 5 images + UBO + 5 SSBOs + 4 tex arrays)
    void InitPathTracePipeline(VkDevice device, VkPhysicalDevice gpu);

    /// Dispatch PathTrace shader — writes to 5 Unity RenderTextures
    /// outputPtr/gbuf0Ptr/gbuf1Ptr: Unity RenderTexture native pointers
    /// diffuseAlbedoPtr/specularAlbedoPtr: DLSS RR albedo G-buffer RT pointers
    /// baseColorPtr/metallicRoughPtr/normalPtr/emissivePtr: Unity Texture2DArray native pointers
    /// width/height: render dimensions
    /// cameraData: camera parameters
    /// lightCount: number of lights in the scene (for NEE)
    /// samplesPerPixel: SPP for the path tracing loop
    void DispatchPathTrace(
        void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
        void* diffuseAlbedoPtr, void* specularAlbedoPtr,
        void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
        int32_t width, int32_t height,
        const VPT_CameraData& cameraData,
        uint32_t lightCount, uint32_t samplesPerPixel);

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
