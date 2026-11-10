// VulkanPathTracePlugin — HDR 环境光贴图管理
//
// 创建 RGBA32F equirect VkImage + VkImageView + VkSampler，
// 构建 alias-method importance 采样表（EnvAccel SSBO）。
// 参考 nvpro_core2/nvvk/hdr_ibl.cpp（createEnvironmentAccel / buildAliasmap）。

#include "vpt_env.h"
#include "vpt_internal.h"
#include "vpt_scene.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

// ═══════════════════════════════════════════════════════════
//  EnvAccel 结构体（与 GLSL shader 中的 EnvAccel 对齐：8B/元素）
// ═══════════════════════════════════════════════════════════
struct EnvAccel
{
    uint32_t alias;
    float    q;
};
static_assert(sizeof(EnvAccel) == 8, "EnvAccel must be 8 bytes");

// ═══════════════════════════════════════════════════════════
//  静态资源
// ═══════════════════════════════════════════════════════════
static VkImage        s_EnvImage       = VK_NULL_HANDLE;
static VkDeviceMemory s_EnvImageMemory = VK_NULL_HANDLE;
static VkImageView    s_EnvImageView   = VK_NULL_HANDLE;
static VkSampler      s_EnvSampler     = VK_NULL_HANDLE;

static VkBuffer       s_EnvAccelBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_EnvAccelMemory = VK_NULL_HANDLE;

static uint32_t s_EnvWidth    = 0;
static uint32_t s_EnvHeight   = 0;
static float    s_EnvIntegral = 1.0f;
static bool     s_EnvReady    = false;

// 延迟销毁的 staging 资源（vkQueueWaitIdle 后清理）
static std::vector<VkBuffer>       s_PendingStagingBufs;
static std::vector<VkDeviceMemory> s_PendingStagingMems;

// ═══════════════════════════════════════════════════════════
//  Alias method（移植自 nvpro_core2/nvvk/hdr_ibl.cpp L244-317）
//  返回 importance 加权积分（用于 pdf 归一化）
// ═══════════════════════════════════════════════════════════
static float buildAliasMap(const std::vector<float>& data, std::vector<EnvAccel>& accel)
{
    uint32_t size = static_cast<uint32_t>(data.size());

    // 计算积分（importance_data 已按 solidAngle 加权，简单求和）
    float sum = 0.0f;
    for (uint32_t i = 0; i < size; i++)
        sum += data[i];
    if (sum == 0.0f) sum = 1.0f;

    // 每个 texel 的 q = radiance / average
    float inverseAverage = static_cast<float>(size) / sum;
    for (uint32_t i = 0; i < size; i++)
    {
        accel[i].q     = data[i] * inverseAverage;
        accel[i].alias = i;
    }

    // 分区：q < 1 从前往后，q >= 1 从后往前
    std::vector<uint32_t> partition(size);
    uint32_t s = 0, large = size;
    for (uint32_t i = 0; i < size; i++)
    {
        if (accel[i].q < 1.0f)
            partition[s++] = i;
        else
            partition[--large] = i;
    }

    // 关联低能量 texel 到高能量 alias
    for (s = 0; s < large && large < size; s++)
    {
        uint32_t smallIdx = partition[s];
        uint32_t highIdx  = partition[large];
        accel[smallIdx].alias = highIdx;
        float diff = 1.0f - accel[smallIdx].q;
        accel[highIdx].q -= diff;
        if (accel[highIdx].q < 1.0f)
            large++;
    }

    return sum;
}

