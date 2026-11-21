// VulkanPathTracePlugin — PathTrace dispatch
//
// 两阶段 dispatch（Plan B）：Phase 1 在 C# 侧准备描述符/UBO，
// Phase 2 通过 IssuePluginEvent 在 Unity 的 VkCommandBuffer 中录制
// vkCmdTraceRaysKHR，结果通过 imageStore 直接写入 Unity 的 RenderTexture（无 CPU 回读）。
//
// 后端：VK_KHR_ray_tracing_pipeline（raygen 迭代弹射 + CH/AH 只做命中记录 + SBT）。
// 旧的 ray query + compute dispatch 路径已整体切换（见 doc/RT_pipeline整体切换实现方案.md），
// 只保留 ray query 的扩展注入作为诊断信息。

#include "vpt_dispatch.h"
#include "vpt_internal.h"
#include "vpt_scene.h"
#include "vpt_env.h"
#include "vpt_dlss.h"
#include "vpt_gpu_timer.h"
#include "vpt_rt.h"
#include "vpt_trace.h"
#include "pathtrace_spv.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <atomic>

// ═══════════════════════════════════════════════════════════
//  PathTrace pipeline state (Phase 3) 与 ImageView 缓存
// ═══════════════════════════════════════════════════════════
static VkDescriptorSetLayout s_PathDSLayout   = VK_NULL_HANDLE;
static VkPipelineLayout      s_PathPipeLayout = VK_NULL_HANDLE;
static VkPipeline            s_PathPipeline   = VK_NULL_HANDLE;
static VkDescriptorPool      s_PathDescPool   = VK_NULL_HANDLE;
// 描述符集环形缓冲（BUG-002 / 验证层 VUID-vkUpdateDescriptorSets-None-03047）：
// 每帧更新「本帧这一套」，而命令缓冲里正在飞行（pending）的旧帧仍引用它自己那套。
// 只有一套 set 时，主线程改写 set 会命中"set 仍被 pending 命令缓冲使用"的规范违背。
static constexpr uint32_t kDescRingSize = 4;   // 覆盖 Unity 最多 ~3 帧在飞
static VkDescriptorSet s_PathDescSets[kDescRingSize] = {};
static uint32_t        s_DescRingCursor = 0;
static VkDescriptorSet s_PathDescSet    = VK_NULL_HANDLE;   // 本帧这一套（Prepare 写、Render 绑）
static VkBuffer      s_PathUBOBuffer  = VK_NULL_HANDLE;
static VkDeviceMemory s_PathUBOMemory  = VK_NULL_HANDLE;
static void*          s_PathUBOMapped  = nullptr;
static VPTRt::BindingTable s_PathSbt;          // 与 pipeline 同生共死
static bool s_PathReady = false;
// 创建失败后不再每帧重试（方案 P3.4）：失败是确定性的（扩展缺失/驱动拒绝链接），
// 每帧重试只会刷屏并浪费编译时间。
static bool s_PathInitFailed = false;
static char s_PathInitError[256] = { 0 };

// Cached ImageViews for 5 output textures (recreated when VkImage changes)
struct ImageViewCache {
    VkImageView   view   = VK_NULL_HANDLE;
    VkImage       image  = VK_NULL_HANDLE;
    // 该图在本插件看来当前所处的布局（Phase 1 从 AccessTexture 取回，
    // Phase 2 的 barrier 用它当 oldLayout；切到 SHADER_READ_ONLY 后同步更新）。
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
// [0]=output, [1]=gbuffer0, [2]=gbuffer1, [3]=diffuseAlbedo(RR), [4]=specularAlbedo(RR)
static ImageViewCache s_OutputViews[5];

// Cached ImageViews for 4 texture arrays (Phase 4)
static ImageViewCache s_TexViews[4]; // [0]=baseColor, [1]=metallicRough, [2]=normal, [3]=emissive
static VkSampler s_TexSampler = VK_NULL_HANDLE;

// ═══════════════════════════════════════════════════════════
//  Utility
// ═══════════════════════════════════════════════════════════

static uint32_t FindMemoryType(VkPhysicalDevice gpu, uint32_t typeFilter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

static VkResult CreateHostBuffer(
    VkDevice device, VkPhysicalDevice gpu,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkBuffer& outBuf, VkDeviceMemory& outMem, void** outMapped)
{
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = vkCreateBuffer(device, &bci, nullptr, &outBuf);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, outBuf, &memReq);

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    res = vkAllocateMemory(device, &mai, nullptr, &outMem);
    if (res != VK_SUCCESS) { vkDestroyBuffer(device, outBuf, nullptr); return res; }

    res = vkBindBufferMemory(device, outBuf, outMem, 0);
    if (res != VK_SUCCESS) return res;

    if (outMapped)
    {
        res = vkMapMemory(device, outMem, 0, size, 0, outMapped);
        if (res != VK_SUCCESS) return res;
    }
    return VK_SUCCESS;
}

// ImageView 重建统计（诊断用）：Unity 在某些情况下**每帧**都会换 VkImage 句柄
// （实测 Editor 下 5 张输出图每帧都变），于是每帧都要销毁/重建 view。
// 这本身不是错误（PT 每帧整图覆写，旧内容丢弃无害），但值得知道发生频率，
// 所以每 ~120 帧汇总打印一次，而不是每帧刷 5 行把 Editor.log 撑大。
static uint32_t s_ViewRecreateCount[5] = { 0, 0, 0, 0, 0 };
static uint32_t s_TexViewRecreateCount[4] = { 0, 0, 0, 0 };
static uint32_t s_ViewStatsFrame = 0;
static uint32_t s_ViewHotWindows = 0;   // 连续「每帧换图」的 120 帧窗口数（诊断用）

// ═══════════════════════════════════════════════════════════
//  PathTrace Camera UBO layout (matches GLSL std140 + row_major)
//  208 bytes: viewInv + projInv + packed vec4s + light count + spp
// ═══════════════════════════════════════════════════════════
struct PathTraceCameraUBO
{
    float    viewInv[16];        // 64B
    float    projInv[16];        // 64B
    float    posFov[4];          // 16B (xyz=position, w=fov)
    float    jitterNearFar[4];   // 16B (xy=jitter, z=near, w=far)
    int32_t  frameMaxDepth[4];   // 16B (x=frameCount, y=maxDepth)
    float    ambientPad[4];     // 16B (xyz=ambientColor, w=pad)
    uint32_t lightCount;         // 4B
    uint32_t samplesPerPixel;    // 4B
    float    envIntensity;       // 4B (HDR env intensity multiplier)
    float    envRotation;        // 4B (HDR env rotation in radians)
    // Total: 208B
};

// ═══════════════════════════════════════════════════════════
//  PathTrace pipeline initialization (Phase 3)
//  18 bindings: TLAS + 5 images + UBO + 6 SSBOs + 5 samplers + 1 env SSBO
//
//  后端 = VK_KHR_ray_tracing_pipeline：7 个 stage 共用一个 VkShaderModule
//  （Slang 单模块，靠 pName 选入口），5 个 shader group，SBT 见 vpt_rt.cpp。
// ═══════════════════════════════════════════════════════════

// 描述符对所有 RT 阶段可见（先用并集保证正确；方案 P3.2）
static const VkShaderStageFlags kRtDescriptorStages =
    VK_SHADER_STAGE_RAYGEN_BIT_KHR
  | VK_SHADER_STAGE_MISS_BIT_KHR
  | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
  | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

void VPTDispatch::InitPathTracePipeline(VkDevice device, VkPhysicalDevice gpu)
{
    if (s_PathReady || s_PathInitFailed) return;

    auto Fail = [&](const char* what, VkResult res) {
        s_PathInitFailed = true;
        snprintf(s_PathInitError, sizeof(s_PathInitError), "%s (VkResult=%d)", what, (int)res);
        fprintf(stderr, "[VPT] Path: %s failed: %d — 后续不再重试。\n", what, (int)res);
        fflush(stderr);
    };

    // 0. 能力 + 扩展函数（失败原因要打印得能直接定位）
    if (!VPT_IsRayTracingPipelineSupported())
    {
        s_PathInitFailed = true;
        snprintf(s_PathInitError, sizeof(s_PathInitError),
                 "VK_KHR_ray_tracing_pipeline unsupported");
        fprintf(stderr, "[VPT] Path: RT pipeline unsupported — 不创建管线。\n");
        return;
    }
    if (!VPTRt::AreFunctionsReady())
    {
        if (!VPTRt::LoadFunctions(VPT_GetInstance().instance, device))
        {
            s_PathInitFailed = true;
            snprintf(s_PathInitError, sizeof(s_PathInitError),
                     "RT extension function '%s' missing", VPTRt::MissingFunction());
            return;
        }
    }

    // 1. Descriptor set layout: 18 bindings (16 original + 2 HDR env)
    //    stageFlags = 全部 RT 阶段并集
    VkDescriptorSetLayoutBinding bindings[18] = {};
    bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; bindings[0].descriptorCount = 1; bindings[0].stageFlags = kRtDescriptorStages;
    bindings[1].binding = 1; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[1].descriptorCount = 1; bindings[1].stageFlags = kRtDescriptorStages;
    bindings[2].binding = 2; bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[2].descriptorCount = 1; bindings[2].stageFlags = kRtDescriptorStages;
    bindings[3].binding = 3; bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[3].descriptorCount = 1; bindings[3].stageFlags = kRtDescriptorStages;
    bindings[4].binding = 4; bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; bindings[4].descriptorCount = 1; bindings[4].stageFlags = kRtDescriptorStages;
    bindings[5].binding = 5; bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[5].descriptorCount = 1; bindings[5].stageFlags = kRtDescriptorStages;
    bindings[6].binding = 6; bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[6].descriptorCount = 1; bindings[6].stageFlags = kRtDescriptorStages;
    bindings[7].binding = 7; bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[7].descriptorCount = 1; bindings[7].stageFlags = kRtDescriptorStages;
    bindings[8].binding = 8; bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[8].descriptorCount = 1; bindings[8].stageFlags = kRtDescriptorStages;
    bindings[9].binding = 9; bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[9].descriptorCount = 1; bindings[9].stageFlags = kRtDescriptorStages;
    bindings[10].binding = 10; bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[10].descriptorCount = 1; bindings[10].stageFlags = kRtDescriptorStages;
    bindings[11].binding = 11; bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[11].descriptorCount = 1; bindings[11].stageFlags = kRtDescriptorStages;
    bindings[12].binding = 12; bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[12].descriptorCount = 1; bindings[12].stageFlags = kRtDescriptorStages;
    bindings[13].binding = 13; bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[13].descriptorCount = 1; bindings[13].stageFlags = kRtDescriptorStages;
    // DLSS RR albedo G-buffers（pt.slang binding 14/15，rgba16f）
    bindings[14].binding = 14; bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[14].descriptorCount = 1; bindings[14].stageFlags = kRtDescriptorStages;
    bindings[15].binding = 15; bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[15].descriptorCount = 1; bindings[15].stageFlags = kRtDescriptorStages;
    // HDR 环境贴图（binding 16 = combined image sampler, binding 17 = EnvAccel SSBO）
    bindings[16].binding = 16; bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[16].descriptorCount = 1; bindings[16].stageFlags = kRtDescriptorStages;
    bindings[17].binding = 17; bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[17].descriptorCount = 1; bindings[17].stageFlags = kRtDescriptorStages;

    VkDescriptorSetLayoutCreateInfo dslCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 18;
    dslCI.pBindings    = bindings;
    VkResult res = vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &s_PathDSLayout);
    if (res != VK_SUCCESS) { Fail("vkCreateDescriptorSetLayout", res); return; }

