// VulkanPathTracePlugin — Unity Vulkan Native Plugin 入口
//
// 职责：
//   1. UnityPluginLoad/Unload：Unity 加载/卸载 DLL 时调用
//   2. 获取 IUnityGraphicsVulkan 接口，拿到 Unity 的 VkDevice/VkPhysicalDevice
//   3. 检查 VK_KHR_acceleration_structure + VK_KHR_ray_query 扩展支持

#include "IUnityInterface.h"
#include "IUnityGraphics.h"          // UnityRenderingEventAndData
#include "IUnityGraphicsVulkan.h"
#include "vpt_intercept.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

// ── 前向声明 ──────────────────────────────────────────────
void CheckDeviceExtensionsAndFeatures();
bool VPT_RegisterIntercept(IUnityGraphicsVulkan* vulkan);

// ── 全局状态 ──────────────────────────────────────────────
static IUnityGraphicsVulkan*   s_UnityVulkan   = nullptr;
static IUnityInterfaces*       s_UnityInterfaces = nullptr;
static UnityVulkanInstance      s_Instance      = {};
static bool                    s_RayQuerySupported = false;
static bool                    s_AccelStructSupported = false;
static bool                    s_Initialized   = false;
static bool                    s_DeviceChecked = false;

// ── Unity 插件入口 ────────────────────────────────────────
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    s_UnityInterfaces = unityInterfaces;

    // 获取 Unity Vulkan 接口
    s_UnityVulkan = unityInterfaces->Get<IUnityGraphicsVulkan>();
    if (s_UnityVulkan == nullptr)
    {
        fprintf(stderr, "[VPT] ERROR: IUnityGraphicsVulkan not available. "
                        "Is Vulkan graphics API enabled?\n");
        return;
    }

    // 注册 vkCreateDevice 拦截（必须在设备创建前调用，需 isPreloaded=1）
    VPT_RegisterIntercept(s_UnityVulkan);

    // 注意：预加载模式下此时 Vulkan 实例可能尚未创建。
    // 不能调用 Instance() —— 它会触发 Vulkan 初始化，与 InterceptInitialization 回调冲突导致崩溃。
    // Instance() 和 CheckDeviceExtensionsAndFeatures 延迟到 VPT_RefreshInstance() 中调用（由 VPT_Initialize 触发）。

    fflush(stderr);
    fprintf(stderr, "[VPT] UnityPluginLoad succeeded.\n");
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload()
{
    fprintf(stderr, "[VPT] UnityPluginUnload.\n");
    s_UnityVulkan     = nullptr;
    s_UnityInterfaces = nullptr;
    s_Initialized     = false;
}

// ── 设备扩展与特性检查 ────────────────────────────────────
void CheckDeviceExtensionsAndFeatures()
{
    s_RayQuerySupported    = false;
    s_AccelStructSupported = false;

    if (s_Instance.physicalDevice == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] WARNING: VkPhysicalDevice is null.\n");
        return;
    }

    // 1. 枚举设备扩展
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(s_Instance.physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(s_Instance.physicalDevice, nullptr, &extCount, extensions.data());

    bool hasAccelStruct = false;
    bool hasRayQuery    = false;
    bool hasDeferredHostOp = false;

    for (const auto& ext : extensions)
    {
        if (strcmp(ext.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0)
            hasAccelStruct = true;
        if (strcmp(ext.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0)
            hasRayQuery = true;
        if (strcmp(ext.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0)
            hasDeferredHostOp = true;
    }

    // 2. 查询物理设备名称
    VkPhysicalDeviceProperties2 devProps2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    vkGetPhysicalDeviceProperties2(s_Instance.physicalDevice, &devProps2);

    fprintf(stderr, "[VPT] GPU: %s\n", devProps2.properties.deviceName);
    fprintf(stderr, "[VPT] Device extensions:\n");
    fprintf(stderr, "[VPT]   VK_KHR_acceleration_structure : %s\n", hasAccelStruct ? "YES" : "NO");
    fprintf(stderr, "[VPT]   VK_KHR_ray_query              : %s\n", hasRayQuery ? "YES" : "NO");
    fprintf(stderr, "[VPT]   VK_KHR_deferred_host_operations: %s\n", hasDeferredHostOp ? "YES" : "NO");

    // 3. 查询 ray query 特性（通过 features2 pNext 链）
    //    链式结构: features2 -> rayQueryFeatures -> accelFeatures
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    rayQueryFeatures.pNext = &accelFeatures;

    VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features2.pNext = &rayQueryFeatures;
    vkGetPhysicalDeviceFeatures2(s_Instance.physicalDevice, &features2);

    s_RayQuerySupported    = hasRayQuery    && rayQueryFeatures.rayQuery;
    s_AccelStructSupported = hasAccelStruct && accelFeatures.accelerationStructure;

    fprintf(stderr, "[VPT] Feature: rayQuery              = %s\n", rayQueryFeatures.rayQuery ? "true" : "false");
    fprintf(stderr, "[VPT] Feature: accelerationStructure = %s\n", accelFeatures.accelerationStructure ? "true" : "false");
    fprintf(stderr, "[VPT] RT Core path tracing: %s\n",
            (s_RayQuerySupported && s_AccelStructSupported) ? "SUPPORTED" : "NOT SUPPORTED");
}

// ── 供 vpt_api.cpp 使用的内部访问器 ──────────────────────
IUnityGraphicsVulkan* VPT_GetUnityVulkan() { return s_UnityVulkan; }
const UnityVulkanInstance& VPT_GetInstance() { return s_Instance; }
bool VPT_IsRayQuerySupported() { return s_RayQuerySupported; }
bool VPT_IsAccelStructSupported() { return s_AccelStructSupported; }
bool VPT_IsInitialized() { return s_Initialized; }
void VPT_SetInitialized(bool v) { s_Initialized = v; }

/// 刷新 Vulkan 实例信息（预加载模式下设备在 UnityPluginLoad 时可能未就绪）
void VPT_RefreshInstance()
{
    if (!s_UnityVulkan) return;
    s_Instance = s_UnityVulkan->Instance();
    if (!s_DeviceChecked && s_Instance.device != VK_NULL_HANDLE && s_Instance.physicalDevice != VK_NULL_HANDLE)
    {
        CheckDeviceExtensionsAndFeatures();
        s_DeviceChecked = true;
        fflush(stderr);
    }
}
