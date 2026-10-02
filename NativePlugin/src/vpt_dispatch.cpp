// VulkanPathTracePlugin — Debug depth dispatch (Phase 2.6)
//
// Creates a compute pipeline from pre-compiled SPIR-V, binds TLAS + VkImage + UBO,
// dispatches, and writes results directly to Unity's RenderTexture via imageStore.
// No CPU readback — data stays entirely on GPU.

#include "vpt_dispatch.h"
#include "vpt_internal.h"
#include "vpt_scene.h"
#include "debug_depth_spv.h"
#include "pathtrace_spv.h"

#include <cstdio>
#include <cstring>
#include <vector>

// ═══════════════════════════════════════════════════════════
//  Pipeline state (created once)
// ═══════════════════════════════════════════════════════════

static VkDescriptorSetLayout s_DSLayout    = VK_NULL_HANDLE;
static VkPipelineLayout      s_PipeLayout  = VK_NULL_HANDLE;
static VkPipeline            s_Pipeline    = VK_NULL_HANDLE;
static VkDescriptorPool      s_DescPool    = VK_NULL_HANDLE;
static VkDescriptorSet       s_DescSet      = VK_NULL_HANDLE;

static VkBuffer      s_UBOBuffer  = VK_NULL_HANDLE;
static VkDeviceMemory s_UBOMemory  = VK_NULL_HANDLE;
static void*          s_UBOMapped  = nullptr;

// Cached output ImageView (recreated when Unity's VkImage changes between frames)
static VkImageView   s_OutputImageView = VK_NULL_HANDLE;
static VkImage        s_CachedImage     = VK_NULL_HANDLE;

// Reusable command pool (reset each frame instead of create/destroy)
static VkCommandPool  s_CmdPool         = VK_NULL_HANDLE;

static bool s_Ready = false;

// ═══════════════════════════════════════════════════════════
//  PathTrace pipeline state (Phase 3)
// ═══════════════════════════════════════════════════════════
static VkDescriptorSetLayout s_PathDSLayout   = VK_NULL_HANDLE;
static VkPipelineLayout      s_PathPipeLayout = VK_NULL_HANDLE;
static VkPipeline            s_PathPipeline   = VK_NULL_HANDLE;
static VkDescriptorPool      s_PathDescPool   = VK_NULL_HANDLE;
static VkDescriptorSet       s_PathDescSet     = VK_NULL_HANDLE;
static VkBuffer      s_PathUBOBuffer  = VK_NULL_HANDLE;
static VkDeviceMemory s_PathUBOMemory  = VK_NULL_HANDLE;
static void*          s_PathUBOMapped  = nullptr;
static bool s_PathReady = false;

// Cached ImageViews for 5 output textures (recreated when VkImage changes)
struct ImageViewCache {
    VkImageView view = VK_NULL_HANDLE;
    VkImage     image = VK_NULL_HANDLE;
};
// [0]=output, [1]=gbuffer0, [2]=gbuffer1, [3]=diffuseAlbedo(RR), [4]=specularAlbedo(RR)
static ImageViewCache s_OutputViews[5];

// Cached ImageViews for 4 texture arrays (Phase 4)
static ImageViewCache s_TexViews[4]; // [0]=baseColor, [1]=metallicRough, [2]=normal, [3]=emissive
static VkSampler s_TexSampler = VK_NULL_HANDLE;

// ═══════════════════════════════════════════════════════════
//  Debug camera UBO layout (matches GLSL std140 + row_major)
// ═══════════════════════════════════════════════════════════

struct DebugCameraUBO
{
    float viewInv[16];      // 64B (row-major, matches Unity Matrix4x4)
    float projInv[16];      // 64B
    float posFar[4];         // 16B (xyz = position, w = farPlane)
    float resolution[2];    // 8B
    float nearPlane;         // 4B
    float pad;               // 4B
    // Total: 160B
};

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
    uint32_t uboPad0;            // 4B
    uint32_t uboPad1;            // 4B
    // Total: 208B
};

// ═══════════════════════════════════════════════════════════
//  Pipeline initialization
// ═══════════════════════════════════════════════════════════

