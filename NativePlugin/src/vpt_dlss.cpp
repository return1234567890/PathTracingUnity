// VulkanPathTracePlugin — NGX DLSS Super Sampling + Ray Reconstruction 集成
//
// 职责：
//   1. NVSDK_NGX_VULKAN_Init 初始化 NGX（设备就绪后）
//   2. 创建/销毁 DLSS feature：
//      - SR 模式: NVSDK_NGX_Feature_SuperSampling（超分）
//      - RR 模式: NVSDK_NGX_Feature_RayReconstruction（降噪+超分一体化）
//   3. Phase 1: 包装 Unity RT 为 NVSDK_NGX_Resource_VK + 填充评估参数
//   4. Phase 2: 通过 CommandRecordingState 获取 Unity VkCommandBuffer，
//               调用 NGX Evaluate 录入命令流
//
// 架构复用 Plan B 两阶段 dispatch 模式（消除 vkQueueWaitIdle）

#include "vpt_dlss.h"
#include "vpt_internal.h"
#include "vpt_dispatch.h"  // EVENT_PATHTRACE

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include <cstdio>
#include <cstring>
#include <windows.h>

#ifdef VPT_DLSS_ENABLED

// ── NGX Core path with matching ApplicationId ──
// _d.lib thunks into NGX Core (_nvngx.dll from the driver). Core loads the
// RR snippet (nvngx_dlssd.dll) from PathListInfo paths and validates its
// embedded app id against the ApplicationId passed to Init - they must match.
// ApplicationId bound to the dev snippet DLL's embedded metadata.
// The dev nvngx_dlssd.dll (from the DLSS SDK package) binds app id 241534723,
// observed in nvngx.log when Core loaded it. NGX Core validates the app id
// against snippet metadata; must match exactly or RR creation is rejected.
static const unsigned long long VPT_DLSS_APP_ID = 241534723ULL;

// ════════════════════════════════════════════════════════════
//  NGX 全局状态
// ═══════════════════════════════════════════════════════════

static bool                 s_NGXInitialized = false;
static NVSDK_NGX_Handle*    s_DLSSHandle      = nullptr;
static NVSDK_NGX_Parameter* s_DLSSParams      = nullptr;
static bool                 s_DLSSReady       = false;
static int32_t              s_DLSSMode         = VPT_DLSS_MODE_SR; // 0=SR, 1=RR

// 缓存的 ImageView（当 Unity VkImage 变化时重建）
struct DLSSImageViewCache {
    VkImageView view  = VK_NULL_HANDLE;
    VkImage     image = VK_NULL_HANDLE;
    VkFormat    format = VK_FORMAT_UNDEFINED;
    uint32_t    width  = 0;
    uint32_t    height = 0;
};
static DLSSImageViewCache s_ColorView;   // 低分辨率颜色输入
static DLSSImageViewCache s_MotionView;  // 低分辨率运动矢量
static DLSSImageViewCache s_DepthView;   // 低分辨率深度（SR: NDC, RR: 线性）
static DLSSImageViewCache s_OutputView; // 全分辨率输出
static DLSSImageViewCache s_NormalRoughView; // RR: normal.xyz + roughness.w（GBuffer1）
static DLSSImageViewCache s_DiffuseAlbedoView;  // RR: diffuse albedo = albedo*(1-metallic)
static DLSSImageViewCache s_SpecularAlbedoView; // RR: specular albedo = EnvBRDFApprox(F0, roughness², NoV)

// RR 待评估参数（Phase 1 填充，Phase 2 消费）
static float s_RRViewMatrix[16] = {0};
static float s_RRProjMatrix[16] = {0};

// 待评估参数（Phase 1 填充，Phase 2 消费）
static float s_PendingJitterX = 0.0f;
static float s_PendingJitterY = 0.0f;
static int   s_PendingReset   = 0;
static uint32_t s_PendingRenderW = 0;
static uint32_t s_PendingRenderH = 0;
static uint32_t s_PendingOutputW = 0;
static uint32_t s_PendingOutputH = 0;
static bool   s_DlssDispatchPending = false;
// RR 模式标志（Phase 2 用此判断走 SR 还是 RR eval 路径）
static bool   s_PendingIsRR = false;

// ═══════════════════════════════════════════════════════════
//  Helper: Access Unity texture and ensure VkImageView
// ═══════════════════════════════════════════════════════════

static bool EnsureDLSSImageView(IUnityGraphicsVulkan* vulkan, VkDevice device,
                                void* nativeTex, DLSSImageViewCache& cache,
                                VkImageLayout layout, VkAccessFlags access,
                                const char* name)
{
    UnityVulkanImage vkImg{};
    bool ok = vulkan->AccessTexture(nativeTex, UnityVulkanWholeImage,
        layout,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        access,
        kUnityVulkanResourceAccess_ObserveOnly, &vkImg);
    if (!ok || vkImg.image == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT-DLSS] AccessTexture failed for %s\n", name);
        return false;
    }

    if (cache.image != vkImg.image)
    {
        if (cache.view)
            vkDestroyImageView(device, cache.view, nullptr);

        VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivCI.image    = vkImg.image;
        ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCI.format   = vkImg.format;
        ivCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCI.subresourceRange.baseMipLevel   = 0;
        ivCI.subresourceRange.levelCount     = 1;
        ivCI.subresourceRange.baseArrayLayer = 0;
        ivCI.subresourceRange.layerCount     = 1;

        VkResult res = vkCreateImageView(device, &ivCI, nullptr, &cache.view);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPT-DLSS] vkCreateImageView %s failed: %d\n", name, res);
            return false;
        }
        cache.image  = vkImg.image;
        cache.format = vkImg.format;
        cache.width  = vkImg.extent.width;
        cache.height = vkImg.extent.height;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