    // 2. Pipeline layout
    VkPipelineLayoutCreateInfo plCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &s_PathDSLayout;
    res = vkCreatePipelineLayout(device, &plCI, nullptr, &s_PathPipeLayout);
    if (res != VK_SUCCESS) { Fail("vkCreatePipelineLayout", res); return; }

    // 3. Shader module from embedded SPIR-V（一份模块，7 个 pName）
    VkShaderModuleCreateInfo smCI{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smCI.codeSize = g_PathTraceSpv_size;
    smCI.pCode    = g_PathTraceSpv;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    res = vkCreateShaderModule(device, &smCI, nullptr, &shaderModule);
    if (res != VK_SUCCESS) { Fail("vkCreateShaderModule(PT SPIR-V)", res); return; }

    // 4. Ray tracing pipeline：7 stage / 5 group
    //    group 顺序必须与 vpt_rt.cpp 的 kGroup* 常量一致。
    enum StageIndex { eRaygen = 0, eMiss, eMissShadow, eClosestHit, eClosestHitShadow, eAnyHit, eAnyHitShadow, eStageCount };
    VkPipelineShaderStageCreateInfo stages[eStageCount] = {};
    for (int i = 0; i < eStageCount; i++)
    {
        stages[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].module = shaderModule;
    }
    stages[eRaygen].stage           = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[eRaygen].pName           = "rgenMain";
    stages[eMiss].stage             = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[eMiss].pName             = "rmissMain";
    stages[eMissShadow].stage       = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[eMissShadow].pName       = "rmissShadow";
    stages[eClosestHit].stage       = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[eClosestHit].pName       = "rchitMain";
    stages[eClosestHitShadow].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[eClosestHitShadow].pName = "rchitShadow";
    stages[eAnyHit].stage           = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[eAnyHit].pName           = "rahitMain";
    stages[eAnyHitShadow].stage     = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[eAnyHitShadow].pName     = "rahitShadow";

    VkRayTracingShaderGroupCreateInfoKHR groups[VPTRt::kGroupCount] = {};
    for (uint32_t i = 0; i < VPTRt::kGroupCount; i++)
    {
        groups[i].sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[i].generalShader      = VK_SHADER_UNUSED_KHR;
        groups[i].closestHitShader   = VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader       = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    // 0: raygen
    groups[VPTRt::kGroupRaygen].type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[VPTRt::kGroupRaygen].generalShader = eRaygen;
    // 1/2: miss（主路径 / 阴影）
    groups[VPTRt::kGroupMissMain].type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[VPTRt::kGroupMissMain].generalShader = eMiss;
    groups[VPTRt::kGroupMissShadow].type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[VPTRt::kGroupMissShadow].generalShader = eMissShadow;
    // 3: hit group 0 = 主路径（CH 只回写命中记录 + AH 镂空）
    groups[VPTRt::kGroupHitMain].type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[VPTRt::kGroupHitMain].closestHitShader = eClosestHit;
    groups[VPTRt::kGroupHitMain].anyHitShader     = eAnyHit;
    // 4: hit group 1 = 阴影（CH 置 0 + AH 累积透射率）
    groups[VPTRt::kGroupHitShadow].type             = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[VPTRt::kGroupHitShadow].closestHitShader = eClosestHitShadow;
    groups[VPTRt::kGroupHitShadow].anyHitShader     = eAnyHitShadow;

    VkRayTracingPipelineCreateInfoKHR rtCI{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
    rtCI.stageCount                   = eStageCount;
    rtCI.pStages                      = stages;
    rtCI.groupCount                   = VPTRt::kGroupCount;
    rtCI.pGroups                      = groups;
    // 阴影光线从 raygen 发出，不在 CH 里递归 ⇒ 深度 1 足够
    rtCI.maxPipelineRayRecursionDepth = 1;
    rtCI.layout                       = s_PathPipeLayout;

    // D3：同步创建，不传 pipeline cache、不做 deferred operation、不测耗时
    res = VPTRt::CreateRayTracingPipelines()(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtCI, nullptr, &s_PathPipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (res != VK_SUCCESS)
    {
        Fail("vkCreateRayTracingPipelinesKHR", res);
        return;
    }

    // 5. SBT（与 pipeline 同生共死）
    if (!VPTRt::CreateBindingTable(device, gpu, s_PathPipeline, s_PathSbt))
    {
        s_PathInitFailed = true;
        snprintf(s_PathInitError, sizeof(s_PathInitError), "SBT creation failed");
        return;
    }

    // 6. Descriptor pool + set（环形，kDescRingSize 套）
    VkDescriptorPoolSize poolSizes[5] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; poolSizes[0].descriptorCount = 1 * kDescRingSize;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;              poolSizes[1].descriptorCount = 5 * kDescRingSize;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;             poolSizes[2].descriptorCount = 1 * kDescRingSize;
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             poolSizes[3].descriptorCount = 6 * kDescRingSize;  // 5 SSBOs + 1 EnvAccel
    poolSizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     poolSizes[4].descriptorCount = 5 * kDescRingSize;  // 4 tex arrays + 1 HDR env

    VkDescriptorPoolCreateInfo dpCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.maxSets       = kDescRingSize;
    dpCI.poolSizeCount = 5;
    dpCI.pPoolSizes    = poolSizes;
    res = vkCreateDescriptorPool(device, &dpCI, nullptr, &s_PathDescPool);
    if (res != VK_SUCCESS) { Fail("vkCreateDescriptorPool", res); return; }

    VkDescriptorSetLayout ringLayouts[kDescRingSize];
    for (uint32_t i = 0; i < kDescRingSize; i++) ringLayouts[i] = s_PathDSLayout;

    VkDescriptorSetAllocateInfo dsAI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsAI.descriptorPool     = s_PathDescPool;
    dsAI.descriptorSetCount = kDescRingSize;
    dsAI.pSetLayouts        = ringLayouts;
    res = vkAllocateDescriptorSets(device, &dsAI, s_PathDescSets);
    if (res != VK_SUCCESS) { Fail("vkAllocateDescriptorSets", res); return; }
    s_DescRingCursor = 0;
    s_PathDescSet    = s_PathDescSets[0];

    // 7. Camera UBO (persistent, mapped)
    res = CreateHostBuffer(device, gpu, sizeof(PathTraceCameraUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, s_PathUBOBuffer, s_PathUBOMemory, &s_PathUBOMapped);
    if (res != VK_SUCCESS) { Fail("UBO create", res); return; }

    // 8. Texture sampler (shared, linear filtering + repeat wrap)
    VkSamplerCreateInfo samplerCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerCI.magFilter = VK_FILTER_LINEAR;
    samplerCI.minFilter = VK_FILTER_LINEAR;
    samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCI.minLod = 0.0f;
    samplerCI.maxLod = VK_LOD_CLAMP_NONE;
    res = vkCreateSampler(device, &samplerCI, nullptr, &s_TexSampler);
    if (res != VK_SUCCESS) { Fail("vkCreateSampler", res); return; }

    s_PathReady = true;
    // 构建指纹：用于在 Unity Console 里确认「当前进程加载的是哪一版 DLL / 内嵌 SPV」。
    // 换 DLL 后没有重启 Unity 时，这里打印的仍然是旧值。
    fprintf(stderr,
            "[VPT] PathTrace RT pipeline initialized (18 bindings, 7 stages, 5 groups). "
            "shader SPV = %u bytes.\n",
            g_PathTraceSpv_size);
    fflush(stderr);
}

// ═══════════════════════════════════════════════════════════
//  AccessTexture 查询节流（BUG-004）
//
//  背景：`AccessTexture()` / `GetNativeTexturePtr()` 属于 Unity 的「资源查询 API」，
//  文档明说「会把资源标记为当帧使用」。每帧对 9 张图各查一次（5 输出图 + 4 纹理数组）
//  在长时间运行后会耗尽 Unity 的 resource ID（20 bit，2^20 条）：
//     Resource ID out of range in GetResource: 1048601 (max is 1048575)
//  之后每帧刷屏 → 主循环被拖死 → 黑屏（详见 doc/bug追踪.md BUG-004）。
//
//  做法：`nativeTex` 指针没变且已经有 view 时，**跳过 AccessTexture**，直接用缓存的
//  image/view；但会按自适应间隔（1 → 600 帧）重新查一次做校验：
//    · 校验时 image 没变 → 间隔翻倍（最长 600 帧，约 3 秒）；
//    · 校验时 image 变了 → 立刻回到逐帧查询（间隔归 1），并记录一次 note。
//  这样「句柄稳定」时查询量降到 1/N，而「Unity 真在换图」时行为与现在完全一致（不会用旧图）。
//
//  ⚠ 绝不缓存「传给 Unity 的指针」：这里缓存的只是我们自己的 view，
//     真正调用 AccessTexture 时用的永远是当帧 C# 传来的 nativeTex。
// ═══════════════════════════════════════════════════════════
static constexpr uint32_t kQueryIntervalMax = 600;

struct QueryThrottle
{
    void*    key        = nullptr;   // 上次成功查询时的 nativeTex 指针
    uint32_t skipLeft   = 0;         // 还要跳过几次查询
    uint32_t interval   = 1;         // 当前自适应间隔
};

static QueryThrottle s_OutThrottle[5];
static QueryThrottle s_TexThrottle[4];
// 诊断计数（每 120 帧汇总后清零）
static uint32_t s_PtrChangeCount[5] = { 0, 0, 0, 0, 0 };
static uint32_t s_TexPtrChangeCount[4] = { 0, 0, 0, 0 };

// 是否可以跳过本次 AccessTexture？
static bool QuerySkip(QueryThrottle& t, void* nativeTex)
{
    if (nativeTex == nullptr) return false;
    if (t.key != nativeTex)   return false;      // 指针变了：必须重新查
    if (t.skipLeft == 0)      return false;      // 到期：需要校验一次
    t.skipLeft--;
    return true;
}

// 查询完成后更新节流状态
static void QueryUpdate(QueryThrottle& t, void* nativeTex, bool imageChanged)
{
    const bool keySame = (t.key == nativeTex);
    if (!keySame)
        t.interval = 1;                          // 新指针：先逐帧确认
    else if (imageChanged)
        t.interval = 1;                          // 背后真的换了图：退回逐帧
    else
        t.interval = (t.interval >= kQueryIntervalMax / 2) ? kQueryIntervalMax : t.interval * 2;

    t.key      = nativeTex;
    t.skipLeft = t.interval;
}

// ═══════════════════════════════════════════════════════════
//  5 张输出图的期望格式（与 shader 的 [[vk::image_format(...)]] 一一对应）
static const VkFormat kOutputFormats[5] = {
    VK_FORMAT_R16G16B16A16_SFLOAT,   // 0 output
    VK_FORMAT_R32G32B32A32_SFLOAT,   // 1 gbufferPosDepth
    VK_FORMAT_R16G16B16A16_SFLOAT,   // 2 gbufferNormalRough
    VK_FORMAT_R16G16B16A16_SFLOAT,   // 3 diffuseAlbedo
    VK_FORMAT_R16G16B16A16_SFLOAT,   // 4 specularAlbedo
};

// ═══════════════════════════════════════════════════════════
//  Helper: access Unity texture and ensure ImageView exists
//
//  ObserveOnly：Unity 只回报句柄，不插 barrier（同步由插件负责）。
//  图片布局由插件自己跟踪：每次 dispatch 结束都会把它恢复到
//  SHADER_READ_ONLY_OPTIMAL，所以下一帧的 oldLayout 就是那个值；
//  只有换了 VkImage 句柄（RT 重建/分辨率变化）才退回 UNDEFINED。
//  硬编码 UNDEFINED 会「丢弃已有内容」（§10④）。
//
//  查询节流见上面的 QueryThrottle（BUG-004 保留的缓解措施）：
//  句柄稳定时把 AccessTexture 摊到 1/600，句柄一变立刻恢复逐帧查询。
// ═══════════════════════════════════════════════════════════
static bool EnsureImageView(IUnityGraphicsVulkan* vulkan, VkDevice device,
                           void* nativeTex, int slot)
{
    if (nativeTex == nullptr) return false;

    // 节流快路径：指针没变 + 已有 view ⇒ 不查 Unity
    if (s_OutputViews[slot].view != VK_NULL_HANDLE && QuerySkip(s_OutThrottle[slot], nativeTex))
        return true;

    UnityVulkanImage vkImg{};
    bool ok = vulkan->AccessTexture(nativeTex, UnityVulkanWholeImage,
        VK_IMAGE_LAYOUT_GENERAL,
        // 阶段/访问必须是真正的消费者：迁移到 RT pipeline 后是 RAY_TRACING（raygen 写图），
        // 之前遗留的 COMPUTE 只是"看着像对"（ObserveOnly 不插 barrier，但语义要写对）
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_SHADER_WRITE_BIT,
        kUnityVulkanResourceAccess_ObserveOnly, &vkImg);
    if (!ok || vkImg.image == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] Path: AccessTexture failed for slot %d\n", slot);
        return false;
    }

    const bool keySame     = (s_OutThrottle[slot].key == nativeTex);
    const bool imageChanged = (s_OutputViews[slot].image != vkImg.image);
    if (!keySame) s_PtrChangeCount[slot]++;

    if (imageChanged)
    {
        if (keySame)
        {
            // 指针没变但句柄变了：Unity 在背后换了 backing image。
            // 说明节流不能停，立刻退回逐帧查询（QueryUpdate 会做），这里只记一次。
            static uint32_t s_NoteCount = 0;
            if (s_NoteCount++ < 3)
                VPTTrace::Line("NOTE: slot %d image changed behind a stable native ptr (0x%p -> 0x%p)",
                               slot, (void*)s_OutputViews[slot].image, (void*)vkImg.image);
        }

        if (s_OutputViews[slot].view)
            vkDestroyImageView(device, s_OutputViews[slot].view, nullptr);

        // shader 侧用 [[vk::image_format(...)]] 固定了存储图格式（rgba16f ×4 + rgba32f ×1）。
        // 与 Unity RT 的实际格式不一致会写成垃圾/触发验证层报错。
        // 只在「格式变化」时打印一次（正常情况下每个 slot 一辈子只打一条）。
        {
            static VkFormat s_LoggedFormat[5] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                                  VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                                  VK_FORMAT_UNDEFINED };
            if (s_LoggedFormat[slot] != vkImg.format)
            {
                s_LoggedFormat[slot] = vkImg.format;
                fprintf(stderr, "[VPT] Path: view slot %d format=%d (shader expects %d)%s\n",
                        slot, (int)vkImg.format, (int)kOutputFormats[slot],
                        vkImg.format == kOutputFormats[slot] ? "" : "  << MISMATCH!");
                if (vkImg.format != kOutputFormats[slot])
                    VPTTrace::Line("WARN: output image slot %d format %d != shader expected %d",
                                   slot, (int)vkImg.format, (int)kOutputFormats[slot]);
            }
        }

        s_ViewRecreateCount[slot]++;

        VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivCI.image    = vkImg.image;
        ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCI.format   = vkImg.format;
        ivCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCI.subresourceRange.baseMipLevel   = 0;
        ivCI.subresourceRange.levelCount     = 1;
        ivCI.subresourceRange.baseArrayLayer = 0;
        ivCI.subresourceRange.layerCount     = 1;

        VkResult res = vkCreateImageView(device, &ivCI, nullptr, &s_OutputViews[slot].view);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPT] Path: vkCreateImageView slot %d failed: %d\n", slot, res);
            return false;
        }
        s_OutputViews[slot].image  = vkImg.image;
        s_OutputViews[slot].layout = VK_IMAGE_LAYOUT_UNDEFINED;   // 新句柄：内容无意义，允许丢弃
    }

    // 注意：这里**不**采信 vkImg.layout。Unity 的布局跟踪不包含插件在
    // AccessTexture(ObserveOnly) 之外做的布局转换，所以它可能永远停在 UNDEFINED
    // （例如 _outputRT 只被 NGX 通过插件自己的 view 读取时），
    // 用了它等于没修「每帧丢弃内容」。插件自己记录的布局才是可信的那份。
    QueryUpdate(s_OutThrottle[slot], nativeTex, imageChanged);
    return true;
}

// ── Ensure Texture2DArray VkImageView (Phase 4) ─────────────
// Creates a 2D_ARRAY view over Unity's Texture2DArray VkImage.
// Cached in s_TexViews[slot]; recreated only when the underlying
// VkImage changes between frames.
static bool EnsureTextureArrayView(
    IUnityGraphicsVulkan* vulkan, VkDevice device,
    void* nativeTex, int slot)
{
    if (!nativeTex) return false;

    // 节流快路径（同 EnsureImageView，见 BUG-004）
    if (s_TexViews[slot].view != VK_NULL_HANDLE && QuerySkip(s_TexThrottle[slot], nativeTex))
        return true;

    UnityVulkanImage vkImg{};
    bool ok = vulkan->AccessTexture(
        nativeTex, UnityVulkanWholeImage,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        // 真正的读者是 raygen / CH / AH（采样纹理数组），不再是 compute
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_SHADER_READ_BIT,
        kUnityVulkanResourceAccess_ObserveOnly, &vkImg);
    if (!ok || vkImg.image == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] Path: AccessTexture failed for tex slot %d\n", slot);
        return false;
    }

