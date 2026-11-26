// ═══════════════════════════════════════════════════════════════════════
//  vk_terrain_probe — 验证 ray query (VK_KHR_ray_query) 能否用
//  AABB(procedural) 几何 + 自定义高度场求交替代三角形地形网格
//
//  验证点：
//   1. BLAS 用 VK_GEOMETRY_TYPE_AABBS_KHR 能否构建
//   2. TLAS 混挂 AABB BLAS 与三角形 BLAS
//   3. 着色器 candidate 循环里能否拿到 gl_RayQueryCandidateIntersectionAABBEXT
//   4. rayQueryGenerateIntersectionEXT 能否提交自定义 t 并成为 committed hit
//   5. 力不透明(FORCE_OPAQUE)实例下的行为
//   6. TerminateOnFirstHit 阴影式遍历
// ═══════════════════════════════════════════════════════════════════════

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#define VK_CHECK(expr)                                                        \
    do {                                                                      \
        VkResult _r = (expr);                                                 \
        if (_r != VK_SUCCESS) {                                               \
            fprintf(stderr, "[FAIL] %s -> %d (%s:%d)\n", #expr, (int)_r,      \
                    __FILE__, __LINE__);                                      \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

static PFN_vkGetAccelerationStructureBuildSizesKHR pGetBuildSizes;
static PFN_vkCreateAccelerationStructureKHR        pCreateAS;
static PFN_vkGetAccelerationStructureDeviceAddressKHR pGetASAddress;
static PFN_vkCmdBuildAccelerationStructuresKHR     pCmdBuildAS;

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory mem    = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr;
    VkDeviceAddress addr  = 0;
};

static uint32_t FindMemType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    fprintf(stderr, "[FAIL] no memory type\n");
    std::exit(1);
}

static Buffer CreateBuffer(VkDevice dev, VkPhysicalDevice gpu, VkDeviceSize size,
                           VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size  = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(dev, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = FindMemType(gpu, req.memoryTypeBits, props);
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    ai.pNext = &flagsInfo;
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(dev, b.buffer, b.mem, 0));
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK_CHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = b.buffer;
        b.addr = vkGetBufferDeviceAddress(dev, &info);
    }
    return b;
}

// ── 高度场配置（与 probe.comp 一致）──
static const int   GRID    = 64;    // cell 数
static const int   PATCH   = 16;    // 每 patch cell 数
static const int   PATCHES = 4;     // 4x4 = 16 patch
static const float BASE_H  = 10.0f;

static float TerrainHeight(int i, int j) {
    float h = BASE_H;
    // 半径 8m 的圆锥凸起，中心 (48,48)
    float dx = (float)i - 48.0f, dz = (float)j - 48.0f;
    float r  = std::sqrt(dx * dx + dz * dz);
    h += 6.0f * std::max(0.0f, 1.0f - r / 8.0f);
    return h;
}

static float Bilinear(const std::vector<float>& h, float x, float z) {
    float gx = std::min(std::max(x, 0.0f), (float)GRID);
    float gz = std::min(std::max(z, 0.0f), (float)GRID);
    int   i  = std::min((int)std::floor(gx), GRID - 1);
    int   j  = std::min((int)std::floor(gz), GRID - 1);
    float fx = gx - i, fz = gz - j;
    auto  H  = [&](int a, int b) { return h[b * (GRID + 1) + a]; };
    float h0 = H(i, j) * (1 - fx) + H(i + 1, j) * fx;
    float h1 = H(i, j + 1) * (1 - fx) + H(i + 1, j + 1) * fx;
    return h0 * (1 - fz) + h1 * fz;
}

