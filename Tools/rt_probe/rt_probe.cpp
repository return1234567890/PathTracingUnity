// ══════════════════════════════════════════════════════════════
//  rt_probe — RT pipeline 离线自检（不需要 Unity）
//
//  为什么有这个东西：本方案的闸门 G0/G1/G2（能建 RT 管线 / 7 入口能链接 /
//  SBT 句柄与 region 正确）本质上都是「驱动 + SPIR-V + SBT 布局」的问题，
//  跟 Unity 无关。把它们搬到命令行里跑，就不必每次都启动 Unity 才能知道
//  「是不是我第一次就写错了」。
//
//  它做的事：
//    1. 用与 vpt_intercept.cpp 相同的扩展/feature 组合建 VkDevice
//       → G0：扩展与 feature 链正确、设备可创建
//    2. 加载 pt.spv（slangc 产物），vkCreateShaderModule
//       → 验证 SPIR-V 版本被本机驱动接受（方案 §6 风险 3/8）
//    3. 建 18 binding 的 descriptor set layout（与 vpt_dispatch.cpp 一致）
//    4. 建 RT pipeline：7 stage / 5 group / recursion=1
//       → G1：7 个入口能被链接（pName 与 group 对应）
//    5. vkGetRayTracingShaderGroupHandlesKHR + 按 properties 对齐生成 SBT
//       → G2：句柄非全零、region 布局符合预期
//    6. （--trace）用 1 个三角形 BLAS/TLAS + 假资源录一次 vkCmdTraceRaysKHR，
//       把 5 张输出图读回：
//       → 冒烟测试 raygen/CH/AH/miss/阴影 group + payload + UBO 矩阵方向
//
//  用法：
//    rt_probe.exe <path-to-pt.spv>            # G0/G1/G2
//    rt_probe.exe <path-to-pt.spv> --trace    # 再加 traceRays 冒烟
//
//  退出码：0 = 全部通过；1 = 有失败项。
// ══════════════════════════════════════════════════════════════

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>

// ═══════════════════════════════════════════════════════════
//  与引擎侧一致的 18 binding 描述符布局（vpt_dispatch.cpp）
// ═══════════════════════════════════════════════════════════
static const VkShaderStageFlags kRtStages =
    VK_SHADER_STAGE_RAYGEN_BIT_KHR
  | VK_SHADER_STAGE_MISS_BIT_KHR
  | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
  | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

// 与 pt_common.slang / vpt_types.h 一致的布局
#pragma pack(push, 1)
struct VPT_Vertex
{
    float pos[3];    float pad0;
    float normal[3]; float pad1;
    float tangent[4];
    float uv[2];     float pad2[2];
    float color[4];
};
struct VPT_Material
{
    float albedo[4];
    float metallic, roughness, bumpScale;
    uint32_t flags;
    float emissionColor[4];
    uint32_t baseTexID, metalRoughTexID, normalTexID, emissiveTexID;
    float uvScaleOffset[4];
    float transmission, ior, diffuseTransmission;
    uint32_t thinWalled;
    float attenuationColor[4];
    float attenuationDistance;
    uint32_t volumeFlags;
    float thickness;
    float alphaCutoff;
};
struct VPT_Light
{
    float positionOrDir[3];
    float range;
    float color[3];
    uint32_t type;
};
struct InstanceInfo
{
    float transformRow0[4];
    float transformRow1[4];
    float transformRow2[4];
    uint32_t vertexOffset, indexOffset, materialID, pad;
};
struct EnvAccel { uint32_t alias; float q; };
#pragma pack(pop)
static_assert(sizeof(VPT_Vertex) == 80, "VPT_Vertex 80B");
static_assert(sizeof(VPT_Material) == 128, "VPT_Material 128B");
static_assert(sizeof(VPT_Light) == 32, "VPT_Light 32B");
static_assert(sizeof(InstanceInfo) == 64, "InstanceInfo 64B");

struct CameraUBO
{
    float viewInv[16];
    float projInv[16];
    float posFov[4];
    float jitterNearFar[4];
    int32_t frameMaxDepth[4];
    float ambientPad[4];
    uint32_t lightCount;
    uint32_t samplesPerPixel;
    float envIntensity;
    float envRotation;
};
static_assert(sizeof(CameraUBO) == 208, "CameraUBO 208B");

// ═══════════════════════════════════════════════════════════
//  小工具
// ═══════════════════════════════════════════════════════════

// 加速结构相关是 KHR 扩展函数，vulkan-1.lib 不导出（与插件里 vpt_scene.cpp 的做法一致：
// 运行时用 vkGetDeviceProcAddr 取）
static PFN_vkGetAccelerationStructureBuildSizesKHR   pfnGetAsBuildSizes   = nullptr;
static PFN_vkCreateAccelerationStructureKHR          pfnCreateAs          = nullptr;
static PFN_vkCmdBuildAccelerationStructuresKHR       pfnCmdBuildAs        = nullptr;
static PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAsAddress     = nullptr;

static int g_Failures = 0;

static void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_Failures++;
}

static uint32_t AlignUp(uint32_t v, uint32_t a)
{
    return a ? ((v + a - 1u) / a * a) : v;
}

static const char* VkStr(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    default: return "VkResult(?)";
    }
}

static bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (const auto& e : exts)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

