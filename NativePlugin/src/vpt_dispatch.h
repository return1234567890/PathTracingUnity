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
    // Writes to 3 Unity RenderTextures (output + 2 G-buffers) via imageStore.

    /// Initialize PathTrace pipeline (14 bindings: TLAS + 3 images + UBO + 5 SSBOs + 4 tex arrays)
    void InitPathTracePipeline(VkDevice device, VkPhysicalDevice gpu);

    /// Dispatch PathTrace shader — writes to 3 Unity RenderTextures
    /// outputPtr/gbuf0Ptr/gbuf1Ptr: Unity RenderTexture native pointers
    /// baseColorPtr/metallicRoughPtr/normalPtr/emissivePtr: Unity Texture2DArray native pointers
    /// width/height: render dimensions
    /// cameraData: camera parameters
    /// lightCount: number of lights in the scene (for NEE)
    /// samplesPerPixel: SPP for the path tracing loop
    void DispatchPathTrace(
        void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
        void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
        int32_t width, int32_t height,
        const VPT_CameraData& cameraData,
        uint32_t lightCount, uint32_t samplesPerPixel);

    /// Cleanup PathTrace pipeline resources
    void DestroyPathTracePipeline(VkDevice device);

    /// Whether PathTrace pipeline is initialized
    bool IsPathTraceReady();

} // namespace VPTDispatch