int main() {
    // ── instance ──
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "vpt-terrain-probe";
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(inst, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(inst, &gpuCount, gpus.data());

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(g, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(g, nullptr, &n, exts.data());
        bool as = false, rq = false;
        for (auto& e : exts) {
            if (!strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) as = true;
            if (!strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME)) rq = true;
        }
        printf("[gpu] %s (vendor=0x%04x) AS=%d RQ=%d\n", p.deviceName, p.vendorID, as, rq);
        if (gpu == VK_NULL_HANDLE && as && rq) gpu = g;
    }
    if (gpu == VK_NULL_HANDLE) { fprintf(stderr, "[FAIL] no RT capable gpu\n"); return 1; }
    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(gpu, &gpuProps);
    printf("[gpu] using %s driver %u\n", gpuProps.deviceName, gpuProps.driverVersion);

    // 扩展可用性（SkipAABB 需要 VK_EXT_ray_flags_primitive_culling）
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, exts.data());
        auto has = [&](const char* name) {
            for (auto& e : exts) if (!strcmp(e.extensionName, name)) return true;
            return false;
        };
        printf("[ext] ray_flags_primitive_culling = %s\n",
               has("VK_EXT_ray_flags_primitive_culling") ? "YES" : "NO");
    }

    // ── queue family ──
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qCount, qf.data());
    uint32_t qIndex = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qIndex = i; break; }
    if (qIndex == UINT32_MAX) { fprintf(stderr, "[FAIL] no compute queue\n"); return 1; }

    // ── features (先行查询) ──
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vk12.pNext = &asFeat;
    asFeat.pNext = &rqFeat;
    feat2.pNext = &vk12;
    vkGetPhysicalDeviceFeatures2(gpu, &feat2);
    printf("[feat] accelerationStructure=%d rayQuery=%d bufferDeviceAddress=%d\n",
           asFeat.accelerationStructure, rqFeat.rayQuery, vk12.bufferDeviceAddress);
    if (!asFeat.accelerationStructure || !rqFeat.rayQuery || !vk12.bufferDeviceAddress) {
        fprintf(stderr, "[FAIL] required features missing\n");
        return 1;
    }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asEnable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    asEnable.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rqEnable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rqEnable.rayQuery = VK_TRUE;
    VkPhysicalDeviceVulkan12Features vk12Enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12Enable.bufferDeviceAddress = VK_TRUE;
    vk12Enable.pNext = &asEnable;
    asEnable.pNext = &rqEnable;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3;
    dci.ppEnabledExtensionNames = devExts;
    dci.pNext = &vk12Enable;
    VkDevice dev;
    VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qIndex, 0, &queue);

    pGetBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureBuildSizesKHR");
    pCreateAS      = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(dev, "vkCreateAccelerationStructureKHR");
    pGetASAddress  = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureDeviceAddressKHR");
    pCmdBuildAS    = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(dev, "vkCmdBuildAccelerationStructuresKHR");
    if (!pGetBuildSizes || !pCreateAS || !pGetASAddress || !pCmdBuildAS) {
        fprintf(stderr, "[FAIL] RT function pointers missing\n");
        return 1;
    }

    // ── BLAS 尺寸测量：三角网格地形 vs AABB patch ──
    // 只调用 vkGetAccelerationStructureBuildSizesKHR（不解引用数据），
    // 因此可以用假地址；测的是本驱动/本 GPU 上的真实 AS 内存与 scratch 需求。
    auto measureTris = [&](int res, double& asMB, double& scratchMB) {
        uint32_t cells = (uint32_t)(res - 1);
        uint32_t tris  = cells * cells * 2;
        uint32_t verts = (uint32_t)(res * res);
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.flags = 0;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = 0;
        g.geometry.triangles.vertexStride = 12;
        g.geometry.triangles.maxVertex = verts - 1;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = 0;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR s{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        pGetBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &tris, &s);
        asMB = s.accelerationStructureSize / 1048576.0;
        scratchMB = s.buildScratchSize / 1048576.0;
    };

    auto measurePatches = [&](int res, int patchCells, double& asKB, uint32_t& prims) {
        int cells = res - 1;
        int n = (cells + patchCells - 1) / patchCells;
        prims = (uint32_t)(n * n);
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.flags = 0;
        g.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        g.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        g.geometry.aabbs.data.deviceAddress = 0;
        g.geometry.aabbs.stride = 24;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR s{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        pGetBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &s);
        asKB = s.accelerationStructureSize / 1024.0;
    };

    printf("\n[AS size] triangle-mesh terrain (FAST_TRACE, full res, no LOD)\n");
    printf("  res\tcells\ttris\tverts\tBLAS MB\tscratch MB\tverts+idx MB(80B/12B)\n");
    for (int res : {129, 257, 513, 1025, 2049, 4097}) {
        double asMB = 0, scMB = 0;
        measureTris(res, asMB, scMB);
        long long v = (long long)res * res;
        double geoMB = v * (80.0 + 12.0) / 1048576.0;   // VPT_Vertex=80B + index 12B/vert(avg 2 tris/vert*3*4B/2)
        printf("  %d\t%d\t%lld\t%lld\t%.1f\t\t%.1f\t\t%.1f\n",
               res, (res - 1) * (res - 1), (long long)(res - 1) * (res - 1) * 2, v, asMB, scMB, geoMB);
    }

    printf("\n[AS size] AABB patches (N x N cells per patch)\n");
    printf("  res\tpatchCells\tprims\tBLAS KB\n");
    for (int res : {513, 1025, 2049}) {
        for (int pc : {8, 16, 32}) {
            double asKB = 0; uint32_t prims = 0;
            measurePatches(res, pc, asKB, prims);
            printf("  %d\t%d\t\t%u\t%.1f\n", res, pc, prims, asKB);
        }
    }

    vkDestroyDevice(dev, nullptr);
    return 0;
}