//  NGX 初始化
// ═══════════════════════════════════════════════════════════

int32_t VPT_DLSS_Init_Internal(
    int32_t renderW, int32_t renderH,
    int32_t outputW, int32_t outputH,
    int32_t qualityMode, int32_t mode)
{
    if (s_DLSSReady)
    {
        // 尺寸/模式变化时销毁重建
        VPT_DLSS_Destroy_Internal();
    }

    if (!VPT_IsInitialized())
    {
        fprintf(stderr, "[VPT-DLSS] Plugin not initialized.\n");
        return -1;
    }

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    const UnityVulkanInstance& inst = VPT_GetInstance();
    if (!vulkan || inst.device == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT-DLSS] Vulkan device not available.\n");
        return -1;
    }

    s_DLSSMode = mode;

    // NGX data path + feature DLL 搜索路径：使用插件 DLL 自身所在目录（Assets/Plugins/x86_64/）
    // （函数级作用域：Init 块与下方扩展诊断块共用）
    wchar_t dllPath[MAX_PATH] = {0};
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&VPT_DLSS_Init_Internal, &hMod);
    GetModuleFileNameW(hMod, dllPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    const wchar_t* dataPath = (lastSlash) ? dllPath : L"./";

    // ── 1. NGX Vulkan Init（仅一次） ──
    if (!s_NGXInitialized)
    {
        fprintf(stderr, "[VPT-DLSS] NGX data path: %S\n", dataPath);
        fprintf(stderr, "[VPT-DLSS] VkInstance=%p VkPhysicalDevice=%p VkDevice=%p\n",
                (void*)inst.instance, (void*)inst.physicalDevice, (void*)inst.device);

        // ── GDPA 透传诊断：NGX 通过 GIPA/GDPA 获取 NVX 扩展函数 ──
        // （vkCreateCuModuleNVX/vkCreateCuFunctionNVX 是 CUDA 二进制导入，
        // transformer 模型（dlssd）运行的核心 API）。Unity 拦截层若不透传
        // 这些函数，RR 创建时拿不到指针即失败，而 SR（CNN）不受影响。
        // A/B 对照：Unity 链 vs 真实 vulkan-1.dll ICD 链。
        {
            PFN_vkGetInstanceProcAddr gipaU = inst.getInstanceProcAddr;
            PFN_vkGetDeviceProcAddr  gdpaU = (PFN_vkGetDeviceProcAddr)gipaU(inst.instance, "vkGetDeviceProcAddr");
            fprintf(stderr, "[VPT-DLSS] Unity chain: GIPA=%p GDPA=%p\n", (void*)gipaU, (void*)gdpaU);

            const char* probeNames[] = {
                "vkCreateCuModuleNVX", "vkCreateCuFunctionNVX",
                "vkGetImageViewHandleNVX", "vkGetImageViewHandle64NVX",
                "vkCmdPushDescriptorSetKHR", "vkCmdPushDescriptorSetWithTemplateKHR",
            };
            for (const char* pn : probeNames)
            {
                void* viaUnity = gdpaU ? (void*)gdpaU(inst.device, pn) : nullptr;
                fprintf(stderr, "[VPT-DLSS] Unity GDPA %-36s -> %p\n", pn, viaUnity);
            }

            // 对照组：真实 ICD loader（vulkan-1.dll）的 GIPA/GDPA
            HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
            if (!hVulkan) hVulkan = LoadLibraryW(L"vulkan-1.dll");
            if (hVulkan)
            {
                auto realGIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(hVulkan, "vkGetInstanceProcAddr");
                if (realGIPA)
                {
                    auto realGDPA = (PFN_vkGetDeviceProcAddr)realGIPA(inst.instance, "vkGetDeviceProcAddr");
                    fprintf(stderr, "[VPT-DLSS] Real chain: GIPA=%p GDPA=%p\n", (void*)realGIPA, (void*)realGDPA);
                    for (const char* pn : probeNames)
                    {
                        void* viaReal = realGDPA ? (void*)realGDPA(inst.device, pn) : nullptr;
                        fprintf(stderr, "[VPT-DLSS] Real  GDPA %-36s -> %p\n", pn, viaReal);
                    }
                }
            }
            else
            {
                fprintf(stderr, "[VPT-DLSS] vulkan-1.dll not loadable for A/B probe\n");
            }
        }

        // NGX Core path: 9-param Init with ApplicationId matching the snippet
        // DLL's embedded metadata (dev nvngx_dlssd.dll binds 241534723, read
        // from nvngx.log). Core locates the RR snippet via PathListInfo and
        // validates the app id when loading it.
        PFN_vkGetInstanceProcAddr gipa = inst.getInstanceProcAddr;
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst.instance, "vkGetDeviceProcAddr");
        fprintf(stderr, "[VPT-DLSS] GIPA=%p GDPA=%p\n", (void*)gipa, (void*)gdpa);

        const wchar_t* pathList[1] = { dataPath };
        NVSDK_NGX_FeatureCommonInfo featureInfo{};
        featureInfo.PathListInfo.Path = pathList;
        featureInfo.PathListInfo.Length = 1;
        featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;

        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init(
            VPT_DLSS_APP_ID, dataPath,
            inst.instance, inst.physicalDevice, inst.device,
            gipa, gdpa, &featureInfo,
            NVSDK_NGX_Version_API);
        fprintf(stderr, "[VPT-DLSS] NVSDK_NGX_VULKAN_Init (appId=%llu) result: 0x%x\n",
                (unsigned long long)VPT_DLSS_APP_ID, result);
        if (result != NVSDK_NGX_Result_Success)
        {
            fprintf(stderr, "[VPT-DLSS] NVSDK_NGX_VULKAN_Init failed.\n");
            return -1;
        }
        s_NGXInitialized = true;
        fprintf(stderr, "[VPT-DLSS] NGX Vulkan Init succeeded.\n");
    }

    // ── 1.5 查询 NGX 能力 ──
    // SR 查询 SuperSampling_Available；RR 查询 SuperSamplingDenoising_Available
    {
        // ── 扩展需求诊断：查询 RR/SR 需要的 Vulkan 设备/instance 扩展 ──
        // CreateFeature 参数矩阵已全部排除（12 组全 0xbad00005），
        // 新假设：Unity VkDevice 未启用 RR 所需的设备扩展。
        {
            const wchar_t* pathListDiag[1] = { dataPath };
            NVSDK_NGX_FeatureCommonInfo featureInfoDiag{};
            featureInfoDiag.PathListInfo.Path = pathListDiag;
            featureInfoDiag.PathListInfo.Length = 1;

            struct FeatDiag { NVSDK_NGX_Feature id; const char* name; };
            const FeatDiag featDiags[] = {
                { NVSDK_NGX_Feature_RayReconstruction, "RR" },
                { NVSDK_NGX_Feature_SuperSampling,     "SR" },
            };
            for (const FeatDiag& fd : featDiags)
            {
                NVSDK_NGX_FeatureDiscoveryInfo discoveryInfo{};
                discoveryInfo.SDKVersion = NVSDK_NGX_Version_API;
                discoveryInfo.FeatureID  = fd.id;
                discoveryInfo.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
                discoveryInfo.Identifier.v.ApplicationId = VPT_DLSS_APP_ID;
                discoveryInfo.ApplicationDataPath = dataPath;
                discoveryInfo.FeatureInfo = &featureInfoDiag;

                uint32_t devExtCount = 0;
                VkExtensionProperties* devExts = nullptr;
                NVSDK_NGX_Result extRes = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                    inst.instance, inst.physicalDevice, &discoveryInfo,
                    &devExtCount, &devExts);
                fprintf(stderr, "[VPT-DLSS] %s device ext requirements: 0x%x, count=%u\n", fd.name, extRes, devExtCount);
                if (extRes == NVSDK_NGX_Result_Success && devExts)
                {
                    for (uint32_t e = 0; e < devExtCount; e++)
                        fprintf(stderr, "[VPT-DLSS] %s   needs device ext: %s (spec %u.%u)\n",
                                fd.name, devExts[e].extensionName,
                                VK_VERSION_MAJOR(devExts[e].specVersion),
                                VK_VERSION_MINOR(devExts[e].specVersion));
                }

                uint32_t instExtCount = 0;
                VkExtensionProperties* instExts = nullptr;
                NVSDK_NGX_Result iRes = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
                    &discoveryInfo, &instExtCount, &instExts);
                fprintf(stderr, "[VPT-DLSS] %s instance ext requirements: 0x%x, count=%u\n", fd.name, iRes, instExtCount);
                if (iRes == NVSDK_NGX_Result_Success && instExts)
                {
                    for (uint32_t e = 0; e < instExtCount; e++)
                        fprintf(stderr, "[VPT-DLSS] %s   needs instance ext: %s\n",
                                fd.name, instExts[e].extensionName);
                }
            }
        }

        NVSDK_NGX_Parameter* capParams = nullptr;
        NVSDK_NGX_Result capResult = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capParams);
        fprintf(stderr, "[VPT-DLSS] GetCapabilityParameters: 0x%x, params=%p\n", capResult, (void*)capParams);
        if (capResult == NVSDK_NGX_Result_Success && capParams)
        {
            int srAvailable = 0;
            NVSDK_NGX_Parameter_GetI(capParams, NVSDK_NGX_Parameter_SuperSampling_Available, &srAvailable);
            fprintf(stderr, "[VPT-DLSS] DLSS SR available: %d\n", srAvailable);

            if (mode == VPT_DLSS_MODE_RR)
            {
                int rrAvailable = 0;
                NVSDK_NGX_Parameter_GetI(capParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvailable);
                fprintf(stderr, "[VPT-DLSS] DLSS RR available: %d\n", rrAvailable);

                // RR 诊断：查询驱动版本和 FeatureInitResult
                int needsUpdate = 0;
                NVSDK_NGX_Parameter_GetI(capParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsUpdate);
                int minMajor = 0, minMinor = 0;
                NVSDK_NGX_Parameter_GetI(capParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &minMajor);
                NVSDK_NGX_Parameter_GetI(capParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minMinor);
                int featInitResult = 0;
                NVSDK_NGX_Parameter_GetI(capParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &featInitResult);
                fprintf(stderr, "[VPT-DLSS] RR NeedsUpdatedDriver=%d, MinDriver=%d.%d, FeatureInitResult=0x%x\n",
                        needsUpdate, minMajor, minMinor, featInitResult);

                if (!rrAvailable)
                {
                    fprintf(stderr, "[VPT-DLSS] Ray Reconstruction not supported on this GPU/driver.\n");
                    NVSDK_NGX_VULKAN_DestroyParameters(capParams);
                    return -2; // RR 不支持，调用方可降级到 SR
                }
            }

            // ── RR/SR 尺寸范围诊断：直接提取 capability params 中的
            //    OptimalSettingsCallback 调用（helper 版会引入 D3D 头依赖）。
            //    callback 只存在于 GetCapabilityParameters 返回的 params 中。
            //    返回 snippet 推荐的渲染尺寸与动态 min/max 范围，
            //    用于验证 CreateFeature 传入的 renderW/H 是否落在支持范围内。
            {
                NVSDK_NGX_PerfQuality_Value diagPQ = NVSDK_NGX_PerfQuality_Value_Balanced;
                switch (qualityMode)
                {
                    case 0: diagPQ = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
                    case 1: diagPQ = NVSDK_NGX_PerfQuality_Value_Balanced; break;
                    case 2: diagPQ = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
                    case 3: diagPQ = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
                }

                struct CbDiag { const char* param; const char* name; };
                const CbDiag cbDiags[] = {
                    { NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, "RR(dlssd)" },
                    { NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback,  "SR(dlss)"  },
                };
                for (const CbDiag& d : cbDiags)
                {
                    void* cbPtr = nullptr;
                    NVSDK_NGX_Parameter_GetVoidPointer(capParams, d.param, &cbPtr);
                    if (!cbPtr)
                    {
                        fprintf(stderr, "[VPT-DLSS] %s OptimalSettings callback: (null)\n", d.name);
                        continue;
                    }
                    NVSDK_NGX_Parameter_SetUI(capParams, NVSDK_NGX_Parameter_Width, (unsigned)outputW);
                    NVSDK_NGX_Parameter_SetUI(capParams, NVSDK_NGX_Parameter_Height, (unsigned)outputH);
                    NVSDK_NGX_Parameter_SetI(capParams, NVSDK_NGX_Parameter_PerfQualityValue, (int)diagPQ);
                    NVSDK_NGX_Parameter_SetI(capParams, NVSDK_NGX_Parameter_RTXValue, 0);
                    PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback cb =
                        (PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)cbPtr;
                    NVSDK_NGX_Result optRes = cb(capParams);
                    unsigned int optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
                    float sharpness = 0.f;
                    NVSDK_NGX_Parameter_GetUI(capParams, NVSDK_NGX_Parameter_OutWidth, &optW);
                    NVSDK_NGX_Parameter_GetUI(capParams, NVSDK_NGX_Parameter_OutHeight, &optH);
                    NVSDK_NGX_Parameter_GetUI(capParams, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &maxW);
                    NVSDK_NGX_Parameter_GetUI(capParams, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &maxH);
                    NVSDK_NGX_Parameter_GetUI(capParams, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &minW);
                    NVSDK_NGX_Parameter_GetUI(capParams, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &minH);
                    NVSDK_NGX_Parameter_GetF(capParams, NVSDK_NGX_Parameter_Sharpness, &sharpness);
                    fprintf(stderr, "[VPT-DLSS] %s OptimalSettings (target=%dx%d PQ=%d): 0x%x optimal=%ux%u dynamic=[%ux%u..%ux%u] sharpness=%.2f\n",
                            d.name, outputW, outputH, (int)diagPQ, optRes,
                            optW, optH, minW, minH, maxW, maxH, sharpness);
                }
            }
            NVSDK_NGX_VULKAN_DestroyParameters(capParams);
        }
    }

    // ── 2. 分配 NGX Parameter ──
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_AllocateParameters(&s_DLSSParams);
    fprintf(stderr, "[VPT-DLSS] AllocateParameters: 0x%x, params=%p\n", result, (void*)s_DLSSParams);
    if (result != NVSDK_NGX_Result_Success || !s_DLSSParams)
    {
        fprintf(stderr, "[VPT-DLSS] AllocateParameters failed.\n");
        return -1;
    }

    // ── 3. 映射 qualityMode ──
    fprintf(stderr, "[VPT-DLSS] CreateFeature params: render=%dx%d output=%dx%d quality=%d mode=%d\n",
            renderW, renderH, outputW, outputH, qualityMode, mode);
    // C# 约定: 0=Quality 1=Balanced 2=Performance 3=UltraPerf
    NVSDK_NGX_PerfQuality_Value perfQuality;
    switch (qualityMode)
    {
        case 0: perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
        case 1: perfQuality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
        case 2: perfQuality = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
        case 3: perfQuality = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
        default: perfQuality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
    }

    // 创建 one-shot command buffer 用于 feature 初始化
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET flag：RR 多组合重试时需多次 begin 同一 cmdBuf（begin 隐式重置）
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                   | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = inst.queueFamilyIndex;
    VkResult vr = vkCreateCommandPool(inst.device, &poolInfo, nullptr, &cmdPool);
    fprintf(stderr, "[VPT-DLSS] vkCreateCommandPool: %d, pool=%p\n", vr, (void*)cmdPool);
    if (vr != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT-DLSS] vkCreateCommandPool failed.\n");
        NVSDK_NGX_VULKAN_DestroyParameters(s_DLSSParams);
        s_DLSSParams = nullptr;
        return -1;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vr = vkAllocateCommandBuffers(inst.device, &allocInfo, &cmdBuf);
    fprintf(stderr, "[VPT-DLSS] vkAllocateCommandBuffers: %d, cmdBuf=%p\n", vr, (void*)cmdBuf);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = vkBeginCommandBuffer(cmdBuf, &beginInfo);
    fprintf(stderr, "[VPT-DLSS] vkBeginCommandBuffer: %d\n", vr);

    if (mode == VPT_DLSS_MODE_RR)
    {
        // ═══ RR: NVSDK_NGX_Feature_RayReconstruction ═══
        // 根因（nvngx_dlssd_310_7_0.log）：
        //   [NgxSwinDenoiser::CreateDldnInstance] Error: HDR Color required
        // RR 的 Preset_D transformer 模型（SwinDenoiser）强制要求 IsHDR flag，
        // 输入颜色须为线性 HDR（路径追踪 radiance 天然满足）；
        // SR 的 CNN 模型无此检查，故此前 SR 成功 / RR 全参数组合失败。
        //
        // DepthInverted 排除（边缘抖动修复 2026-09-07）：
        // 该 flag 语义是「深度缓冲为反向 z 排序（near 大 far 小）」，
        // 仅适用于 SR 的 reversed-Z NDC 深度。RR 用 Linear 深度
        // （LinearDepthOut = curClip.w，near 小 far 大，非反向），
        // 设置该 flag 会使 RR 深度一致性/disocclusion 检测方向颠倒，
        // 边缘深度突变处时域历史被错误取舍 → 模型边缘抖动。
        //
        // AutoExposure 排除（亮处抖动修复 2026-09-07）：
        // 官方参考 Linear 深度下 InFeatureCreateFlags = IsHDR | MVLowRes，
        // 不含 AutoExposure。该 flag 是从 SR 分支沿袭的——SR 输入是
        // 空域滤波后的干净图像，内部曝光估计帧间稳定；而 RR 输入是
        // 1spp 逐帧独立噪声（NEE 随机选灯 + Halton jitter），全图亮度
        // 统计逐帧波动 → AutoExposure 估计出的曝光逐帧波动（日志佐证：
        // dlssd 创建了 AutoExposure / PrevAutoExposureTexture_0/1 内部
        // 纹理）。曝光是全图乘性作用，亮区绝对波动远大于暗区 →
        // 「被光照到的像素抖、暗处不抖」。Streamline 官方文档亦注明
        // "DLSS-RR will ignore DLSS options sharpness and useAutoExposure"。
        // 移除后 evaluate 侧 InPreExposure/InExposureScale=1.0 即固定曝光。
        // 首选组合 = 0x03（与官方参考完全一致），0x43（旧首选）与
        // 0x4b（含 DepthInverted）降为备选对照。
        const int rrFlagCombos[] = {
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR
          | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,                    // 0x03 首选
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR
          | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
          | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure,               // 0x43 备选（旧首选）
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR
          | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
          | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
          | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted,              // 0x4b 末选对照
        };
        const int rrComboCount = (int)(sizeof(rrFlagCombos) / sizeof(rrFlagCombos[0]));

        result = NVSDK_NGX_Result_Fail;
        for (int fi = 0; fi < rrComboCount && result != NVSDK_NGX_Result_Success; fi++)
        {
            NVSDK_NGX_DLSSD_Create_Params rrParams{};
            rrParams.InWidth             = (unsigned)renderW;
            rrParams.InHeight            = (unsigned)renderH;
            rrParams.InTargetWidth       = (unsigned)outputW;
            rrParams.InTargetHeight      = (unsigned)outputH;
            rrParams.InPerfQualityValue  = perfQuality;
            rrParams.InFeatureCreateFlags = rrFlagCombos[fi];
            rrParams.InEnableOutputSubrects = false;
            rrParams.InDenoiseMode       = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            rrParams.InRoughnessMode     = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
            rrParams.InUseHWDepth        = NVSDK_NGX_DLSS_Depth_Type_Linear;

            fprintf(stderr, "[VPT-DLSS] RR create (flags=0x%x%s): W=%u H=%u TW=%u TH=%u PQ=%d\n",
                    rrParams.InFeatureCreateFlags,
                    (fi == 0) ? " refFlags(noAutoExposure)" :
                    (fi == 1) ? " autoExposure-fallback" : " autoExposure+depthInv-fallback",
                    rrParams.InWidth, rrParams.InHeight,
                    rrParams.InTargetWidth, rrParams.InTargetHeight, (int)perfQuality);

            s_DLSSHandle = nullptr;
            result = NGX_VULKAN_CREATE_DLSSD_EXT1(
                inst.device, cmdBuf,
                0, 0,
                &s_DLSSHandle, s_DLSSParams, &rrParams);
            fprintf(stderr, "[VPT-DLSS]   -> 0x%x, handle=%p\n", result, (void*)s_DLSSHandle);

            if (result != NVSDK_NGX_Result_Success)
            {
                // 结束本次记录并重新 begin（pool 带 RESET flag，begin 隐式重置）
                vkEndCommandBuffer(cmdBuf);
                vkBeginCommandBuffer(cmdBuf, &beginInfo);
            }
        }
    }
    else
    {
        // ═══ SR: NVSDK_NGX_Feature_SuperSampling ═══
        // DLSS flags: MVLowRes + AutoExposure + DepthInverted
        // MVLowRes: 运动矢量为低分辨率
        // AutoExposure: 不使用 exposure texture
        // DepthInverted: Unity 使用 reversed-Z（near=1, far=0），深度值近处更高
        // 不设 MVJittered：MV 用未 jitter 的投影矩阵计算，不含 jitter 偏移；
        // DLSS 会用 InJitterOffsetX/Y 内部校正 MV，与 jittered 渲染图像对齐。
        int createFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                        | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                        | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

        NVSDK_NGX_DLSS_Create_Params createParams{};
        createParams.Feature.InWidth           = (unsigned)renderW;
        createParams.Feature.InHeight          = (unsigned)renderH;
        createParams.Feature.InTargetWidth     = (unsigned)outputW;
        createParams.Feature.InTargetHeight    = (unsigned)outputH;
        createParams.Feature.InPerfQualityValue = perfQuality;
        createParams.InFeatureCreateFlags      = createFlags;
        createParams.InEnableOutputSubrects    = false;

        result = NGX_VULKAN_CREATE_DLSS_EXT1(
            inst.device, cmdBuf,
            0, 0,
            &s_DLSSHandle, s_DLSSParams, &createParams);
        fprintf(stderr, "[VPT-DLSS] NGX_VULKAN_CREATE_DLSS_EXT1 (SR): 0x%x, handle=%p\n", result, (void*)s_DLSSHandle);
    }

    vr = vkEndCommandBuffer(cmdBuf);
    fprintf(stderr, "[VPT-DLSS] vkEndCommandBuffer: %d\n", vr);

    if (result == NVSDK_NGX_Result_Success)
    {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(inst.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(inst.graphicsQueue);
    }

    vkFreeCommandBuffers(inst.device, cmdPool, 1, &cmdBuf);
    vkDestroyCommandPool(inst.device, cmdPool, nullptr);

    if (result != NVSDK_NGX_Result_Success)
    {
        fprintf(stderr, "[VPT-DLSS] CreateFeature failed: 0x%x\n", result);
        NVSDK_NGX_VULKAN_DestroyParameters(s_DLSSParams);
        s_DLSSParams = nullptr;
        return -1;
    }

    s_DLSSReady = true;
    fprintf(stderr, "[VPT-DLSS] DLSS feature created (%s): %dx%d -> %dx%d (quality=%d)\n",
            mode == VPT_DLSS_MODE_RR ? "RR" : "SR",
            renderW, renderH, outputW, outputH, qualityMode);
    return 0;
}

