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
//  占位资源（fallback）：HDR 未上传时 binding 16/17 仍需绑定合法句柄，
//  否则 shader 采样未初始化的描述符属于未定义行为。InitFallback() 会建
//  1×1 黑色 RGBA32F 图 + 1 元素全零 EnvAccel，getter 在真实贴图未就绪时
//  自动返回它们；shader 侧另有 envReady 标志走常数 ambient 回退。
//
//  参考：nvpro_core2/nvvk/hdr_ibl.cpp（createEnvironmentAccel / buildAliasmap）
//        nvpro_core2/nvshaders/hdr_env_sampling.h.slang（environmentSample）
// ═══════════════════════════════════════════════════════════

namespace VPTEnv {

    /// 设置 HDR 环境贴图（CPU 侧：行翻转 + alias 表构建 + 深拷贝，可在主线程调用）。
    /// 真正的 VkImage/VkBuffer 创建与上传在 RecordPendingUploads 中录制进
    /// Unity 的命令缓冲。可重复调用（覆盖上一轮待处理数据）。
    void SetEnvironmentMap(const float* pixels, uint32_t width, uint32_t height);

    /// 是否有待处理的上传/占位资源初始化
    bool HasPendingUploads();

    /// 把待处理的上传（HDR 贴图、占位资源首次清屏/布局转换）录制进命令缓冲。
    /// 由 Plan B 事件在渲染线程上调用；内部会在需要时等待 GPU 空闲再拆旧资源。
    /// 返回 true 表示本帧替换了 HDR 贴图资源（descriptor 里的旧句柄已失效，
    /// 调用方本帧不得再 dispatch）。
    bool RecordPendingUploads(VkCommandBuffer cmd);

    /// 创建占位资源（1×1 黑色图 + 全零 alias 表 + 共享 sampler）。
    /// 在 VPT_Initialize 中调用一次；幂等，失败返回 false（此时 getter 返回空句柄，
    /// 调用方需自行跳过 binding 16/17）。
    bool InitFallback(VkDevice device, VkPhysicalDevice gpu);

    /// 销毁 HDR 环境贴图资源（不含占位资源/sampler）。SetEnvironmentMap 内部会调用。
    void DestroyResources();

    /// 销毁全部资源（含占位资源与 sampler）。VPT_Destroy 时调用。
    void Shutdown();

    /// HDR 纹理是否已就绪（真实贴图 + alias 表已创建）
    bool IsReady();

    // ── Getter（供 vpt_dispatch 绑定 descriptor）──
    // 真实贴图未就绪时 GetEnvImageView/GetEnvAccelBuffer 返回占位资源句柄。
    VkImageView GetEnvImageView();
    VkSampler   GetEnvSampler();
    VkBuffer    GetEnvAccelBuffer();
    uint32_t    GetEnvWidth();
    uint32_t    GetEnvHeight();

} // namespace VPTEnv
