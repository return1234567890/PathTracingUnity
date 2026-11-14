// VulkanPathTracePlugin — 场景数据上传 + BLAS/TLAS 构建
//
// 扩展函数通过 vkGetDeviceProcAddr 运行时加载（vulkan-1.lib 不导出 KHR 扩展符号）
// 统一顶点/索引缓冲：所有 mesh 数据合并到单个 VkBuffer，shader 通过 InstanceInfo 偏移访问

#include "vpt_scene.h"
#include "vpt_internal.h"
#include "vpt_types.h"
#include "skinning_spv.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <functional>

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
    // ── 变形支持（骨骼动画 refit）：保存构建描述供 mode=UPDATE 复用 ──
    bool                                allowUpdate = false; // 构建时含 ALLOW_UPDATE，可 refit
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};  // 含 geometry 指针，refit 时改 mode/srcAS 后复用
    VkAccelerationStructureGeometryKHR           geometry{};  // 引用统一缓冲地址（缓冲不重建则长期有效）
    VkAccelerationStructureBuildRangeInfoKHR     rangeInfo{};  // primitiveCount
    // ── 阶段3：累计 refit 次数（达到 rebuild 间隔时改走一次 mode=BUILD 全量重建后清零）──
    uint32_t                    refitCount = 0;
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
static std::vector<uint32_t> s_MeshVertexOffsets; // per-mesh vertex offset
static std::vector<uint32_t> s_MeshIndexOffsets;  // per-mesh index offset
static std::vector<uint32_t> s_MeshVertexCounts;  // per-mesh vertex count（GPU 蒙皮注册时校验）
static bool            s_Ready = false;

// ═════════════════════════════════════════════════════════
//  动态场景：TLAS 环形资源（帧在飞行安全）
//
//  每帧重建 TLAS 时，在飞行中的历史帧仍在读旧 TLAS/InstanceInfo，
//  故用 kTLASRingSize 个槽位轮转：CPU 写入槽位 N，GPU 构建槽位 N，
//  dispatch 读槽位 N，而历史帧读槽位 N-1 / N-2。BLAS 静态不动。
// ═════════════════════════════════════════════════════════

static constexpr uint32_t kTLASRingSize = 4; // 覆盖 Unity 渲染线程最多 3 帧在飞行（与 UBO 同样未加 fence，实用上限）

static BLASResource    s_TLASRing[kTLASRingSize];   // 每槽位：AS handle + 存储 buffer
static BufferResource  s_TLASScratch;               // 共享 scratch（同队列串行构建，可共享）

// VkAccelerationStructureInstanceKHR 数组，host-visible 常驻映射，按槽位偏移轮转
// （仅 TLAS 构建时读一次，带宽微置，host-visible 换零拷贝零分配）
static BufferResource  s_InstanceHost;
static void*            s_InstanceHostMapped = nullptr;

// InstanceInfo（shader 每次命中都读，高频随机访问）→ device-local，
// 每帧从常驻映射的 staging 槽位 vkCmdCopyBuffer 上去
static BufferResource  s_InstanceInfoStaging;   // host-visible 常驻映射
static void*            s_InstanceInfoStagingMapped = nullptr;
static BufferResource  s_InstanceInfoDevice;   // device-local，binding 7 SSBO 绑定它

