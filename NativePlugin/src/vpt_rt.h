// VulkanPathTracePlugin — Ray Tracing Pipeline 支撑层
//
// 职责（与 vpt_dispatch 分工）：
//   1. 用 vkGetDeviceProcAddr 加载 RT pipeline 扩展函数（vulkan-1.lib 不导出 KHR 符号）
//   2. 生成 SBT（Shader Binding Table）：句柄查询 + 对齐 + buffer + 4 个 region
//
// 为什么单独一个文件：vpt_scene.cpp 里已经有一套 AS 扩展函数的加载/清理模式，
// RT pipeline 这一套是「另一组扩展函数 + 一件独立资源（SBT）」，
// 混进 vpt_dispatch 会让那个文件同时承担管线创建和资源管理两件事。
//
// 对齐规则全部来自 VkPhysicalDeviceRayTracingPipelinePropertiesKHR，禁止硬编码：
//   本机参考值（RTX 3070 Ti Laptop / vulkaninfo 实测）
//     shaderGroupHandleSize       = 32
//     shaderGroupHandleAlignment  = 32
//     shaderGroupBaseAlignment    = 64
//     maxShaderGroupStride        = 4096
//     maxRayRecursionDepth        = 31
//     maxRayHitAttributeSize      = 32

#pragma once

#include <vulkan/vulkan.h>

namespace VPTRt {

    // ── 扩展函数加载 ────────────────────────────────────────
    /// 通过 vkGetDeviceProcAddr 加载 vkCreateRayTracingPipelinesKHR /
    /// vkGetRayTracingShaderGroupHandlesKHR / vkCmdTraceRaysKHR / vkGetBufferDeviceAddressKHR。
    /// 返回 false 时用 MissingFunction() 拿到缺失的名字（说明扩展没启用）。
    bool LoadFunctions(VkInstance instance, VkDevice device);
    bool AreFunctionsReady();
    const char* MissingFunction();

    /// 已加载的函数指针（由 LoadFunctions 填充；调用前先确认 AreFunctionsReady()）
    PFN_vkCreateRayTracingPipelinesKHR        CreateRayTracingPipelines();
    PFN_vkGetRayTracingShaderGroupHandlesKHR  GetRayTracingShaderGroupHandles();
    PFN_vkCmdTraceRaysKHR                     CmdTraceRays();

    // ── SBT ────────────────────────────────────────────────
    /// 一个 SBT 的全部 GPU 资源与 4 个 region。
    /// 生命周期 = RT pipeline 的生命周期（一起创建、一起销毁；与分辨率无关）。
    struct BindingTable
    {
        VkBuffer       buffer   = VK_NULL_HANDLE;
        VkDeviceMemory memory   = VK_NULL_HANDLE;
        void*          mapped   = nullptr;
        VkDeviceSize   size     = 0;
        VkDeviceAddress address = 0;

        // region 的 offset 字段是本实现自己带的（便于日志核对），不是 Vulkan 结构成员
        struct Region
        {
            VkDeviceAddress deviceAddress = 0;
            VkDeviceSize    offset        = 0;
            uint32_t        stride        = 0;
            VkDeviceSize    size          = 0;
        };

        Region raygen;
        Region miss;
        Region hit;
        Region callable;
    };

    /// group 布局（必须与 vpt_dispatch 里的 group 创建顺序一致）：
    ///   0 = raygen (GENERAL)
    ///   1 = missMain (GENERAL)        2 = missShadow (GENERAL)
    ///   3 = hit group 0 (主路径 CH+AH) 4 = hit group 1 (阴影 CH+AH)
    static constexpr uint32_t kGroupCount = 5;
    static constexpr uint32_t kGroupRaygen = 0;
    static constexpr uint32_t kGroupMissMain = 1;
    static constexpr uint32_t kGroupMissShadow = 2;
    static constexpr uint32_t kGroupHitMain = 3;
    static constexpr uint32_t kGroupHitShadow = 4;

    /// 创建 SBT：查 group 句柄 → 按 properties 对齐写入 → 取 buffer device address。
    /// 失败时打印具体原因并返回 false（资源已回滚）。
    bool CreateBindingTable(VkDevice device, VkPhysicalDevice gpu, VkPipeline pipeline,
                           BindingTable& out);

    /// 销毁 SBT（unmap + destroy buffer + free memory）。幂等。
    void DestroyBindingTable(VkDevice device, BindingTable& sbt);

} // namespace VPTRt
