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

    // 返回值表示「这条扩展最终是否在启用列表里」（已由 Unity 启用也算）
    auto AddIfMissing = [&](const char* name) -> bool {
        for (const char* ext : extensions)
            if (strcmp(ext, name) == 0) return true;
        if (IsSupported(name)) { extensions.push_back(name); return true; }
        fprintf(stderr, "[VPT] WARN: GPU doesn't support %s\n", name);
        return false;
    };

    // 静默版：用于 Vulkan 1.2+ 已提升为核心的扩展（驱动可能不再单独广告）。
    // 缺失不代表不可用，所以不打印 WARN，避免日志里出现看起来像故障的提示。
    auto AddIfMissingQuiet = [&](const char* name) -> bool {
        for (const char* ext : extensions)
            if (strcmp(ext, name) == 0) return true;
        if (IsSupported(name)) { extensions.push_back(name); return true; }
        return false;
    };

    // 按依赖顺序添加（deferred_host_operations → descriptor_indexing → BDA → AS
    //                    → spirv_1_4 → float_controls → ray_tracing_pipeline）
    // RT pipeline（P0-D1 纯整体切换）：
    //   · VK_KHR_ray_tracing_pipeline 依赖 acceleration_structure + deferred_host_operations；
    //   · VK_KHR_acceleration_structure 依赖 VK_EXT_descriptor_indexing（规范要求显式启用，
    //     否则验证层报 VUID-vkCreateDevice-ppEnabledExtensionNames-01387）；
    //   · 它产出的 SPIR-V 是 1.4 起，Vulkan 1.1 设备必须显式启用 VK_KHR_spirv_1_4；
    //   · VK_KHR_spirv_1_4 的依赖里含 VK_KHR_shader_float_controls（1.1 设备上要显式打开）。
    //   1.2+ 设备上这几者是核心，驱动可能已不再单独广告扩展名 —— 用静默版添加，
    //   缺失不打印 WARN（核心版本本身就允许 SPIR-V 1.4 模块）。
    //
    // 注：旧后端（VK_KHR_ray_query + compute）已于 2026-11-22 清理，这里不再注入 ray_query。
    //
    // ⚠ BDA 只能二选一：VK_KHR_buffer_device_address 与 VK_EXT_buffer_device_address
    //   同时出现在启用列表里违反规范（VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328）。
    //   Unity 自己的扩展表里可能带着 EXT 版（其日志显示 `VK_EXT_buffer_device_address [enabled=0]`，
    //   即 Unity 并不使用它），我们把它从列表里摘掉，统一只保留 KHR 别名 + KHR 的 feature 结构。
    bool extBdaRemoved = false;
    for (auto it = extensions.begin(); it != extensions.end(); )
    {
        if (strcmp(*it, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
        {
            it = extensions.erase(it);
            extBdaRemoved = true;
        }
        else ++it;
    }

    const bool hasDeferred = AddIfMissing(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    AddIfMissingQuiet(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    const bool hasBda      = AddIfMissing(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    const bool hasAccel    = AddIfMissing(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    AddIfMissingQuiet(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    AddIfMissingQuiet(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    const bool hasRtPipeline = AddIfMissing(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    (void)hasDeferred;  // 前两个扩展没有对应 feature 结构，仅扩展名即可
    if (extBdaRemoved)
        fprintf(stderr, "[VPT] Removed VK_EXT_buffer_device_address (Unity had it listed but "
                        "disabled); keeping VK_KHR_buffer_device_address only.\n");

    // 先问一遍 rayTracingPipeline feature 是否真的可用：扩展存在但 feature 为 false 的设备上，
    // 如果照旧把它链进 pNext，vkCreateDevice 会以 VK_ERROR_FEATURE_NOT_PRESENT 直接失败
    // —— 那会把「PT 不可用」升级成「Unity 整个渲染不出图」。所以先查再决定是否注入。
    bool rtFeatureAvailable = false;
    if (hasRtPipeline)
    {
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtProbe{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
        VkPhysicalDeviceFeatures2 probeFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        probeFeatures2.pNext = &rtProbe;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &probeFeatures2);
        rtFeatureAvailable = (rtProbe.rayTracingPipeline == VK_TRUE);
        if (!rtFeatureAvailable)
            fprintf(stderr, "[VPT] WARN: %s present but rayTracingPipeline feature is false; "
                            "not injecting it (PT will be disabled instead of failing vkCreateDevice).\n",
                    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    }
    const bool injectRtPipeline = hasRtPipeline && rtFeatureAvailable;

    // 3. 构建特性链 (pNext chain)
    //    特性必须通过 pNext 显式启用：只启用扩展名不会自动打开 feature 位。
    //    只为本机可用的扩展链入对应结构 —— 否则在不支持 AS 的设备上
    //    vkCreateDevice 会因 VK_ERROR_FEATURE_NOT_PRESENT 直接失败。
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bdaFeatures{};
    bdaFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
    bdaFeatures.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{};
    accelFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelFeatures.accelerationStructure = VK_TRUE;

    // RT pipeline 只需要 rayTracingPipeline 一个 feature：
    //   rayTraversalPrimitiveCulling / rayTracingPipelineTraceRaysIndirect 等
    //   本方案都用不到，不请求（少一个 feature 就少一处驱动拒绝的理由）。
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
    rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtPipelineFeatures.rayTracingPipeline = VK_TRUE;

    // 4. 组装修改后的 VkDeviceCreateInfo
    VkDeviceCreateInfo createInfo = *pCreateInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.ppEnabledExtensionNames = extensions.data();

    // 5. 把我们的特性链插到 pNext 头部，**链尾接回 Unity 原有 pNext 链**
    //    结构顺序: rtPipeline -> accel -> bda -> (Unity 原有 pNext 链)
    //    ⚠ 关键点：只能改「我们自己这条链」的尾指针；把某个 feature 的 pNext
    //    直接指向 Unity 原链会把 accelFeatures/bdaFeatures 从链上摘掉。
    //
    //    另外，同一 sType 在 pNext 链里出现两次属于规范违背（验证层报
    //    duplicate structure），所以 Unity 已经链了同类结构时跳过我们自己那份。
    auto ChainHas = [](const void* pNext, VkStructureType type) -> bool {
        const VkBaseInStructure* p = reinterpret_cast<const VkBaseInStructure*>(pNext);
        for (int guard = 0; p != nullptr && guard < 64; guard++)
        {
            if (p->sType == type) return true;
            p = p->pNext;
        }
        return false;
    };

    void* chainHead = const_cast<void*>(createInfo.pNext);   // 先指向 Unity 原链
    // BDA：EXT 别名已在上面被摘掉，这里统一链 KHR 的 feature 结构
    if (hasBda && !ChainHas(pCreateInfo->pNext, bdaFeatures.sType))
    {
        bdaFeatures.pNext = chainHead;
        chainHead = &bdaFeatures;
    }
    if (hasAccel && !ChainHas(pCreateInfo->pNext, accelFeatures.sType))
    {
        accelFeatures.pNext = chainHead;
        chainHead = &accelFeatures;
    }
    if (injectRtPipeline && !ChainHas(pCreateInfo->pNext, rtPipelineFeatures.sType))
    {
        rtPipelineFeatures.pNext = chainHead;
        chainHead = &rtPipelineFeatures;
    }
    createInfo.pNext = chainHead;

    fprintf(stderr, "[VPT] InterceptedCreateDevice: %u extensions (orig %u), features injected "
                    "(bda=%d accel=%d rtPipeline=%d)\n",
            (uint32_t)extensions.size(), pCreateInfo->enabledExtensionCount,
            (int)hasBda, (int)hasAccel, (int)injectRtPipeline);

    // 6. 调用真正的 vkCreateDevice（打印最终启用的扩展列表，供 NGX 扩展需求对比）
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
