// VulkanPathTracePlugin — Unity Vulkan Native Plugin 入口
//
// 职责：
//   1. UnityPluginLoad/Unload：Unity 加载/卸载 DLL 时调用
//   2. 获取 IUnityGraphicsVulkan 接口，拿到 Unity 的 VkDevice/VkPhysicalDevice
//   3. 检查 VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline 扩展与 feature，
//      并缓存 RT pipeline 属性（SBT 对齐/句柄大小都从这里取）

#include "IUnityInterface.h"
#include "IUnityGraphics.h"          // UnityRenderingEventAndData
#include "IUnityGraphicsVulkan.h"
#include "vpt_intercept.h"
#include "vpt_trace.h"

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
static bool                    s_AccelStructSupported = false;
// RT pipeline（P0）：扩展 + feature + 属性
static bool                    s_RtPipelineExtension = false;
static bool                    s_RtPipelineFeature   = false;
static bool                    s_RtPipelineSupported = false;
static VkPhysicalDeviceRayTracingPipelinePropertiesKHR s_RtProps{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
static bool                    s_Initialized   = false;
static bool                    s_DeviceChecked = false;
static bool                    s_DeviceLost    = false;

// ── Unity 插件入口 ────────────────────────────────────────
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    // 最早的入口：先把诊断日志打开，之后所有步骤都有落盘记录
    VPTTrace::Init();
    VPTTrace::Stage("plugin-load");
    VPTTrace::Line("UnityPluginLoad: enter");

    s_UnityInterfaces = unityInterfaces;

    // 获取 Unity Vulkan 接口
    VPTTrace::Stage("plugin-load:get-IUnityGraphicsVulkan");
    s_UnityVulkan = unityInterfaces->Get<IUnityGraphicsVulkan>();
    if (s_UnityVulkan == nullptr)
    {
        fprintf(stderr, "[VPT] ERROR: IUnityGraphicsVulkan not available. "
                        "Is Vulkan graphics API enabled?\n");
        VPTTrace::Line("ERROR: IUnityGraphicsVulkan not available");
        return;
    }

    // 注册 vkCreateDevice 拦截（必须在设备创建前调用，需 isPreloaded=1）
    VPTTrace::Stage("plugin-load:register-intercept");
    VPT_RegisterIntercept(s_UnityVulkan);

    // 注意：预加载模式下此时 Vulkan 实例可能尚未创建。
    // 不能调用 Instance() —— 它会触发 Vulkan 初始化，与 InterceptInitialization 回调冲突导致崩溃。
    // Instance() 和 CheckDeviceExtensionsAndFeatures 延迟到 VPT_RefreshInstance() 中调用（由 VPT_Initialize 触发）。

    fflush(stderr);
    fprintf(stderr, "[VPT] UnityPluginLoad succeeded.\n");
    VPTTrace::Line("UnityPluginLoad: done (trace=%s)", VPTTrace::Path());
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload()
{
    fprintf(stderr, "[VPT] UnityPluginUnload.\n");
    VPTTrace::Line("UnityPluginUnload");
    // 注意：必须在 DLL 卸载前停掉看门狗线程，否则线程代码会被从内存里抽走
    VPTTrace::Shutdown();
    s_UnityVulkan     = nullptr;
    s_UnityInterfaces = nullptr;
    s_Initialized     = false;
}

// ── 设备扩展与特性检查 ────────────────────────────────────
void CheckDeviceExtensionsAndFeatures()
{
   s_AccelStructSupported = false;
    s_RtPipelineExtension  = false;
    s_RtPipelineFeature    = false;
    s_RtPipelineSupported  = false;

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
   bool hasDeferredHostOp = false;
    bool hasRtPipeline  = false;
    bool hasSpirv14     = false;
    bool hasFloatCtrl   = false;

    for (const auto& ext : extensions)
    {
        if (strcmp(ext.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0)
            hasAccelStruct = true;
       if (strcmp(ext.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0)
            hasDeferredHostOp = true;
        if (strcmp(ext.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0)
            hasRtPipeline = true;
        if (strcmp(ext.extensionName, VK_KHR_SPIRV_1_4_EXTENSION_NAME) == 0)
            hasSpirv14 = true;
        if (strcmp(ext.extensionName, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME) == 0)
            hasFloatCtrl = true;
    }
    s_RtPipelineExtension = hasRtPipeline;

    // 2. 查询物理设备名称 + 版本
    //    apiVersion 决定 slangc 的 SPIR-V 目标：1.1→spirv_1_4 起步（本次固定 1.4，1.1 需要
    //    VK_KHR_spirv_1_4，2/3 核心版本本身就允许 1.4 模块）。
    //    实例版本用 vkEnumerateInstanceVersion 单独打印（Unity 自己创建实例，插件拿不到
    //    VkApplicationInfo，只能问 loader）。
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 devProps2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    devProps2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(s_Instance.physicalDevice, &devProps2);
    s_RtProps = rtProps;

    uint32_t instanceVersion = 0;
    if (&vkEnumerateInstanceVersion != nullptr)   // Vulkan 1.1+ loader
        vkEnumerateInstanceVersion(&instanceVersion);

    const VkPhysicalDeviceProperties& dp = devProps2.properties;
    fprintf(stderr, "[VPT] GPU: %s\n", dp.deviceName);
    fprintf(stderr, "[VPT] API version: device %u.%u.%u (0x%08x), instance %u.%u.%u (0x%08x)\n",
            VK_VERSION_MAJOR(dp.apiVersion), VK_VERSION_MINOR(dp.apiVersion), VK_VERSION_PATCH(dp.apiVersion),
            dp.apiVersion,
            VK_VERSION_MAJOR(instanceVersion), VK_VERSION_MINOR(instanceVersion),
            VK_VERSION_PATCH(instanceVersion), instanceVersion);
    fprintf(stderr, "[VPT] Device extensions:\n");
    fprintf(stderr, "[VPT]   VK_KHR_acceleration_structure   : %s\n", hasAccelStruct ? "YES" : "NO");
    fprintf(stderr, "[VPT]   VK_KHR_deferred_host_operations : %s\n", hasDeferredHostOp ? "YES" : "NO");
    fprintf(stderr, "[VPT]   VK_KHR_ray_tracing_pipeline     : %s\n", hasRtPipeline ? "YES" : "NO");
    fprintf(stderr, "[VPT]   VK_KHR_spirv_1_4               : %s\n", hasSpirv14 ? "YES" : "NO");
    fprintf(stderr, "[VPT]   VK_KHR_shader_float_controls   : %s\n", hasFloatCtrl ? "YES" : "NO");
    if (hasRtPipeline)
    {
        fprintf(stderr, "[VPT] RT pipeline properties: handleSize=%u handleAlignment=%u baseAlignment=%u "
                        "maxShaderGroupStride=%u maxRayRecursionDepth=%u maxRayHitAttributeSize=%u\n",
                s_RtProps.shaderGroupHandleSize, s_RtProps.shaderGroupHandleAlignment,
                s_RtProps.shaderGroupBaseAlignment, s_RtProps.maxShaderGroupStride,
                s_RtProps.maxRayRecursionDepth, s_RtProps.maxRayHitAttributeSize);
    }

    // 3. 查询 RT 特性（通过 features2 pNext 链）
    //    链式结构: features2 -> rtPipelineFeatures -> accelFeatures
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    rtPipelineFeatures.pNext = &accelFeatures;

    VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features2.pNext = &rtPipelineFeatures;
    vkGetPhysicalDeviceFeatures2(s_Instance.physicalDevice, &features2);

    s_AccelStructSupported = hasAccelStruct && accelFeatures.accelerationStructure;
    s_RtPipelineFeature    = rtPipelineFeatures.rayTracingPipeline;

    fprintf(stderr, "[VPT] Feature: accelerationStructure = %s\n", accelFeatures.accelerationStructure ? "true" : "false");
    fprintf(stderr, "[VPT] Feature: rayTracingPipeline    = %s\n", rtPipelineFeatures.rayTracingPipeline ? "true" : "false");

    // 4. 能力判定（P0-D5：不满足就明确报错，不静默回退）
    //    条件 = RT pipeline 扩展存在 && rayTracingPipeline feature && AS 扩展/feature。
    //    失败时把缺哪一项打清楚，C# 侧据此给出具体提示。
    s_RtPipelineSupported = hasRtPipeline && s_RtPipelineFeature && s_AccelStructSupported;
    if (s_RtPipelineSupported)
    {
        fprintf(stderr, "[VPT] RT pipeline path tracing: SUPPORTED\n");
    }
    else
    {
        fprintf(stderr, "[VPT] RT pipeline path tracing: NOT SUPPORTED (extension=%d feature=%d accelStruct=%d)\n",
                (int)(hasRtPipeline), (int)s_RtPipelineFeature, (int)s_AccelStructSupported);
    }
}

// ── 供 vpt_api.cpp 使用的内部访问器 ──────────────────────
IUnityGraphicsVulkan* VPT_GetUnityVulkan() { return s_UnityVulkan; }
const UnityVulkanInstance& VPT_GetInstance() { return s_Instance; }
bool VPT_IsAccelStructSupported() { return s_AccelStructSupported; }
bool VPT_IsRayTracingPipelineSupported() { return s_RtPipelineSupported; }
const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& VPT_GetRayTracingPipelineProperties() { return s_RtProps; }
bool VPT_IsInitialized() { return s_Initialized; }
void VPT_SetInitialized(bool v) { s_Initialized = v; }

// ── 设备丢失状态 ──────────────────────────────────────────
bool VPT_IsDeviceLost() { return s_DeviceLost; }

void VPT_ResetDeviceLost()
{
    if (s_DeviceLost)
        fprintf(stderr, "[VPT] Device lost 标志已复位（重新初始化）。\n");
    s_DeviceLost = false;
}

void VPT_MarkDeviceLost(const char* where, int vkResult)
{
    if (!s_DeviceLost)
    {
        fprintf(stderr,
                "[VPT] FATAL: Vulkan 设备丢失（VkResult=%d，位置=%s）。"
                "GPU 已复位（TDR），Unity 的 VkDevice 无法恢复；"
                "已停止该设备的全部路径追踪请求。\n",
                vkResult, where ? where : "?");
        fflush(stderr);
        VPTTrace::Line("FATAL: device lost (VkResult=%d at %s)", vkResult, where ? where : "?");
    }
    s_DeviceLost = true;
}

bool VPT_CheckVk(VkResult r, const char* where)
{
    if (r == VK_SUCCESS)
        return true;
    if (r == VK_ERROR_DEVICE_LOST)
        VPT_MarkDeviceLost(where, (int)r);
    else
        fprintf(stderr, "[VPT] %s failed: %d\n", where ? where : "vk call", (int)r);
    return false;
}

// 环境变量开关（二分排查用），值为空 / "0" / "n..." / "f..." 视为关闭。
//
// 注意：本函数**没有**做结果缓存，每次调用都会真的读一次环境变量。
// 调用频率：VPT_NO_DLSS 只在 DLSS Init 时一次；VPT_NO_PROBE 只在重建后的探针帧一次；
//   VPT_NO_ENV 在 RecordPendingUploads 里，PT 运行期间是每帧一次（getenv_s 是微秒级，
//   可忽略，但确实不是零）。环境变量在进程内不会变，理论上可以缓存，需要时再加 static 缓存
//   （改这里要重建 DLL）。
bool VPT_EnvFlag(const char* name)
{
    if (!name || !name[0])
        return false;

    char value[64] = { 0 };
    size_t len = 0;
    if (getenv_s(&len, value, sizeof(value), name) != 0 || len == 0)
        return false;

    return !(value[0] == '0' || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' || value[0] == 'F');
}

/// 刷新 Vulkan 实例信息（预加载模式下设备在 UnityPluginLoad 时可能未就绪）
void VPT_RefreshInstance()
{
    if (!s_UnityVulkan) return;

    // Instance() 会驱动 Unity 的 Vulkan 初始化（可能等待设备创建），是初始化阶段
    // 最容易卡住的一步之一：前后各记一条。
    VPTTrace::Stage("refresh-instance:Unity Instance()");
    VPTTrace::Line("VPT_RefreshInstance: calling IUnityGraphicsVulkan::Instance() ...");
    s_Instance = s_UnityVulkan->Instance();
    VPTTrace::Line("VPT_RefreshInstance: Instance() returned (device=%p, physical=%p)",
                   (void*)s_Instance.device, (void*)s_Instance.physicalDevice);

    if (!s_DeviceChecked && s_Instance.device != VK_NULL_HANDLE && s_Instance.physicalDevice != VK_NULL_HANDLE)
    {
        VPTTrace::Stage("refresh-instance:check-extensions");
        CheckDeviceExtensionsAndFeatures();
        s_DeviceChecked = true;
        fflush(stderr);
    }
    VPTTrace::Stage("refresh-instance:done");
}
