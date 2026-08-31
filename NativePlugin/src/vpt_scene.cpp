// VulkanPathTracePlugin — 场景数据上传 + BLAS/TLAS 构建
//
// 扩展函数通过 vkGetDeviceProcAddr 运行时加载（vulkan-1.lib 不导出 KHR 扩展符号）
// 统一顶点/索引缓冲：所有 mesh 数据合并到单个 VkBuffer，shader 通过 InstanceInfo 偏移访问

#include "vpt_scene.h"
#include "vpt_internal.h"
#include "vpt_types.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

// ═══════════════════════════════════════════════════════════
//  扩展函数指针（pfn_ 前缀避免与 SDK 函数原型冲突）
// ═══════════════════════════════════════════════════════════

static PFN_vkGetBufferDeviceAddressKHR              pfn_vkGetBufferDeviceAddressKHR              = nullptr;
static PFN_vkGetAccelerationStructureBuildSizesKHR   pfn_vkGetAccelerationStructureBuildSizesKHR   = nullptr;
static PFN_vkCreateAccelerationStructureKHR          pfn_vkCreateAccelerationStructureKHR          = nullptr;
static PFN_vkDestroyAccelerationStructureKHR         pfn_vkDestroyAccelerationStructureKHR         = nullptr;
static PFN_vkCmdBuildAccelerationStructuresKHR       pfn_vkCmdBuildAccelerationStructuresKHR       = nullptr;
static PFN_vkGetAccelerationStructureDeviceAddressKHR pfn_vkGetAccelerationStructureDeviceAddressKHR = nullptr;
static bool s_ExtensionsReady = false;

static VkDevice s_Device = VK_NULL_HANDLE;

// ═══════════════════════════════════════════════════════════
//  场景资源
// ═══════════════════════════════════════════════════════════

struct BufferResource
{
    VkBuffer         buffer  = VK_NULL_HANDLE;
    VkDeviceMemory   memory  = VK_NULL_HANDLE;
    VkDeviceSize     size    = 0;
    VkDeviceAddress  address = 0;
};

struct BLASResource
{
    VkAccelerationStructureKHR handle        = VK_NULL_HANDLE;
    BufferResource              asBuffer;
    BufferResource              scratchBuffer;
    uint32_t                    primitiveCount = 0;
};

// 每实例元数据（64B，与 GLSL InstanceInfo 结构对齐）
struct InstanceInfo
{
    float    transformRow0[4];  // 16B — object→world 3x4 行主序
    float    transformRow1[4];  // 16B
    float    transformRow2[4];  // 16B
    uint32_t vertexOffset;      // 4B — 在统一顶点缓冲中的起始偏移（顶点数）
    uint32_t indexOffset;       // 4B — 在统一索引缓冲中的起始偏移（索引数）
    uint32_t materialID;        // 4B
    uint32_t pad;               // 4B
    // 合计 64B
};
static_assert(sizeof(InstanceInfo) == 64, "InstanceInfo must be 64 bytes");

static BufferResource  s_UnifiedVertexBuffer;
static BufferResource  s_UnifiedIndexBuffer;
static BufferResource  s_MaterialBuffer;
static BufferResource  s_LightBuffer;
static std::vector<BLASResource>    s_BLASes;
static BufferResource  s_InstanceBuffer;   // TLAS instance data
static BufferResource  s_InstanceInfoBuffer; // Per-instance metadata for shader
static std::vector<uint32_t> s_MeshVertexOffsets; // per-mesh vertex offset
static std::vector<uint32_t> s_MeshIndexOffsets;  // per-mesh index offset
static BLASResource    s_TLAS;
static bool            s_Ready = false;

// ═══════════════════════════════════════════════════════════
//  工具函数
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

static VkDeviceAddress GetBufferAddress(VkDevice device, VkBuffer buf)
{
    if (!pfn_vkGetBufferDeviceAddressKHR)
    {
        fprintf(stderr, "[VPT] FATAL: pfn_vkGetBufferDeviceAddressKHR is null! "
                        "VK_KHR_buffer_device_address not enabled on device.\n");
        fflush(stderr);
        return 0;
    }
    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = buf;
    return pfn_vkGetBufferDeviceAddressKHR(device, &info);
}

