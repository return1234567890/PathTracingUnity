// VulkanPathTracePlugin — HDR 环境光贴图管理
//
// 创建 RGBA32F equirect VkImage + VkImageView + VkSampler，
// 构建 alias-method importance 采样表（EnvAccel SSBO）。
// 参考 nvpro_core2/nvvk/hdr_ibl.cpp（createEnvironmentAccel / buildAliasmap）。

#include "vpt_env.h"
#include "vpt_internal.h"
#include "vpt_scene.h"
#include "vpt_trace.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>

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

// ── 延迟上传状态（Plan B：主线程只准备 CPU 数据，命令在渲染线程录制）──
static std::vector<float>    s_PendingPixels;
static std::vector<EnvAccel> s_PendingAccel;
static uint32_t              s_PendingWidth  = 0;
static uint32_t              s_PendingHeight = 0;
static bool                  s_EnvPending    = false;
static bool                  s_FallbackNeedsRecord = false;

// 主线程（SetEnvironmentMap）与渲染线程（RecordPendingUploads）共享上述状态
static std::recursive_mutex  s_EnvMutex;

// ═══════════════════════════════════════════════════════════
//  占位资源（HDR 未上传时 binding 16/17 仍要绑合法句柄）
//  1×1 黑色 RGBA32F + 1 元素全零 EnvAccel，常驻到 Shutdown。
// ═══════════════════════════════════════════════════════════
static VkImage        s_FallbackImage       = VK_NULL_HANDLE;
static VkDeviceMemory s_FallbackImageMemory = VK_NULL_HANDLE;
static VkImageView    s_FallbackImageView   = VK_NULL_HANDLE;
static VkBuffer       s_FallbackAccelBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_FallbackAccelMemory = VK_NULL_HANDLE;

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
//  共享 sampler（真实贴图与占位图共用：linear + clamp-to-edge，适合 equirect）
// ═══════════════════════════════════════════════════════════
static bool EnsureSampler(VkDevice device)
{
    if (s_EnvSampler != VK_NULL_HANDLE) return true;

    VkSamplerCreateInfo samplerCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerCI.magFilter    = VK_FILTER_LINEAR;
    samplerCI.minFilter    = VK_FILTER_LINEAR;
    samplerCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.minLod       = 0.0f;
    samplerCI.maxLod       = VK_LOD_CLAMP_NONE;
    VkResult res = vkCreateSampler(device, &samplerCI, nullptr, &s_EnvSampler);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPTEnv] vkCreateSampler failed: %d\n", res);
        s_EnvSampler = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
