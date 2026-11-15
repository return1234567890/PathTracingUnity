// VulkanPathTracePlugin — 场景数据上传 + BLAS/TLAS 构建
//
// 扩展函数通过 vkGetDeviceProcAddr 运行时加载（vulkan-1.lib 不导出 KHR 扩展符号）
// 统一顶点/索引缓冲：所有 mesh 数据合并到单个 VkBuffer，shader 通过 InstanceInfo 偏移访问

#include "vpt_scene.h"
#include "vpt_internal.h"
#include "vpt_types.h"
#include "vpt_trace.h"
#include "skinning_spv.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <functional>
#include <mutex>

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
// 每材质 flags 快照：TLAS 实例的 opaque 标记要按材质判断，而 WriteInstanceData 在动态场景
// 的每帧路径上也会被调用（那时 s_PendingMaterials 已被清空），所以单独留一份。
static std::vector<uint32_t> s_MaterialFlagsSnapshot;
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

// 实例数超过容量时的延迟扩容（主线程只拷贝，渲染线程里等待 idle 再重建）
static bool                          s_ResizePending = false;
static uint32_t                      s_ResizeCount   = 0;
static std::vector<VPT_InstanceData> s_ResizeInstances;

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

// ═══════════════════════════════════════════════════════════
//  延迟场景推送（Plan B）
//
//  C# 侧 VPT_UpdateScene 可能来自主线程（Start / OnValidate→Invoke），
//  而 Unity 的 VkQueue 归渲染线程所有：主线程直接 vkQueueSubmit /
//  vkDeviceWaitIdle 与渲染线程并发使用同一条队列，会产生 GPU 挂死
//  （实测触发 nvlddmkm TDR + VK_ERROR_DEVICE_LOST）。
//
//  因此 PushScene 只做 CPU 深拷贝，真正的资源创建/上传/AS 构建放在
//  RecordSceneUpdateIfNeeded 里，用 Plan B 的 Unity command buffer 录制，
//  由 Unity 自己提交 —— 全程不出现插件自己的 vkQueueSubmit。
// ═══════════════════════════════════════════════════════════

struct PendingMeshData
{
    std::vector<VPT_Vertex> vertices;
    std::vector<uint32_t>   indices;
    uint32_t                materialID = 0;
    uint32_t                flags      = 0;
};

// 主线程（PushScene / UpdateInstances / 注册蒙皮）与渲染线程（*Record*IfNeeded）
// 会同时访问这些待处理数据：一把锁把它们串行化（recursive 便于内部 helper 复用）。
static std::recursive_mutex s_PendingMutex;

static std::vector<PendingMeshData>  s_PendingMeshes;
static std::vector<VPT_Material>     s_PendingMaterials;
static std::vector<VPT_Light>        s_PendingLights;
static std::vector<VPT_InstanceData> s_PendingSceneInstances;
static bool                          s_ScenePending = false;

// 重建帧之后做一次 GPU 同步探针（见 vpt_scene.h 说明）
static bool                          s_NeedGpuSyncProbe = false;

bool VPTScene::TakeGpuSyncProbe()
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);
    const bool v = s_NeedGpuSyncProbe;
    s_NeedGpuSyncProbe = false;
    return v;
}