static uint32_t s_InstanceCapacity = 0;  // 每槽位最大实例数
static uint32_t s_CurrentRing      = 0;  // 当前写入/构建/读取的槽位
static uint32_t s_PendingCount     = 0;  // 待构建的实例数
static bool     s_TLASPending      = false; // UpdateInstances 已写主机数据，待 build

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
    s_MeshVertexCounts.clear();
    s_MeshVertexOffsets.reserve(meshCount);
    s_MeshIndexOffsets.reserve(meshCount);
    s_MeshVertexCounts.reserve(meshCount);

    for (int32_t i = 0; i < meshCount; i++)
    {
        s_MeshVertexOffsets.push_back((uint32_t)(totalVertSize / sizeof(VPT_Vertex)));
        s_MeshIndexOffsets.push_back((uint32_t)(totalIdxSize / sizeof(uint32_t)));
        s_MeshVertexCounts.push_back(meshes[i].vertexCount);
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
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // 蒙皮 base 副本需从统一缓冲读出（RegisterSkinnedMesh）
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

    // ── 变形 mesh（骨骼动画）：加 ALLOW_UPDATE，scratch 需容纳 build 与 update 两种尺寸 ──
    bool allowUpdate = (mesh.flags & VPT_MESH_FLAG_DEFORMABLE) != 0;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (allowUpdate)
        buildInfo.flags    |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
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

    VkResult r2 = CreateBufferResource(device, gpu,
        std::max({ sizeInfo.buildScratchSize, sizeInfo.updateScratchSize, (VkDeviceSize)1 }),
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

    // ── 保存构建描述供后续 refit（mode=UPDATE）复用 ──
    // 注意：push_back 会拷贝 BLASResource，geometry 指针必须指向容器内自身的成员
    blas.allowUpdate = allowUpdate;
    blas.geometry    = geom;
    blas.rangeInfo   = rangeInfo;
    blas.buildInfo   = buildInfo;

    s_BLASes.push_back(std::move(blas));
    s_BLASes.back().buildInfo.pGeometries = &s_BLASes.back().geometry;

    fprintf(stderr, "[VPT] BLAS[%u]: %u verts (offset=%u), %u tris%s\n",
            blasIndex, mesh.vertexCount, vertOffset, primCount,
            allowUpdate ? " [deformable: ALLOW_UPDATE]" : "");
}

// ═══════════════════════════════════════════════════════════
//  TLAS 环形资源管理 + 每帧重建（动态场景）
// ═══════════════════════════════════════════════════════════

// 销毁 TLAS 环形资源（调用前 GPU 必须 idle）
static void DestroyTLASRingResources(VkDevice device)
{
    for (uint32_t i = 0; i < kTLASRingSize; i++)
        DestroyBLASResource(device, s_TLASRing[i]);
    DestroyBufferResource(device, s_TLASScratch);

    if (s_InstanceHostMapped) { vkUnmapMemory(device, s_InstanceHost.memory); s_InstanceHostMapped = nullptr; }
    DestroyBufferResource(device, s_InstanceHost);

    if (s_InstanceInfoStagingMapped) { vkUnmapMemory(device, s_InstanceInfoStaging.memory); s_InstanceInfoStagingMapped = nullptr; }
    DestroyBufferResource(device, s_InstanceInfoStaging);
    DestroyBufferResource(device, s_InstanceInfoDevice);

    s_InstanceCapacity = 0;
    s_TLASPending = false;
}

// （重新）分配 TLAS 环形资源，容量 >= capacity。扩容时调用前 GPU 必须 idle。
static bool EnsureTLASResources(VkDevice device, VkPhysicalDevice gpu, uint32_t capacity)
{
    if (capacity == 0) capacity = 1;
    if (capacity <= s_InstanceCapacity && s_InstanceCapacity > 0)
        return true;

    if (s_InstanceCapacity > 0)
        DestroyTLASRingResources(device);

    // ── 查询容量对应的构建尺寸（instance 数据地址不影响尺寸，填 0 即可）──
    VkAccelerationStructureGeometryInstancesDataKHR instData{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    instData.data.deviceAddress = 0;

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType     = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = instData;

    uint32_t primCount = capacity;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pfn_vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primCount, &sizeInfo);

    // ── 每槽位 TLAS 存储 buffer + handle ──
    for (uint32_t i = 0; i < kTLASRingSize; i++)
    {
        BLASResource& slot = s_TLASRing[i];
        VkResult r1 = CreateBufferResource(device, gpu, sizeInfo.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, slot.asBuffer);
        if (r1 != VK_SUCCESS) {
            fprintf(stderr, "[VPT] TLAS ring[%u] AS buffer alloc FAILED: %d\n", i, r1);
            return false;
        }

        VkAccelerationStructureCreateInfoKHR asci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asci.buffer = slot.asBuffer.buffer;
        asci.size   = sizeInfo.accelerationStructureSize;
        asci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VkResult r2 = pfn_vkCreateAccelerationStructureKHR(device, &asci, nullptr, &slot.handle);
        if (r2 != VK_SUCCESS) {
            fprintf(stderr, "[VPT] TLAS ring[%u] vkCreateAccelerationStructure FAILED: %d\n", i, r2);
            return false;
        }
    }

    // ── 共享 scratch（同队列构建串行执行，单个 scratch 即可）──
    VkResult r3 = CreateBufferResource(device, gpu, std::max(sizeInfo.buildScratchSize, (VkDeviceSize)1),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s_TLASScratch);
    if (r3 != VK_SUCCESS) {
        fprintf(stderr, "[VPT] TLAS scratch buffer alloc FAILED: %d\n", r3);
        return false;
    }

    // ── 实例数组：host-visible 常驻映射（仅 TLAS 构建时读一次，带宽微量）──
    VkDeviceSize instSize = (VkDeviceSize)kTLASRingSize * capacity * sizeof(VkAccelerationStructureInstanceKHR);
    r3 = CreateBufferResource(device, gpu, instSize,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s_InstanceHost);
    if (r3 != VK_SUCCESS || vkMapMemory(device, s_InstanceHost.memory, 0, instSize, 0, &s_InstanceHostMapped) != VK_SUCCESS) {
        fprintf(stderr, "[VPT] Instance host buffer alloc FAILED: %d\n", r3);
        return false;
    }

    // ── InstanceInfo：staging（host-visible 映射）→ device（高频 shader 读）──
    VkDeviceSize infoSize = (VkDeviceSize)kTLASRingSize * capacity * sizeof(InstanceInfo);
    r3 = CreateBufferResource(device, gpu, infoSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s_InstanceInfoStaging);
    if (r3 != VK_SUCCESS || vkMapMemory(device, s_InstanceInfoStaging.memory, 0, infoSize, 0, &s_InstanceInfoStagingMapped) != VK_SUCCESS) {
        fprintf(stderr, "[VPT] InstanceInfo staging alloc FAILED: %d\n", r3);
        return false;
    }
    r3 = CreateBufferResource(device, gpu, infoSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s_InstanceInfoDevice);
    if (r3 != VK_SUCCESS) {
        fprintf(stderr, "[VPT] InstanceInfo device buffer alloc FAILED: %d\n", r3);
        return false;
    }

    s_InstanceCapacity = capacity;
    s_CurrentRing      = 0;
    s_TLASPending      = false;
    fprintf(stderr, "[VPT] TLAS ring allocated: %u slots x %u instances\n", kTLASRingSize, capacity);
    return true;
}

// CPU 侧：把实例数据写入指定槽位的 host 映射内存（零分配、零同步）
static bool s_InvalidBlasIDWarned = false;   // 越界 blasID 只告警一次（避免每帧刷屏）

static void WriteInstanceData(const VPT_InstanceData* instances, int32_t instanceCount, uint32_t ringIdx)
{
    VkDeviceSize instOff = (VkDeviceSize)ringIdx * s_InstanceCapacity * sizeof(VkAccelerationStructureInstanceKHR);
    VkDeviceSize infoOff = (VkDeviceSize)ringIdx * s_InstanceCapacity * sizeof(InstanceInfo);

    auto* vkInstances = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(
        (uint8_t*)s_InstanceHostMapped + instOff);
    auto* infoData = reinterpret_cast<InstanceInfo*>(
        (uint8_t*)s_InstanceInfoStagingMapped + infoOff);

    const uint32_t blasCount = (uint32_t)s_BLASes.size();
    if (blasCount == 0) return;   // 没有 BLAS 就没有实例可写，避免退化到 0 号也越界

    for (int32_t i = 0; i < instanceCount; i++)
    {
        const VPT_InstanceData& src = instances[i];
        VkAccelerationStructureInstanceKHR& dst = vkInstances[i];

        // ── blasID 边界检查（§10⑤）：越界 = 越界读 + 野 AS 地址，
        //    vkCmdBuildAccelerationStructuresKHR 拿到垃圾 device address 会直接崩驱动。
        //    处理策略：告警一次 + 退化到第 0 个 BLAS（画面错但不会崩）。
        uint32_t blasID = src.blasID;
        if (blasID >= blasCount || blasID >= s_MeshVertexOffsets.size())
        {
            if (!s_InvalidBlasIDWarned)
            {
                fprintf(stderr, "[VPT] WARN: instance %d has invalid blasID %u "
                                "(blasCount=%u); clamping to 0.\n",
                        i, src.blasID, blasCount);
                s_InvalidBlasIDWarned = true;
            }
            blasID = 0;
        }

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = s_BLASes[blasID].handle;
        dst.accelerationStructureReference = pfn_vkGetAccelerationStructureDeviceAddressKHR(s_Device, &addrInfo);

        memcpy(&dst.transform, src.transform, sizeof(float) * 12);
        dst.instanceCustomIndex = src.materialID;
        dst.mask = 0xFF;
        dst.instanceShaderBindingTableRecordOffset = 0;
        dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
                  | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;

        // 填充 InstanceInfo（供 shader 读取顶点属性用；下标必须与 TLAS 实例数组一致）
        memcpy(infoData[i].transformRow0, &src.transform[0], sizeof(float) * 4);
        memcpy(infoData[i].transformRow1, &src.transform[4], sizeof(float) * 4);
        memcpy(infoData[i].transformRow2, &src.transform[8], sizeof(float) * 4);
        infoData[i].vertexOffset = s_MeshVertexOffsets[blasID];
        infoData[i].indexOffset  = s_MeshIndexOffsets[blasID];
        infoData[i].materialID   = src.materialID;
        infoData[i].pad          = 0;
    }
}

// GPU 侧：向 cmd 录制 InstanceInfo 上传 + TLAS 重建（目标槽位 ringIdx）
static void RecordTLASBuild(VkCommandBuffer cmd, VkDevice device, uint32_t ringIdx, uint32_t instanceCount)
{
    if (instanceCount == 0) return;

    VkDeviceSize instOff = (VkDeviceSize)ringIdx * s_InstanceCapacity * sizeof(VkAccelerationStructureInstanceKHR);
    VkDeviceSize infoOff = (VkDeviceSize)ringIdx * s_InstanceCapacity * sizeof(InstanceInfo);

    // Barrier A: host 写 staging → transfer 读
    VkMemoryBarrier hostToTransfer{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    hostToTransfer.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &hostToTransfer, 0, nullptr, 0, nullptr);

    // InstanceInfo staging → device-local（本槽位切片）
    VkBufferCopy infoCopy{};
    infoCopy.srcOffset = infoOff;
    infoCopy.dstOffset = infoOff;
    infoCopy.size      = (VkDeviceSize)instanceCount * sizeof(InstanceInfo);
    vkCmdCopyBuffer(cmd, s_InstanceInfoStaging.buffer, s_InstanceInfoDevice.buffer, 1, &infoCopy);

    // Barrier B: host 写实例数组 + transfer 写 InstanceInfo → AS 构建/着色器读
    VkMemoryBarrier preBuild{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    preBuild.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    preBuild.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &preBuild, 0, nullptr, 0, nullptr);

    // ── TLAS 重建（每帧全量 rebuild；instance 数少成本极低，且无 refit 退化问题）──
    VkAccelerationStructureGeometryInstancesDataKHR instData{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    instData.data.deviceAddress = s_InstanceHost.address + instOff;

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType     = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances = instData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &geom;
    buildInfo.dstAccelerationStructure = s_TLASRing[ringIdx].handle;
    buildInfo.scratchData.deviceAddress = s_TLASScratch.address;

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    pfn_vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

    // Barrier C: TLAS 构建写 → 本帧 dispatch 读
    VkMemoryBarrier postBuild{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    postBuild.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    postBuild.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &postBuild, 0, nullptr, 0, nullptr);
}

// ═══════════════════════════════════════════════════════════
//  骨骼动画：BLAS 原地 refit
//
//  GPU 蒙皮 compute 就地写统一顶点缓冲后，dispatch 前录制 mode=UPDATE refit。
//  hit shader 从统一顶点缓冲直接读 VPT_Vertex 插值法线/切线，
//  顶点就地更新后着色属性自动跟随，无需改 pathtrace.glsl。
// ═══════════════════════════════════════════════════════════

static bool                    s_BlasRefitPending     = false;

// ── 阶段3：refit 质量衰减对策 ──
// refit 是原地包络微调，大幅变形多帧后 BVH 遍历效率持续下降；
// 变形 BLAS 累计 refit 达到间隔后改走一次全量 rebuild（mode=BUILD）恢复质量。
// interval=0 禁用（始终 refit）。计数随 s_BLASes 在场景重建时一起清零。
static uint32_t                s_DeformableRebuildInterval = 0;
static bool                    s_RebuildLogged = false; // 首次触发全量 rebuild 时提示一次

// ── GPU 蒙皮（阶段2）状态：声明在此，供 RecordBLASRefitIfNeeded 联动消费 ──
//
// 每个注册的蒙皮 mesh 持有：bind-pose base 副本（蒙皮 compute 只读源）+
// 权重/关节索引 SSBO + 关节矩阵 host 环形缓冲（kTLASRingSize 槽，帧在飞安全）。
// 统一顶点缓冲的着色属性（UV/颜色）与 BLAS 引用地址均不变，
// 蒙皮就地写 pos/normal/tangent 后 hit shader 着色属性自动跟随。
struct SkinMeshResource
{
    uint32_t     blasID       = 0;
    uint32_t     vertexCount  = 0;
    uint32_t     jointCount   = 0;
    BufferResource baseVertices;   // device-local：bind-pose VPT_Vertex 副本
    BufferResource weights;        // device-local：vec4/顶点
    BufferResource joints;        // device-local：ivec4/顶点
    BufferResource jointMatrices; // host-visible 常驻映射：kTLASRingSize × jointCount × 64B
    void*        jointMapped  = nullptr;
};
static std::vector<SkinMeshResource> s_SkinMeshes;          // 已注册蒙皮 mesh（按 blasID 查找）
static std::vector<uint32_t>        s_PendingSkinUpdates;    // 待蒙皮 dispatch 的 blasID（同帧重复推送原位覆盖）
static bool                         s_SkinPending     = false;
static uint32_t                     s_JointRing        = 0;   // 当前关节矩阵写入槽位
static bool                         s_SkinNotRegisteredWarned = false;

// GPU 侧：录制 BLAS refit + barrier 链。
// refit 列表来自 GPU 蒙皮更新（RecordGPUSkinningIfNeeded 已先行录制蒙皮 compute
// 并把待 refit 的 blasID 留在 s_PendingSkinUpdates）。
void VPTScene::RecordBLASRefitIfNeeded(VkCommandBuffer cmd)
{
    if (!s_BlasRefitPending || cmd == VK_NULL_HANDLE || s_PendingSkinUpdates.empty())
    {
        s_PendingSkinUpdates.clear();
        s_BlasRefitPending = false;
        return;
    }

    // ── BLAS 原地 refit（srcAS = dstAS，mode=UPDATE）──
    // 各 BLAS 独享 scratch，理论上无共享冲突；仍按参考项目在连续 refit 间
    // 插 AS→AS barrier，成本可忽略，规避驱动乱序执行隐患。
    // 阶段3：累计 refit 达到 s_DeformableRebuildInterval 的 BLAS 本帧改走全量 rebuild
    //（dstAS/scratch 复用现有资源，scratch 构建时已按 max(build,update) 分配）
    for (size_t i = 0; i < s_PendingSkinUpdates.size(); i++)
    {
        BLASResource& blas = s_BLASes[s_PendingSkinUpdates[i]];

        bool fullRebuild = s_DeformableRebuildInterval > 0
                        && ++blas.refitCount >= s_DeformableRebuildInterval;
        if (fullRebuild)
        {
            blas.refitCount = 0;
            if (!s_RebuildLogged)
            {
                fprintf(stderr, "[VPT] BLAS[%u] deformable refit count reached interval %u, "
                                "switching to periodic full rebuild (mode=BUILD).\n",
                        s_PendingSkinUpdates[i], s_DeformableRebuildInterval);
                s_RebuildLogged = true;
            }
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = blas.buildInfo;
        buildInfo.mode = fullRebuild
            ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        buildInfo.srcAccelerationStructure  = fullRebuild ? VK_NULL_HANDLE : blas.handle;
        buildInfo.dstAccelerationStructure  = blas.handle;
        buildInfo.scratchData.deviceAddress = blas.scratchBuffer.address;

        VkAccelerationStructureBuildRangeInfoKHR rangeInfo = blas.rangeInfo;
        const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
        pfn_vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);

        if (i + 1 < s_PendingSkinUpdates.size())
        {
            VkMemoryBarrier asToAs{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            asToAs.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            asToAs.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                0, 1, &asToAs, 0, nullptr, 0, nullptr);
        }
    }

    // Barrier C: refit 写 → 本帧 dispatch 读 + TLAS 构建读
    VkMemoryBarrier postRefit{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    postRefit.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    postRefit.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &postRefit, 0, nullptr, 0, nullptr);

    // 消费完毕
    s_PendingSkinUpdates.clear();
    s_BlasRefitPending = false;
}

// 阶段3：refit 质量衰减对策——设置变形 BLAS 全量 rebuild 间隔（C# Inspector 暴露）。
// interval=0 禁用；计数按 BLAS 在 RecordBLASRefitIfNeeded 中累计（即 refit 实际
// 发生次数，静止帧不计数），场景重建时随 s_BLASes 一起清零。
void VPTScene::SetDeformableRebuildInterval(uint32_t interval)
{
    s_DeformableRebuildInterval = interval;
    s_RebuildLogged = false; // 场景/设置变化后允许再次提示，便于确认参数生效
}

// ═════════════════════════════════════════════════════════
//  骨骼动画（阶段2 GPU 蒙皮）：对齐参考项目 AnimationVk
//
//  静态数据一次性上传（RegisterSkinnedMesh）：base 副本自统一顶点缓冲复制、
//  权重/关节索引经一次性 staging 上传；每帧只传关节矩阵（host-visible
//  常驻映射环形槽，参考项目性能关键）。蒙皮 compute 无 descriptor set，
//  所有缓冲地址经 push constant BDA 指针传入，就地写统一顶点缓冲后
//  BLAS refit（由 RecordBLASRefitIfNeeded 统一录制）。
// ═════════════════════════════════════════════════════════

// push constant 布局（与 skinning.comp.glsl 一致，48B）
struct SkinPushConstants
{
    uint64_t baseVertexAddr; // bind-pose 副本（device-local，蒙皮只读源）
    uint64_t outVertexAddr;   // 统一顶点缓冲中该 mesh 区段首地址（就地写目标）
    uint64_t weightsAddr;
    uint64_t jointsAddr;
    uint64_t jointMatsAddr;   // 关节矩阵（本帧环形槽位）
    uint32_t vertexCount;
    uint32_t numJoints;
};
static_assert(sizeof(SkinPushConstants) == 48, "SkinPushConstants must be 48 bytes");

static VkPipelineLayout s_SkinPipeLayout = VK_NULL_HANDLE;
static VkPipeline       s_SkinPipeline   = VK_NULL_HANDLE;

// 銷毁蒙皮资源（GPU idle 后由 DestroyResources 调用）
static void DestroySkinResources(VkDevice device)
{
    for (auto& m : s_SkinMeshes)
    {
        if (m.jointMapped) { vkUnmapMemory(device, m.jointMatrices.memory); m.jointMapped = nullptr; }
        DestroyBufferResource(device, m.baseVertices);
        DestroyBufferResource(device, m.weights);
        DestroyBufferResource(device, m.joints);
        DestroyBufferResource(device, m.jointMatrices);
    }
    s_SkinMeshes.clear();
    s_PendingSkinUpdates.clear();
    s_SkinPending = false;
    s_JointRing   = 0;

    if (s_SkinPipeline) { vkDestroyPipeline(device, s_SkinPipeline, nullptr); s_SkinPipeline = VK_NULL_HANDLE; }
    if (s_SkinPipeLayout) { vkDestroyPipelineLayout(device, s_SkinPipeLayout, nullptr); s_SkinPipeLayout = VK_NULL_HANDLE; }
}

// 惰性创建蒙皮 compute 管线（无 descriptor set，仅 push constant）
static bool EnsureSkinPipeline(VkDevice device)
{
    if (s_SkinPipeline != VK_NULL_HANDLE)
        return true;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(SkinPushConstants);

    VkPipelineLayoutCreateInfo plCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges   = &pushRange;
    VkResult res = vkCreatePipelineLayout(device, &plCI, nullptr, &s_SkinPipeLayout);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] Skin: vkCreatePipelineLayout failed: %d\n", res);
        return false;
    }

    VkShaderModuleCreateInfo smCI{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smCI.codeSize = g_SkinningSpv_size;
    smCI.pCode    = g_SkinningSpv;
    VkShaderModule shaderModule;
    res = vkCreateShaderModule(device, &smCI, nullptr, &shaderModule);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] Skin: vkCreateShaderModule failed: %d\n", res);
        return false;
    }

    VkComputePipelineCreateInfo cpCI{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module  = shaderModule;
    cpCI.stage.pName    = "main";
    cpCI.layout        = s_SkinPipeLayout;
    res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &s_SkinPipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (res != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT] Skin: vkCreateComputePipelines failed: %d\n", res);
        return false;
    }

    fprintf(stderr, "[VPT] Skinning pipeline initialized (push-constant BDA, 256/invocation).\n");
    return true;
}