static uint32_t FindMemoryType(VkPhysicalDevice gpu, uint32_t typeBits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

// ═══════════════════════════════════════════════════════════
//  Validation layer 消息回调
// ═══════════════════════════════════════════════════════════

static uint32_t g_ValidationErrors = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/)
{
    const char* level = "INFO";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { level = "ERROR"; g_ValidationErrors++; }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = "WARN";
    printf("  [VVL %s] %s\n", level, data->pMessage);
    return VK_FALSE;
}

// ═══════════════════════════════════════════════════════════
//  缓冲/图像小助手
// ═══════════════════════════════════════════════════════════

struct Buf
{
    VkBuffer       buffer   = VK_NULL_HANDLE;
    VkDeviceMemory memory   = VK_NULL_HANDLE;
    void*          mapped   = nullptr;
    VkDeviceSize   size     = 0;
    VkDeviceAddress address = 0;
};

static Buf CreateBuffer(VkDevice dev, VkPhysicalDevice gpu, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool withAddress)
{
    Buf b; b.size = size;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (withAddress) bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(dev, &bci, nullptr, &b.buffer) != VK_SUCCESS) { b.buffer = VK_NULL_HANDLE; return b; }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);

    VkMemoryAllocateFlagsInfo fi{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    if (withAddress) { fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; }

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = withAddress ? &fi : nullptr;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, req.memoryTypeBits, props);
    if (vkAllocateMemory(dev, &mai, nullptr, &b.memory) != VK_SUCCESS) return b;
    vkBindBufferMemory(dev, b.buffer, b.memory, 0);

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(dev, b.memory, 0, size, 0, &b.mapped);

    if (withAddress)
    {
        VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        ai.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(dev, &ai);
    }
    return b;
}

static void DestroyBuf(VkDevice dev, Buf& b)
{
    if (b.mapped) vkUnmapMemory(dev, b.memory);
    if (b.buffer) vkDestroyBuffer(dev, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev, b.memory, nullptr);
    b = {};
}

struct Img
{
    VkImage     image  = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

static Img CreateImage(VkDevice dev, VkPhysicalDevice gpu, VkFormat fmt,
                       uint32_t w, uint32_t h, uint32_t layers, bool sampled)
{
    Img img;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = fmt;
    ci.extent        = { w, h, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = layers;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = VK_IMAGE_USAGE_STORAGE_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ci, nullptr, &img.image) != VK_SUCCESS)
    {
        printf("  !! vkCreateImage failed (format=%d %ux%u layers=%u sampled=%d)\n",
               (int)fmt, w, h, layers, (int)sampled);
        return img;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img.image, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, req.memoryTypeBits, 0);
    if (vkAllocateMemory(dev, &mai, nullptr, &img.memory) != VK_SUCCESS) return img;
    vkBindImageMemory(dev, img.image, img.memory, 0);

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
    vkCreateImageView(dev, &vi, nullptr, &img.view);
    return img;
}

// ═══════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════