// 创建 device-local buffer 并通过 staging buffer 上传数据。
// staging buffer 被记录到 s_StagingBuffers，等下一次场景重建（或 VPT_Destroy）
// 时统一销毁 —— 这些拷贝是本帧录进 Unity 命令缓冲的，不能立即释放。
//
// 返回值必须检查：分配失败时 outDevice.buffer/address 会是 0，若继续把它录进
// 命令缓冲（vkCmdCopyBuffer 目标为 null、AS build 读地址 0），GPU 会直接挂死
// 并触发 TDR —— 这正是"第二次 Play 卡死"最可能的形态。
static bool CreateDeviceLocalBufferWithData(
    VkDevice device, VkPhysicalDevice gpu,
    VkCommandBuffer cmd,
    const void* srcData, VkDeviceSize size,
    VkBufferUsageFlags usage,
    BufferResource& outDevice)
{
    // 1. device-local 目标 buffer（GPU VRAM）
    VkResult r = CreateBufferResource(device, gpu, size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        outDevice);
    if (r != VK_SUCCESS)
    {
        VPTTrace::Line("alloc FAILED: device buffer size=%llu usage=0x%x -> %d",
                       (unsigned long long)size, (unsigned)usage, (int)r);
        VPT_CheckVk(r, "CreateDeviceLocalBufferWithData(device)");
        return false;
    }

    // 2. host-visible staging buffer（仅用于一次性上传）
    BufferResource staging{};
    r = CreateBufferResource(device, gpu, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging);
    if (r != VK_SUCCESS)
    {
        VPTTrace::Line("alloc FAILED: staging buffer size=%llu -> %d",
                       (unsigned long long)size, (int)r);
        VPT_CheckVk(r, "CreateDeviceLocalBufferWithData(staging)");
        DestroyBufferResource(device, outDevice);
        return false;
    }

    // 3. CPU 写入 staging
    void* m = nullptr;
    r = vkMapMemory(device, staging.memory, 0, size, 0, &m);
    if (r != VK_SUCCESS || m == nullptr)
    {
        VPTTrace::Line("vkMapMemory(staging) FAILED -> %d", (int)r);
        VPT_CheckVk(r, "vkMapMemory(staging)");
        DestroyBufferResource(device, staging);
        DestroyBufferResource(device, outDevice);
        return false;
    }
    memcpy(m, srcData, size);
    vkUnmapMemory(device, staging.memory);

    // 4. GPU 拷贝 staging → device-local（记录到 command buffer）
    VkBufferCopy copyRegion{ 0, 0, size };
    vkCmdCopyBuffer(cmd, staging.buffer, outDevice.buffer, 1, &copyRegion);

    // 5. staging 延迟销毁
    s_StagingBuffers.push_back(staging);
    return true;
}

// ═══════════════════════════════════════════════════════════
//  统一缓冲区创建 + 数据上传（device-local + staging）
// ═══════════════════════════════════════════════════════════

static bool CreateUnifiedBuffers(
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
        totalVertSize += meshCount > 0 ? (VkDeviceSize)meshes[i].vertexCount * sizeof(VPT_Vertex) : 0;
        totalIdxSize  += meshCount > 0 ? (VkDeviceSize)meshes[i].indexCount * sizeof(uint32_t) : 0;
    }

    if (totalVertSize == 0) totalVertSize = 4;
    if (totalIdxSize == 0) totalIdxSize = 4;

    // 2. 创建统一顶点缓冲（device-local + staging 上传）
    //    先收集所有顶点数据到临时内存
    std::vector<uint8_t> vertData(totalVertSize);
    std::vector<uint8_t> idxData(totalIdxSize);
    for (int32_t i = 0; i < meshCount; i++)
    {
        VkDeviceSize vSize = (VkDeviceSize)meshes[i].vertexCount * sizeof(VPT_Vertex);
        VkDeviceSize iSize = (VkDeviceSize)meshes[i].indexCount * sizeof(uint32_t);
        if (vSize > 0 && meshes[i].vertices)
            memcpy(vertData.data() + (size_t)s_MeshVertexOffsets[i] * sizeof(VPT_Vertex),
                   meshes[i].vertices, (size_t)vSize);
        if (iSize > 0 && meshes[i].indices)
            memcpy(idxData.data() + (size_t)s_MeshIndexOffsets[i] * sizeof(uint32_t),
                   meshes[i].indices, (size_t)iSize);
    }

    if (!CreateDeviceLocalBufferWithData(device, gpu, cmd,
        vertData.data(), totalVertSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, // 蒙皮 base 副本需从统一缓冲读出（RegisterSkinnedMesh）
        s_UnifiedVertexBuffer))
    {
        VPTTrace::Line("scene: unified VERTEX buffer upload failed (%llu bytes) -> abort rebuild",
                       (unsigned long long)totalVertSize);
        return false;
    }

    if (!CreateDeviceLocalBufferWithData(device, gpu, cmd,
        idxData.data(), totalIdxSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        s_UnifiedIndexBuffer))
    {
        VPTTrace::Line("scene: unified INDEX buffer upload failed (%llu bytes) -> abort rebuild",
                       (unsigned long long)totalIdxSize);
        DestroyBufferResource(device, s_UnifiedVertexBuffer);
        return false;
    }

    // 顶点/索引缓冲的 device address 必须非 0：AS build 直接拿它当输入指针
    if (s_UnifiedVertexBuffer.address == 0 || s_UnifiedIndexBuffer.address == 0)
    {
        VPTTrace::Line("scene: unified buffer device address is ZERO (v=%llu i=%llu) -> abort rebuild",
                       (unsigned long long)s_UnifiedVertexBuffer.address,
                       (unsigned long long)s_UnifiedIndexBuffer.address);
        DestroyBufferResource(device, s_UnifiedVertexBuffer);
        DestroyBufferResource(device, s_UnifiedIndexBuffer);
        return false;
    }

    VPTTrace::Line("scene: unified buffers ok (verts=%llu idx=%llu, addr=0x%llx/0x%llx)",
                   (unsigned long long)totalVertSize, (unsigned long long)totalIdxSize,
                   (unsigned long long)s_UnifiedVertexBuffer.address,
                   (unsigned long long)s_UnifiedIndexBuffer.address);
    return true;
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
    // 几何级不再一律标 OPAQUE：半透明实例必须 non-opaque，硬件才会为它产生 candidate 交点
    //（阴影 any-hit 遍历依赖 candidate）。不透明实例改由 TLAS 实例级的 FORCE_OPAQUE 兜住，
    // 依然走硬件快路径，见 WriteInstanceData。
    // 主路径 rayQuery 仍传 gl_RayFlagsOpaqueEXT，所以本次改动对主光线零影响。
    geom.flags            = 0;

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

        // ── 实例 opaque 标记：只给真不透明材质 FORCE_OPAQUE ──
        // 半透明实例必须 non-opaque，否则硬件不会给出 candidate 交点，
        // shader 的 any-hit 阴影遍历就看不到玻璃（玻璃会退化成纯遮挡）。
        // 不透明实例保留 FORCE_OPAQUE：硬件 opaque 快路径 + TerminateOnFirstHit 立即结束遍历。
        // materialID 越界（正常不会）时按不透明处理，保证不会把实心物体误判成玻璃。
        const bool transparentMat =
            src.materialID < s_MaterialFlagsSnapshot.size() &&
            (s_MaterialFlagsSnapshot[src.materialID] & VPT_MAT_FLAG_TRANSPARENT) != 0u;

        dst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if (!transparentMat)
            dst.flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;

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