// 一次性命令提交辅助（注册为低频路径）：创建临时 pool/cmd，录制后提交并等待完成
static bool OneShotSubmit(VkDevice device, uint32_t queueFamily,
                          const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = queueFamily;
    VkCommandPool pool;
    if (vkCreateCommandPool(device, &poolCI, nullptr, &pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool = pool;
    allocCI.commandBufferCount = 1;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocCI, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    record(cmd);
    vkEndCommandBuffer(cmd);

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(inst.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst.graphicsQueue);

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyCommandPool(device, pool, nullptr);
    return true;
}

// 释放单个 SkinMeshResource（防御重复注册/失败路径共用）
static void DestroySkinMeshResource(VkDevice device, SkinMeshResource& m)
{
    if (m.jointMapped) { vkUnmapMemory(device, m.jointMatrices.memory); m.jointMapped = nullptr; }
    DestroyBufferResource(device, m.baseVertices);
    DestroyBufferResource(device, m.weights);
    DestroyBufferResource(device, m.joints);
    DestroyBufferResource(device, m.jointMatrices);
    m = {};
}

void VPTScene::RegisterSkinnedMesh(uint32_t blasID,
                                    const float* weights, const int32_t* joints,
                                    uint32_t vertexCount, uint32_t jointCount)
{
    if (!s_ExtensionsReady || !s_Ready || !weights || !joints
        || vertexCount == 0 || jointCount == 0)
        return;
    if (blasID >= s_BLASes.size())
    {
        fprintf(stderr, "[VPT] RegisterSkinnedMesh: blasID %u out of range (%zu BLASes)\n",
                blasID, s_BLASes.size());
        return;
    }
    if (!s_BLASes[blasID].allowUpdate)
    {
        fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] built without ALLOW_UPDATE. Skipped.\n", blasID);
        return;
    }
    if (blasID >= s_MeshVertexCounts.size() || vertexCount != s_MeshVertexCounts[blasID])
    {
        fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] vertex count mismatch "
                        "(%u given, %u uploaded). Skipped.\n",
                blasID, vertexCount,
                blasID < s_MeshVertexCounts.size() ? s_MeshVertexCounts[blasID] : 0);
        return;
    }

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    VkPhysicalDevice gpu = inst.physicalDevice;

    if (!EnsureSkinPipeline(device))
        return;

    // 防御：同 BLAS 重复注册 → 先释放并移除旧条目（正常流程在场景重建时已整体销毁）
    for (auto it = s_SkinMeshes.begin(); it != s_SkinMeshes.end(); ++it)
    {
        if (it->blasID == blasID)
        {
            fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] already registered, re-registering.\n", blasID);
            DestroySkinMeshResource(device, *it);
            s_SkinMeshes.erase(it);
            break;
        }
    }

    SkinMeshResource sm{};
    sm.blasID      = blasID;
    sm.vertexCount = vertexCount;
    sm.jointCount  = jointCount;

    VkDeviceSize baseBytes = (VkDeviceSize)vertexCount * sizeof(VPT_Vertex);
    VkDeviceSize attrBytes  = (VkDeviceSize)vertexCount * 16; // vec4 / ivec4 每顶点
    VkDeviceSize matBytes   = (VkDeviceSize)kTLASRingSize * jointCount * 64;

    constexpr VkBufferUsageFlags ssboUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                           | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    bool ok = CreateBufferResource(device, gpu, baseBytes,
        ssboUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sm.baseVertices) == VK_SUCCESS
        && CreateBufferResource(device, gpu, attrBytes,
            ssboUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sm.weights) == VK_SUCCESS
        && CreateBufferResource(device, gpu, attrBytes,
            ssboUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sm.joints) == VK_SUCCESS
        && CreateBufferResource(device, gpu, matBytes,
            ssboUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            sm.jointMatrices) == VK_SUCCESS
        && vkMapMemory(device, sm.jointMatrices.memory, 0, matBytes, 0, &sm.jointMapped) == VK_SUCCESS;

    if (ok)
    {
        // weights/joints 一次性 staging（注册低频，独立提交等待完成）
        BufferResource wStaging{}, jStaging{};
        void* wm = nullptr, * jm = nullptr;
        ok = CreateBufferResource(device, gpu, attrBytes,
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  wStaging) == VK_SUCCESS
              && CreateBufferResource(device, gpu, attrBytes,
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  jStaging) == VK_SUCCESS;
        if (ok)
        {
            vkMapMemory(device, wStaging.memory, 0, attrBytes, 0, &wm);
            memcpy(wm, weights, (size_t)attrBytes);
            vkUnmapMemory(device, wStaging.memory);
            vkMapMemory(device, jStaging.memory, 0, attrBytes, 0, &jm);
            memcpy(jm, joints, (size_t)attrBytes);
            vkUnmapMemory(device, jStaging.memory);
        }

        if (ok)
        {
            OneShotSubmit(device, inst.queueFamilyIndex, [&](VkCommandBuffer cmd)
            {
                // base 副本：直接自统一顶点缓冲复制对应区段（此时内容即 bind-pose）
                // 注意 srcOffset 必须指向该 mesh 自己的区段（偏移 0 是第一个 mesh 的顶点）
                VkBufferCopy baseRegion{};
                baseRegion.srcOffset = (VkDeviceSize)s_MeshVertexOffsets[blasID] * sizeof(VPT_Vertex);
                baseRegion.dstOffset = 0;
                baseRegion.size      = baseBytes;
                vkCmdCopyBuffer(cmd, s_UnifiedVertexBuffer.buffer, sm.baseVertices.buffer, 1, &baseRegion);

                VkBufferCopy attrRegion{};
                attrRegion.size = attrBytes;
                vkCmdCopyBuffer(cmd, wStaging.buffer, sm.weights.buffer, 1, &attrRegion);
                vkCmdCopyBuffer(cmd, jStaging.buffer,  sm.joints.buffer,  1, &attrRegion);
            });
        }

        DestroyBufferResource(device, wStaging);
        DestroyBufferResource(device, jStaging);
    }

    if (!ok)
    {
        fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] resource creation FAILED\n", blasID);
        DestroySkinMeshResource(device, sm);
        return;
    }

    s_SkinMeshes.push_back(std::move(sm));

    fprintf(stderr, "[VPT] Skinned mesh registered: BLAS[%u], %u verts, %u joints\n",
            blasID, vertexCount, jointCount);
}

