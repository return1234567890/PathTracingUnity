// VulkanPathTracePlugin — Ray Tracing Pipeline 支撑层（实现）

#include "vpt_rt.h"
#include "vpt_internal.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace VPTRt {

// ═══════════════════════════════════════════════════════════
//  扩展函数指针
// ═══════════════════════════════════════════════════════════

static PFN_vkCreateRayTracingPipelinesKHR       pfn_vkCreateRayTracingPipelinesKHR       = nullptr;
static PFN_vkGetRayTracingShaderGroupHandlesKHR pfn_vkGetRayTracingShaderGroupHandlesKHR = nullptr;
static PFN_vkCmdTraceRaysKHR                    pfn_vkCmdTraceRaysKHR                    = nullptr;
static PFN_vkGetBufferDeviceAddressKHR          pfn_vkGetBufferDeviceAddressKHR          = nullptr;

static const char* s_Missing = "";

PFN_vkCreateRayTracingPipelinesKHR       CreateRayTracingPipelines()       { return pfn_vkCreateRayTracingPipelinesKHR; }
PFN_vkGetRayTracingShaderGroupHandlesKHR GetRayTracingShaderGroupHandles() { return pfn_vkGetRayTracingShaderGroupHandlesKHR; }
PFN_vkCmdTraceRaysKHR                    CmdTraceRays()                    { return pfn_vkCmdTraceRaysKHR; }

bool AreFunctionsReady()
{
    return pfn_vkCreateRayTracingPipelinesKHR
        && pfn_vkGetRayTracingShaderGroupHandlesKHR
        && pfn_vkCmdTraceRaysKHR
        && pfn_vkGetBufferDeviceAddressKHR;
}

const char* MissingFunction() { return s_Missing; }

bool LoadFunctions(VkInstance instance, VkDevice device)
{
    if (!instance || !device)
    {
        s_Missing = "instance/device";
        return false;
    }

    PFN_vkGetInstanceProcAddr gipa = VPT_GetInstance().getInstanceProcAddr;
    if (!gipa)
    {
        s_Missing = "vkGetInstanceProcAddr";
        return false;
    }

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (!gdpa)
    {
        s_Missing = "vkGetDeviceProcAddr";
        return false;
    }

    // 注意：这些符号只在 VK_KHR_ray_tracing_pipeline 已在该 VkDevice 上启用时才返回非空。
    // 返回空 = 拦截器没把扩展注进去（isPreloaded / InterceptInitialization 的问题）。
    bool allOk = true;
    auto Load = [&](const char* name) -> void* {
        void* fn = (void*)gdpa(device, name);
        if (!fn) { allOk = false; s_Missing = name; }
        return fn;
    };

    pfn_vkCreateRayTracingPipelinesKHR =
        (PFN_vkCreateRayTracingPipelinesKHR)Load("vkCreateRayTracingPipelinesKHR");
    pfn_vkGetRayTracingShaderGroupHandlesKHR =
        (PFN_vkGetRayTracingShaderGroupHandlesKHR)Load("vkGetRayTracingShaderGroupHandlesKHR");
    pfn_vkCmdTraceRaysKHR =
        (PFN_vkCmdTraceRaysKHR)Load("vkCmdTraceRaysKHR");
    pfn_vkGetBufferDeviceAddressKHR =
        (PFN_vkGetBufferDeviceAddressKHR)Load("vkGetBufferDeviceAddressKHR");
    // KHR / EXT / core 三个别名都试一遍（Unity 可能只启用了 EXT 版）
    if (!pfn_vkGetBufferDeviceAddressKHR)
        pfn_vkGetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR)gdpa(device, "vkGetBufferDeviceAddressEXT");
    if (!pfn_vkGetBufferDeviceAddressKHR)
        pfn_vkGetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR)gdpa(device, "vkGetBufferDeviceAddress");

    struct { const char* name; bool ok; } table[] = {
        { "vkCreateRayTracingPipelinesKHR",       pfn_vkCreateRayTracingPipelinesKHR       != nullptr },
        { "vkGetRayTracingShaderGroupHandlesKHR", pfn_vkGetRayTracingShaderGroupHandlesKHR != nullptr },
        { "vkCmdTraceRaysKHR",                    pfn_vkCmdTraceRaysKHR                    != nullptr },
        { "vkGetBufferDeviceAddressKHR",          pfn_vkGetBufferDeviceAddressKHR          != nullptr },
    };
    for (const auto& t : table)
        fprintf(stderr, "[VPT] RT fn %-40s : %s\n", t.name, t.ok ? "loaded" : "MISSING");

    if (allOk)
        fprintf(stderr, "[VPT] RT pipeline extension functions loaded.\n");
    else
        fprintf(stderr, "[VPT] ERROR: RT extension function '%s' missing — "
                        "VK_KHR_ray_tracing_pipeline was not enabled on Unity's VkDevice.\n", s_Missing);

    return allOk;
}