// ── 延迟蒙皮注册（Plan B）──
// C# 在 VPT_UpdateScene 之后紧接着调用 VPT_RegisterSkinnedMesh，而场景构建已经
// 挪到渲染事件里，此刻 BLAS 还没建好。所以这里只做 CPU 拷贝排队，
// buffer 创建 + 拷贝由 RecordSkinRegistrationIfNeeded 录进 Unity 的命令缓冲。
struct PendingSkinReg
{
    uint32_t             blasID      = 0;
    uint32_t             vertexCount = 0;
    uint32_t             jointCount  = 0;
    std::vector<float>   weights;
    std::vector<int32_t> joints;
};
static std::vector<PendingSkinReg> s_PendingSkinRegs;

void VPTScene::RegisterSkinnedMesh(uint32_t blasID,
                                    const float* weights, const int32_t* joints,
                                    uint32_t vertexCount, uint32_t jointCount)
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (!weights || !joints || vertexCount == 0 || jointCount == 0)
        return;

    PendingSkinReg reg;
    reg.blasID      = blasID;
    reg.vertexCount = vertexCount;
    reg.jointCount  = jointCount;
    reg.weights.assign(weights, weights + (size_t)vertexCount * 4);
    reg.joints.assign(joints, joints + (size_t)vertexCount * 4);

    // 同 BLAS 重复注册（场景重建后 C# 会重新注册）→ 覆盖旧队列条目
    for (auto& r : s_PendingSkinRegs)
    {
        if (r.blasID == blasID) { r = std::move(reg); return; }
    }
    s_PendingSkinRegs.push_back(std::move(reg));

    fprintf(stderr, "[VPT] RegisterSkinnedMesh queued: BLAS[%u], %u verts, %u joints\n",
            blasID, vertexCount, jointCount);
}

