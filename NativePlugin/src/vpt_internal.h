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