    // Recreate view only when the underlying VkImage changes
    const bool texKeySame     = (s_TexThrottle[slot].key == nativeTex);
    const bool texImageChanged = (s_TexViews[slot].image != vkImg.image);
    if (!texKeySame) s_TexPtrChangeCount[slot]++;

    if (texImageChanged)
    {
        if (s_TexViews[slot].view)
            vkDestroyImageView(device, s_TexViews[slot].view, nullptr);

        VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivCI.image    = vkImg.image;
        ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        ivCI.format   = vkImg.format;
        ivCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCI.subresourceRange.baseMipLevel   = 0;
        ivCI.subresourceRange.levelCount     = 1;
        ivCI.subresourceRange.baseArrayLayer = 0;
        ivCI.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

        VkResult res = vkCreateImageView(device, &ivCI, nullptr, &s_TexViews[slot].view);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPT] Path: vkCreateImageView tex slot %d failed: %d\n", slot, res);
            s_TexViews[slot].view  = VK_NULL_HANDLE;
            s_TexViews[slot].image = VK_NULL_HANDLE;
            return false;
        }
        s_TexViews[slot].image = vkImg.image;
        s_TexViewRecreateCount[slot]++;
    }
    QueryUpdate(s_TexThrottle[slot], nativeTex, texImageChanged);
    return true;
}