void VPTScene::UpdateSkinning(uint32_t blasID, const float* jointMatrices, uint32_t jointCount)
{
    if (!s_Ready || !jointMatrices || jointCount == 0)
        return;

    SkinMeshResource* sm = nullptr;
    for (auto& m : s_SkinMeshes)
        if (m.blasID == blasID) { sm = &m; break; }
    if (!sm)
    {
        if (!s_SkinNotRegisteredWarned)
        {
            fprintf(stderr, "[VPT] UpdateSkinning: BLAS[%u] not registered "
                            "(VPT_RegisterSkinnedMesh missing). Skipped.\n", blasID);
            s_SkinNotRegisteredWarned = true;
        }
        return;
    }
    if (jointCount != sm->jointCount)
    {
        fprintf(stderr, "[VPT] UpdateSkinning: BLAS[%u] joint count changed (%u -> %u). Skipped.\n",
                blasID, sm->jointCount, jointCount);
        return;
    }

    // 写入当前环形槽位（零分配零同步；同帧/未消费重复推送原位覆盖同槽）
    VkDeviceSize off = (VkDeviceSize)s_JointRing * sm->jointCount * 64;
    memcpy((uint8_t*)sm->jointMapped + off, jointMatrices, (size_t)jointCount * 64);

    if (std::find(s_PendingSkinUpdates.begin(), s_PendingSkinUpdates.end(), blasID)
        == s_PendingSkinUpdates.end())
        s_PendingSkinUpdates.push_back(blasID);

    s_SkinPending     = true;
    s_BlasRefitPending = true; // 蒙皮后 refit 由 RecordBLASRefitIfNeeded 统一录制
}