void VPTDispatch::InitPipeline(VkDevice device, VkPhysicalDevice gpu)
{
    if (s_Ready) return;

    // 1. Descriptor set layout: TLAS + Storage Image + UBO
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding            = 0;
    bindings[0].descriptorType    = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding            = 1;
    bindings[1].descriptorType    = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding            = 2;
    bindings[2].descriptorType    = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount    = 1;
    bindings[2].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 3;
    dslCI.pBindings    = bindings;

    VkResult res = vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &s_DSLayout);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] vkCreateDescriptorSetLayout failed: %d\n", res); return; }

    // 2. Pipeline layout
    VkPipelineLayoutCreateInfo plCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &s_DSLayout;

    res = vkCreatePipelineLayout(device, &plCI, nullptr, &s_PipeLayout);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] vkCreatePipelineLayout failed: %d\n", res); return; }

    // 3. Shader module from embedded SPIR-V
    VkShaderModuleCreateInfo smCI{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smCI.codeSize = g_DebugDepthSpvSize;
    smCI.pCode    = g_DebugDepthSpv;

    VkShaderModule shaderModule;
    res = vkCreateShaderModule(device, &smCI, nullptr, &shaderModule);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] vkCreateShaderModule failed: %d\n", res); return; }

    // 4. Compute pipeline
    VkComputePipelineCreateInfo cpCI{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module  = shaderModule;
    cpCI.stage.pName    = "main";
    cpCI.layout        = s_PipeLayout;

    res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &s_Pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] vkCreateComputePipelines failed: %d\n", res); return; }

    // 5. Descriptor pool + set
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = 1;

    VkDescriptorPoolCreateInfo dpCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.maxSets       = 1;
    dpCI.poolSizeCount = 3;
    dpCI.pPoolSizes    = poolSizes;

    res = vkCreateDescriptorPool(device, &dpCI, nullptr, &s_DescPool);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] vkCreateDescriptorPool failed: %d\n", res); return; }

    VkDescriptorSetAllocateInfo dsAI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsAI.descriptorPool     = s_DescPool;
    dsAI.descriptorSetCount = 1;
    dsAI.pSetLayouts       = &s_DSLayout;

    res = vkAllocateDescriptorSets(device, &dsAI, &s_DescSet);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] vkAllocateDescriptorSets failed: %d\n", res); return; }

    // 6. Camera UBO (persistent, mapped)
    res = CreateHostBuffer(device, gpu, sizeof(DebugCameraUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, s_UBOBuffer, s_UBOMemory, &s_UBOMapped);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] UBO create failed: %d\n", res); return; }

    // 7. Command pool (reused per frame, no create/destroy overhead)
    VkCommandPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = VPT_GetInstance().queueFamilyIndex;
    res = vkCreateCommandPool(device, &poolCI, nullptr, &s_CmdPool);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Command pool create failed: %d\n", res); return; }

    s_Ready = true;
    fprintf(stderr, "[VPT] Debug pipeline initialized.\n");
}

// ═══════════════════════════════════════════════════════════
//  Dispatch — writes directly to Unity's RenderTexture (no CPU readback)
// ═══════════════════════════════════════════════════════════