// ═══════════════════════════════════════════════════════════
//  Plan B: Two-phase dispatch (eliminates vkQueueWaitIdle)
//  Phase 1 = PrepareDispatch（CPU 侧准备），Phase 2 = RenderCallback（录制进 Unity 的 cb）。
//  旧的 DispatchPathTrace 直连路径（自建 cb + vkQueueSubmit + vkQueueWaitIdle）已删除，
//  避免同一份录入逻辑维护两份（见 §7.6）。
// ═══════════════════════════════════════════════════════════

// Pending dispatch state (set by PrepareDispatch, consumed by RenderCallback)
static int32_t  s_PendingWidth   = 0;
static int32_t  s_PendingHeight  = 0;
static bool     s_DispatchPending = false;

// ── 暂停状态（C# 重建场景前用来"关掉 PT 并等 GPU 空闲"）──
static std::atomic<bool> s_PauseRequested{ false };
static std::atomic<bool> s_Paused{ false };

void VPTDispatch::RequestPause()
{
    s_PauseRequested.store(true);
    s_DispatchPending = false;
    VPTTrace::Line("PT pause requested");
}

void VPTDispatch::RequestResume()
{
    s_PauseRequested.store(false);
    s_Paused.store(false);
    VPTTrace::Line("PT resumed");
}

bool VPTDispatch::IsPaused()         { return s_Paused.load(); }
bool VPTDispatch::IsPauseRequested() { return s_PauseRequested.load(); }