// ═══════════════════════════════════════════════════════════
//  DLSS feature 销毁
// ═══════════════════════════════════════════════════════════

void VPT_DLSS_Destroy_Internal()
{
    VkDevice device = VPT_GetInstance().device;

    if (s_DLSSHandle)
    {
        NVSDK_NGX_VULKAN_ReleaseFeature(s_DLSSHandle);
        s_DLSSHandle = nullptr;
    }

    // 释放 AllocateParameters 分配的参数映射
    if (s_DLSSParams)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(s_DLSSParams);
        s_DLSSParams = nullptr;
    }

    // 清理缓存的 ImageView
    auto cleanupView = [&](DLSSImageViewCache& c) {
        if (c.view && device != VK_NULL_HANDLE)
            vkDestroyImageView(device, c.view, nullptr);
        c.view = VK_NULL_HANDLE;
        c.image = VK_NULL_HANDLE;
    };
    cleanupView(s_ColorView);
    cleanupView(s_MotionView);
    cleanupView(s_DepthView);
    cleanupView(s_OutputView);
    cleanupView(s_NormalRoughView);
    cleanupView(s_DiffuseAlbedoView);
    cleanupView(s_SpecularAlbedoView);

    s_DLSSReady = false;
    s_DLSSParams = nullptr;

    // Shutdown NGX Core (device-level)
    if (s_NGXInitialized)
    {
        NVSDK_NGX_VULKAN_Shutdown1(device);
        s_NGXInitialized = false;
    }

    fprintf(stderr, "[VPT-DLSS] DLSS feature destroyed.\n");
}