void VPTDispatch::DispatchDebugDepth(
    void* nativeTexturePtr,
    int32_t width, int32_t height,
    const VPT_CameraData& cameraData)
{
    if (!s_Ready || !nativeTexturePtr || width <= 0 || height <= 0)
        return;

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();

    VkAccelerationStructureKHR tlas = VPTScene::GetTLAS();
    if (tlas == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] DispatchDebugDepth: TLAS not ready\n");
        return;
    }

    // ── Access Unity's RenderTexture as VkImage (ObserveOnly: just query, no barriers) ──
    UnityVulkanImage vkImg{};
    bool ok = vulkan->AccessTexture(nativeTexturePtr, UnityVulkanWholeImage,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        kUnityVulkanResourceAccess_ObserveOnly, &vkImg);
    if (!ok || vkImg.image == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] AccessTexture failed\n");
        return;
    }

    // Recreate ImageView if the underlying VkImage changed (Unity may reallocate textures)
    if (s_CachedImage != vkImg.image)
    {
        if (s_OutputImageView)
        {
            vkDestroyImageView(device, s_OutputImageView, nullptr);
            s_OutputImageView = VK_NULL_HANDLE;
        }

        VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivCI.image = vkImg.image;
        ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCI.format = vkImg.format;
        ivCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCI.subresourceRange.baseMipLevel = 0;
        ivCI.subresourceRange.levelCount = 1;
        ivCI.subresourceRange.baseArrayLayer = 0;
        ivCI.subresourceRange.layerCount = 1;

        VkResult res = vkCreateImageView(device, &ivCI, nullptr, &s_OutputImageView);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPT] vkCreateImageView failed: %d\n", res);
            return;
        }
        s_CachedImage = vkImg.image;
    }

    // ── Fill camera UBO (host-visible, persistent mapping) ──
    DebugCameraUBO uboData{};
    memcpy(uboData.viewInv, cameraData.viewInv, sizeof(float) * 16);
    memcpy(uboData.projInv, cameraData.projInv, sizeof(float) * 16);
    uboData.posFar[0] = cameraData.position[0];
    uboData.posFar[1] = cameraData.position[1];
    uboData.posFar[2] = cameraData.position[2];
    uboData.posFar[3] = cameraData.farPlane;
    uboData.resolution[0] = (float)width;
    uboData.resolution[1] = (float)height;
    uboData.nearPlane = cameraData.nearPlane;
    memcpy(s_UBOMapped, &uboData, sizeof(DebugCameraUBO));

    // ── Update descriptor set (TLAS + StorageImage + UBO) ──
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = VK_NULL_HANDLE;
    imgInfo.imageView = s_OutputImageView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = s_UBOBuffer;
    uboInfo.offset = 0;
    uboInfo.range  = sizeof(DebugCameraUBO);

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &asInfo;
    writes[0].dstSet = s_DescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = s_DescSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &imgInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = s_DescSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &uboInfo;

    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    // ── Command buffer (allocated from reusable pool) ──
    VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool = s_CmdPool;
    allocCI.commandBufferCount = 1;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocCI, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Barrier 1: UBO host write → compute shader read
    VkBufferMemoryBarrier uboBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    uboBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    uboBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    uboBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uboBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uboBarrier.buffer = s_UBOBuffer;
    uboBarrier.offset = 0;
    uboBarrier.size = sizeof(DebugCameraUBO);

    // Barrier 2: Image UNDEFINED → GENERAL (prepare for compute write, discard old contents)
    VkImageMemoryBarrier imgToGeneral{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    imgToGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgToGeneral.image = vkImg.image;
    imgToGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgToGeneral.subresourceRange.baseMipLevel = 0;
    imgToGeneral.subresourceRange.levelCount = 1;
    imgToGeneral.subresourceRange.baseArrayLayer = 0;
    imgToGeneral.subresourceRange.layerCount = 1;
    imgToGeneral.srcAccessMask = 0;
    imgToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &uboBarrier, 1, &imgToGeneral);

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_PipeLayout, 0, 1, &s_DescSet, 0, nullptr);

    uint32_t gx = (width + 7) / 8;
    uint32_t gy = (height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier 3: Image GENERAL → SHADER_READ_ONLY_OPTIMAL (for Unity Blit)
    VkImageMemoryBarrier imgToReadOnly{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    imgToReadOnly.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgToReadOnly.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgToReadOnly.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgToReadOnly.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgToReadOnly.image = vkImg.image;
    imgToReadOnly.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgToReadOnly.subresourceRange.baseMipLevel = 0;
    imgToReadOnly.subresourceRange.levelCount = 1;
    imgToReadOnly.subresourceRange.baseArrayLayer = 0;
    imgToReadOnly.subresourceRange.layerCount = 1;
    imgToReadOnly.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    imgToReadOnly.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &imgToReadOnly);

    vkEndCommandBuffer(cmd);

    // Submit + wait (ensures compute finishes before Unity uses the image)
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkResult qRes = vkQueueSubmit(inst.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst.graphicsQueue);
    if (qRes != VK_SUCCESS) {
        fprintf(stderr, "[VPT] DispatchDebugDepth: vkQueueSubmit failed: %d\n", qRes);
    }

    vkFreeCommandBuffers(device, s_CmdPool, 1, &cmd);
}