//  InitFallback — 占位资源（幂等）
//
//  为什么需要：PrepareDispatch 原先只在 VPTEnv::IsReady() 时写 binding 16/17，
//  而 shader 的 miss 分支与 NEE 环境分支会无条件采样 envHdrTex / 读 envAccel，
//  未上传 HDR 时这两个描述符从未被写过 → 采样未定义行为（黑屏/花屏/读到别的资源）。
//  现在初始化时就绑一份 1×1 黑色 + 全零 alias 表，描述符永远合法；
//  真正的「常数 ambient 回退」由 shader 侧的 envReady 标志完成。
// ═══════════════════════════════════════════════════════════
bool VPTEnv::InitFallback(VkDevice device, VkPhysicalDevice gpu)
{
    if (device == VK_NULL_HANDLE || gpu == VK_NULL_HANDLE) return false;
    if (s_FallbackImageView != VK_NULL_HANDLE && s_FallbackAccelBuffer != VK_NULL_HANDLE)
        return true;

    if (!EnsureSampler(device)) return false;

    // ── 1. 1×1 RGBA32F 黑色图（device-local）──
    if (s_FallbackImage == VK_NULL_HANDLE)
    {
        VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imgCI.imageType     = VK_IMAGE_TYPE_2D;
        imgCI.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
        imgCI.extent        = { 1, 1, 1 };
        imgCI.mipLevels     = 1;
        imgCI.arrayLayers   = 1;
        imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult res = vkCreateImage(device, &imgCI, nullptr, &s_FallbackImage);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPTEnv] fallback vkCreateImage failed: %d\n", res);
            s_FallbackImage = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, s_FallbackImage, &memReq);
        VkMemoryAllocateInfo imgAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        imgAlloc.allocationSize  = memReq.size;
        imgAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        res = vkAllocateMemory(device, &imgAlloc, nullptr, &s_FallbackImageMemory);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPTEnv] fallback vkAllocateMemory(image) failed: %d\n", res);
            s_FallbackImageMemory = VK_NULL_HANDLE;
            return false;
        }
        vkBindImageMemory(device, s_FallbackImage, s_FallbackImageMemory, 0);
    }

    // ── 2. 1 元素全零 EnvAccel（q=0 → shader 取 alias 分支，radiance=0）──
    if (s_FallbackAccelBuffer == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo bCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bCI.size        = sizeof(EnvAccel);
        bCI.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult res = vkCreateBuffer(device, &bCI, nullptr, &s_FallbackAccelBuffer);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPTEnv] fallback vkCreateBuffer failed: %d\n", res);
            s_FallbackAccelBuffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, s_FallbackAccelBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize  = memReq.size;
        allocInfo.memoryTypeIndex = VPTScene::FindMemoryType(gpu, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        res = vkAllocateMemory(device, &allocInfo, nullptr, &s_FallbackAccelMemory);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPTEnv] fallback vkAllocateMemory(buffer) failed: %d\n", res);
            s_FallbackAccelMemory = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device, s_FallbackAccelBuffer, s_FallbackAccelMemory, 0);
    }

    // ── 3. View ──
    if (s_FallbackImageView == VK_NULL_HANDLE)
    {
        VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivCI.image    = s_FallbackImage;
        ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCI.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        ivCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCI.subresourceRange.baseMipLevel   = 0;
        ivCI.subresourceRange.levelCount     = 1;
        ivCI.subresourceRange.baseArrayLayer = 0;
        ivCI.subresourceRange.layerCount     = 1;
        VkResult res = vkCreateImageView(device, &ivCI, nullptr, &s_FallbackImageView);
        if (res != VK_SUCCESS)
        {
            fprintf(stderr, "[VPTEnv] fallback vkCreateImageView failed: %d\n", res);
            s_FallbackImageView = VK_NULL_HANDLE;
            return false;
        }
    }

    // ── 4. 清屏/布局转换/填 alias 表延后到 RecordPendingUploads（Unity 命令缓冲内） ──
    // 这里只创建资源：初始化发生在主线程（VPT_Initialize），不能在主线程提交队列命令。
    s_FallbackNeedsRecord = true;

    fprintf(stderr, "[VPTEnv] fallback env resources created (record deferred to Unity command buffer).\n");
    return true;
}

// ═══════════════════════════════════════════════════════════
//  SetEnvironmentMap — CPU 侧准备（行翻转 + alias 表 + 深拷贝）
//
//  这里不碰任何 Vulkan 队列/设备：VkImage/VkBuffer 创建、staging 上传与
//  布局转换全部由 RecordPendingUploads 录进 Unity 的命令缓冲（事件内、渲染线程）。
// ═══════════════════════════════════════════════════════════
void VPTEnv::SetEnvironmentMap(const float* pixels, uint32_t width, uint32_t height)
{
    std::lock_guard<std::recursive_mutex> guard(s_EnvMutex);

    if (!pixels || width == 0 || height == 0) return;

    // ── 复制像素数据（createEnvironmentAccel 会将 PDF 写入 alpha 通道）──
    const size_t floatCount = (size_t)width * height * 4;
    s_PendingPixels.resize(floatCount);
    memcpy(s_PendingPixels.data(), pixels, floatCount * sizeof(float));

    // 翻转行序：Unity GetPixels 从底行返回（row0=底部），
    // Vulkan vkCmdCopyBufferToImage 将 row0 当顶部。翻转后 alias 表与纹理一致。
    for (uint32_t y = 0; y < height / 2; y++)
    {
        float* rowTop = s_PendingPixels.data() + (size_t)y * width * 4;
        float* rowBot = s_PendingPixels.data() + (size_t)(height - 1 - y) * width * 4;
        std::swap_ranges(rowTop, rowTop + width * 4, rowBot);
    }

    // ── 构建 alias 表（CPU 端）──
    float integral = 1.0f;
    s_PendingAccel = createEnvironmentAccel(s_PendingPixels.data(), width, height, integral);
    s_EnvIntegral   = integral;
    s_PendingWidth  = width;
    s_PendingHeight = height;
    s_EnvPending    = true;

    fprintf(stderr, "[VPTEnv] HDR env map queued: %ux%u, integral=%.4f "
                    "(upload deferred to Unity command buffer)\n",
            width, height, integral);
    VPTTrace::Line("env: queued %ux%u (CPU prep done)", width, height);
}

