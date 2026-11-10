#pragma once

#include "IUnityInterface.h"
#include "vpt_types.h"
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

    // ── DLSS NGX 接口（由 vpt_dlss.cpp 实现，vpt_api.cpp 导出） ──
    //  仅保留 DLSS Ray Reconstruction（RR）：降噪 + 超分一体化。

    /// 初始化 NGX DLSS Ray Reconstruction feature
    /// renderW/H: 低分辨率渲染尺寸; outputW/H: 全分辨率输出尺寸
    /// qualityMode: 0=Quality 1=Balanced 2=Performance 3=UltraPerf
    /// 返回: 0=成功, -1=NGX不可用/初始化失败, -2=RR 不支持
    int32_t VPT_DLSS_Init_Internal(
        int32_t renderW, int32_t renderH,
        int32_t outputW, int32_t outputH,
        int32_t qualityMode);

    /// 销毁 NGX DLSS feature 并释放资源
    void VPT_DLSS_Destroy_Internal();

    /// Phase 1: 准备 DLSS RR Evaluate dispatch
    /// 绑定 Color/MotionVectors/LinearDepth/Output + Normal+Roughness、albedo G-buffer、视图/投影矩阵
    /// normalRoughPtr: GBuffer1 (normal.xyz + roughness.w)
    /// linearDepthPtr: 视空间线性深度 RT
    /// diffuseAlbedoPtr/specularAlbedoPtr: RR albedo G-buffer（RGBA16F，输入分辨率）
    /// viewMatrix/projMatrix: float[16] 行主序（不含 jitter）
    void VPT_DLSS_PrepareRRDispatch_Internal(
        void* colorLowPtr, void* motionLowPtr, void* linearDepthPtr, void* outputHighPtr,
        void* normalRoughPtr,
        void* diffuseAlbedoPtr, void* specularAlbedoPtr,
        int32_t renderW, int32_t renderH,
        int32_t outputW, int32_t outputH,
        float jitterX, float jitterY, int32_t reset,
        const float* viewMatrix, const float* projMatrix);

    /// Phase 2: DLSS 渲染回调（在 Unity 命令缓冲执行期间触发）
    /// 通过 CommandRecordingState 获取 Unity 的 VkCommandBuffer，
    /// 调用 NVSDK_NGX_VULKAN_EvaluateFeature_C 录入 DLSS Evaluate
    void VPT_DLSS_RenderCallback();

    /// NGX 是否已初始化且 DLSS feature 已创建
    bool VPT_DLSS_IsReady();

    /// 渲染事件 ID（与 vpt_dispatch.h 的 EVENT_PATHTRACE 区分）
    static constexpr int EVENT_DLSS = 2;

#ifdef __cplusplus
}
#endif