// ═══════════════════════════════════════════════════════════
//  Cleanup
// ═══════════════════════════════════════════════════════════

void VPTDispatch::DestroyPipeline(VkDevice device)
{
    if (!s_Ready) return;
    vkDeviceWaitIdle(device);

    if (s_OutputImageView) { vkDestroyImageView(device, s_OutputImageView, nullptr); s_OutputImageView = VK_NULL_HANDLE; }
    s_CachedImage = VK_NULL_HANDLE;

    if (s_CmdPool) { vkDestroyCommandPool(device, s_CmdPool, nullptr); s_CmdPool = VK_NULL_HANDLE; }

    if (s_UBOMapped) { vkUnmapMemory(device, s_UBOMemory); s_UBOMapped = nullptr; }
    if (s_UBOBuffer) { vkDestroyBuffer(device, s_UBOBuffer, nullptr); s_UBOBuffer = VK_NULL_HANDLE; }
    if (s_UBOMemory) { vkFreeMemory(device, s_UBOMemory, nullptr); s_UBOMemory = VK_NULL_HANDLE; }

    if (s_DescPool) { vkDestroyDescriptorPool(device, s_DescPool, nullptr); s_DescPool = VK_NULL_HANDLE; }
    if (s_Pipeline) { vkDestroyPipeline(device, s_Pipeline, nullptr); s_Pipeline = VK_NULL_HANDLE; }
    if (s_PipeLayout) { vkDestroyPipelineLayout(device, s_PipeLayout, nullptr); s_PipeLayout = VK_NULL_HANDLE; }
    if (s_DSLayout) { vkDestroyDescriptorSetLayout(device, s_DSLayout, nullptr); s_DSLayout = VK_NULL_HANDLE; }

    s_Ready = false;
    fprintf(stderr, "[VPT] Debug pipeline destroyed.\n");
}

bool VPTDispatch::IsPipelineReady() { return s_Ready; }

// ═══════════════════════════════════════════════════════════
//  PathTrace pipeline initialization (Phase 3)
//  10 bindings: TLAS + 3 images + UBO + 5 SSBOs
// ═══════════════════════════════════════════════════════════