bool VPTEnv::HasPendingUploads()
{
    std::lock_guard<std::recursive_mutex> guard(s_EnvMutex);
    return s_EnvPending || s_FallbackNeedsRecord;
}

// 拆 HDR 贴图本身（不做 wait；调用方负责保证 GPU 空闲）
static void DestroyEnvResourcesNoWait(VkDevice device)
{
    if (device == VK_NULL_HANDLE) return;
    if (s_EnvImageView)   { vkDestroyImageView(device, s_EnvImageView, nullptr);   s_EnvImageView = VK_NULL_HANDLE; }
    if (s_EnvImage)       { vkDestroyImage(device, s_EnvImage, nullptr);           s_EnvImage = VK_NULL_HANDLE; }
    if (s_EnvImageMemory) { vkFreeMemory(device, s_EnvImageMemory, nullptr);       s_EnvImageMemory = VK_NULL_HANDLE; }
    if (s_EnvAccelBuffer) { vkDestroyBuffer(device, s_EnvAccelBuffer, nullptr);    s_EnvAccelBuffer = VK_NULL_HANDLE; }
    if (s_EnvAccelMemory) { vkFreeMemory(device, s_EnvAccelMemory, nullptr);       s_EnvAccelMemory = VK_NULL_HANDLE; }
    s_EnvReady = false;
}

// host-visible staging（TRANSFER_SRC）
static bool CreateHostStaging(VkDevice device, VkPhysicalDevice gpu, VkDeviceSize size,
                              VkBuffer& outBuf, VkDeviceMemory& outMem)
{
    outBuf = VK_NULL_HANDLE;
    outMem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = size;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!VPT_CheckVk(vkCreateBuffer(device, &bci, nullptr, &outBuf), "vkCreateBuffer(env staging)"))
    {
        outBuf = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, outBuf, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = VPTScene::FindMemoryType(gpu, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!VPT_CheckVk(vkAllocateMemory(device, &ai, nullptr, &outMem), "vkAllocateMemory(env staging)"))
    {
        vkDestroyBuffer(device, outBuf, nullptr);
        outBuf = VK_NULL_HANDLE;
        outMem = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device, outBuf, outMem, 0);
    return true;
}