void VPTScene::RecordSkinRegistrationIfNeeded(VkCommandBuffer cmd)
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (s_PendingSkinRegs.empty() || cmd == VK_NULL_HANDLE)
        return;
    if (!s_Ready)
        return;                       // 场景还没建好：下一帧再说
    if (VPT_IsDeviceLost())
    {
        s_PendingSkinRegs.clear();
        return;
    }

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    VkPhysicalDevice gpu = inst.physicalDevice;
    if (device == VK_NULL_HANDLE)
        return;

    for (auto it = s_PendingSkinRegs.begin(); it != s_PendingSkinRegs.end(); )
    {
        PendingSkinReg reg = std::move(*it);
        it = s_PendingSkinRegs.erase(it);

        // ── 校验（场景重建后 BLAS 数量/顶点数可能已变化）──
        if (reg.blasID >= s_BLASes.size())
        {
            fprintf(stderr, "[VPT] RegisterSkinnedMesh: blasID %u out of range (%zu BLASes), dropped.\n",
                    reg.blasID, s_BLASes.size());
            continue;
        }
        if (!s_BLASes[reg.blasID].allowUpdate)
        {
            fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] built without ALLOW_UPDATE. Skipped.\n", reg.blasID);
            continue;
        }
        if (reg.blasID >= s_MeshVertexCounts.size() || reg.vertexCount != s_MeshVertexCounts[reg.blasID])
        {
            fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] vertex count mismatch "
                            "(%u given, %u uploaded). Skipped.\n",
                    reg.blasID, reg.vertexCount,
                    reg.blasID < s_MeshVertexCounts.size() ? s_MeshVertexCounts[reg.blasID] : 0);
            continue;
        }

        if (!EnsureSkinPipeline(device))
            continue;

        // 防御：同 BLAS 重复注册 → 先释放并移除旧条目（正常流程在场景重建时已整体销毁）
        for (auto sit = s_SkinMeshes.begin(); sit != s_SkinMeshes.end(); ++sit)
        {
            if (sit->blasID == reg.blasID)
            {
                fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] already registered, re-registering.\n", reg.blasID);
                DestroySkinMeshResource(device, *sit);
                s_SkinMeshes.erase(sit);
                break;
            }
        }

        SkinMeshResource sm{};
        sm.blasID      = reg.blasID;
        sm.vertexCount = reg.vertexCount;
        sm.jointCount  = reg.jointCount;

        VkDeviceSize baseBytes = (VkDeviceSize)reg.vertexCount * sizeof(VPT_Vertex);
        VkDeviceSize attrBytes  = (VkDeviceSize)reg.vertexCount * 16; // vec4 / ivec4 每顶点
        VkDeviceSize matBytes   = (VkDeviceSize)kTLASRingSize * reg.jointCount * 64;

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

        if (!ok)
        {
            fprintf(stderr, "[VPT] RegisterSkinnedMesh: BLAS[%u] resource creation FAILED\n", reg.blasID);
            DestroySkinMeshResource(device, sm);
            continue;
        }

        // weights/joints staging：host-visible，常驻到下次场景重建（拷贝是本帧录制的）
        BufferResource wStaging{}, jStaging{};
        ok = CreateBufferResource(device, gpu, attrBytes,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 wStaging) == VK_SUCCESS
          && CreateBufferResource(device, gpu, attrBytes,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 jStaging) == VK_SUCCESS;
        if (!ok)
        {
            DestroyBufferResource(device, wStaging);
            DestroyBufferResource(device, jStaging);
            DestroySkinMeshResource(device, sm);
            continue;
        }

        void* wm = nullptr;
        void* jm = nullptr;
        vkMapMemory(device, wStaging.memory, 0, attrBytes, 0, &wm);
        memcpy(wm, reg.weights.data(), (size_t)attrBytes);
        vkUnmapMemory(device, wStaging.memory);
        vkMapMemory(device, jStaging.memory, 0, attrBytes, 0, &jm);
        memcpy(jm, reg.joints.data(), (size_t)attrBytes);
        vkUnmapMemory(device, jStaging.memory);

        // Barrier: 之前对统一顶点缓冲/新 buffer 的写 → transfer 读
        VkMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        toTransfer.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &toTransfer, 0, nullptr, 0, nullptr);

        // base 副本：直接自统一顶点缓冲复制对应区段（此时内容即 bind-pose）
        VkBufferCopy baseRegion{};
        baseRegion.srcOffset = (VkDeviceSize)s_MeshVertexOffsets[reg.blasID] * sizeof(VPT_Vertex);
        baseRegion.dstOffset = 0;
        baseRegion.size      = baseBytes;
        vkCmdCopyBuffer(cmd, s_UnifiedVertexBuffer.buffer, sm.baseVertices.buffer, 1, &baseRegion);

        VkBufferCopy attrRegion{};
        attrRegion.size = attrBytes;
        vkCmdCopyBuffer(cmd, wStaging.buffer, sm.weights.buffer, 1, &attrRegion);
        vkCmdCopyBuffer(cmd, jStaging.buffer, sm.joints.buffer,  1, &attrRegion);

        // Barrier: transfer 写 → 蒙皮 compute 读
        VkMemoryBarrier toCompute{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &toCompute, 0, nullptr, 0, nullptr);

        s_StagingBuffers.push_back(wStaging);
        s_StagingBuffers.push_back(jStaging);

        s_SkinMeshes.push_back(std::move(sm));

        fprintf(stderr, "[VPT] Skinned mesh registered: BLAS[%u], %u verts, %u joints\n",
                reg.blasID, reg.vertexCount, reg.jointCount);
    }
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