void VPTDispatch::InitPathTracePipeline(VkDevice device, VkPhysicalDevice gpu)
{
    if (s_PathReady) return;

    // 1. Descriptor set layout: 16 bindings (10 + 2 albedo images + 4 texture arrays)
    VkDescriptorSetLayoutBinding bindings[16] = {};
    bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1; bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[1].descriptorCount = 1; bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2; bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[2].descriptorCount = 1; bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3; bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[3].descriptorCount = 1; bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4; bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; bindings[4].descriptorCount = 1; bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5; bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[5].descriptorCount = 1; bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[6].binding = 6; bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[6].descriptorCount = 1; bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[7].binding = 7; bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[7].descriptorCount = 1; bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[8].binding = 8; bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[8].descriptorCount = 1; bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[9].binding = 9; bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[9].descriptorCount = 1; bindings[9].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[10].binding = 10; bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[10].descriptorCount = 1; bindings[10].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[11].binding = 11; bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[11].descriptorCount = 1; bindings[11].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[12].binding = 12; bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[12].descriptorCount = 1; bindings[12].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[13].binding = 13; bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bindings[13].descriptorCount = 1; bindings[13].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // DLSS RR albedo G-buffers（pathtrace.glsl binding 14/15，rgba16f）
    bindings[14].binding = 14; bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[14].descriptorCount = 1; bindings[14].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[15].binding = 15; bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; bindings[15].descriptorCount = 1; bindings[15].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 16;
    dslCI.pBindings    = bindings;
    VkResult res = vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &s_PathDSLayout);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkCreateDescriptorSetLayout failed: %d\n", res); return; }

    // 2. Pipeline layout
    VkPipelineLayoutCreateInfo plCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &s_PathDSLayout;
    res = vkCreatePipelineLayout(device, &plCI, nullptr, &s_PathPipeLayout);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkCreatePipelineLayout failed: %d\n", res); return; }

    // 3. Shader module from embedded SPIR-V
    VkShaderModuleCreateInfo smCI{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smCI.codeSize = g_PathTraceSpv_size;
    smCI.pCode    = g_PathTraceSpv;
    VkShaderModule shaderModule;
    res = vkCreateShaderModule(device, &smCI, nullptr, &shaderModule);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkCreateShaderModule failed: %d\n", res); return; }

    // 4. Compute pipeline
    VkComputePipelineCreateInfo cpCI{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module  = shaderModule;
    cpCI.stage.pName    = "main";
    cpCI.layout        = s_PathPipeLayout;
    res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &s_PathPipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkCreateComputePipelines failed: %d\n", res); return; }

    // 5. Descriptor pool + set
    VkDescriptorPoolSize poolSizes[5] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;              poolSizes[1].descriptorCount = 5;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;             poolSizes[2].descriptorCount = 1;
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;             poolSizes[3].descriptorCount = 5;
    poolSizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;     poolSizes[4].descriptorCount = 4;

    VkDescriptorPoolCreateInfo dpCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.maxSets       = 1;
    dpCI.poolSizeCount = 5;
    dpCI.pPoolSizes    = poolSizes;
    res = vkCreateDescriptorPool(device, &dpCI, nullptr, &s_PathDescPool);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkCreateDescriptorPool failed: %d\n", res); return; }

    VkDescriptorSetAllocateInfo dsAI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsAI.descriptorPool     = s_PathDescPool;
    dsAI.descriptorSetCount = 1;
    dsAI.pSetLayouts       = &s_PathDSLayout;
    res = vkAllocateDescriptorSets(device, &dsAI, &s_PathDescSet);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkAllocateDescriptorSets failed: %d\n", res); return; }

    // 6. Camera UBO (persistent, mapped)
    res = CreateHostBuffer(device, gpu, sizeof(PathTraceCameraUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, s_PathUBOBuffer, s_PathUBOMemory, &s_PathUBOMapped);
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: UBO create failed: %d\n", res); return; }

    // 7. Texture sampler (shared, linear filtering + repeat wrap)
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
    if (res != VK_SUCCESS) { fprintf(stderr, "[VPT] Path: vkCreateSampler failed: %d\n", res); return; }

    s_PathReady = true;
    fprintf(stderr, "[VPT] PathTrace pipeline initialized (16 bindings).\n");
}

// ═══════════════════════════════════════════════════════════
//  Helper: access Unity texture and ensure ImageView exists
// ═══════════════════════════════════════════════════════════
static bool EnsureImageView(IUnityGraphicsVulkan* vulkan, VkDevice device,
                           void* nativeTex, int slot)
{
    UnityVulkanImage vkImg{};
    bool ok = vulkan->AccessTexture(nativeTex, UnityVulkanWholeImage,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        kUnityVulkanResourceAccess_ObserveOnly, &vkImg);
    if (!ok || vkImg.image == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] Path: AccessTexture failed for slot %d\n", slot);
        return false;
    }

    if (s_OutputViews[slot].image != vkImg.image)
    {
        if (s_OutputViews[slot].view)
            vkDestroyImageView(device, s_OutputViews[slot].view, nullptr);

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
        s_OutputViews[slot].image = vkImg.image;
    }
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

    UnityVulkanImage vkImg{};
    bool ok = vulkan->AccessTexture(
        nativeTex, UnityVulkanWholeImage,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        kUnityVulkanResourceAccess_ObserveOnly, &vkImg);
    if (!ok || vkImg.image == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] Path: AccessTexture failed for tex slot %d\n", slot);
        return false;
    }

    // Recreate view only when the underlying VkImage changes
    if (s_TexViews[slot].image != vkImg.image)
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
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
//  DispatchPathTrace — multi-bounce PBR path tracing
// ═══════════════════════════════════════════════════════════

void VPTDispatch::DispatchPathTrace(
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

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();

    VkAccelerationStructureKHR tlas = VPTScene::GetTLAS();
    if (tlas == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] DispatchPathTrace: TLAS not ready\n");
        return;
    }

    // ── Access 5 Unity RenderTextures as VkImages (write) ──
    if (!EnsureImageView(vulkan, device, outputPtr, 0)) return;
    if (!EnsureImageView(vulkan, device, gbuf0Ptr,  1)) return;
    if (!EnsureImageView(vulkan, device, gbuf1Ptr,  2)) return;
    if (!EnsureImageView(vulkan, device, diffuseAlbedoPtr,  3)) return;
    if (!EnsureImageView(vulkan, device, specularAlbedoPtr, 4)) return;

    // ── Access 4 Unity Texture2DArrays as VkImages (read) ──
    if (baseColorPtr)    EnsureTextureArrayView(vulkan, device, baseColorPtr,    0);
    if (metallicRoughPtr) EnsureTextureArrayView(vulkan, device, metallicRoughPtr, 1);
    if (normalPtr)       EnsureTextureArrayView(vulkan, device, normalPtr,       2);
    if (emissivePtr)     EnsureTextureArrayView(vulkan, device, emissivePtr,     3);

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
    ubo.frameMaxDepth[2] = 0;
    ubo.frameMaxDepth[3] = 0;
    ubo.ambientPad[0] = cameraData.ambientColor[0];
    ubo.ambientPad[1] = cameraData.ambientColor[1];
    ubo.ambientPad[2] = cameraData.ambientColor[2];
    ubo.ambientPad[3] = cameraData.pad;
    // lightCount and samplesPerPixel will be set by caller via cameraData... 
    // Actually VPT_CameraData doesn't have lightCount/SPP, so we get from scene or use defaults
    ubo.lightCount = lightCount;
    ubo.samplesPerPixel = samplesPerPixel;
    // Safety: if light SSBO was never created (e.g. lights not pushed during scene update),
    // force lightCount to 0 so the shader doesn't read from an invalid SSBO
    if (VPTScene::GetLightBuffer() == VK_NULL_HANDLE)
        ubo.lightCount = 0;
    memcpy(s_PathUBOMapped, &ubo, sizeof(PathTraceCameraUBO));

    // ── Update descriptor set: TLAS + 3 images + UBO + 5 SSBOs ──
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

    // ── Texture array descriptor info (4 combined image samplers) ──
    VkDescriptorImageInfo texInfos[4] = {};
    for (int i = 0; i < 4; i++)
    {
        texInfos[i].sampler     = s_TexSampler;
        texInfos[i].imageView   = s_TexViews[i].view;
        texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkWriteDescriptorSet writes[16] = {};
    // binding 0: TLAS
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &asInfo;
    writes[0].dstSet = s_PathDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    // bindings 1-3: storage images
    for (int i = 0; i < 5; i++)
    {
        writes[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1 + i].dstSet = s_PathDescSet;
        writes[1 + i].dstBinding = 1 + i;
        writes[1 + i].descriptorCount = 1;
        writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1 + i].pImageInfo = &imgInfos[i];
    }

    // binding 4: UBO
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = s_PathDescSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &uboInfo;

    // bindings 5-9: SSBOs
    for (int i = 0; i < 5; i++)
    {
        writes[5 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5 + i].dstSet = s_PathDescSet;
        writes[5 + i].dstBinding = 5 + i;
        writes[5 + i].descriptorCount = 1;
        writes[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5 + i].pBufferInfo = &ssboInfos[i];
    }

    // bindings 10-13: texture arrays (combined image sampler)
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

    vkUpdateDescriptorSets(device, 16, writes, 0, nullptr);

    // ── Command buffer ──
    VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool = s_CmdPool;
    allocCI.commandBufferCount = 1;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocCI, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Barrier 1: UBO host write → compute read
    VkBufferMemoryBarrier uboBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    uboBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    uboBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    uboBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uboBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uboBarrier.buffer = s_PathUBOBuffer;
    uboBarrier.offset = 0;
    uboBarrier.size = sizeof(PathTraceCameraUBO);

    // Barrier 2: 3 images UNDEFINED → GENERAL
    VkImageMemoryBarrier imgBarriers[5] = {};
    for (int i = 0; i < 5; i++)
    {
        imgBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imgBarriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarriers[i].image = s_OutputViews[i].image;
        imgBarriers[i].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        imgBarriers[i].subresourceRange.baseMipLevel   = 0;
        imgBarriers[i].subresourceRange.levelCount     = 1;
        imgBarriers[i].subresourceRange.baseArrayLayer = 0;
        imgBarriers[i].subresourceRange.layerCount     = 1;
        imgBarriers[i].srcAccessMask = 0;
        imgBarriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &uboBarrier, 5, imgBarriers);

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_PathPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_PathPipeLayout, 0, 1, &s_PathDescSet, 0, nullptr);

    uint32_t gx = (width + 7) / 8;
    uint32_t gy = (height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier 3: 3 images GENERAL → SHADER_READ_ONLY_OPTIMAL
    for (int i = 0; i < 5; i++)
    {
        imgBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imgBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 5, imgBarriers);

    vkEndCommandBuffer(cmd);

    // Submit + wait
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkResult qRes = vkQueueSubmit(inst.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst.graphicsQueue);
    if (qRes != VK_SUCCESS)
        fprintf(stderr, "[VPT] DispatchPathTrace: vkQueueSubmit failed: %d\n", qRes);

    vkFreeCommandBuffers(device, s_CmdPool, 1, &cmd);
}