// ═══════════════════════════════════════════════════════════
//  构建环境贴图 importance 采样加速结构（移植 createEnvironmentAccel）
//  输入：RGBA32F pixels (width*height*4)
//  输出：EnvAccel 数组 + integral
// ═══════════════════════════════════════════════════════════
static std::vector<EnvAccel> createEnvironmentAccel(
    float* pixels, uint32_t width, uint32_t height, float& integral)
{
    const uint32_t rx = width;
    const uint32_t ry = height;

    std::vector<EnvAccel> envAccel(rx * ry);
    std::vector<float>    importanceData(rx * ry);

    const float stepPhi   = 2.0f * 3.14159265359f / static_cast<float>(rx);
    const float stepTheta = 3.14159265359f / static_cast<float>(ry);
    float cosTheta0 = 1.0f;

    // 遍历每个 texel，计算 solidAngle × max(r,g,b)
    for (uint32_t y = 0; y < ry; y++)
    {
        float theta1     = static_cast<float>(y + 1) * stepTheta;
        float cosTheta1  = std::cos(theta1);
        float area       = (cosTheta0 - cosTheta1) * stepPhi;  // solid angle
        cosTheta0        = cosTheta1;

        for (uint32_t x = 0; x < rx; x++)
        {
            uint32_t idx   = y * rx + x;
            uint32_t idx4  = idx * 4;
            float r = pixels[idx4 + 0];
            float g = pixels[idx4 + 1];
            float b = pixels[idx4 + 2];
            float maxComp = std::max(r, std::max(g, b));
            importanceData[idx] = area * std::max(maxComp, 0.0f);
        }
    }

    integral = buildAliasMap(importanceData, envAccel);
    if (integral == 0.0f) integral = 1.0f;

    // 将 PDF 写入 alpha 通道（供 shader environmentSample 读取 texel.a）
    // pdf_i = max(r,g,b)_i / integral
    const float invIntegral = 1.0f / integral;
    for (uint32_t i = 0; i < rx * ry; i++)
    {
        uint32_t idx4 = i * 4;
        float maxComp = std::max(pixels[idx4], std::max(pixels[idx4 + 1], pixels[idx4 + 2]));
        pixels[idx4 + 3] = maxComp * invIntegral;
    }

    return envAccel;
}