void VPTScene::RecordGPUSkinningIfNeeded(VkCommandBuffer cmd)
{
    if (!s_SkinPending || cmd == VK_NULL_HANDLE || s_PendingSkinUpdates.empty())
        return;

    // Barrier A: host 写关节矩阵 → compute 读
    VkMemoryBarrier hostToCompute{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    hostToCompute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &hostToCompute, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_SkinPipeline);

    for (uint32_t blasID : s_PendingSkinUpdates)
    {
        SkinMeshResource* sm = nullptr;
        for (auto& m : s_SkinMeshes)
            if (m.blasID == blasID) { sm = &m; break; }
        if (!sm)
            continue;

        SkinPushConstants pc{};
        pc.baseVertexAddr = sm->baseVertices.address;
        pc.outVertexAddr  = s_UnifiedVertexBuffer.address
                          + (VkDeviceSize)s_MeshVertexOffsets[blasID] * sizeof(VPT_Vertex);
        pc.weightsAddr    = sm->weights.address;
        pc.jointsAddr     = sm->joints.address;
        pc.jointMatsAddr  = sm->jointMatrices.address
                          + (VkDeviceSize)s_JointRing * sm->jointCount * 64;
        pc.vertexCount    = sm->vertexCount;
        pc.numJoints      = sm->jointCount;

        vkCmdPushConstants(cmd, s_SkinPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (sm->vertexCount + 255) / 256, 1, 1);
    }

    // Barrier B: compute 写统一顶点缓冲 → BLAS refit（AS build）读 + 着色器读
    VkMemoryBarrier skinBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    skinBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    skinBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                              | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &skinBarrier, 0, nullptr, 0, nullptr);

    // 消费：轮转关节矩阵槽位（在飞历史帧仍在读旧槽）；
    // s_PendingSkinUpdates 保留给紧随其后的 RecordBLASRefitIfNeeded 做 refit
    s_JointRing  = (s_JointRing + 1) % kTLASRingSize;
    s_SkinPending = false;
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

    // ── TLAS 环形资源分配（静态拓扑变化时一次性）+ 首次构建（槽位 0）──
    if (EnsureTLASResources(device, gpu, (uint32_t)(su.instanceCount > 0 ? su.instanceCount : 1)))
    {
        s_CurrentRing = 0;
        WriteInstanceData(su.instances, su.instanceCount, 0);
        RecordTLASBuild(cmd, device, 0, (uint32_t)su.instanceCount);
    }
    else
    {
        fprintf(stderr, "[VPT] TLAS ring allocation FAILED\n");
    }

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
    DestroyTLASRingResources(s_Device);
    DestroySkinResources(s_Device);
    s_SkinNotRegisteredWarned = false;
    s_MeshVertexOffsets.clear();
    s_MeshIndexOffsets.clear();
    s_MeshVertexCounts.clear();

    s_Ready = false;
}