int main(int argc, char** argv)
{
    const char* spvPath = (argc > 1 && argv[1][0] != '-') ? argv[1] : "pt.spv";
    bool doTrace = false;
    // 默认不开验证层：部分环境下（本机沙箱实测）VK_LAYER_KHRONOS_validation 会让
    // vkCreateInstance 失败甚至直接崩；需要时显式 --validation。
    bool wantValidation = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--trace") == 0) doTrace = true;
        if (strcmp(argv[i], "--validation") == 0) wantValidation = true;
        if (strcmp(argv[i], "--no-validation") == 0) wantValidation = false;
    }

    printf("=== RT pipeline probe ===\n");
    printf("SPIR-V: %s\n", spvPath);

    // ── 读 SPIR-V ──
    std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
    if (!f)
    {
        printf("!! cannot open %s\n", spvPath);
        return 1;
    }
    const size_t spvSize = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> spv(spvSize);
    f.read(reinterpret_cast<char*>(spv.data()), (std::streamsize)spvSize);
    printf("SPIR-V size: %zu bytes\n", spvSize);

    // ═══════════ 1. Instance ═══════════
    printf("\n[1] Instance\n");
    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    printf("  loader apiVersion: %u.%u.%u\n",
           VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion), VK_VERSION_PATCH(loaderVersion));

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool hasValidation = false;
    for (const auto& l : layers)
        if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) hasValidation = true;
    hasValidation = hasValidation && wantValidation;

    std::vector<const char*> instExts = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    if (hasValidation) instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "URPPT-rt_probe";
    appInfo.apiVersion = VK_API_VERSION_1_1;   // 与 Unity 2022.3 的 Vulkan 版本对齐

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    if (hasValidation)
    {
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &validationLayer;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&ici, nullptr, &instance);
    Check(res == VK_SUCCESS, "vkCreateInstance");
    if (res != VK_SUCCESS) { printf("  !! %s (code %d)\n", VkStr(res), (int)res); return 1; }

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (hasValidation)
    {
        auto createMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (createMessenger)
        {
            VkDebugUtilsMessengerCreateInfoEXT dci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = DebugCallback;
            createMessenger(instance, &dci, nullptr, &messenger);
            printf("  validation layer: ON\n");
        }
    }
    else
    {
        printf("  validation layer: not available (skipped)\n");
    }

    // ═══════════ 2. 物理设备 + RT 能力 ═══════════
    printf("\n[2] Physical device\n");
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    if (gpus.empty()) { printf("  !! no Vulkan device\n"); return 1; }

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
    for (VkPhysicalDevice candidate : gpus)
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, exts.data());
        if (HasExtension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
            && HasExtension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME))
        {
            gpu = candidate;
            break;
        }
    }
    if (gpu == VK_NULL_HANDLE) { printf("  !! no device with VK_KHR_ray_tracing_pipeline\n"); return 1; }

    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(gpu, &props2);
    printf("  GPU: %s\n", props2.properties.deviceName);
    printf("  device apiVersion: %u.%u.%u\n",
           VK_VERSION_MAJOR(props2.properties.apiVersion),
           VK_VERSION_MINOR(props2.properties.apiVersion),
           VK_VERSION_PATCH(props2.properties.apiVersion));
    printf("  RT props: handleSize=%u handleAlignment=%u baseAlignment=%u maxStride=%u maxRecursion=%u maxHitAttr=%u\n",
           rtProps.shaderGroupHandleSize, rtProps.shaderGroupHandleAlignment,
           rtProps.shaderGroupBaseAlignment, rtProps.maxShaderGroupStride,
           rtProps.maxRayRecursionDepth, rtProps.maxRayHitAttributeSize);

    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &devExtCount, devExts.data());
    const bool hasSpirv14 = HasExtension(devExts, VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    const bool hasFloatControls = HasExtension(devExts, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    printf("  VK_KHR_spirv_1_4=%d VK_KHR_shader_float_controls=%d\n", (int)hasSpirv14, (int)hasFloatControls);
    // 验证层依赖检查用：AS 依赖 descriptor_indexing；BDA 的 KHR/EXT 两个别名
    printf("  VK_EXT_descriptor_indexing=%d  VK_KHR_buffer_device_address=%d  VK_EXT_buffer_device_address=%d\n",
           (int)HasExtension(devExts, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME),
           (int)HasExtension(devExts, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
           (int)HasExtension(devExts, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));

    // ═══════════ 3. Device（与 vpt_intercept.cpp 相同的注入组合）═══════════
    printf("\n[3] Device\n");
    std::vector<const char*> devExtsEnabled = {
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    };
    // 1.1 设备要显式启用 spirv_1_4（1.2+ 是核心，扩展名可能不再广告）
    if (props2.properties.apiVersion < VK_API_VERSION_1_2 && hasSpirv14)
        devExtsEnabled.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    if (hasFloatControls)
        devExtsEnabled.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);

    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bdaFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR };
    bdaFeat.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    asFeat.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    rtFeat.rayTracingPipeline = VK_TRUE;

    bdaFeat.pNext = nullptr;
    asFeat.pNext  = &bdaFeat;
    rtFeat.pNext  = &asFeat;

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfs.data());
    uint32_t qfIndex = 0;
    for (uint32_t i = 0; i < qfCount; i++)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfIndex = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qfIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &rtFeat;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExtsEnabled.size();
    dci.ppEnabledExtensionNames = devExtsEnabled.data();

    VkDevice dev = VK_NULL_HANDLE;
    res = vkCreateDevice(gpu, &dci, nullptr, &dev);
    Check(res == VK_SUCCESS, "vkCreateDevice (RT extensions + features injected)");
    if (res != VK_SUCCESS) { printf("  !! %s\n", VkStr(res)); return 1; }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qfIndex, 0, &queue);

    auto pfnCreateRT = (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(dev, "vkCreateRayTracingPipelinesKHR");
    auto pfnGetHandles = (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(dev, "vkGetRayTracingShaderGroupHandlesKHR");
    auto pfnTraceRays = (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(dev, "vkCmdTraceRaysKHR");
    Check(pfnCreateRT && pfnGetHandles && pfnTraceRays, "RT extension function pointers loaded");

    pfnGetAsBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureBuildSizesKHR");
    pfnCreateAs        = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(dev, "vkCreateAccelerationStructureKHR");
    pfnCmdBuildAs      = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(dev, "vkCmdBuildAccelerationStructuresKHR");
    pfnGetAsAddress    = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureDeviceAddressKHR");
    Check(pfnGetAsBuildSizes && pfnCreateAs && pfnCmdBuildAs && pfnGetAsAddress, "AS extension function pointers loaded");

    // ═══════════ 4. Shader module ═══════════
    printf("\n[4] Shader module\n");
    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = spvSize;
    smci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    res = vkCreateShaderModule(dev, &smci, nullptr, &module);
    Check(res == VK_SUCCESS, "vkCreateShaderModule(pt.spv) — SPIR-V 版本被驱动接受");
    if (res != VK_SUCCESS) { printf("  !! %s（多半是 SPIR-V 版本高于设备 Vulkan 版本）\n", VkStr(res)); return 1; }

    // ═══════════ 5. Descriptor set layout（18 binding）═══════════
    printf("\n[5] Descriptor set layout (18 bindings)\n");
    VkDescriptorSetLayoutBinding bindings[18] = {};
    for (int i = 0; i < 18; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = kRtStages;
    }
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (int i = 5; i <= 9; i++) bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (int i = 10; i <= 13; i++) bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 18;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    res = vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl);
    Check(res == VK_SUCCESS, "vkCreateDescriptorSetLayout(18 RT-stage bindings)");
    if (res != VK_SUCCESS) return 1;

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    res = vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout);
    Check(res == VK_SUCCESS, "vkCreatePipelineLayout");

    // ═══════════ 6. RT pipeline（7 stage / 5 group / recursion=1）═══════════
    printf("\n[6] Ray tracing pipeline (G0/G1)\n");
    enum { eRaygen = 0, eMiss, eMissShadow, eClosestHit, eClosestHitShadow, eAnyHit, eAnyHitShadow, eStageCount };
    const char* names[eStageCount] = {
        "rgenMain", "rmissMain", "rmissShadow", "rchitMain", "rchitShadow", "rahitMain", "rahitShadow" };
    const VkShaderStageFlagBits flags[eStageCount] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR };

    VkPipelineShaderStageCreateInfo stages[eStageCount] = {};
    for (int i = 0; i < eStageCount; i++)
    {
        stages[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage  = flags[i];
        stages[i].module = module;
        stages[i].pName  = names[i];
    }

    VkRayTracingShaderGroupCreateInfoKHR groups[5] = {};
    for (int i = 0; i < 5; i++)
    {
        groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[i].generalShader = VK_SHADER_UNUSED_KHR;
        groups[i].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[0].generalShader = eRaygen;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[1].generalShader = eMiss;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[2].generalShader = eMissShadow;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[3].closestHitShader = eClosestHit; groups[3].anyHitShader = eAnyHit;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].closestHitShader = eClosestHitShadow; groups[4].anyHitShader = eAnyHitShadow;

    VkRayTracingPipelineCreateInfoKHR rtci{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
    rtci.stageCount = eStageCount;
    rtci.pStages = stages;
    rtci.groupCount = 5;
    rtci.pGroups = groups;
    rtci.maxPipelineRayRecursionDepth = 1;
    rtci.layout = pipeLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    res = pfnCreateRT(dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtci, nullptr, &pipeline);
    Check(res == VK_SUCCESS, "vkCreateRayTracingPipelinesKHR (7 entries linked)");
    if (res != VK_SUCCESS) { printf("  !! %s\n", VkStr(res)); return 1; }

    // ═══════════ 7. SBT（G2）═══════════
    printf("\n[7] Shader binding table (G2)\n");
    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t stride = AlignUp(handleSize, rtProps.shaderGroupHandleAlignment);
    const uint32_t baseAlign = rtProps.shaderGroupBaseAlignment;
    const uint32_t raygenStride = AlignUp(stride, baseAlign);
    const uint64_t raygenSize = raygenStride;
    const uint64_t missSize = AlignUp(stride * 2, baseAlign);
    const uint64_t hitSize  = AlignUp(stride * 2, baseAlign);
    const uint64_t totalSize = raygenSize + missSize + hitSize;

    std::vector<uint8_t> handles(5 * handleSize, 0);
    res = pfnGetHandles(dev, pipeline, 0, 5, (uint32_t)handles.size(), handles.data());
    Check(res == VK_SUCCESS, "vkGetRayTracingShaderGroupHandlesKHR(5 groups)");

    bool anyNonZero = false;
    for (uint8_t b : handles) if (b) { anyNonZero = true; break; }
    Check(anyNonZero, "group handles are non-zero");

    Buf sbt = CreateBuffer(dev, gpu, totalSize,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    Check(sbt.buffer != VK_NULL_HANDLE, "SBT buffer created");
    Check(sbt.address != 0, "SBT buffer device address != 0 (DEVICE_ADDRESS allocation flag)");

    uint8_t* sbtBase = static_cast<uint8_t*>(sbt.mapped);
    memset(sbtBase, 0, (size_t)totalSize);
    memcpy(sbtBase + 0 * raygenStride, handles.data() + 0 * handleSize, handleSize);
    memcpy(sbtBase + raygenSize + 0 * stride, handles.data() + 1 * handleSize, handleSize);
    memcpy(sbtBase + raygenSize + 1 * stride, handles.data() + 2 * handleSize, handleSize);
    memcpy(sbtBase + raygenSize + missSize + 0 * stride, handles.data() + 3 * handleSize, handleSize);
    memcpy(sbtBase + raygenSize + missSize + 1 * stride, handles.data() + 4 * handleSize, handleSize);

    VkStridedDeviceAddressRegionKHR raygenRegion{ sbt.address, raygenStride, raygenSize };
    VkStridedDeviceAddressRegionKHR missRegion  { sbt.address + raygenSize, stride, missSize };
    VkStridedDeviceAddressRegionKHR hitRegion   { sbt.address + raygenSize + missSize, stride, hitSize };
    VkStridedDeviceAddressRegionKHR callableRegion{ 0, 0, 0 };
    Check((raygenRegion.deviceAddress % baseAlign) == 0, "raygen address aligned to shaderGroupBaseAlignment");
    Check((missRegion.deviceAddress % baseAlign) == 0, "miss address aligned to shaderGroupBaseAlignment");
    Check((hitRegion.deviceAddress % baseAlign) == 0, "hit address aligned to shaderGroupBaseAlignment");
    Check(raygenRegion.stride == raygenRegion.size, "raygen stride == size (spec requirement)");
    printf("  SBT: total=%llu B raygen{addr=0x%llx stride=%llu size=%llu} miss{...+%llu} hit{...+%llu}\n",
           (unsigned long long)totalSize, (unsigned long long)raygenRegion.deviceAddress,
           (unsigned long long)raygenRegion.stride, (unsigned long long)raygenRegion.size,
           (unsigned long long)raygenSize, (unsigned long long)missSize);
    if (!doTrace)
    {
        printf("\n[8] traceRays smoke: skipped (pass --trace)\n");
    }
    else
    {
        // ═══════════ 8. traceRays 冒烟 ═══════════
        printf("\n[8] traceRays smoke test\n");
        const uint32_t W = 8, H = 8;

        // ── 假场景：1 个三角形在 x=2 的 YZ 平面上，法线 (-1,0,0) ──
        Buf vb = CreateBuffer(dev, gpu, 3 * sizeof(VPT_Vertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        {
            VPT_Vertex* v = static_cast<VPT_Vertex*>(vb.mapped);
            memset(v, 0, 3 * sizeof(VPT_Vertex));
            float pos[3][3] = { { 2.0f, -2.0f, -2.0f }, { 2.0f, 2.0f, -2.0f }, { 2.0f, 0.0f, 2.0f } };
            for (int i = 0; i < 3; i++)
            {
                memcpy(v[i].pos, pos[i], sizeof(pos[i]));
                v[i].normal[0] = -1.0f; v[i].normal[1] = 0.0f; v[i].normal[2] = 0.0f;
                v[i].tangent[0] = 0.0f; v[i].tangent[1] = 1.0f; v[i].tangent[2] = 0.0f; v[i].tangent[3] = 1.0f;
                v[i].color[0] = v[i].color[1] = v[i].color[2] = v[i].color[3] = 1.0f;
            }
        }
        // 索引顺序让几何法线 = (-1,0,0)（朝向相机），否则阴影偏移会把射线推到三角形
        // 背面、NEE 的阴影光线被自己挡住 → 命中像素恒为 0，测试判据就失去意义
        uint32_t triIdx[3] = { 0, 2, 1 };
        Buf ib = CreateBuffer(dev, gpu, sizeof(triIdx),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        memcpy(ib.mapped, triIdx, sizeof(triIdx));

        Buf instBuf = CreateBuffer(dev, gpu, sizeof(InstanceInfo),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        {
            InstanceInfo* ii = static_cast<InstanceInfo*>(instBuf.mapped);
            memset(ii, 0, sizeof(InstanceInfo));
            ii[0].transformRow0[0] = 1.0f;
            ii[0].transformRow1[1] = 1.0f;
            ii[0].transformRow2[2] = 1.0f;
            ii[0].vertexOffset = 0; ii[0].indexOffset = 0; ii[0].materialID = 0;
        }
        Buf matBuf = CreateBuffer(dev, gpu, sizeof(VPT_Material),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        {
            VPT_Material* m = static_cast<VPT_Material*>(matBuf.mapped);
            memset(m, 0, sizeof(VPT_Material));
            m[0].albedo[0] = 0.8f; m[0].albedo[1] = 0.2f; m[0].albedo[2] = 0.1f; m[0].albedo[3] = 1.0f;
            m[0].metallic = 0.0f; m[0].roughness = 0.5f; m[0].ior = 1.5f;
            m[0].flags = 128u;        // MAT_ALPHA_TEST：让 rahitMain 真正被调用
            m[0].alphaCutoff = 0.5f;
            m[0].baseTexID = 0xFFFFFFFFu; m[0].metalRoughTexID = 0xFFFFFFFFu;
            m[0].normalTexID = 0xFFFFFFFFu; m[0].emissiveTexID = 0xFFFFFFFFu;
            m[0].uvScaleOffset[0] = 1.0f; m[0].uvScaleOffset[1] = 1.0f;
        }
        Buf lightBuf = CreateBuffer(dev, gpu, sizeof(VPT_Light),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        {
            VPT_Light* l = static_cast<VPT_Light*>(lightBuf.mapped);
            memset(l, 0, sizeof(VPT_Light));
            l[0].positionOrDir[0] = 0.0f; l[0].positionOrDir[1] = 0.0f; l[0].positionOrDir[2] = -1.0f;
            l[0].range = 1e34f;
            l[0].color[0] = 50.0f; l[0].color[1] = 50.0f; l[0].color[2] = 50.0f;
            l[0].type = 0;
        }
        Buf envAccelBuf = CreateBuffer(dev, gpu, sizeof(EnvAccel),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        {
            EnvAccel* e = static_cast<EnvAccel*>(envAccelBuf.mapped);
            e[0].alias = 0; e[0].q = 1.0f;
        }

        Buf ubo = CreateBuffer(dev, gpu, sizeof(CameraUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        {
            CameraUBO* c = static_cast<CameraUBO*>(ubo.mapped);
            memset(c, 0, sizeof(CameraUBO));
            // viewInv = 绕 Y 轴把 +Z 转成 +X（行主序）。若矩阵方向解释反了（转置），
            // 光线会朝 -X → 全部 miss → 输出恒为 ambient，测试能直接看出来。
            c->viewInv[0] = 0.0f;  c->viewInv[1] = 0.0f; c->viewInv[2] = 1.0f;  c->viewInv[3] = 0.0f;
            c->viewInv[4] = 0.0f;  c->viewInv[5] = 1.0f; c->viewInv[6] = 0.0f;  c->viewInv[7] = 0.0f;
            c->viewInv[8] = -1.0f; c->viewInv[9] = 0.0f; c->viewInv[10] = 0.0f; c->viewInv[11] = 0.0f;
            c->viewInv[12] = 0.0f; c->viewInv[13] = 0.0f; c->viewInv[14] = 0.0f; c->viewInv[15] = 1.0f;
            c->projInv[0] = 1.0f; c->projInv[5] = 1.0f; c->projInv[10] = 1.0f; c->projInv[15] = 1.0f;
            c->posFov[0] = 0.0f; c->posFov[1] = 0.0f; c->posFov[2] = 0.0f; c->posFov[3] = 1.0f;
            c->jitterNearFar[0] = 0.0f; c->jitterNearFar[1] = 0.0f;
            c->jitterNearFar[2] = 0.01f; c->jitterNearFar[3] = 100.0f;
            c->frameMaxDepth[0] = 1;  // frameCount
            c->frameMaxDepth[1] = 4;  // maxDepth
            c->frameMaxDepth[2] = 0;  // envReady = 0 → 常数 ambient 回退（结果可预测）
            c->ambientPad[0] = 0.2f; c->ambientPad[1] = 0.4f; c->ambientPad[2] = 0.6f;
            c->lightCount = 1;
            c->samplesPerPixel = 1;
            c->envIntensity = 1.0f;
        }

        // ── 5 张输出图 + 4 张纹理数组 + HDR 环境图 ──
        Img outImg   = CreateImage(dev, gpu, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 1, false);
        Img gbuf0    = CreateImage(dev, gpu, VK_FORMAT_R32G32B32A32_SFLOAT, W, H, 1, false);
        Img gbuf1    = CreateImage(dev, gpu, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 1, false);
        Img albDiff  = CreateImage(dev, gpu, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 1, false);
        Img albSpec  = CreateImage(dev, gpu, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 1, false);
        Img tex0 = CreateImage(dev, gpu, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, true);
        Img tex1 = CreateImage(dev, gpu, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, true);
        Img tex2 = CreateImage(dev, gpu, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, true);
        Img tex3 = CreateImage(dev, gpu, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, true);
        Img envImg = CreateImage(dev, gpu, VK_FORMAT_R16G16B16A16_SFLOAT, 2, 2, 1, true);
        Check(outImg.view && gbuf0.view && gbuf1.view && albDiff.view && albSpec.view, "output images created");
        Check(tex0.view && tex1.view && tex2.view && tex3.view && envImg.view, "texture array + env images created");

        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler sampler = VK_NULL_HANDLE;
        vkCreateSampler(dev, &sci, nullptr, &sampler);

        // ── BLAS + TLAS（1 个三角形）──
        VkAccelerationStructureGeometryKHR geo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;   // 与 vpt_scene 一致；实例级 FORCE_OPAQUE 由 ALPHA_TEST 决定
        geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = vb.address;
        geo.geometry.triangles.vertexStride = sizeof(VPT_Vertex);
        geo.geometry.triangles.maxVertex = 2;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = ib.address;
        geo.geometry.triangles.transformData.deviceAddress = 0;

        VkAccelerationStructureBuildGeometryInfoKHR blasInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        blasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blasInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        blasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        blasInfo.geometryCount = 1;
        blasInfo.pGeometries = &geo;
        uint32_t primCount = 1;

        VkAccelerationStructureBuildSizesInfoKHR blasSizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pfnGetAsBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                           &blasInfo, &primCount, &blasSizes);

        Buf blasBuf = CreateBuffer(dev, gpu, blasSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, true);
        VkAccelerationStructureCreateInfoKHR asci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asci.buffer = blasBuf.buffer;
        asci.size = blasSizes.accelerationStructureSize;
        asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        pfnCreateAs(dev, &asci, nullptr, &blas);
        Buf blasScratch = CreateBuffer(dev, gpu, blasSizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, true);

        blasInfo.dstAccelerationStructure = blas;
        blasInfo.scratchData.deviceAddress = blasScratch.address;

        // TLAS 实例
        Buf instAsBuf = CreateBuffer(dev, gpu, sizeof(VkAccelerationStructureInstanceKHR),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);

        VkAccelerationStructureDeviceAddressInfoKHR ddai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        ddai.accelerationStructure = blas;
        VkDeviceAddress blasAddr = pfnGetAsAddress(dev, &ddai);

        {
            VkAccelerationStructureInstanceKHR* inst =
                static_cast<VkAccelerationStructureInstanceKHR*>(instAsBuf.mapped);
            memset(inst, 0, sizeof(*inst));
            inst->transform.matrix[0][0] = 1.0f;
            inst->transform.matrix[1][1] = 1.0f;
            inst->transform.matrix[2][2] = 1.0f;
            inst->instanceCustomIndex = 0;   // materialID
            inst->mask = 0xFF;
            inst->instanceShaderBindingTableRecordOffset = 0;
            // 材质带 MAT_ALPHA_TEST ⇒ 不标 FORCE_OPAQUE（与 vpt_scene 的规则一致），
            // 让 rahitMain 真的被调用一次
            inst->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst->accelerationStructureReference = blasAddr;
        }

        VkAccelerationStructureGeometryKHR tgeo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tgeo.geometry.instances.data.deviceAddress = instAsBuf.address;
        tgeo.geometry.instances.arrayOfPointers = VK_FALSE;

        VkAccelerationStructureBuildGeometryInfoKHR tlasInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        tlasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasInfo.geometryCount = 1;
        tlasInfo.pGeometries = &tgeo;
        uint32_t tlasPrim = 1;

        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pfnGetAsBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                           &tlasInfo, &tlasPrim, &tlasSizes);

        Buf tlasBuf = CreateBuffer(dev, gpu, tlasSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, true);
        VkAccelerationStructureCreateInfoKHR tlasAsci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        tlasAsci.buffer = tlasBuf.buffer;
        tlasAsci.size = tlasSizes.accelerationStructureSize;
        tlasAsci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VkAccelerationStructureKHR tlasAs = VK_NULL_HANDLE;
        pfnCreateAs(dev, &tlasAsci, nullptr, &tlasAs);

        VkDeviceSize scratchSize = blasSizes.buildScratchSize > tlasSizes.buildScratchSize
                                 ? blasSizes.buildScratchSize : tlasSizes.buildScratchSize;
        Buf scratch = CreateBuffer(dev, gpu, scratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, true);

        tlasInfo.dstAccelerationStructure = tlasAs;
        tlasInfo.scratchData.deviceAddress = scratch.address;

        // ── 命令缓冲：建 AS → 描述符 → 屏障 → traceRays → 屏障 → 回读 ──
        VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.queueFamilyIndex = qfIndex;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        vkCreateCommandPool(dev, &cpci, nullptr, &cmdPool);

        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(dev, &cbai, &cb);

        VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &cbbi);

        VkAccelerationStructureBuildRangeInfoKHR blasRange{ 1, 0, 0, 0 };
        VkAccelerationStructureBuildRangeInfoKHR tlasRange{ 1, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pBlasRange = &blasRange;
        const VkAccelerationStructureBuildRangeInfoKHR* pTlasRange = &tlasRange;

        // host 写（顶点/索引/实例数据在 HOST_COHERENT 内存里）→ AS 构建读
        VkMemoryBarrier hostToAs{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        hostToAs.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostToAs.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                               | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                             | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 1, &hostToAs, 0, nullptr, 0, nullptr);

        pfnCmdBuildAs(cb, 1, &blasInfo, &pBlasRange);

        // BLAS 写完 → TLAS 构建读（TLAS 引用的就是刚建的 BLAS，缺这个屏障会读到未完成结构）
        {
            VkMemoryBarrier blasToTlas{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            blasToTlas.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            blasToTlas.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cb,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 0, 1, &blasToTlas, 0, nullptr, 0, nullptr);
        }

        pfnCmdBuildAs(cb, 1, &tlasInfo, &pTlasRange);

        VkMemoryBarrier asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        asBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        asBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                             | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 1, &asBarrier, 0, nullptr, 0, nullptr);

        // 5 张输出图 → GENERAL
        VkImageMemoryBarrier imgB[5]{};
        Img* outs[5] = { &outImg, &gbuf0, &gbuf1, &albDiff, &albSpec };
        for (int i = 0; i < 5; i++)
        {
            imgB[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            imgB[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imgB[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgB[i].srcQueueFamilyIndex = imgB[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            imgB[i].image = outs[i]->image;
            imgB[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            imgB[i].srcAccessMask = 0;
            imgB[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 0, nullptr, 0, nullptr, 5, imgB);

        // 纹理数组 + env → SHADER_READ_ONLY
        VkImageMemoryBarrier texB[5]{};
        Img* tximgs[5] = { &tex0, &tex1, &tex2, &tex3, &envImg };
        for (int i = 0; i < 5; i++)
        {
            texB[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            texB[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            texB[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            texB[i].srcQueueFamilyIndex = texB[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            texB[i].image = tximgs[i]->image;
            texB[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            texB[i].srcAccessMask = 0;
            texB[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 0, nullptr, 0, nullptr, 5, texB);

        // ── 描述符 ──
        VkDescriptorPoolSize poolSizes[5] = {
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 },
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = 1;
        dpci.poolSizeCount = 5;
        dpci.pPoolSizes = poolSizes;
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(dev, &dpci, nullptr, &descPool);

        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        res = vkAllocateDescriptorSets(dev, &dsai, &ds);
        Check(res == VK_SUCCESS, "vkAllocateDescriptorSets");

        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &tlasAs;

        VkDescriptorImageInfo imgInfos[5];
        for (int i = 0; i < 5; i++)
        {
            imgInfos[i] = { VK_NULL_HANDLE, outs[i]->view, VK_IMAGE_LAYOUT_GENERAL };
        }
        VkDescriptorBufferInfo uboInfo{ ubo.buffer, 0, sizeof(CameraUBO) };
        VkBuffer ssboBufs[5] = { vb.buffer, ib.buffer, instBuf.buffer, matBuf.buffer, lightBuf.buffer };
        VkDescriptorBufferInfo ssboInfos[5];
        for (int i = 0; i < 5; i++) ssboInfos[i] = { ssboBufs[i], 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo texInfos[4];
        for (int i = 0; i < 4; i++) texInfos[i] = { sampler, tximgs[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo envInfo{ sampler, envImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo envAccelInfo{ envAccelBuf.buffer, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[18] = {};
        for (int i = 0; i < 18; i++)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = ds;
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
        }
        writes[0].pNext = &asWrite;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        for (int i = 0; i < 5; i++)
        {
            writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1 + i].pImageInfo = &imgInfos[i];
        }
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &uboInfo;
        for (int i = 0; i < 5; i++)
        {
            writes[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[5 + i].pBufferInfo = &ssboInfos[i];
        }
        for (int i = 0; i < 4; i++)
        {
            writes[10 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[10 + i].pImageInfo = &texInfos[i];
        }
        writes[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[14].pImageInfo = &imgInfos[3];
        writes[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[15].pImageInfo = &imgInfos[4];
        writes[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[16].pImageInfo = &envInfo;
        writes[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[17].pBufferInfo = &envAccelInfo;
        vkUpdateDescriptorSets(dev, 18, writes, 0, nullptr);

        // ── traceRays ──
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeLayout, 0, 1, &ds, 0, nullptr);
        pfnTraceRays(cb, &raygenRegion, &missRegion, &hitRegion, &callableRegion, W, H, 1);

        for (int i = 0; i < 5; i++)
        {
            imgB[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgB[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            imgB[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            imgB[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 5, imgB);

        // 回读 outputImg（rgba16f）+ gbufferPosDepth（rgba32f，用来判断到底有没有命中）
        const VkDeviceSize outBytes  = W * H * 8;
        const VkDeviceSize gbuf0Bytes = W * H * 16;
        Buf readback = CreateBuffer(dev, gpu, outBytes + gbuf0Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { W, H, 1 };
        vkCmdCopyImageToBuffer(cb, outImg.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);

        imgB[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgB[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imgB[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imgB[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &imgB[1]);
        VkBufferImageCopy region2{};
        region2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region2.imageExtent = { W, H, 1 };
        region2.bufferOffset = outBytes;
        vkCmdCopyImageToBuffer(cb, gbuf0.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region2);

        VkMemoryBarrier finalBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        finalBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &finalBarrier, 0, nullptr, 0, nullptr);

        res = vkEndCommandBuffer(cb);
        Check(res == VK_SUCCESS, "record command buffer (build AS + traceRays + readback)");

        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        res = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        Check(res == VK_SUCCESS, "vkQueueSubmit");
        res = vkQueueWaitIdle(queue);
        Check(res == VK_SUCCESS, "vkQueueWaitIdle");

        // half → float
        auto halfToFloat = [](uint16_t h) -> float {
            uint32_t sign = (h >> 15) & 0x1u;
            uint32_t exp  = (h >> 10) & 0x1Fu;
            uint32_t mant = h & 0x3FFu;
            float v;
            if (exp == 0)
                v = (mant == 0) ? 0.0f : std::ldexp((float)mant, -24);
            else if (exp == 31)
                v = (mant == 0) ? INFINITY : NAN;
            else
                v = std::ldexp((float)(mant | 0x400u), (int)exp - 25);
            return sign ? -v : v;
        };

        const uint16_t* px = static_cast<const uint16_t*>(readback.mapped);
        bool anyNonFinite = false, anyNonZero = false;
        float sum = 0.0f;
        for (uint32_t i = 0; i < W * H; i++)
        {
            float r = halfToFloat(px[i * 4 + 0]);
            float g = halfToFloat(px[i * 4 + 1]);
            float b = halfToFloat(px[i * 4 + 2]);
            if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) anyNonFinite = true;
            if (std::fabs(r) + std::fabs(g) + std::fabs(b) > 1e-6f) anyNonZero = true;
            sum += r + g + b;
        }

        printf("  output[0..3] = (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)\n",
               halfToFloat(px[0]), halfToFloat(px[1]), halfToFloat(px[2]),
               halfToFloat(px[4]), halfToFloat(px[5]), halfToFloat(px[6]),
               halfToFloat(px[8]), halfToFloat(px[9]), halfToFloat(px[10]),
               halfToFloat(px[12]), halfToFloat(px[13]), halfToFloat(px[14]));
        printf("  mean radiance (rgb) = %.5f\n", sum / (float)(W * H * 3));

        // gbuf0 = (worldPos, hitT)：全零说明所有像素都 miss（没有命中几何）
        {
            const float* g0 = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(readback.mapped) + outBytes);
            bool anyHitPixel = false;
            for (uint32_t i = 0; i < W * H; i++)
                if (g0[i * 4 + 3] > 0.0f) { anyHitPixel = true; break; }
            printf("  gbufferPosDepth[0] = (%.3f %.3f %.3f, hitT=%.3f)  anyHit=%d\n",
                   g0[0], g0[1], g0[2], g0[3], (int)anyHitPixel);
            Check(anyHitPixel, "at least one pixel recorded a hit in gbufferPosDepth");
        }

        Check(!anyNonFinite, "radiance is finite (no NaN/Inf)");
        Check(anyNonZero, "radiance is not all-zero (rays hit the test triangle)");

        // 命中测试：三角形只覆盖画面中间一块，四角本来就 miss（= ambient）。
        // 判据是「不是所有像素都等于 ambient」；若 viewInv 被当成列主序（转置），
        // 光线会朝 -X 全部 miss → 整张图恰好是 ambient(0.2,0.4,0.6)。
        {
            float r0 = halfToFloat(px[0]), g0 = halfToFloat(px[1]), b0 = halfToFloat(px[2]);
            (void)r0; (void)g0; (void)b0;
            uint32_t ambientPixels = 0;
            for (uint32_t i = 0; i < W * H; i++)
            {
                float r = halfToFloat(px[i * 4 + 0]);
                float g = halfToFloat(px[i * 4 + 1]);
                float b = halfToFloat(px[i * 4 + 2]);
                if (std::fabs(r - 0.2f) < 1e-3f && std::fabs(g - 0.4f) < 1e-3f && std::fabs(b - 0.6f) < 1e-3f)
                    ambientPixels++;
            }
            printf("  pixels equal to ambient: %u / %u (miss 比例)\n", ambientPixels, W * H);
            Check(ambientPixels < W * H,
                  "UBO 矩阵方向正确（存在非 ambient 像素：mul(row_major M, v) 未被转置）");
        }

        vkDestroyBuffer(dev, readback.buffer, nullptr);
        vkFreeMemory(dev, readback.memory, nullptr);
    }

    // ═══════════ 9. 汇总 ═══════════
    printf("\n[9] Summary\n");
    printf("  validation errors: %u\n", g_ValidationErrors);
    if (g_ValidationErrors > 0) g_Failures++;
    printf("  failures: %d\n", g_Failures);
    printf("%s\n", g_Failures == 0 ? "=== PASS ===" : "=== FAIL ===");
    return g_Failures == 0 ? 0 : 1;
}