// ═══════════════════════════════════════════════════════════
//  Plan B: Two-phase dispatch (eliminates vkQueueWaitIdle)
//  PrepareDispatch + RenderCallback replace DispatchPathTrace.
// ═══════════════════════════════════════════════════════════

// Pending dispatch state (set by PrepareDispatch, consumed by RenderCallback)
static int32_t  s_PendingWidth   = 0;
static int32_t  s_PendingHeight  = 0;
static bool     s_DispatchPending = false;

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

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    VkDevice device = VPT_GetInstance().device;

    // ── Access 5 Unity RenderTextures as VkImages (ObserveOnly: query handles only) ──
    if (!EnsureImageView(vulkan, device, outputPtr, 0)) return;
    if (!EnsureImageView(vulkan, device, gbuf0Ptr,  1)) return;
    if (!EnsureImageView(vulkan, device, gbuf1Ptr,  2)) return;
    if (!EnsureImageView(vulkan, device, diffuseAlbedoPtr,  3)) return;
    if (!EnsureImageView(vulkan, device, specularAlbedoPtr, 4)) return;

    // ── Access 4 Unity Texture2DArrays as VkImages (read) ──
    if (baseColorPtr)    EnsureTextureArrayView(vulkan, device, baseColorPtr,    0);
    if (metallicRoughPtr) EnsureTextureArrayView(vulkan, device, metallicRoughPtr, 1);
    if (normalPtr)       EnsureTextureArrayView(vulkan, device, normalPtr,       2);
    if (emissivePtr)     EnsureTextureArrayView(vulkan, device, emissivePtr,     3);

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
    ubo.frameMaxDepth[2] = 0;
    ubo.frameMaxDepth[3] = 0;
    ubo.ambientPad[0] = cameraData.ambientColor[0];
    ubo.ambientPad[1] = cameraData.ambientColor[1];
    ubo.ambientPad[2] = cameraData.ambientColor[2];
    ubo.ambientPad[3] = cameraData.pad;
    ubo.lightCount = lightCount;
    ubo.samplesPerPixel = samplesPerPixel;
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

    VkDescriptorImageInfo texInfos[4] = {};
    for (int i = 0; i < 4; i++)
    {
        texInfos[i].sampler     = s_TexSampler;
        texInfos[i].imageView   = s_TexViews[i].view;
        texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkWriteDescriptorSet writes[16] = {};
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

    vkUpdateDescriptorSets(device, 16, writes, 0, nullptr);

    // ── Store pending dispatch dimensions for RenderCallback ──
    s_PendingWidth    = width;
    s_PendingHeight   = height;
    s_DispatchPending = true;
}