static VkResult CreateBufferResource(
    VkDevice device, VkPhysicalDevice gpu,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
    BufferResource& out)
{
    out = {};

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = vkCreateBuffer(device, &bci, nullptr, &out.buffer);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, out.buffer, &memReq);

    VkMemoryAllocateFlagsInfo allocFlags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR;
    allocFlags.deviceMask = 0;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, memReq.memoryTypeBits, memProps);
    mai.pNext = &allocFlags;

    res = vkAllocateMemory(device, &mai, nullptr, &out.memory);
    if (res != VK_SUCCESS) return res;

    res = vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (res != VK_SUCCESS) return res;

    out.size    = size;
    out.address = GetBufferAddress(device, out.buffer);
    return VK_SUCCESS;
}

static void DestroyBufferResource(VkDevice device, BufferResource& br)
{
    if (br.buffer) { vkDestroyBuffer(device, br.buffer, nullptr); br.buffer = VK_NULL_HANDLE; }
    if (br.memory) { vkFreeMemory(device, br.memory, nullptr);   br.memory = VK_NULL_HANDLE; }
    br.address = 0;
}

static void DestroyBLASResource(VkDevice device, BLASResource& br)
{
    if (br.handle) { pfn_vkDestroyAccelerationStructureKHR(device, br.handle, nullptr); br.handle = VK_NULL_HANDLE; }
    DestroyBufferResource(device, br.asBuffer);
    DestroyBufferResource(device, br.scratchBuffer);
    br.primitiveCount = 0;
}

// ═══════════════════════════════════════════════════════════
//  扩展函数加载
// ═══════════════════════════════════════════════════════════