void VPTDispatch::InvalidateTextureViews()
{
    VkDevice device = VPT_GetInstance().device;
    for (int i = 0; i < 4; i++)
    {
        if (s_TexViews[i].view && device != VK_NULL_HANDLE)
            vkDestroyImageView(device, s_TexViews[i].view, nullptr);
        s_TexViews[i].view  = VK_NULL_HANDLE;
        s_TexViews[i].image = VK_NULL_HANDLE;
    }
}

// ── Phase 1: PrepareDispatch ──────────────────────────────
// Called from C# before cmd.IssuePluginEvent.
// Accesses Unity textures (ObserveOnly), fills UBO, updates descriptor sets.
// Does NOT create/submit command buffers — just prepares GPU state.

void VPTDispatch::PrepareDispatch(
    void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
    void* diffuseAlbedoPtr, void* specularAlbedoPtr,
    void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
    int32_t width, int32_t height,
    const VPT_CameraData& cameraData,
    uint32_t lightCount, uint32_t samplesPerPixel)
{
    if (!s_PathReady || !outputPtr || !gbuf0Ptr || !gbuf1Ptr || !diffuseAlbedoPtr || !specularAlbedoPtr
        || width <= 0 || height <= 0)
        return;

    // 暂停期间（C# 正在重建场景/销毁贴图）不碰 Unity 纹理
    if (s_Paused.load() || s_PauseRequested.load())
        return;

    if (VPT_IsDeviceLost())
        return;

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    VkDevice device = VPT_GetInstance().device;

    // ── Access 5 Unity RenderTextures as VkImages (ObserveOnly: query handles only) ──
    // 说明：AccessTexture 会进入 Unity 的纹理访问协议，若与渲染线程状态冲突会阻塞，
    // 是主线程最容易卡住的位置之一 —— 每一个都单独记阶段。
    VPTTrace::Stage("prepare:AccessTexture(output)");
    if (!EnsureImageView(vulkan, device, outputPtr, 0)) return;
    VPTTrace::Stage("prepare:AccessTexture(gbuffer0)");
    if (!EnsureImageView(vulkan, device, gbuf0Ptr,  1)) return;
    VPTTrace::Stage("prepare:AccessTexture(gbuffer1)");
    if (!EnsureImageView(vulkan, device, gbuf1Ptr,  2)) return;
    VPTTrace::Stage("prepare:AccessTexture(diffuseAlbedo)");
    if (!EnsureImageView(vulkan, device, diffuseAlbedoPtr,  3)) return;
    VPTTrace::Stage("prepare:AccessTexture(specularAlbedo)");
    if (!EnsureImageView(vulkan, device, specularAlbedoPtr, 4)) return;

    // ── Access 4 Unity Texture2DArrays as VkImages (read) ──
    VPTTrace::Stage("prepare:AccessTextureArray(baseColor)");
    if (baseColorPtr)    EnsureTextureArrayView(vulkan, device, baseColorPtr,    0);
    VPTTrace::Stage("prepare:AccessTextureArray(metallicRough)");
    if (metallicRoughPtr) EnsureTextureArrayView(vulkan, device, metallicRoughPtr, 1);
    VPTTrace::Stage("prepare:AccessTextureArray(normal)");
    if (normalPtr)       EnsureTextureArrayView(vulkan, device, normalPtr,       2);
    VPTTrace::Stage("prepare:AccessTextureArray(emissive)");
    if (emissivePtr)     EnsureTextureArrayView(vulkan, device, emissivePtr,     3);

    VPTTrace::Stage("prepare:ubo+descriptors");

    // ── 每 ~120 帧汇总一次 ImageView 重建次数 ──
    // 正常情况下（句柄稳定）应该全是 0；若输出图那边每 120 帧 ≈ 120，说明 Unity 每帧
    // 都在换 VkImage 句柄 —— 见 doc/bug追踪.md BUG-002 / BUG-003。
    if (++s_ViewStatsFrame >= 120)
    {
        s_ViewStatsFrame = 0;
        VPTTrace::Line("view recreate /120 frames: out=%u,%u,%u,%u,%u texArray=%u,%u,%u,%u "
                       "| ptrChanged out=%u,%u,%u,%u,%u tex=%u,%u,%u,%u "
                       "| queryInterval out=%u tex=%u",
                       s_ViewRecreateCount[0], s_ViewRecreateCount[1], s_ViewRecreateCount[2],
                       s_ViewRecreateCount[3], s_ViewRecreateCount[4],
                       s_TexViewRecreateCount[0], s_TexViewRecreateCount[1],
                       s_TexViewRecreateCount[2], s_TexViewRecreateCount[3],
                       s_PtrChangeCount[0], s_PtrChangeCount[1], s_PtrChangeCount[2],
                       s_PtrChangeCount[3], s_PtrChangeCount[4],
                       s_TexPtrChangeCount[0], s_TexPtrChangeCount[1],
                       s_TexPtrChangeCount[2], s_TexPtrChangeCount[3],
                       s_OutThrottle[0].interval, s_TexThrottle[0].interval);

        // 持续「每帧都换图」= 上游在每帧重建 RenderTexture。最常见的原因是
        // Assets/ 被反复触发刷新（例如 NGX 把日志写进 Assets/），于是 URP renderer
        // 资产重载 → Feature.Create → 全部 RT 重建 + DLSS 重初始化 → 再写日志 ……
        // 这个循环会持续吃掉 Unity 的 resource ID，最终 GetResource 越界 → 黑屏 + 编辑器卡死
        // （2026-11-22 实测，见 doc/bug追踪.md BUG-003）。日志里点到为止，便于下次直接定位。
        const bool hot = s_ViewRecreateCount[0] >= 90;   // 120 帧里 90 帧以上换了图
        s_ViewHotWindows = hot ? (s_ViewHotWindows + 1) : 0;
        if (s_ViewHotWindows == 5)
        {
            VPTTrace::Line("WARN: output images changed every frame for %u windows — "
                           "上游很可能在每帧重建 RenderTexture（Assets 刷新→renderer 重载？）；"
                           "持续下去会耗尽 Unity resource ID 并导致黑屏",
                           s_ViewHotWindows);
            fprintf(stderr, "[VPT] WARN: RT images churning every frame (%u windows) — "
                            "check for repeated AssetDatabase refresh / renderer reload.\n",
                    s_ViewHotWindows);
        }
        for (int i = 0; i < 5; i++) { s_ViewRecreateCount[i] = 0; s_PtrChangeCount[i] = 0; }
        for (int i = 0; i < 4; i++) { s_TexViewRecreateCount[i] = 0; s_TexPtrChangeCount[i] = 0; }
    }

    // ── Fill PathTraceCameraUBO from VPT_CameraData ──
    PathTraceCameraUBO ubo{};
    memcpy(ubo.viewInv,  cameraData.viewInv,  64);
    memcpy(ubo.projInv,  cameraData.projInv,  64);
    ubo.posFov[0] = cameraData.position[0];
    ubo.posFov[1] = cameraData.position[1];
    ubo.posFov[2] = cameraData.position[2];
    ubo.posFov[3] = cameraData.fov;
    ubo.jitterNearFar[0] = cameraData.jitter[0];
    ubo.jitterNearFar[1] = cameraData.jitter[1];
    ubo.jitterNearFar[2] = cameraData.nearPlane;
    ubo.jitterNearFar[3] = cameraData.farPlane;
    ubo.frameMaxDepth[0] = cameraData.frameCount;
    ubo.frameMaxDepth[1] = cameraData.maxDepth;
    // z = envReady：HDR 未上传时 shader 走常数 ambient 回退（而不是采样黑图）
    ubo.frameMaxDepth[2] = VPTEnv::IsReady() ? 1 : 0;
    ubo.frameMaxDepth[3] = 0;
    ubo.ambientPad[0] = cameraData.ambientColor[0];
    ubo.ambientPad[1] = cameraData.ambientColor[1];
    ubo.ambientPad[2] = cameraData.ambientColor[2];
    ubo.ambientPad[3] = cameraData.pad;
    ubo.lightCount = lightCount;
    ubo.samplesPerPixel = samplesPerPixel;
    ubo.envIntensity = cameraData.envIntensity;
    ubo.envRotation  = cameraData.envRotation;
    if (VPTScene::GetLightBuffer() == VK_NULL_HANDLE)
        ubo.lightCount = 0;
    memcpy(s_PathUBOMapped, &ubo, sizeof(PathTraceCameraUBO));

    // ── Update descriptor set: TLAS + 3 images + UBO + 5 SSBOs + 4 tex arrays ──
    VkAccelerationStructureKHR tlas = VPTScene::GetTLAS();
    if (tlas == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] PrepareDispatch: TLAS not ready\n");
        return;
    }

    VkWriteDescriptorSetAccelerationStructureKHR asInfo{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas;

    VkDescriptorImageInfo imgInfos[5] = {};
    for (int i = 0; i < 5; i++)
    {
        imgInfos[i].sampler     = VK_NULL_HANDLE;
        imgInfos[i].imageView   = s_OutputViews[i].view;
        imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = s_PathUBOBuffer;
    uboInfo.offset = 0;
    uboInfo.range  = sizeof(PathTraceCameraUBO);

    VkBuffer ssboBuffers[5] = {
        VPTScene::GetUnifiedVertexBuffer(),
        VPTScene::GetUnifiedIndexBuffer(),
        VPTScene::GetInstanceInfoBuffer(),
        VPTScene::GetMaterialBuffer(),
        VPTScene::GetLightBuffer()
    };
    VkDescriptorBufferInfo ssboInfos[5] = {};
    for (int i = 0; i < 5; i++)
    {
        ssboInfos[i].buffer = ssboBuffers[i];
        ssboInfos[i].offset = 0;
        ssboInfos[i].range  = VK_WHOLE_SIZE;
    }
    // InstanceInfo 绑定当前 TLAS 环形槽位切片（动态场景每帧变化）
    ssboInfos[2].offset = VPTScene::GetInstanceInfoOffset();

    VkDescriptorImageInfo texInfos[4] = {};
    for (int i = 0; i < 4; i++)
    {
        texInfos[i].sampler     = s_TexSampler;
        texInfos[i].imageView   = s_TexViews[i].view;
        texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ── HDR env descriptor info (binding 16/17) ──
    // VPTEnv 在真实贴图未就绪时返回 1×1 黑色占位图 + 全零 alias 表，
    // 所以这里永远可以绑合法句柄（§10② 的坑）。
    VkDescriptorImageInfo envImgInfo{};
    envImgInfo.sampler     = VPTEnv::GetEnvSampler();
    envImgInfo.imageView   = VPTEnv::GetEnvImageView();
    envImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo envAccelInfo{};
    envAccelInfo.buffer = VPTEnv::GetEnvAccelBuffer();
    envAccelInfo.offset = 0;
    envAccelInfo.range  = envAccelInfo.buffer != VK_NULL_HANDLE ? VK_WHOLE_SIZE : 0;

    // ── 环形推进：本帧用下一套 descriptor set ──
    // 上一帧/上上帧的那几套可能还被 pending 的命令缓冲引用，绝对不能改写它们，
    // 否则验证层报 VUID-vkUpdateDescriptorSets-None-03047（BUG-002）。
    s_PathDescSet = s_PathDescSets[s_DescRingCursor % kDescRingSize];
    s_DescRingCursor++;

    VkWriteDescriptorSet writes[18] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &asInfo;
    writes[0].dstSet = s_PathDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    for (int i = 0; i < 5; i++)
    {
        writes[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1 + i].dstSet = s_PathDescSet;
        writes[1 + i].dstBinding = 1 + i;
        writes[1 + i].descriptorCount = 1;
        writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1 + i].pImageInfo = &imgInfos[i];
    }

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = s_PathDescSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &uboInfo;

    for (int i = 0; i < 5; i++)
    {
        writes[5 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5 + i].dstSet = s_PathDescSet;
        writes[5 + i].dstBinding = 5 + i;
        writes[5 + i].descriptorCount = 1;
        writes[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5 + i].pBufferInfo = &ssboInfos[i];
    }

    for (int i = 0; i < 4; i++)
    {
        writes[10 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[10 + i].dstSet = s_PathDescSet;
        writes[10 + i].dstBinding = 10 + i;
        writes[10 + i].descriptorCount = 1;
        writes[10 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[10 + i].pImageInfo = &texInfos[i];
    }

    // bindings 14-15: DLSS RR albedo G-buffers (storage images)
    for (int i = 0; i < 2; i++)
    {
        writes[14 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[14 + i].dstSet = s_PathDescSet;
        writes[14 + i].dstBinding = 14 + i;
        writes[14 + i].descriptorCount = 1;
        writes[14 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[14 + i].pImageInfo = &imgInfos[3 + i];
    }

    // binding 16/17: HDR env texture + EnvAccel SSBO
    // 始终写入：未上传 HDR 时绑的是 VPTEnv 的占位资源；只有占位资源创建失败
    // （句柄为 null）才退回只写 16 个，避免给 vkUpdateDescriptorSets 传空句柄。
    uint32_t writeCount = 16;
    if (envImgInfo.imageView != VK_NULL_HANDLE && envImgInfo.sampler != VK_NULL_HANDLE
        && envAccelInfo.buffer != VK_NULL_HANDLE)
    {
        writes[16].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[16].dstSet = s_PathDescSet;
        writes[16].dstBinding = 16;
        writes[16].descriptorCount = 1;
        writes[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[16].pImageInfo = &envImgInfo;

        writes[17].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[17].dstSet = s_PathDescSet;
        writes[17].dstBinding = 17;
        writes[17].descriptorCount = 1;
        writes[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[17].pBufferInfo = &envAccelInfo;

        writeCount = 18;
    }
    else
    {
        static bool s_WarnedNoEnv = false;
        if (!s_WarnedNoEnv)
        {
            fprintf(stderr, "[VPT] WARN: env fallback resources missing (VPTEnv::InitFallback failed); "
                            "binding 16/17 left unwritten — HDR env sampling is undefined.\n");
            s_WarnedNoEnv = true;
        }
    }

    vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);

    // ── Store pending dispatch dimensions for RenderCallback ──
    s_PendingWidth    = width;
    s_PendingHeight   = height;
    s_DispatchPending = true;
}

// ── Phase 2: RenderCallback ────────────────────────────────
// Called by Unity during command buffer execution via IssuePluginEvent.
// Gets Unity's VkCommandBuffer via CommandRecordingState, records
// barriers + dispatch into it. No vkQueueSubmit / vkQueueWaitIdle.
//
// 这里同时承担所有"延迟到渲染线程"的 GPU 工作：
//   · 场景推送（buffer 上传 + BLAS/TLAS 构建）
//   · HDR 环境贴图上传 / 占位资源初始化
//   · NGX DLSS Feature 创建
//   · 蒙皮 mesh 注册
// 即使本帧没有 dispatch，只要上面有排队的工作，也必须进来录制。

void VPTDispatch::RenderCallback(int eventID)
{
    if (eventID != EVENT_PATHTRACE)
        return;

    // ── 暂停：C# 正在重建场景（要销毁 PT 正在采样的贴图数组）──
    // 先把 GPU 排空、确认空闲，再让 C# 去动资源。
    if (s_Paused.load())
        return;

    if (s_PauseRequested.load())
    {
        const VkDevice pauseDevice = VPT_GetInstance().device;
        VPTTrace::Stage("event:pause-sync");
        VPTTrace::Line("PT pause: vkDeviceWaitIdle begin ...");
        const VkResult pr = vkDeviceWaitIdle(pauseDevice);
        VPTTrace::Line("PT pause: vkDeviceWaitIdle -> %d", (int)pr);
        VPT_CheckVk(pr, "vkDeviceWaitIdle(pause before scene rebuild)");
        s_Paused.store(true);
        s_DispatchPending = false;
        VPTTrace::Stage("idle");
        return;
    }

    const bool dispatchPending = s_DispatchPending;
    const bool scenePending    = VPTScene::IsSceneUpdatePending();
    const bool envPending      = VPTEnv::HasPendingUploads();
    const bool dlssPending     = VPT_DLSS_HasPendingCreate();

    VPTTrace::Stage("event:begin");
    if (!dispatchPending && !scenePending && !envPending && !dlssPending)
        return;

    s_DispatchPending = false;  // consume pending state

    if (VPT_IsDeviceLost())
    {
        // 设备已丢失：什么都不录，也不再申请任何资源
        fprintf(stderr, "[VPT] RenderCallback skipped: device lost.\n");
        VPTTrace::Line("PT event: skipped (device lost)");
        return;
    }

    // 场景上传/环境贴图/DLSS 创建都不需要 PT pipeline；dispatch 由下方单独判定
    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    if (!vulkan)
        return;

    // ── 设备变化检测（每 ~120 帧一次）──
    // TDR 之后 Unity 的 Vulkan 后端有时会重建 VkDevice：我们缓存的所有句柄就全失效了。
    // 这里主动比对一次，避免拿着旧 device 的资源往新设备上录制（那会直接把 GPU 挂死）。
    if ((VPTTrace::Frame() % 120) == 0)
    {
        const UnityVulkanInstance& cached = VPT_GetInstance();
        const UnityVulkanInstance  cur    = vulkan->Instance();
        if (cur.device != cached.device || cur.physicalDevice != cached.physicalDevice)
        {
            VPTTrace::Line("DEVICE CHANGED: cached=%p/%p unity=%p/%p -> stop",
                           (void*)cached.device, (void*)cached.physicalDevice,
                           (void*)cur.device, (void*)cur.physicalDevice);
            fprintf(stderr, "[VPT] FATAL: Unity 重建了 VkDevice（TDR 恢复），插件缓存已失效。\n");
            VPT_MarkDeviceLost("Unity VkDevice recreated after TDR", 0);
            return;
        }
    }

    // ── 重建帧之后的 GPU 同步探针 ──
    // 重建帧里录了 728 个 BLAS + TLAS + 纹理上传，由 Unity 提交，插件看不到 VkResult。
    // 这里等一次 GPU 空闲：如果那一帧把 GPU 搞挂了，2s 后会以 VK_ERROR_DEVICE_LOST
    // 形式返回，我们立刻停机并让 C# 关掉 PT —— 编辑器不会静默卡死几十秒。
    if (VPTScene::TakeGpuSyncProbe() && !VPT_EnvFlag("VPT_NO_PROBE"))
    {
        VPTTrace::Stage("event:gpu-sync-probe");
        const VkDevice probeDevice = VPT_GetInstance().device;
        VPTTrace::Line("gpu-sync probe: vkDeviceWaitIdle after rebuild begin ...");
        const VkResult pr = vkDeviceWaitIdle(probeDevice);
        VPTTrace::Line("gpu-sync probe: returned %d", (int)pr);
        if (!VPT_CheckVk(pr, "vkDeviceWaitIdle(post-rebuild probe)"))
        {
            fprintf(stderr, "[VPT] FATAL: 重建帧之后 GPU 同步失败（%d），设备已丢失。\n", (int)pr);
            return;
        }
    }

    // ── Get Unity's current VkCommandBuffer ──
    // DontCare mode: we record into Unity's current command buffer, no direct queue submit.
    // (Allow mode would disable access to Unity's command buffer, returning null)
    VPTTrace::Stage("event:CommandRecordingState");
    UnityVulkanRecordingState recState{};
    const bool haveState = vulkan->CommandRecordingState(&recState, kUnityVulkanGraphicsQueueAccess_DontCare);
    if (!haveState)
    {
        fprintf(stderr, "[VPT] RenderCallback: CommandRecordingState failed\n");
        VPTTrace::Line("PT event: CommandRecordingState FAILED");
        return;
    }

    VkCommandBuffer cmd = recState.commandBuffer;
    if (cmd == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] RenderCallback: Unity command buffer is null\n");
        VPTTrace::Line("PT event: CommandRecordingState returned null cmd");
        return;
    }

    // ── 0) 延迟场景重建（必须在任何 TLAS/dispatch 之前；会销毁旧 TLAS）──
    VPTTrace::Stage("event:scene-update");
    const bool sceneRebuilt = VPTScene::RecordSceneUpdateIfNeeded(cmd);

    // ── 0.1) 延迟 HDR 环境贴图上传 + 占位资源初始化 ──
    VPTTrace::Stage("event:env-upload");
    const bool envReplaced = VPTEnv::RecordPendingUploads(cmd);

    // ── 0.2) 延迟 DLSS Feature 创建（NGX 往 Unity 的命令缓冲里录初始化命令）──
    VPTTrace::Stage("event:dlss-create");
    VPT_DLSS_RecordPendingCreate(cmd);

    // ── 0.3) 延迟蒙皮 mesh 注册（依赖刚构建好的统一顶点缓冲）──
    VPTTrace::Stage("event:skin-register");
    VPTScene::RecordSkinRegistrationIfNeeded(cmd);

    // 本帧刚重建过场景 / 环境贴图：descriptor set 里的 TLAS、buffer、env 句柄
    // 都已过期，本帧不再 dispatch（下一帧 C# 会重新 PrepareDispatch）。
    if (sceneRebuilt || envReplaced)
    {
        fprintf(stderr, "[VPT] %s this frame; path trace dispatch skipped.\n",
                sceneRebuilt ? "Scene rebuilt" : "Env map replaced");
        VPTTrace::Line("PT event: %s -> dispatch skipped", sceneRebuilt ? "scene rebuilt" : "env replaced");
        // Unity 侧重建后会换一批 Texture2DArray：丢掉旧 ImageView，避免句柄复用导致
        // 缓存命中一个指向已销毁图像的 view。
        if (sceneRebuilt)
            VPTDispatch::InvalidateTextureViews();
        return;
    }

    if (!dispatchPending || !s_PathReady)
        return;
    // 防御：没有 SBT 就不可能 traceRays（正常路径下 ready⇒SBT 已建）
    if (s_PathSbt.buffer == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] RenderCallback: SBT missing, dispatch skipped.\n");
        VPTTrace::Line("PT event: SBT missing -> dispatch skipped");
        return;
    }

    // ── GPU 计时：T0（本帧开始），随后每个阶段结束各打一个点 ──
    VPTGpuTimer::BeginFrame(cmd);

    // ── 骨骼动画：GPU 蒙皮 dispatch（若有更新，须先于 refit/TLAS，蒙皮 compute 就地写统一顶点缓冲）──
    VPTTrace::Stage("dispatch:gpu-skinning");
    VPTScene::RecordGPUSkinningIfNeeded(cmd);
    VPTGpuTimer::Mark(cmd);   // T1: 蒙皮结束

    // ── 骨骼动画：变形顶点上传 + BLAS refit（若有更新，需先于 TLAS 重建）──
    VPTTrace::Stage("dispatch:blas-refit");
    VPTScene::RecordBLASRefitIfNeeded(cmd);
    VPTGpuTimer::Mark(cmd);   // T2: BLAS refit 结束

    // ── 动态场景：每帧 TLAS 重建（若本帧有实例更新，优先于 dispatch 录制）──
    VPTTrace::Stage("dispatch:tlas-update");
    const bool tlasResized = VPTScene::RecordTLASUpdateIfNeeded(cmd);
    VPTGpuTimer::Mark(cmd);   // T3: TLAS 重建结束

    // 实例容量扩容会重建 TLAS 环形 buffer：descriptor set 里的旧句柄已失效，
    // 本帧不能 dispatch（下一帧 PrepareDispatch 会重新绑定）。
    if (tlasResized)
    {
        fprintf(stderr, "[VPT] TLAS ring resized; path trace dispatch skipped this frame.\n");
        return;
    }

    // ── Barrier 1: UBO host write → ray tracing 读 + 5 images → GENERAL ──
    // oldLayout 用插件自己跟踪的布局：每次 dispatch 结束都恢复成
    // SHADER_READ_ONLY_OPTIMAL，所以稳态下就是它；只有新句柄（RT 重建/换分辨率）
    // 才是 UNDEFINED（这时丢弃内容是正确的）。不再每帧丢弃上一帧内容 —— §10④。
    VkBufferMemoryBarrier uboBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    uboBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    uboBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    uboBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uboBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uboBarrier.buffer = s_PathUBOBuffer;
    uboBarrier.offset = 0;
    uboBarrier.size = sizeof(PathTraceCameraUBO);

    VkImageMemoryBarrier imgBarriers[5] = {};
    for (int i = 0; i < 5; i++)
    {
        const VkImageLayout oldLayout = s_OutputViews[i].layout;

        imgBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imgBarriers[i].oldLayout = oldLayout;
        imgBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarriers[i].image = s_OutputViews[i].image;
        imgBarriers[i].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        imgBarriers[i].subresourceRange.baseMipLevel   = 0;
        imgBarriers[i].subresourceRange.levelCount     = 1;
        imgBarriers[i].subresourceRange.baseArrayLayer = 0;
        imgBarriers[i].subresourceRange.layerCount     = 1;
        // 旧布局若是 UNDEFINED（首次使用/新句柄），内容本就无意义，srcAccess 也须为 0；
        // 否则同步上一轮读（Unity 的 compute / NGX / Blit）与本次写。
        imgBarriers[i].srcAccessMask = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            ? 0
            : VK_ACCESS_SHADER_READ_BIT;
        imgBarriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }

    VPTTrace::Stage("dispatch:barrier-1-images-to-general");
    vkCmdPipelineBarrier(cmd,
        // 上一轮的使用者是 Unity 的 compute（MotionVector）、NGX（calc stage=compute）
        // 与 Blit（fragment 采样），所以 srcStage 覆盖 compute + fragment。
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        // 消费者从 compute 换成 ray tracing（raygen 里写 5 张图）
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 0, nullptr, 1, &uboBarrier, 5, imgBarriers);

    // ── Trace rays ──
    // 直接用像素尺寸（不再 (w+7)/8）；一次调用覆盖整张图，raygen 内部跑完整个弹射循环。
    VPTTrace::Stage("dispatch:bind-pipeline");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, s_PathPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        s_PathPipeLayout, 0, 1, &s_PathDescSet, 0, nullptr);

    VPTTrace::Stage("dispatch:trace-rays");
    VkStridedDeviceAddressRegionKHR raygenRegion{ s_PathSbt.raygen.deviceAddress, s_PathSbt.raygen.stride, s_PathSbt.raygen.size };
    VkStridedDeviceAddressRegionKHR missRegion  { s_PathSbt.miss.deviceAddress,   s_PathSbt.miss.stride,   s_PathSbt.miss.size };
    VkStridedDeviceAddressRegionKHR hitRegion   { s_PathSbt.hit.deviceAddress,    s_PathSbt.hit.stride,    s_PathSbt.hit.size };
    // callable：全零 region 表示「没有 callable shader」
    VkStridedDeviceAddressRegionKHR callableRegion{ 0, 0, 0 };

    VPTRt::CmdTraceRays()(cmd,
        &raygenRegion, &missRegion, &hitRegion, &callableRegion,
        (uint32_t)s_PendingWidth, (uint32_t)s_PendingHeight, 1u);
    VPTTrace::Stage("dispatch:trace-rays-recorded");

    // ── Barrier 2: 5 images GENERAL → SHADER_READ_ONLY (for Unity compute reads) ──
    for (int i = 0; i < 5; i++)
    {
        imgBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imgBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    // src 换成 ray tracing；dst 保持 compute + fragment（MotionVector compute / NGX / Blit 都在后面读）
    VPTTrace::Stage("dispatch:barrier-2-images-to-sro");
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 5, imgBarriers);

    // 记录本帧结束时这 5 张图的布局，供下一帧 barrier 的 oldLayout 使用
    for (int i = 0; i < 5; i++)
        s_OutputViews[i].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VPTGpuTimer::Mark(cmd);   // T4: 路径追踪 dispatch 结束
}

bool VPTDispatch::IsDispatchPending() { return s_DispatchPending; }

// ═══════════════════════════════════════════════════════════
//  PathTrace pipeline cleanup
// ═══════════════════════════════════════════════════════════

void VPTDispatch::DestroyPathTracePipeline(VkDevice device)
{
    // 不按 s_PathReady 提前返回：创建中途失败时也会留下半成品（layout/pool/UBO），
    // 这里对每个句柄单独判空，既能清理半成品也不会碰未创建的东西。
    if (device == VK_NULL_HANDLE)
    {
        s_PathReady = false;
        s_PathInitFailed = false;
        return;
    }

    const bool hadAnything = s_PathReady || s_PathPipeline != VK_NULL_HANDLE || s_PathSbt.buffer != VK_NULL_HANDLE;
    // 设备已丢失时 vkDeviceWaitIdle 可能长时间不返回，直接跳过等待
    if (hadAnything && !VPT_IsDeviceLost())
        VPT_CheckVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(DestroyPathTracePipeline)");

    for (int i = 0; i < 5; i++)
    {
        if (s_OutputViews[i].view) { vkDestroyImageView(device, s_OutputViews[i].view, nullptr); s_OutputViews[i].view = VK_NULL_HANDLE; }
        s_OutputViews[i].image  = VK_NULL_HANDLE;
        s_OutputViews[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    for (int i = 0; i < 4; i++)
    {
        if (s_TexViews[i].view) { vkDestroyImageView(device, s_TexViews[i].view, nullptr); s_TexViews[i].view = VK_NULL_HANDLE; }
        s_TexViews[i].image = VK_NULL_HANDLE;
    }
    if (s_TexSampler) { vkDestroySampler(device, s_TexSampler, nullptr); s_TexSampler = VK_NULL_HANDLE; }

    if (s_PathUBOMapped) { vkUnmapMemory(device, s_PathUBOMemory); s_PathUBOMapped = nullptr; }
    if (s_PathUBOBuffer) { vkDestroyBuffer(device, s_PathUBOBuffer, nullptr); s_PathUBOBuffer = VK_NULL_HANDLE; }
    if (s_PathUBOMemory) { vkFreeMemory(device, s_PathUBOMemory, nullptr); s_PathUBOMemory = VK_NULL_HANDLE; }

    // SBT 与 pipeline 同生共死：先 SBT 后 pipeline
    VPTRt::DestroyBindingTable(device, s_PathSbt);

    if (s_PathDescPool) { vkDestroyDescriptorPool(device, s_PathDescPool, nullptr); s_PathDescPool = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < kDescRingSize; i++) s_PathDescSets[i] = VK_NULL_HANDLE;
    s_PathDescSet    = VK_NULL_HANDLE;
    s_DescRingCursor = 0;
    if (s_PathPipeline) { vkDestroyPipeline(device, s_PathPipeline, nullptr); s_PathPipeline = VK_NULL_HANDLE; }
    if (s_PathPipeLayout) { vkDestroyPipelineLayout(device, s_PathPipeLayout, nullptr); s_PathPipeLayout = VK_NULL_HANDLE; }
    if (s_PathDSLayout) { vkDestroyDescriptorSetLayout(device, s_PathDSLayout, nullptr); s_PathDSLayout = VK_NULL_HANDLE; }

    s_PathReady = false;
    s_PathInitFailed = false;
    s_PathInitError[0] = '\0';
    fprintf(stderr, "[VPT] PathTrace RT pipeline destroyed.\n");
}

bool VPTDispatch::IsPathTraceReady() { return s_PathReady; }

bool VPTDispatch::IsPathTraceFailed() { return s_PathInitFailed; }

const char* VPTDispatch::PathTraceError() { return s_PathInitError; }