// ═══════════════════════════════════════════════════════════
//  工具
// ═══════════════════════════════════════════════════════════

static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    if (alignment == 0) return value;
    return (value + alignment - 1u) / alignment * alignment;
}

static uint32_t FindMemoryType(VkPhysicalDevice gpu, uint32_t typeFilter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

// ═══════════════════════════════════════════════════════════
//  SBT
// ═══════════════════════════════════════════════════════════

void DestroyBindingTable(VkDevice device, BindingTable& sbt)
{
    if (device == VK_NULL_HANDLE) { sbt = {}; return; }

    if (sbt.mapped) { vkUnmapMemory(device, sbt.memory); sbt.mapped = nullptr; }
    if (sbt.buffer) { vkDestroyBuffer(device, sbt.buffer, nullptr); sbt.buffer = VK_NULL_HANDLE; }
    if (sbt.memory) { vkFreeMemory(device, sbt.memory, nullptr); sbt.memory = VK_NULL_HANDLE; }
    sbt = {};
}

bool CreateBindingTable(VkDevice device, VkPhysicalDevice gpu, VkPipeline pipeline,
                        BindingTable& out)
{
    out = {};

    if (!AreFunctionsReady())
    {
        fprintf(stderr, "[VPT] SBT: RT functions not loaded (missing '%s')\n", MissingFunction());
        return false;
    }

    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& props = VPT_GetRayTracingPipelineProperties();
    const uint32_t handleSize      = props.shaderGroupHandleSize;
    const uint32_t handleAlignment = props.shaderGroupHandleAlignment;
    const uint32_t baseAlignment   = props.shaderGroupBaseAlignment;
    if (handleSize == 0 || handleAlignment == 0 || baseAlignment == 0)
    {
        fprintf(stderr, "[VPT] SBT: invalid RT properties (handle=%u align=%u base=%u)\n",
                handleSize, handleAlignment, baseAlignment);
        return false;
    }

    // stride：每条记录的最小跨度（句柄大小按 handleAlignment 上取整）
    const uint32_t stride = AlignUp(handleSize, handleAlignment);
    // raygen 只有 1 条记录，且规范要求它的 stride 与 size 都是 baseAlignment 对齐
    const uint32_t raygenStride = AlignUp(stride, baseAlignment);

    const uint32_t missCount = 2;   // rmissMain / rmissShadow
    const uint32_t hitCount  = 2;   // group0 主路径 / group1 阴影
    const uint32_t callableCount = 0;

    // 每个 region 的起点与大小都按 baseAlignment 对齐（避免驱动对 "region 必须是
    // baseAlignment 倍数" 的实现差异；region 内部记录仍按 stride 排布）
    const VkDeviceSize raygenRegionSize   = raygenStride;
    const VkDeviceSize missRegionSize     = AlignUp((uint32_t)(stride * missCount), baseAlignment);
    const VkDeviceSize hitRegionSize      = AlignUp((uint32_t)(stride * hitCount), baseAlignment);
    const VkDeviceSize callableRegionSize = 0;
    const VkDeviceSize totalSize = raygenRegionSize + missRegionSize + hitRegionSize + callableRegionSize;

    const VkDeviceSize raygenOffset   = 0;
    const VkDeviceSize missOffset     = raygenOffset + raygenRegionSize;
    const VkDeviceSize hitOffset      = missOffset + missRegionSize;
    const VkDeviceSize callableOffset = hitOffset + hitRegionSize;

    if (stride > props.maxShaderGroupStride)
    {
        fprintf(stderr, "[VPT] SBT: stride %u exceeds maxShaderGroupStride %u\n",
                stride, props.maxShaderGroupStride);
        return false;
    }

    // ── 1. host-visible + device-address 的 buffer ──
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = totalSize;
    bci.usage       = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = vkCreateBuffer(device, &bci, nullptr, &out.buffer);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] SBT: vkCreateBuffer failed: %d\n", res);
        DestroyBindingTable(device, out);
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, out.buffer, &memReq);

    const uint32_t memType = FindMemoryType(gpu, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == UINT32_MAX)
    {
        fprintf(stderr, "[VPT] SBT: no HOST_VISIBLE|HOST_COHERENT memory type for SBT buffer\n");
        DestroyBindingTable(device, out);
        return false;
    }

    // ⚠ 分配必须带 VkMemoryAllocateFlagsInfo{DEVICE_ADDRESS_BIT}，
    //   否则 vkGetBufferDeviceAddressKHR 直接失败（方案 §6 风险 2）。
    VkMemoryAllocateFlagsInfo allocFlags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext           = &allocFlags;
    mai.allocationSize  = memReq.size;
    mai.memoryTypeIndex = memType;

    res = vkAllocateMemory(device, &mai, nullptr, &out.memory);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] SBT: vkAllocateMemory failed: %d\n", res);
        DestroyBindingTable(device, out);
        return false;
    }

    res = vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] SBT: vkBindBufferMemory failed: %d\n", res);
        DestroyBindingTable(device, out);
        return false;
    }

    res = vkMapMemory(device, out.memory, 0, totalSize, 0, &out.mapped);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] SBT: vkMapMemory failed: %d\n", res);
        DestroyBindingTable(device, out);
        return false;
    }
    out.size = totalSize;

    // ── 2. 取 5 个 group 句柄 ──
    const uint32_t groupCount = kGroupCount;
    const uint32_t handleBytes = groupCount * handleSize;
    std::vector<uint8_t> handles(handleBytes, 0);

    res = pfn_vkGetRayTracingShaderGroupHandlesKHR(device, pipeline, 0, groupCount, handleBytes, handles.data());
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] SBT: vkGetRayTracingShaderGroupHandlesKHR failed: %d\n", res);
        DestroyBindingTable(device, out);
        return false;
    }

    // 句柄全零 = 驱动没填 / 管线坏；记下来但继续（接下来 traceRays 会明确失败）
    bool anyNonZero = false;
    for (uint8_t b : handles)
        if (b != 0) { anyNonZero = true; break; }

    // ── 3. 写入 SBT ──
    uint8_t* base = static_cast<uint8_t*>(out.mapped);
    memset(base, 0, (size_t)totalSize);

    auto copyRecord = [&](VkDeviceSize regionOffset, uint32_t recordIndex, uint32_t strideBytes,
                          uint32_t groupIndex) {
        uint8_t* dst = base + regionOffset + (VkDeviceSize)recordIndex * strideBytes;
        memcpy(dst, handles.data() + (size_t)groupIndex * handleSize, handleSize);
    };

    copyRecord(raygenOffset, 0, raygenStride, kGroupRaygen);
    copyRecord(missOffset,   0, stride,       kGroupMissMain);
    copyRecord(missOffset,   1, stride,       kGroupMissShadow);
    copyRecord(hitOffset,    0, stride,       kGroupHitMain);
    copyRecord(hitOffset,    1, stride,       kGroupHitShadow);

    // ── 4. buffer device address → 4 个 region ──
    VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    addrInfo.buffer = out.buffer;
    out.address = pfn_vkGetBufferDeviceAddressKHR(device, &addrInfo);
    if (out.address == 0)
    {
        fprintf(stderr, "[VPT] SBT: vkGetBufferDeviceAddressKHR returned 0 "
                        "(missing DEVICE_ADDRESS allocation flag?)\n");
        DestroyBindingTable(device, out);
        return false;
    }

    out.raygen.offset        = raygenOffset;
    out.raygen.stride        = raygenStride;
    out.raygen.size          = raygenRegionSize;
    out.raygen.deviceAddress = out.address + raygenOffset;

    out.miss.offset        = missOffset;
    out.miss.stride        = stride;
    out.miss.size          = missRegionSize;
    out.miss.deviceAddress = out.address + missOffset;

    out.hit.offset        = hitOffset;
    out.hit.stride        = stride;
    out.hit.size          = hitRegionSize;
    out.hit.deviceAddress = out.address + hitOffset;

    // callable：空 region（address/size 全 0 是合法的「没有 callable」表示）
    out.callable = {};
    (void)callableOffset;
    (void)callableRegionSize;

    // ── 5. 校验打印（G2 判据：句柄非全零 + region 布局符合预期）──
    fprintf(stderr,
            "[VPT] SBT: size=%llu B (handle=%u stride=%u raygenStride=%u baseAlign=%u)\n",
            (unsigned long long)totalSize, handleSize, stride, raygenStride, baseAlignment);
    fprintf(stderr,
            "[VPT] SBT: raygen{off=%llu stride=%u size=%llu} miss{off=%llu stride=%u size=%llu} "
            "hit{off=%llu stride=%u size=%llu} callable{empty}\n",
            (unsigned long long)out.raygen.offset, out.raygen.stride, (unsigned long long)out.raygen.size,
            (unsigned long long)out.miss.offset, out.miss.stride, (unsigned long long)out.miss.size,
            (unsigned long long)out.hit.offset, out.hit.stride, (unsigned long long)out.hit.size);
    fprintf(stderr, "[VPT] SBT: address=0x%llx handles=%s (first bytes:",
            (unsigned long long)out.address, anyNonZero ? "non-zero OK" : "ALL ZERO !");
    for (uint32_t i = 0; i < handleSize && i < 8u; i++)
        fprintf(stderr, " %02x", handles[i]);
    fprintf(stderr, " ...)\n");

    if (!anyNonZero)
    {
        fprintf(stderr, "[VPT] ERROR: SBT group handles are all zero — pipeline is unusable.\n");
        DestroyBindingTable(device, out);
        return false;
    }

    return true;
}

} // namespace VPTRt