// ═══════════════════════════════════════════════════════════
//  SetEnvironmentMap — 主入口
// ═══════════════════════════════════════════════════════════
void VPTEnv::SetEnvironmentMap(const float* pixels, uint32_t width, uint32_t height)
{
    if (!pixels || width == 0 || height == 0) return;

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    VkPhysicalDevice gpu = inst.physicalDevice;

    // 销毁上轮资源
    DestroyResources();
    vkDeviceWaitIdle(device);

    // ── 构建 alias 表（CPU 端）──
    // 复制像素数据（createEnvironmentAccel 会将 PDF 写入 alpha 通道）
    VkDeviceSize pixelCount = VkDeviceSize(width) * height * 4;
    std::vector<float> pixelData(pixelCount);
    memcpy(pixelData.data(), pixels, static_cast<size_t>(pixelCount * sizeof(float)));

    // 翻转行序：Unity GetPixels 从底行返回（row0=底部），
    // Vulkan vkCmdCopyBufferToImage 将 row0 当顶部。翻转后 alias 表与纹理一致。
    for (uint32_t y = 0; y < height / 2; y++)
    {
        float* rowTop = pixelData.data() + y * width * 4;
        float* rowBot = pixelData.data() + (height - 1 - y) * width * 4;
        std::swap_ranges(rowTop, rowTop + width * 4, rowBot);
    }

    float integral = 1.0f;
    std::vector<EnvAccel> envAccel = createEnvironmentAccel(pixelData.data(), width, height, integral);
    s_EnvIntegral = integral;

    // ── 创建 command buffer ──
    VkCommandPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = inst.queueFamilyIndex;
    VkCommandPool pool;
    VkResult res = vkCreateCommandPool(device, &poolCI, nullptr, &pool);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPTEnv] vkCreateCommandPool failed: %d\n", res);
        return;
    }

    VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool = pool;
    allocCI.commandBufferCount = 1;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocCI, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    s_PendingStagingBufs.clear();
    s_PendingStagingMems.clear();

    // 前置声明：避免 goto 跳过变量初始化（C2362）
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };

    // ── 1. 创建 RGBA32F VkImage + staging 上传 ──
    VkDeviceSize imageSize = VkDeviceSize(width) * height * 4 * sizeof(float);

    {
        VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imgCI.imageType     = VK_IMAGE_TYPE_2D;
        imgCI.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
        imgCI.extent.width  = width;
        imgCI.extent.height = height;
        imgCI.extent.depth  = 1;
        imgCI.mipLevels     = 1;
        imgCI.arrayLayers   = 1;
        imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        res = vkCreateImage(device, &imgCI, nullptr, &s_EnvImage);
        if (res != VK_SUCCESS) { fprintf(stderr, "[VPTEnv] vkCreateImage failed: %d\n", res); goto cleanup_pool; }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, s_EnvImage, &memReq);
        VkMemoryAllocateInfo imgAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        imgAlloc.allocationSize = memReq.size;
        imgAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        res = vkAllocateMemory(device, &imgAlloc, nullptr, &s_EnvImageMemory);
        if (res != VK_SUCCESS) { fprintf(stderr, "[VPTEnv] vkAllocateMemory image failed: %d\n", res); goto cleanup_pool; }
        vkBindImageMemory(device, s_EnvImage, s_EnvImageMemory, 0);

        // staging buffer for image
        VkBuffer       imgStaging = VK_NULL_HANDLE;
        VkDeviceMemory imgStagingMem = VK_NULL_HANDLE;
        VkBufferCreateInfo ibci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ibci.size = imageSize;
        ibci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ibci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &ibci, nullptr, &imgStaging);
        VkMemoryRequirements isReq;
        vkGetBufferMemoryRequirements(device, imgStaging, &isReq);
        VkMemoryAllocateInfo isAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        isAlloc.allocationSize = isReq.size;
        isAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, isReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &isAlloc, nullptr, &imgStagingMem);
        vkBindBufferMemory(device, imgStaging, imgStagingMem, 0);

        void* mapped = nullptr;
        vkMapMemory(device, imgStagingMem, 0, imageSize, 0, &mapped);
        memcpy(mapped, pixelData.data(), static_cast<size_t>(imageSize));
        vkUnmapMemory(device, imgStagingMem);

        // layout transition UNDEFINED → TRANSFER_DST
        VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = s_EnvImage;
        toTransfer.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel   = 0;
        toTransfer.subresourceRange.levelCount     = 1;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount     = 1;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        // copy buffer → image
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel       = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount     = 1;
        copyRegion.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(cmd, imgStaging, s_EnvImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // layout transition TRANSFER_DST → SHADER_READ_ONLY
        VkImageMemoryBarrier toShader{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = s_EnvImage;
        toShader.subresourceRange = toTransfer.subresourceRange;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);

        s_PendingStagingBufs.push_back(imgStaging);
        s_PendingStagingMems.push_back(imgStagingMem);
    }

    // ── 2. 创建 EnvAccel SSBO + staging 上传 ──
    {
        VkDeviceSize accelSize = VkDeviceSize(width) * height * sizeof(EnvAccel);

        // device-local 目标
        VkBufferCreateInfo dbCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        dbCI.size = accelSize;
        dbCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        dbCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &dbCI, nullptr, &s_EnvAccelBuffer);
        VkMemoryRequirements dReq;
        vkGetBufferMemoryRequirements(device, s_EnvAccelBuffer, &dReq);
        VkMemoryAllocateInfo dAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        dAlloc.allocationSize = dReq.size;
        dAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, dReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &dAlloc, nullptr, &s_EnvAccelMemory);
        vkBindBufferMemory(device, s_EnvAccelBuffer, s_EnvAccelMemory, 0);

        // staging
        VkBuffer       accelStaging = VK_NULL_HANDLE;
        VkDeviceMemory accelStagingMem = VK_NULL_HANDLE;
        VkBufferCreateInfo asCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        asCI.size = accelSize;
        asCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        asCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &asCI, nullptr, &accelStaging);
        VkMemoryRequirements asReq;
        vkGetBufferMemoryRequirements(device, accelStaging, &asReq);
        VkMemoryAllocateInfo asAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        asAlloc.allocationSize = asReq.size;
        asAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, asReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &asAlloc, nullptr, &accelStagingMem);
        vkBindBufferMemory(device, accelStaging, accelStagingMem, 0);

        void* amapped = nullptr;
        vkMapMemory(device, accelStagingMem, 0, accelSize, 0, &amapped);
        memcpy(amapped, envAccel.data(), static_cast<size_t>(accelSize));
        vkUnmapMemory(device, accelStagingMem);

        VkBufferCopy copyRegion{ 0, 0, accelSize };
        vkCmdCopyBuffer(cmd, accelStaging, s_EnvAccelBuffer, 1, &copyRegion);

        s_PendingStagingBufs.push_back(accelStaging);
        s_PendingStagingMems.push_back(accelStagingMem);
    }

    vkEndCommandBuffer(cmd);

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    res = vkQueueSubmit(inst.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst.graphicsQueue);
    if (res != VK_SUCCESS)
        fprintf(stderr, "[VPTEnv] vkQueueSubmit failed: %d\n", res);

    // 销毁 staging buffers（GPU 已 idle）
    for (size_t i = 0; i < s_PendingStagingBufs.size(); i++)
    {
        if (s_PendingStagingBufs[i]) vkDestroyBuffer(device, s_PendingStagingBufs[i], nullptr);
        if (s_PendingStagingMems[i]) vkFreeMemory(device, s_PendingStagingMems[i], nullptr);
    }
    s_PendingStagingBufs.clear();
    s_PendingStagingMems.clear();

    vkFreeCommandBuffers(device, pool, 1, &cmd);