// ═══════════════════════════════════════════════════════════
//  动态场景：每帧实例更新（仅刚体 Transform，BLAS 不动）
// ═══════════════════════════════════════════════════════════

void VPTScene::UpdateInstances(const VPT_InstanceData* instances, int32_t instanceCount)
{
    if (!s_ExtensionsReady || !instances || instanceCount <= 0 || s_BLASes.empty())
        return;

    // 扩容（实例数超过预分配容量，低频路径）：需 GPU idle 后重建环形资源
    if ((uint32_t)instanceCount > s_InstanceCapacity)
    {
        const UnityVulkanInstance& inst = VPT_GetInstance();
        vkDeviceWaitIdle(inst.device);
        if (!EnsureTLASResources(inst.device, inst.physicalDevice, (uint32_t)instanceCount))
            return;
        // 扩容后所有槽位均未构建，当前帧必须重建（s_TLASPending 由下方统一置位）
    }

    // 轮转槽位：历史在飞行帧仍在读旧槽位，写入新槽位避免覆盖
    s_CurrentRing = (s_CurrentRing + 1) % kTLASRingSize;
    WriteInstanceData(instances, instanceCount, s_CurrentRing);

    s_PendingCount = (uint32_t)instanceCount;
    s_TLASPending  = true;
}