void VPTScene::LoadExtensionFunctions(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkDevice device)
{
    s_Device = device;
    s_ExtensionsReady = false;

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (!gdpa)
    {
        fprintf(stderr, "[VPT] FATAL: vkGetDeviceProcAddr not found!\n");
        fflush(stderr);
        return;
    }

    fprintf(stderr, "[VPT] Loading device-level extension functions via vkGetDeviceProcAddr...\n");

    #define LOAD(name) pfn_##name = (PFN_##name)gdpa(device, #name); \
        if (pfn_##name) { fprintf(stderr, "[VPT]   " #name " : OK\n"); } \
        else { fprintf(stderr, "[VPT]   " #name " : NOT FOUND (extension not enabled on device)\n"); }

    LOAD(vkGetBufferDeviceAddressKHR);
    LOAD(vkGetAccelerationStructureBuildSizesKHR);
    LOAD(vkCreateAccelerationStructureKHR);
    LOAD(vkDestroyAccelerationStructureKHR);
    LOAD(vkCmdBuildAccelerationStructuresKHR);
    LOAD(vkGetAccelerationStructureDeviceAddressKHR);

    #undef LOAD
    fflush(stderr);

    bool allReady = pfn_vkGetBufferDeviceAddressKHR
                  && pfn_vkGetAccelerationStructureBuildSizesKHR
                  && pfn_vkCreateAccelerationStructureKHR
                  && pfn_vkDestroyAccelerationStructureKHR
                  && pfn_vkCmdBuildAccelerationStructuresKHR
                  && pfn_vkGetAccelerationStructureDeviceAddressKHR;

    if (!allReady)
    {
        fprintf(stderr, "[VPT] FATAL: Some RT extension functions are null! "
                        "VK_KHR_acceleration_structure / VK_KHR_buffer_device_address "
                        "may not be enabled on Unity's VkDevice.\n"
                        "Ensure the plugin is preloaded (isPreloaded=1) so "
                        "InterceptInitialization can inject RT extensions.\n");
        fflush(stderr);
        return;
    }

    s_ExtensionsReady = true;
    fprintf(stderr, "[VPT] All extension functions loaded successfully.\n");
    fflush(stderr);
}

// ═══════════════════════════════════════════════════════════
//  Device-local buffer 创建 + staging 上传
// ═══════════════════════════════════════════════════════════

static std::vector<BufferResource> s_StagingBuffers;

// 创建 device-local buffer 并通过 staging buffer 上传数据。
// staging buffer 被记录到 s_StagingBuffers，调用方需在 vkQueueWaitIdle 后销毁。
static void CreateDeviceLocalBufferWithData(
    VkDevice device, VkPhysicalDevice gpu,
    VkCommandBuffer cmd,
    const void* srcData, VkDeviceSize size,
    VkBufferUsageFlags usage,
    BufferResource& outDevice)
{
    // 1. device-local 目标 buffer（GPU VRAM）
    CreateBufferResource(device, gpu, size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        outDevice);

    // 2. host-visible staging buffer（仅用于一次性上传）
    BufferResource staging{};
    CreateBufferResource(device, gpu, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging);

    // 3. CPU 写入 staging
    void* m = nullptr;
    vkMapMemory(device, staging.memory, 0, size, 0, &m);
    memcpy(m, srcData, size);
    vkUnmapMemory(device, staging.memory);

    // 4. GPU 拷贝 staging → device-local（记录到 command buffer）
    VkBufferCopy copyRegion{ 0, 0, size };
    vkCmdCopyBuffer(cmd, staging.buffer, outDevice.buffer, 1, &copyRegion);

    // 5. staging 延迟销毁
    s_StagingBuffers.push_back(staging);
}

// ═══════════════════════════════════════════════════════════
//  统一缓冲区创建 + 数据上传（device-local + staging）
// ═══════════════════════════════════════════════════════════

static void CreateUnifiedBuffers(
    VkDevice device, VkPhysicalDevice gpu,
    VkCommandBuffer cmd,
    const VPT_MeshData* meshes, int32_t meshCount)
{
    // 1. 计算总大小
    VkDeviceSize totalVertSize = 0;
    VkDeviceSize totalIdxSize = 0;
    s_MeshVertexOffsets.clear();
    s_MeshIndexOffsets.clear();
    s_MeshVertexOffsets.reserve(meshCount);
    s_MeshIndexOffsets.reserve(meshCount);

    for (int32_t i = 0; i < meshCount; i++)
    {
        s_MeshVertexOffsets.push_back((uint32_t)(totalVertSize / sizeof(VPT_Vertex)));
        s_MeshIndexOffsets.push_back((uint32_t)(totalIdxSize / sizeof(uint32_t)));
        totalVertSize += meshes[i].vertexCount * sizeof(VPT_Vertex);
        totalIdxSize  += meshes[i].indexCount * sizeof(uint32_t);
    }

    if (totalVertSize == 0) totalVertSize = 4;
    if (totalIdxSize == 0) totalIdxSize = 4;

    // 2. 创建统一顶点缓冲（device-local + staging 上传）
    //    先收集所有顶点数据到临时内存
    std::vector<uint8_t> vertData(totalVertSize);
    std::vector<uint8_t> idxData(totalIdxSize);
    for (int32_t i = 0; i < meshCount; i++)
    {
        VkDeviceSize vSize = meshes[i].vertexCount * sizeof(VPT_Vertex);
        VkDeviceSize iSize = meshes[i].indexCount * sizeof(uint32_t);
        memcpy(vertData.data() + s_MeshVertexOffsets[i] * sizeof(VPT_Vertex),
               meshes[i].vertices, vSize);
        memcpy(idxData.data() + s_MeshIndexOffsets[i] * sizeof(uint32_t),
               meshes[i].indices, iSize);
    }

    CreateDeviceLocalBufferWithData(device, gpu, cmd,
        vertData.data(), totalVertSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        s_UnifiedVertexBuffer);

    CreateDeviceLocalBufferWithData(device, gpu, cmd,
        idxData.data(), totalIdxSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        s_UnifiedIndexBuffer);
}

// ═══════════════════════════════════════════════════════════
//  BLAS 构建（引用统一缓冲中的偏移）
// ═══════════════════════════════════════════════════════════

static void BuildBLAS(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice gpu,
                      const VPT_MeshData& mesh, uint32_t blasIndex)
{
    uint32_t vertOffset = s_MeshVertexOffsets[blasIndex];
    uint32_t idxOffset  = s_MeshIndexOffsets[blasIndex];

    // 几何描述 — 引用统一缓冲中的偏移地址
    VkAccelerationStructureGeometryTrianglesDataKHR triData{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
    triData.vertexFormat  = VK_FORMAT_R32G32B32_SFLOAT;
    triData.vertexData.deviceAddress = s_UnifiedVertexBuffer.address + vertOffset * sizeof(VPT_Vertex);
    triData.vertexStride = sizeof(VPT_Vertex);
    triData.maxVertex    = mesh.vertexCount - 1;
    triData.indexType    = VK_INDEX_TYPE_UINT32;
    triData.indexData.deviceAddress = s_UnifiedIndexBuffer.address + idxOffset * sizeof(uint32_t);

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType     = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.geometry.triangles = triData;
    geom.flags            = VK_GEOMETRY_OPAQUE_BIT_KHR;

    uint32_t primCount = mesh.indexCount / 3;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pfn_vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primCount, &sizeInfo);

    BLASResource blas;
    VkResult r1 = CreateBufferResource(device, gpu, sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, blas.asBuffer);
    if (r1 != VK_SUCCESS) {
        fprintf(stderr, "[VPT] BLAS[%u] AS buffer alloc FAILED: %d\n", blasIndex, r1);
        return;
    }

    VkResult r2 = CreateBufferResource(device, gpu, std::max(sizeInfo.buildScratchSize, (VkDeviceSize)1),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, blas.scratchBuffer);
    if (r2 != VK_SUCCESS) {
        fprintf(stderr, "[VPT] BLAS[%u] scratch buffer alloc FAILED: %d\n", blasIndex, r2);
        return;
    }

    VkAccelerationStructureCreateInfoKHR asci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    asci.buffer = blas.asBuffer.buffer;
    asci.size   = sizeInfo.accelerationStructureSize;
    asci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkResult r3 = pfn_vkCreateAccelerationStructureKHR(device, &asci, nullptr, &blas.handle);
    if (r3 != VK_SUCCESS) {
        fprintf(stderr, "[VPT] BLAS[%u] vkCreateAccelerationStructure FAILED: %d\n", blasIndex, r3);
        return;
    }
    blas.primitiveCount = primCount;

    buildInfo.dstAccelerationStructure = blas.handle;
    buildInfo.scratchData.deviceAddress = blas.scratchBuffer.address;

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    pfn_vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    s_BLASes.push_back(blas);

    fprintf(stderr, "[VPT] BLAS[%u]: %u verts (offset=%u), %u tris\n",
            blasIndex, mesh.vertexCount, vertOffset, primCount);
}

// ═══════════════════════════════════════════════════════════
//  TLAS 构建 + InstanceInfo 缓冲
// ═══════════════════════════════════════════════════════════

static void BuildTLAS(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice gpu,
                      const VPT_InstanceData* instances, int32_t instanceCount)
{
    // ── 构建 VkAccelerationStructureInstanceKHR 数组 ──
    std::vector<VkAccelerationStructureInstanceKHR> vkInstances(instanceCount);
    std::vector<InstanceInfo> infoData(instanceCount);

    for (int32_t i = 0; i < instanceCount; i++)
    {
        const VPT_InstanceData& src = instances[i];
        VkAccelerationStructureInstanceKHR& dst = vkInstances[i];

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = s_BLASes[src.blasID].handle;
        dst.accelerationStructureReference = pfn_vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

        memcpy(&dst.transform, src.transform, sizeof(float) * 12);
        dst.instanceCustomIndex = src.materialID;
        dst.mask = 0xFF;
        dst.instanceShaderBindingTableRecordOffset = 0;
        dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
                  | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;

        // 填充 InstanceInfo（供 shader 读取顶点属性用）
        memcpy(infoData[i].transformRow0, &src.transform[0], sizeof(float) * 4);
        memcpy(infoData[i].transformRow1, &src.transform[4], sizeof(float) * 4);
        memcpy(infoData[i].transformRow2, &src.transform[8], sizeof(float) * 4);
        infoData[i].vertexOffset = s_MeshVertexOffsets[src.blasID];
        infoData[i].indexOffset  = s_MeshIndexOffsets[src.blasID];
        infoData[i].materialID   = src.materialID;
        infoData[i].pad          = 0;
    }

    // ── 创建 InstanceInfo SSBO（shader 可读）──
    VkDeviceSize infoSize = instanceCount * sizeof(InstanceInfo);
    CreateBufferResource(device, gpu, infoSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        s_InstanceInfoBuffer);
    void* infoMapped = nullptr;
    vkMapMemory(device, s_InstanceInfoBuffer.memory, 0, infoSize, 0, &infoMapped);
    memcpy(infoMapped, infoData.data(), infoSize);
    vkUnmapMemory(device, s_InstanceInfoBuffer.memory);

    // ── 创建 TLAS instance buffer ──
    VkDeviceSize bufSize = vkInstances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    CreateBufferResource(device, gpu, bufSize,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s_InstanceBuffer);

    void* mapped = nullptr;
    vkMapMemory(device, s_InstanceBuffer.memory, 0, bufSize, 0, &mapped);
    memcpy(mapped, vkInstances.data(), bufSize);
    vkUnmapMemory(device, s_InstanceBuffer.memory);

    VkAccelerationStructureGeometryInstancesDataKHR instData{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    instData.data.deviceAddress = s_InstanceBuffer.address;

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType     = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = instData;

    uint32_t primCount = instanceCount;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pfn_vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primCount, &sizeInfo);

    VkResult r1 = CreateBufferResource(device, gpu, sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s_TLAS.asBuffer);
    if (r1 != VK_SUCCESS) { fprintf(stderr, "[VPT] TLAS AS buffer alloc FAILED: %d\n", r1); return; }

    VkResult r2 = CreateBufferResource(device, gpu, std::max(sizeInfo.buildScratchSize, (VkDeviceSize)1),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s_TLAS.scratchBuffer);
    if (r2 != VK_SUCCESS) { fprintf(stderr, "[VPT] TLAS scratch buffer alloc FAILED: %d\n", r2); return; }

    VkAccelerationStructureCreateInfoKHR asci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    asci.buffer = s_TLAS.asBuffer.buffer;
    asci.size   = sizeInfo.accelerationStructureSize;
    asci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkResult r3 = pfn_vkCreateAccelerationStructureKHR(device, &asci, nullptr, &s_TLAS.handle);
    if (r3 != VK_SUCCESS) { fprintf(stderr, "[VPT] TLAS vkCreateAccelerationStructure FAILED: %d\n", r3); return; }

    buildInfo.dstAccelerationStructure = s_TLAS.handle;
    buildInfo.scratchData.deviceAddress = s_TLAS.scratchBuffer.address;

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    pfn_vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    fprintf(stderr, "[VPT] TLAS: %d instances built\n", instanceCount);
}

// ═══════════════════════════════════════════════════════════
//  场景更新主入口
// ═══════════════════════════════════════════════════════════

void VPTScene::UpdateScene(const VPT_SceneUpdate& su)
{
    s_Ready = false;
    DestroyResources();

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    VkPhysicalDevice gpu = inst.physicalDevice;
    s_Device = device;

    // 清空上一轮 staging buffers（此时 GPU 已 idle，DestroyResources 中 vkDeviceWaitIdle 已保证）
    for (auto& sb : s_StagingBuffers)
        DestroyBufferResource(device, sb);
    s_StagingBuffers.clear();

    // ── 创建 command buffer（提前到此处，因为 staging copy 需要录入）──
    VkCommandPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = inst.queueFamilyIndex;
    VkCommandPool pool;
    vkCreateCommandPool(device, &poolCI, nullptr, &pool);

    VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool = pool;
    allocCI.commandBufferCount = 1;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocCI, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // ── device-local buffer 创建 + staging 上传（材质 / 光源 / 顶点 / 索引）──
    VkDeviceSize matSize = su.materialCount * sizeof(VPT_Material);
    if (matSize > 0)
    {
        CreateDeviceLocalBufferWithData(device, gpu, cmd,
            su.materials, matSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            s_MaterialBuffer);
    }

    VkDeviceSize lightSize = su.lightCount * sizeof(VPT_Light);
    if (lightSize > 0)
    {
        CreateDeviceLocalBufferWithData(device, gpu, cmd,
            su.lights, lightSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            s_LightBuffer);
    }

    // 统一顶点/索引缓冲（device-local + staging）
    CreateUnifiedBuffers(device, gpu, cmd, su.meshes, su.meshCount);

    // ── Barrier: staging copy (TRANSFER_WRITE) → BLAS build + shader read ──
    VkMemoryBarrier copyBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    copyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                               | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
        | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &copyBarrier, 0, nullptr, 0, nullptr);

    s_BLASes.reserve(su.meshCount);
    for (int32_t i = 0; i < su.meshCount; i++)
        BuildBLAS(cmd, device, gpu, su.meshes[i], i);

    // CRITICAL: Memory barrier between BLAS builds and TLAS build.
    VkMemoryBarrier blasBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        0, 1, &blasBarrier, 0, nullptr, 0, nullptr);

    BuildTLAS(cmd, device, gpu, su.instances, su.instanceCount);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkResult qRes = vkQueueSubmit(inst.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst.graphicsQueue);
    if (qRes != VK_SUCCESS) {
        fprintf(stderr, "[VPT] vkQueueSubmit FAILED: %d\n", qRes);
    }

    // ── GPU 完成，销毁 staging buffers ──
    for (auto& sb : s_StagingBuffers)
        DestroyBufferResource(device, sb);
    s_StagingBuffers.clear();

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, nullptr);

    s_Ready = true;
    fprintf(stderr, "[VPT] Scene: %d meshes, %d mats, %d lights, %d instances\n",
            su.meshCount, su.materialCount, su.lightCount, su.instanceCount);
}