bool VPTEnv::RecordPendingUploads(VkCommandBuffer cmd)
{
    std::lock_guard<std::recursive_mutex> guard(s_EnvMutex);

    if (cmd == VK_NULL_HANDLE) return false;

    // 二分排查开关：VPT_NO_ENV=1 跳过 HDR 环境贴图上传（继续用 1x1 黑色占位图）
    if (VPT_EnvFlag("VPT_NO_ENV"))
    {
        if (s_EnvPending)
        {
            VPTTrace::Line("env: upload skipped by VPT_NO_ENV");
            s_EnvPending = false;
            s_PendingPixels.clear();
            s_PendingAccel.clear();
        }
    }

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    VkPhysicalDevice gpu = inst.physicalDevice;
    if (device == VK_NULL_HANDLE) return false;

    if (VPT_IsDeviceLost())
    {
        if (s_EnvPending || s_FallbackNeedsRecord)
            fprintf(stderr, "[VPTEnv] pending upload dropped: device lost.\n");
        s_EnvPending = false;
        s_FallbackNeedsRecord = false;
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }

    // ── 1. 占位资源首次初始化：清黑 + 布局转 SHADER_READ_ONLY + 填零 alias 表 ──
    if (s_FallbackNeedsRecord && s_FallbackImage != VK_NULL_HANDLE && s_FallbackAccelBuffer != VK_NULL_HANDLE)
    {
        s_FallbackNeedsRecord = false;

        VkImageSubresourceRange range{};
        range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel   = 0;
        range.levelCount     = 1;
        range.baseArrayLayer = 0;
        range.layerCount     = 1;

        VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image               = s_FallbackImage;
        toTransfer.subresourceRange    = range;
        toTransfer.srcAccessMask       = 0;
        toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkClearColorValue black{};
        black.float32[0] = 0.0f; black.float32[1] = 0.0f;
        black.float32[2] = 0.0f; black.float32[3] = 1.0f;   // alpha 存 pdf，占位无意义
        vkCmdClearColorImage(cmd, s_FallbackImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &black, 1, &range);

        VkImageMemoryBarrier toShader{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toShader.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image               = s_FallbackImage;
        toShader.subresourceRange    = range;
        toShader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);

        EnvAccel zeroAccel{ 0u, 0.0f };
        vkCmdUpdateBuffer(cmd, s_FallbackAccelBuffer, 0, sizeof(EnvAccel), &zeroAccel);

        VkMemoryBarrier accelBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        accelBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        accelBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &accelBarrier, 0, nullptr, 0, nullptr);

        fprintf(stderr, "[VPTEnv] fallback env resources recorded (1x1 black + zero alias table).\n");
    }

    if (!s_EnvPending)
        return false;

    s_EnvPending = false;

    // ── 2. 旧贴图可能仍被在飞行帧采样：等 GPU 空闲后再拆 ──
    VPTTrace::Stage("env:vkDeviceWaitIdle");
    VPTTrace::Line("env: vkDeviceWaitIdle(before destroy) begin ...");
    if (!VPT_CheckVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(env upload)"))
    {
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        VPTTrace::Line("env: vkDeviceWaitIdle failed -> upload dropped");
        return false;
    }
    VPTTrace::Line("env: vkDeviceWaitIdle done");

    VPTTrace::Stage("env:create-resources");
    DestroyEnvResourcesNoWait(device);

    // 上一轮 staging：此刻 GPU 已空闲，可以安全释放
    for (size_t i = 0; i < s_PendingStagingBufs.size(); i++)
    {
        if (s_PendingStagingBufs[i]) vkDestroyBuffer(device, s_PendingStagingBufs[i], nullptr);
        if (s_PendingStagingMems[i]) vkFreeMemory(device, s_PendingStagingMems[i], nullptr);
    }
    s_PendingStagingBufs.clear();
    s_PendingStagingMems.clear();

    const uint32_t width  = s_PendingWidth;
    const uint32_t height = s_PendingHeight;
    const VkDeviceSize imageSize = (VkDeviceSize)width * height * 4 * sizeof(float);

    // ── 3. VkImage（device-local）+ staging ──
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

    if (!VPT_CheckVk(vkCreateImage(device, &imgCI, nullptr, &s_EnvImage), "vkCreateImage(env)"))
    {
        s_EnvImage = VK_NULL_HANDLE;
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, s_EnvImage, &memReq);
    VkMemoryAllocateInfo imgAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imgAlloc.allocationSize  = memReq.size;
    imgAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!VPT_CheckVk(vkAllocateMemory(device, &imgAlloc, nullptr, &s_EnvImageMemory), "vkAllocateMemory(env image)"))
    {
        vkDestroyImage(device, s_EnvImage, nullptr);
        s_EnvImage = VK_NULL_HANDLE;
        s_EnvImageMemory = VK_NULL_HANDLE;
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }
    vkBindImageMemory(device, s_EnvImage, s_EnvImageMemory, 0);

    VkBuffer       imgStaging    = VK_NULL_HANDLE;
    VkDeviceMemory imgStagingMem = VK_NULL_HANDLE;
    if (!CreateHostStaging(device, gpu, imageSize, imgStaging, imgStagingMem))
    {
        DestroyEnvResourcesNoWait(device);
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }

    void* mapped = nullptr;
    vkMapMemory(device, imgStagingMem, 0, imageSize, 0, &mapped);
    memcpy(mapped, s_PendingPixels.data(), (size_t)imageSize);
    vkUnmapMemory(device, imgStagingMem);

    // ── 4. EnvAccel SSBO（device-local）+ staging ──
    const VkDeviceSize accelSize = (VkDeviceSize)width * height * sizeof(EnvAccel);

    VkBufferCreateInfo dbCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    dbCI.size        = accelSize;
    dbCI.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    dbCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!VPT_CheckVk(vkCreateBuffer(device, &dbCI, nullptr, &s_EnvAccelBuffer), "vkCreateBuffer(env accel)"))
    {
        s_EnvAccelBuffer = VK_NULL_HANDLE;
        vkDestroyBuffer(device, imgStaging, nullptr);
        vkFreeMemory(device, imgStagingMem, nullptr);
        DestroyEnvResourcesNoWait(device);
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }

    VkMemoryRequirements dReq;
    vkGetBufferMemoryRequirements(device, s_EnvAccelBuffer, &dReq);
    VkMemoryAllocateInfo dAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    dAlloc.allocationSize  = dReq.size;
    dAlloc.memoryTypeIndex = VPTScene::FindMemoryType(gpu, dReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!VPT_CheckVk(vkAllocateMemory(device, &dAlloc, nullptr, &s_EnvAccelMemory), "vkAllocateMemory(env accel)"))
    {
        vkDestroyBuffer(device, imgStaging, nullptr);
        vkFreeMemory(device, imgStagingMem, nullptr);
        DestroyEnvResourcesNoWait(device);
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }
    vkBindBufferMemory(device, s_EnvAccelBuffer, s_EnvAccelMemory, 0);

    VkBuffer       accelStaging    = VK_NULL_HANDLE;
    VkDeviceMemory accelStagingMem = VK_NULL_HANDLE;
    if (!CreateHostStaging(device, gpu, accelSize, accelStaging, accelStagingMem))
    {
        vkDestroyBuffer(device, imgStaging, nullptr);
        vkFreeMemory(device, imgStagingMem, nullptr);
        DestroyEnvResourcesNoWait(device);
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }

    void* amapped = nullptr;
    vkMapMemory(device, accelStagingMem, 0, accelSize, 0, &amapped);
    memcpy(amapped, s_PendingAccel.data(), (size_t)accelSize);
    vkUnmapMemory(device, accelStagingMem);

    // ── 5. 录制：布局转换 → 图像拷贝 → 转 SHADER_READ_ONLY → SSBO 拷贝 + barrier ──
    VkImageSubresourceRange range{};
    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;

    VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image               = s_EnvImage;
    toTransfer.subresourceRange    = range;
    toTransfer.srcAccessMask       = 0;
    toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel       = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount     = 1;
    copyRegion.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, imgStaging, s_EnvImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    VkImageMemoryBarrier toShader{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toShader.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image               = s_EnvImage;
    toShader.subresourceRange    = range;
    toShader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);

    VkBufferCopy accelCopy{ 0, 0, accelSize };
    vkCmdCopyBuffer(cmd, accelStaging, s_EnvAccelBuffer, 1, &accelCopy);

    VkMemoryBarrier accelBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    accelBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    accelBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &accelBarrier, 0, nullptr, 0, nullptr);

    // staging 保留到下一次上传 / Shutdown（拷贝是本帧录进命令缓冲的）
    s_PendingStagingBufs.push_back(imgStaging);
    s_PendingStagingMems.push_back(imgStagingMem);
    s_PendingStagingBufs.push_back(accelStaging);
    s_PendingStagingMems.push_back(accelStagingMem);

    // ── 6. ImageView + sampler ──
    VkImageViewCreateInfo ivCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivCI.image    = s_EnvImage;
    ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivCI.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    ivCI.subresourceRange = range;
    if (!VPT_CheckVk(vkCreateImageView(device, &ivCI, nullptr, &s_EnvImageView), "vkCreateImageView(env)"))
    {
        s_EnvImageView = VK_NULL_HANDLE;
        DestroyEnvResourcesNoWait(device);
        s_PendingPixels.clear();
        s_PendingAccel.clear();
        return false;
    }

    EnsureSampler(device);

    s_EnvWidth  = width;
    s_EnvHeight = height;
    s_EnvReady  = true;

    s_PendingPixels.clear();
    s_PendingAccel.clear();

    fprintf(stderr, "[VPTEnv] HDR env map recorded: %ux%u, integral=%.4f, ready=%d\n",
            width, height, s_EnvIntegral, (int)s_EnvReady);
    VPTTrace::Line("env: recorded %ux%u OK", width, height);
    VPTTrace::Stage("env:done");
    return true;
}