// ═══════════════════════════════════════════════════════════
//  Phase 1: 准备 DLSS Evaluate dispatch
// ═══════════════════════════════════════════════════════════

void VPT_DLSS_PrepareDispatch_Internal(
    void* colorLowPtr, void* motionLowPtr, void* depthLowPtr, void* outputHighPtr,
    int32_t renderW, int32_t renderH,
    int32_t outputW, int32_t outputH,
    float jitterX, float jitterY, int32_t reset)
{
    if (!s_DLSSReady || !colorLowPtr || !motionLowPtr || !depthLowPtr || !outputHighPtr)
        return;

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    VkDevice device = VPT_GetInstance().device;

    // ── Access Unity textures and ensure ImageViews ──
    // Color: read-only, Shader Read Only layout
    if (!EnsureDLSSImageView(vulkan, device, colorLowPtr, s_ColorView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "Color")) return;

    // Motion: read-only
    if (!EnsureDLSSImageView(vulkan, device, motionLowPtr, s_MotionView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "Motion")) return;

    // Depth: read-only
    if (!EnsureDLSSImageView(vulkan, device, depthLowPtr, s_DepthView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "Depth")) return;

    // Output: write-only, General layout
    if (!EnsureDLSSImageView(vulkan, device, outputHighPtr, s_OutputView,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT, "Output")) return;

    // ── Store pending params for Phase 2 ──
    s_PendingJitterX = jitterX;
    s_PendingJitterY = jitterY;
    s_PendingReset    = reset;
    s_PendingRenderW  = (uint32_t)renderW;
    s_PendingRenderH  = (uint32_t)renderH;
    s_PendingOutputW  = (uint32_t)outputW;
    s_PendingOutputH  = (uint32_t)outputH;
    s_PendingIsRR     = false;
    s_DlssDispatchPending = true;
}