void VPTScene::RecordTLASUpdateIfNeeded(VkCommandBuffer cmd)
{
    if (!s_TLASPending || cmd == VK_NULL_HANDLE)
        return;

    RecordTLASBuild(cmd, s_Device, s_CurrentRing, s_PendingCount);
    s_TLASPending = false;
}

VkAccelerationStructureKHR VPTScene::GetTLAS() { return s_TLASRing[s_CurrentRing].handle; }
bool VPTScene::IsReady() { return s_Ready; }
bool VPTScene::AreExtensionsReady() { return s_ExtensionsReady; }

// ── 公开工具函数（委托到文件内 static 实现）──
uint32_t VPTScene::FindMemoryType(VkPhysicalDevice gpu, uint32_t typeFilter, VkMemoryPropertyFlags props)
{
    return ::FindMemoryType(gpu, typeFilter, props);
}

// ── Getter（供 dispatch 绑定 SSBO）──
VkBuffer VPTScene::GetUnifiedVertexBuffer() { return s_UnifiedVertexBuffer.buffer; }
VkBuffer VPTScene::GetUnifiedIndexBuffer() { return s_UnifiedIndexBuffer.buffer; }
VkBuffer VPTScene::GetInstanceInfoBuffer() { return s_InstanceInfoDevice.buffer; }
VkDeviceSize VPTScene::GetInstanceInfoOffset()
{
    return (VkDeviceSize)s_CurrentRing * s_InstanceCapacity * sizeof(InstanceInfo);
}
VkBuffer VPTScene::GetMaterialBuffer() { return s_MaterialBuffer.buffer; }
VkBuffer VPTScene::GetLightBuffer() { return s_LightBuffer.buffer; }