// ── Phase 2: RenderCallback ────────────────────────────────
// Called by Unity during command buffer execution via IssuePluginEvent.
// Gets Unity's VkCommandBuffer via CommandRecordingState, records
// barriers + dispatch into it. No vkQueueSubmit / vkQueueWaitIdle.

void VPTDispatch::RenderCallback(int eventID)
{
    if (eventID != EVENT_PATHTRACE || !s_DispatchPending)
        return;

    s_DispatchPending = false;  // consume pending state

    if (!s_PathReady)
        return;

    IUnityGraphicsVulkan* vulkan = VPT_GetUnityVulkan();
    if (!vulkan)
        return;

    // ── Get Unity's current VkCommandBuffer ──
    // DontCare mode: we record into Unity's current command buffer, no direct queue submit.
    // (Allow mode would disable access to Unity's command buffer, returning null)
    UnityVulkanRecordingState recState{};
    if (!vulkan->CommandRecordingState(&recState, kUnityVulkanGraphicsQueueAccess_DontCare))
    {
        fprintf(stderr, "[VPT] RenderCallback: CommandRecordingState failed\n");
        return;
    }

    VkCommandBuffer cmd = recState.commandBuffer;
    if (cmd == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[VPT] RenderCallback: Unity command buffer is null\n");
        return;
    }

    VkDevice device = VPT_GetInstance().device;

    // ── Barrier 1: UBO host write → compute read + 3 images → GENERAL ──
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
        imgBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imgBarriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imgBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imgBarriers[i].image = s_OutputViews[i].image;
        imgBarriers[i].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        imgBarriers[i].subresourceRange.baseMipLevel   = 0;
        imgBarriers[i].subresourceRange.levelCount     = 1;
        imgBarriers[i].subresourceRange.baseArrayLayer = 0;
        imgBarriers[i].subresourceRange.layerCount     = 1;
        imgBarriers[i].srcAccessMask = 0;
        imgBarriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &uboBarrier, 5, imgBarriers);

    // ── Dispatch ──
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_PathPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_PathPipeLayout, 0, 1, &s_PathDescSet, 0, nullptr);

    uint32_t gx = (s_PendingWidth + 7) / 8;
    uint32_t gy = (s_PendingHeight + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier 2: 3 images GENERAL → SHADER_READ_ONLY (for Unity compute reads) ──
    for (int i = 0; i < 5; i++)
    {
        imgBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgBarriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        imgBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 5, imgBarriers);
}