// ═══════════════════════════════════════════════════════════
//  场景更新（Plan B：主线程只拷贝，渲染线程录制）
// ═══════════════════════════════════════════════════════════

void VPTScene::PushScene(const VPT_SceneUpdate& su)
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (su.meshCount <= 0 || !su.meshes)
    {
        fprintf(stderr, "[VPT] PushScene: empty scene ignored.\n");
        return;
    }

    // 深拷贝（调用方的指针/托管内存返回后即可释放）。先释放上一轮待处理数据，
    // 避免反复拖 Inspector 时内存翻倍。
    s_PendingMeshes.clear();
    s_PendingMaterials.clear();
    s_PendingLights.clear();
    s_PendingSceneInstances.clear();

    s_PendingMeshes.resize((size_t)su.meshCount);
    for (int32_t i = 0; i < su.meshCount; i++)
    {
        const VPT_MeshData& src = su.meshes[i];
        PendingMeshData& dst = s_PendingMeshes[(size_t)i];
        dst.materialID = src.materialID;
        dst.flags      = src.flags;
        if (src.vertices && src.vertexCount)
        {
            dst.vertices.assign(src.vertices, src.vertices + src.vertexCount);
        }
        if (src.indices && src.indexCount)
        {
            dst.indices.assign(src.indices, src.indices + src.indexCount);
        }
    }

    if (su.materials && su.materialCount > 0)
        s_PendingMaterials.assign(su.materials, su.materials + su.materialCount);
    if (su.lights && su.lightCount > 0)
        s_PendingLights.assign(su.lights, su.lights + su.lightCount);
    if (su.instances && su.instanceCount > 0)
        s_PendingSceneInstances.assign(su.instances, su.instances + su.instanceCount);

    s_ScenePending = true;
    fprintf(stderr, "[VPT] PushScene queued: %d meshes, %d mats, %d lights, %d instances "
                    "(deferred to Unity command buffer)\n",
            su.meshCount, su.materialCount, su.lightCount, su.instanceCount);
    VPTTrace::Line("PushScene: queued %d meshes, %d mats, %d lights, %d instances",
                   su.meshCount, su.materialCount, su.lightCount, su.instanceCount);
}

bool VPTScene::IsSceneUpdatePending() { return s_ScenePending; }

// 拆场景资源（不做 wait；调用方负责先保证 GPU 空闲）
static void DestroySceneResourcesNoWait(VkDevice device)
{
    if (device == VK_NULL_HANDLE) return;

    for (auto& blas : s_BLASes) DestroyBLASResource(device, blas);
    s_BLASes.clear();
    DestroyBufferResource(device, s_UnifiedVertexBuffer);
    DestroyBufferResource(device, s_UnifiedIndexBuffer);
    DestroyBufferResource(device, s_MaterialBuffer);
    DestroyBufferResource(device, s_LightBuffer);
    DestroyTLASRingResources(device);
    DestroySkinResources(device);
    s_SkinNotRegisteredWarned = false;
    s_MeshVertexOffsets.clear();
    s_MeshIndexOffsets.clear();
    s_MeshVertexCounts.clear();
    s_MaterialFlagsSnapshot.clear();

    s_Ready = false;
}