cleanup_pool:
    vkDestroyCommandPool(device, pool, nullptr);

    // ── 3. 创建 ImageView ──
    if (s_EnvImage != VK_NULL_HANDLE)
    {
        VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivCI.image = s_EnvImage;
        ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCI.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ivCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCI.subresourceRange.baseMipLevel   = 0;
        ivCI.subresourceRange.levelCount     = 1;
        ivCI.subresourceRange.baseArrayLayer = 0;
        ivCI.subresourceRange.layerCount     = 1;
        res = vkCreateImageView(device, &ivCI, nullptr, &s_EnvImageView);
        if (res != VK_SUCCESS)
            fprintf(stderr, "[VPTEnv] vkCreateImageView failed: %d\n", res);
    }

    // ── 4. 创建 Sampler（linear + clamp-to-edge，适合 equirect）──
    if (s_EnvSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo samplerCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerCI.magFilter    = VK_FILTER_LINEAR;
        samplerCI.minFilter    = VK_FILTER_LINEAR;
        samplerCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.minLod       = 0.0f;
        samplerCI.maxLod       = VK_LOD_CLAMP_NONE;
        res = vkCreateSampler(device, &samplerCI, nullptr, &s_EnvSampler);
        if (res != VK_SUCCESS)
            fprintf(stderr, "[VPTEnv] vkCreateSampler failed: %d\n", res);
    }

    s_EnvWidth  = width;
    s_EnvHeight = height;
    s_EnvReady  = (s_EnvImage != VK_NULL_HANDLE && s_EnvImageView != VK_NULL_HANDLE &&
                   s_EnvAccelBuffer != VK_NULL_HANDLE);
    fprintf(stderr, "[VPTEnv] HDR env map created: %ux%u, integral=%.4f, ready=%d\n",
            width, height, s_EnvIntegral, (int)s_EnvReady);
}

// ═══════════════════════════════════════════════════════════
//  DestroyResources
// ═══════════════════════════════════════════════════════════
void VPTEnv::DestroyResources()
{
    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);

    if (s_EnvImageView)   { vkDestroyImageView(device, s_EnvImageView, nullptr);   s_EnvImageView = VK_NULL_HANDLE; }
    if (s_EnvImage)       { vkDestroyImage(device, s_EnvImage, nullptr);           s_EnvImage = VK_NULL_HANDLE; }
    if (s_EnvImageMemory) { vkFreeMemory(device, s_EnvImageMemory, nullptr);       s_EnvImageMemory = VK_NULL_HANDLE; }
    if (s_EnvAccelBuffer) { vkDestroyBuffer(device, s_EnvAccelBuffer, nullptr);    s_EnvAccelBuffer = VK_NULL_HANDLE; }
    if (s_EnvAccelMemory) { vkFreeMemory(device, s_EnvAccelMemory, nullptr);       s_EnvAccelMemory = VK_NULL_HANDLE; }
    if (s_EnvSampler)     { vkDestroySampler(device, s_EnvSampler, nullptr);       s_EnvSampler = VK_NULL_HANDLE; }
    s_EnvReady = false;
}

// ── Getter ──
bool        VPTEnv::IsReady()           { return s_EnvReady; }
VkImageView VPTEnv::GetEnvImageView()   { return s_EnvImageView; }
VkSampler   VPTEnv::GetEnvSampler()     { return s_EnvSampler; }
VkBuffer    VPTEnv::GetEnvAccelBuffer() { return s_EnvAccelBuffer; }
uint32_t    VPTEnv::GetEnvWidth()       { return s_EnvWidth; }
uint32_t    VPTEnv::GetEnvHeight()      { return s_EnvHeight; }