bool VPTDispatch::IsDispatchPending() { return s_DispatchPending; }

// ═══════════════════════════════════════════════════════════
//  PathTrace pipeline cleanup
// ═══════════════════════════════════════════════════════════

void VPTDispatch::DestroyPathTracePipeline(VkDevice device)
{
    if (!s_PathReady) return;
    vkDeviceWaitIdle(device);

    for (int i = 0; i < 5; i++)
    {
        if (s_OutputViews[i].view) { vkDestroyImageView(device, s_OutputViews[i].view, nullptr); s_OutputViews[i].view = VK_NULL_HANDLE; }
        s_OutputViews[i].image = VK_NULL_HANDLE;
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

    if (s_PathDescPool) { vkDestroyDescriptorPool(device, s_PathDescPool, nullptr); s_PathDescPool = VK_NULL_HANDLE; }
    if (s_PathPipeline) { vkDestroyPipeline(device, s_PathPipeline, nullptr); s_PathPipeline = VK_NULL_HANDLE; }
    if (s_PathPipeLayout) { vkDestroyPipelineLayout(device, s_PathPipeLayout, nullptr); s_PathPipeLayout = VK_NULL_HANDLE; }
    if (s_PathDSLayout) { vkDestroyDescriptorSetLayout(device, s_PathDSLayout, nullptr); s_PathDSLayout = VK_NULL_HANDLE; }

    s_PathReady = false;
    fprintf(stderr, "[VPT] PathTrace pipeline destroyed.\n");
}

bool VPTDispatch::IsPathTraceReady() { return s_PathReady; }