bool VPTScene::RecordSceneUpdateIfNeeded(VkCommandBuffer cmd)
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (!s_ScenePending)
        return false;

    const UnityVulkanInstance& inst = VPT_GetInstance();
    VkDevice device = inst.device;
    VkPhysicalDevice gpu = inst.physicalDevice;

    if (device == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE)
        return false;   // 设备/命令缓冲还没就绪，下一帧再试

    if (VPT_IsDeviceLost())
    {
        // 设备已丢失：丢弃这次推送，绝不再向死设备要资源
        s_ScenePending = false;
        s_PendingMeshes.clear();
        s_PendingMaterials.clear();
        s_PendingLights.clear();
        s_PendingSceneInstances.clear();
        fprintf(stderr, "[VPT] PushScene dropped: device lost.\n");
        VPTTrace::Line("scene: dropped (device lost)");
        return false;
    }

    VPTTrace::Stage("scene:begin");
    VPTTrace::Line("scene: begin rebuild (%zu meshes, %zu mats, %zu lights, %zu instances)",
                   s_PendingMeshes.size(), s_PendingMaterials.size(),
                   s_PendingLights.size(), s_PendingSceneInstances.size());

    // ── 旧资源/上一轮 staging 仍可能被在飞行帧读取：先等 GPU 空闲。
    //    这一步现在跑在渲染线程 + 事件内（Unity 保证上一帧已提交），
    //    不会和 Unity 的 submit 抢同一条队列。 ──
    if (s_Device != VK_NULL_HANDLE)
    {
        VPTTrace::Stage("scene:vkDeviceWaitIdle");
        VPTTrace::Line("scene: vkDeviceWaitIdle(before destroy) begin ...");
        const VkResult wr = vkDeviceWaitIdle(device);
        VPTTrace::Line("scene: vkDeviceWaitIdle returned %d", (int)wr);
        if (!VPT_CheckVk(wr, "vkDeviceWaitIdle(scene rebuild)"))
        {
            s_ScenePending = false;
            return false;
        }
    }

    for (auto& sb : s_StagingBuffers)
        DestroyBufferResource(device, sb);
    s_StagingBuffers.clear();

    VPTTrace::Stage("scene:destroy-old");
    s_Device = device;
    DestroySceneResourcesNoWait(device);

    // Unity 命令缓冲：本次上传/构建全部录进去，由 Unity 在帧末提交。
    VkCommandBuffer cmdBuf = cmd;

    // ── 由待处理数据组装 VPT_MeshData（仅 BuildBLAS/CreateUnifiedBuffers 读描述信息）──
    std::vector<VPT_MeshData> meshes(s_PendingMeshes.size());
    for (size_t i = 0; i < s_PendingMeshes.size(); i++)
    {
        const PendingMeshData& m = s_PendingMeshes[i];
        meshes[i].vertices    = m.vertices.empty() ? nullptr : m.vertices.data();
        meshes[i].vertexCount = (uint32_t)m.vertices.size();
        meshes[i].indices     = m.indices.empty() ? nullptr : m.indices.data();
        meshes[i].indexCount  = (uint32_t)m.indices.size();
        meshes[i].materialID  = m.materialID;
        meshes[i].flags       = m.flags;
    }

    // ── device-local buffer 创建 + staging 上传（材质 / 光源 / 顶点 / 索引）──
    // 任何一步分配失败都必须放弃整个重建：把 null buffer / 0 地址录进命令缓冲
    // 会让 GPU 直接挂死（TDR），而 CPU 侧没有任何报错。
    VPTTrace::Stage("scene:buffers");
    VkDeviceSize matSize = (VkDeviceSize)s_PendingMaterials.size() * sizeof(VPT_Material);
    if (matSize > 0)
    {
        if (!CreateDeviceLocalBufferWithData(device, gpu, cmdBuf,
            s_PendingMaterials.data(), matSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            s_MaterialBuffer))
        {
            VPTTrace::Line("scene: material buffer upload failed -> abort rebuild");
            DestroySceneResourcesNoWait(device);
            s_ScenePending = false;
            s_PendingMeshes.clear(); s_PendingMaterials.clear();
            s_PendingLights.clear(); s_PendingSceneInstances.clear();
            return false;
        }

        // 材质 flags 快照：WriteInstanceData 要用它决定实例的 FORCE_OPAQUE。
        // 必须在这里做（s_PendingMaterials 在本次重建末尾就会被清空，而动态场景每帧
        // 更新实例时仍需要这份数据）。
        s_MaterialFlagsSnapshot.clear();
        s_MaterialFlagsSnapshot.reserve(s_PendingMaterials.size());
        for (const VPT_Material& m : s_PendingMaterials)
            s_MaterialFlagsSnapshot.push_back(m.flags);
    }

    VkDeviceSize lightSize = (VkDeviceSize)s_PendingLights.size() * sizeof(VPT_Light);
    if (lightSize > 0)
    {
        if (!CreateDeviceLocalBufferWithData(device, gpu, cmdBuf,
            s_PendingLights.data(), lightSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            s_LightBuffer))
        {
            VPTTrace::Line("scene: light buffer upload failed -> abort rebuild");
            DestroySceneResourcesNoWait(device);
            s_ScenePending = false;
            s_PendingMeshes.clear(); s_PendingMaterials.clear();
            s_PendingLights.clear(); s_PendingSceneInstances.clear();
            return false;
        }
    }

    // 统一顶点/索引缓冲（device-local + staging）
    if (!CreateUnifiedBuffers(device, gpu, cmdBuf, meshes.data(), (int32_t)meshes.size()))
    {
        VPTTrace::Line("scene: unified buffer creation failed -> abort rebuild");
        DestroySceneResourcesNoWait(device);
        s_ScenePending = false;
        s_PendingMeshes.clear(); s_PendingMaterials.clear();
        s_PendingLights.clear(); s_PendingSceneInstances.clear();
        return false;
    }

    // ── Barrier: staging copy (TRANSFER_WRITE) → BLAS build + shader read ──
    VkMemoryBarrier copyBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    copyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
                               | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
        | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &copyBarrier, 0, nullptr, 0, nullptr);

    s_BLASes.reserve(meshes.size());
    VPTTrace::Stage("scene:build-blas");
    VPTTrace::Line("scene: building %zu BLAS ...", meshes.size());

    int32_t skippedMeshes = 0;
    for (int32_t i = 0; i < (int32_t)meshes.size(); i++)
    {
        // 退化几何直接跳过：vertexCount==0 会让 maxVertex 变成 0xFFFFFFFF，
        // indexCount<3 时 primitiveCount=0，这两种 AS build 都可能把驱动拖死。
        if (meshes[i].vertexCount == 0 || meshes[i].indexCount < 3)
        {
            if (skippedMeshes < 8)
                VPTTrace::Line("scene: SKIP degenerate mesh[%d] verts=%u idx=%u",
                               i, meshes[i].vertexCount, meshes[i].indexCount);
            skippedMeshes++;
            s_MeshVertexCounts[i] = 0;
            continue;
        }
        BuildBLAS(cmdBuf, device, gpu, meshes[i], (uint32_t)i);
    }
    if (skippedMeshes > 0)
        VPTTrace::Line("scene: %d degenerate mesh(es) skipped", skippedMeshes);
    VPTTrace::Line("scene: BLAS built (%zu / %zu)", s_BLASes.size(), meshes.size());

    if (s_BLASes.empty())
    {
        VPTTrace::Line("scene: NO valid BLAS at all -> abort rebuild");
        DestroySceneResourcesNoWait(device);
        s_ScenePending = false;
        s_PendingMeshes.clear();
        s_PendingMaterials.clear();
        s_PendingLights.clear();
        s_PendingSceneInstances.clear();
        return false;
    }

    // CRITICAL: Memory barrier between BLAS builds and TLAS build.
    VkMemoryBarrier blasBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        0, 1, &blasBarrier, 0, nullptr, 0, nullptr);

    // ── TLAS 环形资源分配（静态拓扑变化时一次性）+ 首次构建（槽位 0）──
    VPTTrace::Stage("scene:build-tlas");
    const int32_t instanceCount = (int32_t)s_PendingSceneInstances.size();
    if (EnsureTLASResources(device, gpu, (uint32_t)(instanceCount > 0 ? instanceCount : 1)))
    {
        s_CurrentRing = 0;
        WriteInstanceData(s_PendingSceneInstances.data(), instanceCount, 0);
        RecordTLASBuild(cmdBuf, device, 0, (uint32_t)instanceCount);
    }
    else
    {
        fprintf(stderr, "[VPT] TLAS ring allocation FAILED\n");
    }

    s_Ready = true;
    fprintf(stderr, "[VPT] Scene recorded: %zu meshes, %zu mats, %zu lights, %d instances\n",
            s_PendingMeshes.size(), s_PendingMaterials.size(),
            s_PendingLights.size(), instanceCount);
    VPTTrace::Line("scene: recorded OK (%zu meshes, %d instances, ready=%d)",
                   s_PendingMeshes.size(), instanceCount, (int)s_Ready);
    VPTTrace::Stage("scene:done");

    // 构建完成后可以释放待处理 CPU 数据（顶点/索引已在 GPU 缓冲里）
    s_PendingMeshes.clear();
    s_PendingMaterials.clear();
    s_PendingLights.clear();
    s_PendingSceneInstances.clear();
    s_ScenePending = false;

    // 新场景已经按推送的实例建好 TLAS，之前排队的"扩容"请求作废
    s_ResizePending = false;
    s_ResizeInstances.clear();

    // 这一帧录进 Unity 命令缓冲的 BLAS/TLAS/上传量很大：下一帧做一次同步探针，
    // 万一 GPU 在重建帧上挂了，能立刻拿到 VK_ERROR_DEVICE_LOST 而不是静默继续。
    s_NeedGpuSyncProbe = true;

    // staging buffer 不在这里销毁：它们刚被录进本帧命令缓冲，等下一次重建
    // （或 VPT_Destroy，那里会先等 GPU 空闲）再统一释放。
    return true;
}

