#pragma once

// ── Vulkan 设备创建拦截（注入 RT 扩展和特性）──────────────
// 通过 IUnityGraphicsVulkan::InterceptInitialization 注册回调，
// 在 Unity 创建 VkDevice 前修改 VkDeviceCreateInfo，添加：
//   VK_KHR_deferred_host_operations
//   VK_EXT_descriptor_indexing   （AS 的规范依赖）
//   VK_KHR_buffer_device_address
//   VK_KHR_acceleration_structure
//   VK_KHR_spirv_1_4 / VK_KHR_shader_float_controls（1.1 设备需要）
//   VK_KHR_ray_tracing_pipeline  （PT 后端；旧 ray_query 后端已删除）
// 以及对应的 feature 结构。

/// 注册 Vulkan 拦截回调。必须在 kUnityGfxDeviceEventInitialize 之前调用。
/// 返回 true 表示成功注册，false 表示为时已晚（需设 isPreloaded=1）。
bool VPT_RegisterIntercept(class IUnityGraphicsVulkan* vulkan);
