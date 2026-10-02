// VulkanPathTracePlugin — VkCreateDevice 拦截
//
// 通过 InterceptInitialization 拦截 vkCreateDevice 调用，
// 向 VkDeviceCreateInfo 注入 RT 扩展和特性结构。
//
// 要求：插件必须设为 preloaded (isPreloaded=1)，
// 否则 InterceptInitialization 返回 false（为时已晚）。

#include "vpt_intercept.h"
#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

// ═══════════════════════════════════════════════════════════
//  全局状态
// ═══════════════════════════════════════════════════════════

static PFN_vkGetInstanceProcAddr s_RealGIPA     = nullptr;
static PFN_vkCreateDevice         s_RealCreateDevice = nullptr;
static bool                       s_InterceptActive = false;

// ═══════════════════════════════════════════════════════════
//  拦截的 vkCreateDevice
// ═══════════════════════════════════════════════════════════

static VkResult VKAPI_CALL InterceptedCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    // 1. 收集已有扩展
    std::vector<const char*> extensions(
        pCreateInfo->ppEnabledExtensionNames,
        pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);

    // 2. 检查物理设备是否支持所需扩展，只添加支持的
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> supportedExts(extCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, supportedExts.data());

    auto IsSupported = [&](const char* name) -> bool {
        for (const auto& e : supportedExts)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    auto AddIfMissing = [&](const char* name) {
        for (const char* ext : extensions)
            if (strcmp(ext, name) == 0) return;
        if (IsSupported(name))
            extensions.push_back(name);
        else
            fprintf(stderr, "[VPT] WARN: GPU doesn't support %s\n", name);
    };

    // 按依赖顺序添加
    AddIfMissing(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    AddIfMissing(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    AddIfMissing(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    AddIfMissing(VK_KHR_RAY_QUERY_EXTENSION_NAME);

    // 3. 构建特性链 (pNext chain)
    //    特性必须通过 pNext 显式启用
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bdaFeatures{};
    bdaFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
    bdaFeatures.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{};
    accelFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelFeatures.pNext = &bdaFeatures;
    accelFeatures.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.pNext = &accelFeatures;
    rayQueryFeatures.rayQuery = VK_TRUE;

    // 4. 组装修改后的 VkDeviceCreateInfo
    VkDeviceCreateInfo createInfo = *pCreateInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    // 将特性链插入 pNext 头部
    // 结构顺序: rayQuery -> accel -> bda -> (Unity 原有 pNext 链)
    rayQueryFeatures.pNext = (void*)createInfo.pNext;
    createInfo.pNext = &rayQueryFeatures;

    fprintf(stderr, "[VPT] InterceptedCreateDevice: %u extensions (orig %u), RT features injected\n",
            (uint32_t)extensions.size(), pCreateInfo->enabledExtensionCount);

    // 5. 调用真正的 vkCreateDevice（打印最终启用的扩展列表，供 NGX 扩展需求对比）
    fprintf(stderr, "[VPT] Device extensions enabled (%u):\n", (uint32_t)extensions.size());
    for (const char* ext : extensions)
        fprintf(stderr, "[VPT]   %s\n", ext);
    return s_RealCreateDevice(physicalDevice, &createInfo, pAllocator, pDevice);
}

// ═══════════════════════════════════════════════════════════
//  拦截的 vkGetInstanceProcAddr
// ═══════════════════════════════════════════════════════════

static PFN_vkVoidFunction VKAPI_CALL InterceptedGIPA(VkInstance instance, const char* pName)
{
    PFN_vkVoidFunction func = s_RealGIPA(instance, pName);

    if (pName && strcmp(pName, "vkCreateDevice") == 0 && func)
    {
        s_RealCreateDevice = (PFN_vkCreateDevice)func;
        fprintf(stderr, "[VPT] Intercepting vkCreateDevice to inject RT extensions.\n");
        return (PFN_vkVoidFunction)InterceptedCreateDevice;
    }

    return func;
}

// ═══════════════════════════════════════════════════════════
//  回调入口（UnityVulkanInitCallback 签名）
// ═══════════════════════════════════════════════════════════

static PFN_vkGetInstanceProcAddr UNITY_INTERFACE_API
InterceptInitCallback(PFN_vkGetInstanceProcAddr getInstanceProcAddr, void* /*userdata*/)
{
    s_RealGIPA = getInstanceProcAddr;
    s_InterceptActive = true;
    fprintf(stderr, "[VPT] InterceptInitCallback registered. Will inject RT extensions into vkCreateDevice.\n");
    return InterceptedGIPA;
}

// ═══════════════════════════════════════════════════════════
//  公开注册函数
// ═══════════════════════════════════════════════════════════

bool VPT_RegisterIntercept(IUnityGraphicsVulkan* vulkan)
{
    if (!vulkan)
    {
        fprintf(stderr, "[VPT] VPT_RegisterIntercept: IUnityGraphicsVulkan is null.\n");
        return false;
    }

    bool ok = vulkan->InterceptInitialization(InterceptInitCallback, nullptr);
    if (!ok)
    {
        fprintf(stderr, "[VPT] InterceptInitialization failed! "
                        "Plugin must be preloaded (isPreloaded=1 in .meta) "
                        "and loaded before graphics init.\n");
        return false;
    }

    fprintf(stderr, "[VPT] InterceptInitialization registered successfully.\n");
    return true;
}