// ═══════════════════════════════════════════════════════════
//  资源销毁
// ═══════════════════════════════════════════════════════════

void VPTScene::DestroyResources()
{
    if (s_Device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(s_Device);

    for (auto& blas : s_BLASes) DestroyBLASResource(s_Device, blas);
    s_BLASes.clear();
    DestroyBufferResource(s_Device, s_UnifiedVertexBuffer);
    DestroyBufferResource(s_Device, s_UnifiedIndexBuffer);
    DestroyBufferResource(s_Device, s_MaterialBuffer);
    DestroyBufferResource(s_Device, s_LightBuffer);
    DestroyBufferResource(s_Device, s_InstanceBuffer);
    DestroyBufferResource(s_Device, s_InstanceInfoBuffer);
    DestroyBLASResource(s_Device, s_TLAS);
    s_MeshVertexOffsets.clear();
    s_MeshIndexOffsets.clear();

    s_Ready = false;
}

VkAccelerationStructureKHR VPTScene::GetTLAS() { return s_TLAS.handle; }
bool VPTScene::IsReady() { return s_Ready; }
bool VPTScene::AreExtensionsReady() { return s_ExtensionsReady; }

// ── Getter（供 dispatch 绑定 SSBO）──
VkBuffer VPTScene::GetUnifiedVertexBuffer() { return s_UnifiedVertexBuffer.buffer; }
VkBuffer VPTScene::GetUnifiedIndexBuffer() { return s_UnifiedIndexBuffer.buffer; }
VkBuffer VPTScene::GetInstanceInfoBuffer() { return s_InstanceInfoBuffer.buffer; }
VkBuffer VPTScene::GetMaterialBuffer() { return s_MaterialBuffer.buffer; }
VkBuffer VPTScene::GetLightBuffer() { return s_LightBuffer.buffer; }
