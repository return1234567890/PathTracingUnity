#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

// ═══════════════════════════════════════════════════════════
//  HDR 环境光贴图管理命名空间
//
//  负责创建 RGBA32F equirect VkImage + VkImageView + VkSampler，
//  构建 alias-method importance 采样表（EnvAccel SSBO），
//  供 pathtrace.glsl binding 16（纹理）/ 17（SSBO）使用。
//
//  参考：nvpro_core2/nvvk/hdr_ibl.cpp（createEnvironmentAccel / buildAliasmap）
//        nvpro_core2/nvshaders/hdr_env_sampling.h.slang（environmentSample）
// ═══════════════════════════════════════════════════════════

namespace VPTEnv {

    /// 设置 HDR 环境贴图
    /// pixels: RGBA32F 数据（每像素 4 float，width*height 个像素）
    /// width/height: equirect 贴图尺寸
    /// 在原生侧创建 VkImage + alias 表 + SSBO。可重复调用（先销毁上轮资源）。
    void SetEnvironmentMap(const float* pixels, uint32_t width, uint32_t height);

    /// 销毁所有 HDR 环境资源
    void DestroyResources();

    /// HDR 纹理是否已就绪（VkImage + alias 表已创建）
    bool IsReady();

    // ── Getter（供 vpt_dispatch 绑定 descriptor）──
    VkImageView GetEnvImageView();
    VkSampler   GetEnvSampler();
    VkBuffer    GetEnvAccelBuffer();
    uint32_t    GetEnvWidth();
    uint32_t    GetEnvHeight();

} // namespace VPTEnv