// ═══════════════════════════════════════════════════════════
//  Phase 1: 准备 DLSS RR Evaluate dispatch（RR 模式）
//  额外绑定 Normal+Roughness (GBuffer1)、LinearDepth、相机矩阵
// ═══════════════════════════════════════════════════════════

void VPT_DLSS_PrepareRRDispatch_Internal(
    void* colorLowPtr, void* motionLowPtr, void* linearDepthPtr, void* outputHighPtr,
    void* normalRoughPtr,
    void* diffuseAlbedoPtr, void* specularAlbedoPtr,
    int32_t renderW, int32_t renderH,
    int32_t outputW, int32_t outputH,
    float jitterX, float jitterY, int32_t reset,
    const float* viewMatrix, const float* projMatrix)
{
    if (!s_DLSSReady || !colorLowPtr || !motionLowPtr || !linearDepthPtr || !outputHighPtr || !normalRoughPtr
        || !diffuseAlbedoPtr || !specularAlbedoPtr)
        return;

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    VkDevice device = VPT_GetInstance().device;

    // ── Access Unity textures and ensure ImageViews ──
    // Color: read-only
    if (!EnsureDLSSImageView(vulkan, device, colorLowPtr, s_ColorView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "Color")) return;

    // Motion: read-only
    if (!EnsureDLSSImageView(vulkan, device, motionLowPtr, s_MotionView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "Motion")) return;

    // LinearDepth: read-only (RR 使用线性深度，复用 s_DepthView 缓存槽)
    if (!EnsureDLSSImageView(vulkan, device, linearDepthPtr, s_DepthView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "LinearDepth")) return;

    // NormalRoughness: read-only (GBuffer1: normal.xyz + roughness.w)
    if (!EnsureDLSSImageView(vulkan, device, normalRoughPtr, s_NormalRoughView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "NormalRough")) return;

    // DiffuseAlbedo: read-only (RR 材质分解 G-buffer，RGBA16F)
    if (!EnsureDLSSImageView(vulkan, device, diffuseAlbedoPtr, s_DiffuseAlbedoView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "DiffuseAlbedo")) return;

    // SpecularAlbedo: read-only (RR 材质分解 G-buffer，RGBA16F)
    if (!EnsureDLSSImageView(vulkan, device, specularAlbedoPtr, s_SpecularAlbedoView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, "SpecularAlbedo")) return;

    // Output: write-only, General layout
    if (!EnsureDLSSImageView(vulkan, device, outputHighPtr, s_OutputView,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT, "Output")) return;

    // ── Store pending params for Phase 2 ──
    s_PendingJitterX = jitterX;
    s_PendingJitterY = jitterY;
    s_PendingReset    = reset;
    s_PendingRenderW  = (uint32_t)renderW;
    s_PendingRenderH  = (uint32_t)renderH;
    s_PendingOutputW  = (uint32_t)outputW;
    s_PendingOutputH  = (uint32_t)outputH;
    s_PendingIsRR     = true;
    // 拷贝矩阵（16 float，行主序，不含 jitter）
    if (viewMatrix) memcpy(s_RRViewMatrix, viewMatrix, sizeof(float) * 16);
    if (projMatrix) memcpy(s_RRProjMatrix, projMatrix, sizeof(float) * 16);
    s_DlssDispatchPending = true;
}

