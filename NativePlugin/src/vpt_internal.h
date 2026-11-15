#pragma once

#include "IUnityInterface.h"
#include "IUnityGraphics.h"          // UnityRenderingEventAndData
#include "IUnityGraphicsVulkan.h"

// ── 内部访问器（由 plugin_main.cpp 提供，供 vpt_api.cpp 等模块使用）──

/// 获取 Unity Vulkan 接口
IUnityGraphicsVulkan* VPT_GetUnityVulkan();

/// 获取 Unity 创建的 Vulkan 实例信息
const UnityVulkanInstance& VPT_GetInstance();

/// 刷新 Vulkan 实例信息（预加载模式下设备在 UnityPluginLoad 时可能未就绪）
void VPT_RefreshInstance();

/// 设备是否支持 VK_KHR_ray_query
bool VPT_IsRayQuerySupported();

/// 设备是否支持 VK_KHR_acceleration_structure
bool VPT_IsAccelStructSupported();

/// 插件是否已初始化
bool VPT_IsInitialized();
void VPT_SetInitialized(bool v);

// ── 设备丢失（TDR / VK_ERROR_DEVICE_LOST）状态 ────────────
//
// GPU 复位后 Unity 的 VkDevice 不会再恢复：此时任何 vkQueueSubmit /
// vkDeviceWaitIdle / AS 构建都会失败或直接卡死驱动。检测到丢失后：
//   1. 所有录制路径立刻跳过（不再往命令缓冲里写任何东西）
//   2. C# 侧通过 VPT_GetDeviceStatus 得知后关闭路径追踪并提示用户重开
// 下次 VPT_Initialize / VPT_Destroy 会重置该标志（重进 Play 即恢复）。

/// 设备是否已丢失
bool VPT_IsDeviceLost();

/// 标记设备丢失（只打印一次）
void VPT_MarkDeviceLost(const char* where, int vkResult);

/// 复位丢失标志（VPT_Initialize / VPT_Destroy 时调用）
void VPT_ResetDeviceLost();

/// 检查 VkResult：失败时记录；VK_ERROR_DEVICE_LOST 时自动置丢失标志。
/// 返回 true 表示成功。
bool VPT_CheckVk(VkResult r, const char* where);

/// 读取环境变量开关（值为空 / "0" / "false" 视为关闭）。
/// 用于卡死问题的快速二分排查：
///   VPT_NO_DLSS=1   跳过 NGX（DLSS）初始化与评估
///   VPT_NO_ENV=1    跳过 HDR 环境贴图上传
///   VPT_NO_PROBE=1  关闭"重建后 GPU 同步探针"
bool VPT_EnvFlag(const char* name);