// ═══════════════════════════════════════════════════════════
//  DestroyResources — 只拆 HDR 贴图本身（sampler 与占位资源常驻）
// ═══════════════════════════════════════════════════════════
void VPTEnv::DestroyResources()
{
    std::lock_guard<std::recursive_mutex> guard(s_EnvMutex);

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    if (device == VK_NULL_HANDLE) return;

    // 设备已丢失时 vkDeviceWaitIdle 可能长时间不返回，直接跳过等待
    if (!VPT_IsDeviceLost())
        VPT_CheckVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(VPTEnv::DestroyResources)");

    DestroyEnvResourcesNoWait(device);

    s_EnvPending = false;
    s_FallbackNeedsRecord = false;
    s_PendingPixels.clear();
    s_PendingAccel.clear();
}

// ═══════════════════════════════════════════════════════════
//  Shutdown — 拆全部（HDR 贴图 + 占位资源 + sampler + staging）
// ═══════════════════════════════════════════════════════════
void VPTEnv::Shutdown()
{
    std::lock_guard<std::recursive_mutex> guard(s_EnvMutex);

    DestroyResources();

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    if (device == VK_NULL_HANDLE) return;
    if (!VPT_IsDeviceLost())
        VPT_CheckVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(VPTEnv::Shutdown)");

    for (size_t i = 0; i < s_PendingStagingBufs.size(); i++)
    {
        if (s_PendingStagingBufs[i]) vkDestroyBuffer(device, s_PendingStagingBufs[i], nullptr);
        if (s_PendingStagingMems[i]) vkFreeMemory(device, s_PendingStagingMems[i], nullptr);
    }
    s_PendingStagingBufs.clear();
    s_PendingStagingMems.clear();

    if (s_FallbackImageView)   { vkDestroyImageView(device, s_FallbackImageView, nullptr);   s_FallbackImageView = VK_NULL_HANDLE; }
    if (s_FallbackImage)       { vkDestroyImage(device, s_FallbackImage, nullptr);           s_FallbackImage = VK_NULL_HANDLE; }
    if (s_FallbackImageMemory) { vkFreeMemory(device, s_FallbackImageMemory, nullptr);       s_FallbackImageMemory = VK_NULL_HANDLE; }
    if (s_FallbackAccelBuffer) { vkDestroyBuffer(device, s_FallbackAccelBuffer, nullptr);    s_FallbackAccelBuffer = VK_NULL_HANDLE; }
    if (s_FallbackAccelMemory) { vkFreeMemory(device, s_FallbackAccelMemory, nullptr);       s_FallbackAccelMemory = VK_NULL_HANDLE; }
    if (s_EnvSampler)          { vkDestroySampler(device, s_EnvSampler, nullptr);            s_EnvSampler = VK_NULL_HANDLE; }
}

// ── Getter ──
// 真实贴图未就绪时返回占位资源：binding 16/17 永远有合法句柄可绑。
bool        VPTEnv::IsReady()           { return s_EnvReady; }
VkImageView VPTEnv::GetEnvImageView()   { return s_EnvReady ? s_EnvImageView : s_FallbackImageView; }
VkSampler   VPTEnv::GetEnvSampler()     { return s_EnvSampler; }
VkBuffer    VPTEnv::GetEnvAccelBuffer() { return s_EnvReady ? s_EnvAccelBuffer : s_FallbackAccelBuffer; }
uint32_t    VPTEnv::GetEnvWidth()       { return s_EnvWidth; }
uint32_t    VPTEnv::GetEnvHeight()      { return s_EnvHeight; }