// ═══════════════════════════════════════════════════════════
//  Phase 2: DLSS Render Callback
//  通过 CommandRecordingState 获取 Unity VkCommandBuffer，
//  根据 s_PendingIsRR 走 SR (NGX_VULKAN_EVALUATE_DLSS_EXT)
//  或 RR (NGX_VULKAN_EVALUATE_DLSSD_EXT) 评估路径
// ═══════════════════════════════════════════════════════════

void VPT_DLSS_RenderCallback()
{
    if (!s_DlssDispatchPending || !s_DLSSReady)
        return;

    s_DlssDispatchPending = false;

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    if (!vulkan)
        return;

    UnityVulkanRecordingState recState{};
    if (!vulkan->CommandRecordingState(&recState, kUnityVulkanGraphicsQueueAccess_DontCare))
    {
        fprintf(stderr, "[VPT-DLSS] RenderCallback: CommandRecordingState failed\n");
        return;
    }

    VkCommandBuffer cmd = recState.commandBuffer;
    if (cmd == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT-DLSS] RenderCallback: Unity command buffer is null\n");
        return;
    }

    // ── 包装 NGX 资源（共享） ──
    VkImageSubresourceRange subRange{};
    subRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subRange.baseMipLevel   = 0;
    subRange.levelCount     = 1;
    subRange.baseArrayLayer = 0;
    subRange.layerCount     = 1;

    NVSDK_NGX_Resource_VK colorRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        s_ColorView.view, s_ColorView.image, subRange,
        s_ColorView.format, s_ColorView.width, s_ColorView.height, false);

    NVSDK_NGX_Resource_VK motionRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        s_MotionView.view, s_MotionView.image, subRange,
        s_MotionView.format, s_MotionView.width, s_MotionView.height, false);

    NVSDK_NGX_Resource_VK depthRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        s_DepthView.view, s_DepthView.image, subRange,
        s_DepthView.format, s_DepthView.width, s_DepthView.height, false);

    NVSDK_NGX_Resource_VK outputRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        s_OutputView.view, s_OutputView.image, subRange,
        s_OutputView.format, s_OutputView.width, s_OutputView.height, true);

    NVSDK_NGX_Result result;

    if (s_PendingIsRR)
    {
        // ═══ RR: NGX_VULKAN_EVALUATE_DLSSD_EXT ═══
        // RR 额外绑定 Normal+Roughness、Diffuse/Specular Albedo、WorldToView/ViewToClip 矩阵
        NVSDK_NGX_Resource_VK normalRoughRes = NVSDK_NGX_Create_ImageView_Resource_VK(
            s_NormalRoughView.view, s_NormalRoughView.image, subRange,
            s_NormalRoughView.format, s_NormalRoughView.width, s_NormalRoughView.height, false);

        // RR albedo G-buffer（主 kernel 首命中写入的材质分解）
        NVSDK_NGX_Resource_VK diffuseAlbedoRes = NVSDK_NGX_Create_ImageView_Resource_VK(
            s_DiffuseAlbedoView.view, s_DiffuseAlbedoView.image, subRange,
            s_DiffuseAlbedoView.format, s_DiffuseAlbedoView.width, s_DiffuseAlbedoView.height, false);

        NVSDK_NGX_Resource_VK specularAlbedoRes = NVSDK_NGX_Create_ImageView_Resource_VK(
            s_SpecularAlbedoView.view, s_SpecularAlbedoView.image, subRange,
            s_SpecularAlbedoView.format, s_SpecularAlbedoView.width, s_SpecularAlbedoView.height, false);

        NVSDK_NGX_VK_DLSSD_Eval_Params rrEval{};
        rrEval.pInColor       = &colorRes;
        rrEval.pInOutput      = &outputRes;
        rrEval.pInDepth       = &depthRes;
        rrEval.pInMotionVectors = &motionRes;
        rrEval.pInNormals     = &normalRoughRes;
        rrEval.pInRoughness   = &normalRoughRes; // Packed 模式：roughness 从 normals.w 读取
        // RR guide 3.4 节要求的材质分解输入：
        // Diffuse = albedo*(1-metallic)，Specular = EnvBRDFApprox(F0, roughness², NoV)
        rrEval.pInDiffuseAlbedo  = &diffuseAlbedoRes;
        rrEval.pInSpecularAlbedo = &specularAlbedoRes;
        // Jitter 取负（边缘抖动修复 2026-09-07）：
        // 渲染侧 pathtrace.glsl 的 jitter 应用方向为 uv = (pixel + 0.5 + jitter)/res，
        // 与 NVIDIA nvpro 官方参考（gltf_pathtrace.slang L786 subpixelJitter = jitter + 0.5）
        // 完全一致；而官方参考传 NGX 时取负（dlss_wrapper.cpp L390
        // InJitterOffsetX = -info.jitter.x）。此前 RR 沿用 SR 的 +jitter 传法，
        // transformer 模型的帧间亚像素对齐每帧偏差 2×jitter（随 Halton 相位变化）
        // → 模型边缘轻微抖动。SR 分支保持 +jitter 不动（CNN 模型对此不敏感，
        // 实测静止不抖，避免引入回归）。
        rrEval.InJitterOffsetX = -s_PendingJitterX;
        rrEval.InJitterOffsetY = -s_PendingJitterY;
        rrEval.InRenderSubrectDimensions.Width  = s_PendingRenderW;
        rrEval.InRenderSubrectDimensions.Height = s_PendingRenderH;
        rrEval.InReset         = s_PendingReset;
        // MV kernel 输出 UV 空间运动矢量，乘以渲染分辨率转为像素空间
        rrEval.InMVScaleX      = (float)s_PendingRenderW;
        rrEval.InMVScaleY      = (float)s_PendingRenderH;
        // 固定曝光（无 AutoExposure flag 时即生效，官方参考同此约定）
        rrEval.InPreExposure   = 1.0f;
        rrEval.InExposureScale = 1.0f;
        // RR 相机矩阵（不含 jitter，行主序 float[16]）
        rrEval.pInWorldToViewMatrix = s_RRViewMatrix;
        rrEval.pInViewToClipMatrix  = s_RRProjMatrix;

        result = NGX_VULKAN_EVALUATE_DLSSD_EXT(
            cmd, s_DLSSHandle, s_DLSSParams, &rrEval);

        if (result != NVSDK_NGX_Result_Success)
            fprintf(stderr, "[VPT-DLSS] RR EvaluateFeature failed: 0x%x\n", result);
    }
    else
    {
        // ═══ SR: NGX_VULKAN_EVALUATE_DLSS_EXT ═══
        NVSDK_NGX_VK_DLSS_Eval_Params evalParams{};
        evalParams.Feature.pInColor   = &colorRes;
        evalParams.Feature.pInOutput  = &outputRes;
        evalParams.Feature.InSharpness = 0.0f;
        evalParams.pInDepth            = &depthRes;
        evalParams.pInMotionVectors    = &motionRes;
        evalParams.InJitterOffsetX    = s_PendingJitterX;
        evalParams.InJitterOffsetY    = s_PendingJitterY;
        evalParams.InRenderSubrectDimensions.Width  = s_PendingRenderW;
        evalParams.InRenderSubrectDimensions.Height = s_PendingRenderH;
        evalParams.InReset             = s_PendingReset;
        // MV kernel 输出 UV 空间运动矢量 (prevUV - curUV)，DLSS 期望像素空间。
        // 乘以渲染分辨率将 UV → 像素偏移量。
        evalParams.InMVScaleX          = (float)s_PendingRenderW;
        evalParams.InMVScaleY          = (float)s_PendingRenderH;
        evalParams.InPreExposure       = 1.0f;
        evalParams.InExposureScale     = 1.0f;

        result = NGX_VULKAN_EVALUATE_DLSS_EXT(
            cmd, s_DLSSHandle, s_DLSSParams, &evalParams);

        if (result != NVSDK_NGX_Result_Success)
            fprintf(stderr, "[VPT-DLSS] SR EvaluateFeature failed: 0x%x\n", result);
    }
}

bool VPT_DLSS_IsReady()
{
    return s_DLSSReady;
}

#else // !VPT_DLSS_ENABLED

// ── Stub implementations when NGX SDK is not available ──

int32_t VPT_DLSS_Init_Internal(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)
{
    fprintf(stderr, "[VPT-DLSS] NGX SDK not linked. DLSS unavailable.\n");
    return -1;
}

void VPT_DLSS_Destroy_Internal() {}
void VPT_DLSS_PrepareDispatch_Internal(void*, void*, void*, void*, int32_t, int32_t, int32_t, int32_t, float, float, int32_t) {}
void VPT_DLSS_PrepareRRDispatch_Internal(void*, void*, void*, void*, void*, void*, void*, int32_t, int32_t, int32_t, int32_t, float, float, int32_t, const float*, const float*) {}
void VPT_DLSS_RenderCallback() {}
bool VPT_DLSS_IsReady() { return false; }

#endif // VPT_DLSS_ENABLED