// ═══════════════════════════════════════════════════════════
//  资源销毁
// ═══════════════════════════════════════════════════════════

void VPTScene::DestroyResources()
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (s_Device == VK_NULL_HANDLE) return;

    VPTTrace::Stage("scene:destroy-all");
    VPTTrace::Line("DestroyResources: enter (deviceLost=%d)", (int)VPT_IsDeviceLost());

    // 设备已丢失时 vkDeviceWaitIdle 可能长时间不返回，直接跳过等待
    if (!VPT_IsDeviceLost())
    {
        VPTTrace::Line("DestroyResources: vkDeviceWaitIdle begin ...");
        VPT_CheckVk(vkDeviceWaitIdle(s_Device), "vkDeviceWaitIdle(VPTScene::DestroyResources)");
        VPTTrace::Line("DestroyResources: vkDeviceWaitIdle done");
    }

    DestroySceneResourcesNoWait(s_Device);

    for (auto& sb : s_StagingBuffers)
        DestroyBufferResource(s_Device, sb);
    s_StagingBuffers.clear();

    s_PendingMeshes.clear();
    s_PendingMaterials.clear();
    s_PendingLights.clear();
    s_PendingSceneInstances.clear();
    s_ScenePending = false;
    s_PendingSkinRegs.clear();
}

// ═══════════════════════════════════════════════════════════
//  动态场景：每帧实例更新（仅刚体 Transform，BLAS 不动）
// ═══════════════════════════════════════════════════════════

void VPTScene::UpdateInstances(const VPT_InstanceData* instances, int32_t instanceCount)
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (!s_ExtensionsReady || !instances || instanceCount <= 0)
        return;

    // 扩容（实例数超过预分配容量）：需要拆/建环形资源 → 必须在渲染线程上做，
    // 主线程只把数据拷下来排队（主线程 vkDeviceWaitIdle 会与渲染线程抢队列）。
    if ((uint32_t)instanceCount > s_InstanceCapacity)
    {
        VPTTrace::Line("UpdateInstances: capacity %u < %d, deferring resize",
                       s_InstanceCapacity, instanceCount);
        s_ResizeInstances.assign(instances, instances + instanceCount);
        s_ResizeCount   = (uint32_t)instanceCount;
        s_ResizePending = true;
        return;
    }

    if (s_BLASes.empty())
        return;

    // 轮转槽位：历史在飞行帧仍在读旧槽位，写入新槽位避免覆盖
    s_CurrentRing = (s_CurrentRing + 1) % kTLASRingSize;
    WriteInstanceData(instances, instanceCount, s_CurrentRing);

    s_PendingCount = (uint32_t)instanceCount;
    s_TLASPending  = true;
}

bool VPTScene::RecordTLASUpdateIfNeeded(VkCommandBuffer cmd)
{
    std::lock_guard<std::recursive_mutex> guard(s_PendingMutex);

    if (cmd == VK_NULL_HANDLE)
        return false;

    // 低频：实例数超容量 → 此时（渲染线程、事件内）才等待 GPU idle 并重建环形资源
    if (s_ResizePending)
    {
        if (VPT_IsDeviceLost())
        {
            s_ResizePending = false;
            s_ResizeInstances.clear();
            return false;
        }
        s_ResizePending = false;

        VPTTrace::Stage("tlas:resize-wait-idle");
        VPTTrace::Line("tlas: resize to %u instances, vkDeviceWaitIdle begin ...", s_ResizeCount);
        const UnityVulkanInstance& inst = VPT_GetInstance();
        if (!VPT_CheckVk(vkDeviceWaitIdle(inst.device), "vkDeviceWaitIdle(TLAS resize)"))
        {
            s_ResizeInstances.clear();
            return true;   // 资源可能已被拆，本帧不得 dispatch
        }
        if (!EnsureTLASResources(inst.device, inst.physicalDevice, s_ResizeCount))
        {
            s_ResizeInstances.clear();
            return true;
        }

        s_CurrentRing = 0;
        WriteInstanceData(s_ResizeInstances.data(), (int32_t)s_ResizeCount, 0);
        RecordTLASBuild(cmd, inst.device, 0, s_ResizeCount);
        s_ResizeInstances.clear();

        s_PendingCount = s_ResizeCount;
        s_TLASPending  = false;
        return true;       // 环形 buffer 句柄已变，descriptor 失效
    }

    if (!s_TLASPending)
        return false;

    RecordTLASBuild(cmd, s_Device, s_CurrentRing, s_PendingCount);
    s_TLASPending = false;
    return false;
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
