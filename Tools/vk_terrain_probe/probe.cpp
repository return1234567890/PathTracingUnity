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

    // ── 高度场数据 ──
    std::vector<float> heights((GRID + 1) * (GRID + 1));
    for (int j = 0; j <= GRID; j++)
        for (int i = 0; i <= GRID; i++)
            heights[j * (GRID + 1) + i] = TerrainHeight(i, j);

    // patch AABB
    std::vector<float> patchMin(PATCHES * PATCHES * 4), patchMax(PATCHES * PATCHES * 4);
    for (int pz = 0; pz < PATCHES; pz++)
        for (int px = 0; px < PATCHES; px++) {
            int idx = pz * PATCHES + px;
            float hmin = 1e30f, hmax = -1e30f;
            for (int j = pz * PATCH; j <= (pz + 1) * PATCH; j++)
                for (int i = px * PATCH; i <= (px + 1) * PATCH; i++) {
                    float h = heights[j * (GRID + 1) + i];
                    hmin = std::min(hmin, h);
                    hmax = std::max(hmax, h);
                }
            patchMin[idx * 4 + 0] = (float)(px * PATCH);
            patchMin[idx * 4 + 1] = hmin - 0.01f;
            patchMin[idx * 4 + 2] = (float)(pz * PATCH);
            patchMin[idx * 4 + 3] = 0.0f;
            patchMax[idx * 4 + 0] = (float)((px + 1) * PATCH);
            patchMax[idx * 4 + 1] = hmax + 0.01f;
            patchMax[idx * 4 + 2] = (float)((pz + 1) * PATCH);
            patchMax[idx * 4 + 3] = 0.0f;
        }

    printf("[data] heightmap %dx%d, %d patches (AABB primitives)\n", GRID + 1, GRID + 1, PATCHES * PATCHES);

    // ── 上传缓冲 ──
    Buffer heightBuf = CreateBuffer(dev, gpu, heights.size() * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(heightBuf.mapped, heights.data(), heights.size() * sizeof(float));

    Buffer patchBuf = CreateBuffer(dev, gpu, (patchMin.size() + patchMax.size()) * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(patchBuf.mapped, patchMin.data(), patchMin.size() * sizeof(float));
    memcpy((char*)patchBuf.mapped + patchMin.size() * sizeof(float), patchMax.data(), patchMax.size() * sizeof(float));

    // AABB 数据：tightly packed {minX,minY,minZ,maxX,maxY,maxZ}
    std::vector<float> aabbs(PATCHES * PATCHES * 6);
    for (int p = 0; p < PATCHES * PATCHES; p++) {
        aabbs[p * 6 + 0] = patchMin[p * 4 + 0]; aabbs[p * 6 + 1] = patchMin[p * 4 + 1]; aabbs[p * 6 + 2] = patchMin[p * 4 + 2];
        aabbs[p * 6 + 3] = patchMax[p * 4 + 0]; aabbs[p * 6 + 4] = patchMax[p * 4 + 1]; aabbs[p * 6 + 5] = patchMax[p * 4 + 2];
    }
    Buffer aabbBuf = CreateBuffer(dev, gpu, aabbs.size() * sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(aabbBuf.mapped, aabbs.data(), aabbs.size() * sizeof(float));

    // 三角形 quad（x∈[100,104], y=10, z∈[0,4]）
    float quadVerts[4][3] = {{100, 10, 0}, {104, 10, 0}, {104, 10, 4}, {100, 10, 4}};
    uint16_t quadIdx[6] = {0, 1, 2, 0, 2, 3};
    Buffer vertBuf = CreateBuffer(dev, gpu, sizeof(quadVerts),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(vertBuf.mapped, quadVerts, sizeof(quadVerts));
    Buffer idxBuf = CreateBuffer(dev, gpu, sizeof(quadIdx),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(idxBuf.mapped, quadIdx, sizeof(quadIdx));

    // ── BLAS: terrain (AABBS) ──
    VkAccelerationStructureKHR blasTerrain = VK_NULL_HANDLE, blasQuad = VK_NULL_HANDLE;
    Buffer blasTerrainStorage{}, blasTerrainScratch{}, blasQuadStorage{}, blasQuadScratch{};
    VkAccelerationStructureGeometryKHR terrainGeom{};
    VkAccelerationStructureGeometryKHR quadGeom{};
    {
        // 需要把 geom 存起来供录制使用 —— 直接内联重建
        uint32_t prim = PATCHES * PATCHES;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.flags = 0;
        g.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        g.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        g.geometry.aabbs.data.deviceAddress = aabbBuf.addr;
        g.geometry.aabbs.stride = 6 * sizeof(float);
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        pGetBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prim, &sizes);
        blasTerrainStorage = CreateBuffer(dev, gpu, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        blasTerrainScratch = CreateBuffer(dev, gpu, sizes.buildScratchSize + 256,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        aci.buffer = blasTerrainStorage.buffer;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VK_CHECK(pCreateAS(dev, &aci, nullptr, &blasTerrain));
        terrainGeom = g;
        printf("[blas] AABB prims=%u asSize=%.3f MB scratch=%.3f MB\n", prim,
               sizes.accelerationStructureSize / 1048576.0, sizes.buildScratchSize / 1048576.0);
    }
    {
        uint32_t prim = 2;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.flags = 0;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = vertBuf.addr;
        g.geometry.triangles.vertexStride = 3 * sizeof(float);
        g.geometry.triangles.maxVertex = 3;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT16;
        g.geometry.triangles.indexData.deviceAddress = idxBuf.addr;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        pGetBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prim, &sizes);
        blasQuadStorage = CreateBuffer(dev, gpu, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        blasQuadScratch = CreateBuffer(dev, gpu, sizes.buildScratchSize + 256,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        aci.buffer = blasQuadStorage.buffer;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VK_CHECK(pCreateAS(dev, &aci, nullptr, &blasQuad));
        quadGeom = g;
        printf("[blas] TRI prims=%u asSize=%.3f MB\n", prim, sizes.accelerationStructureSize / 1048576.0);
    }

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addrInfo.accelerationStructure = blasTerrain;
    VkDeviceAddress blasTerrainAddr = pGetASAddress(dev, &addrInfo);
    addrInfo.accelerationStructure = blasQuad;
    VkDeviceAddress blasQuadAddr = pGetASAddress(dev, &addrInfo);

    // ── TLAS：3 个实例（terrain / terrain FORCE_OPAQUE 偏移 +200 / quad）──
    std::vector<VkAccelerationStructureInstanceKHR> tinst(3);
    auto fillInst = [](VkAccelerationStructureInstanceKHR& d, VkDeviceAddress blas, float tx,
                       uint32_t custom, uint32_t flags) {
        memset(&d, 0, sizeof(d));
        d.transform.matrix[0][0] = 1; d.transform.matrix[1][1] = 1; d.transform.matrix[2][2] = 1;
        d.transform.matrix[0][3] = tx;
        d.instanceCustomIndex = custom;
        d.mask = 0xFF;
        d.instanceShaderBindingTableRecordOffset = 0;
        d.flags = flags;
        d.accelerationStructureReference = blas;
    };
    fillInst(tinst[0], blasTerrainAddr, 0.0f, 0, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
    fillInst(tinst[1], blasTerrainAddr, 200.0f, 1,
             VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    fillInst(tinst[2], blasQuadAddr, 0.0f, 2,
             VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);

    Buffer instBuf = CreateBuffer(dev, gpu, tinst.size() * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(instBuf.mapped, tinst.data(), tinst.size() * sizeof(VkAccelerationStructureInstanceKHR));

    // TLAS
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    Buffer tlasStorage{}, tlasScratch{};
    {
        uint32_t prim = (uint32_t)tinst.size();
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        g.geometry.instances.arrayOfPointers = VK_FALSE;
        g.geometry.instances.data.deviceAddress = instBuf.addr;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        pGetBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prim, &sizes);
        tlasStorage = CreateBuffer(dev, gpu, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        tlasScratch = CreateBuffer(dev, gpu, sizes.buildScratchSize + 256,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        aci.buffer = tlasStorage.buffer;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(pCreateAS(dev, &aci, nullptr, &tlas));
        printf("[tlas] instances=%u asSize=%.3f MB\n", prim, sizes.accelerationStructureSize / 1048576.0);
    }

    // ── 实例平移（世界→局部，仅验证用）──
    float instOffsets[4][4] = {{0,0,0,0}, {200,0,0,0}, {0,0,0,0}, {0,0,0,0}};
    Buffer instOffsetBuf = CreateBuffer(dev, gpu, sizeof(instOffsets),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(instOffsetBuf.mapped, instOffsets, sizeof(instOffsets));

    // ── 输出 buffer（8 条 ray × 12 uint）──
    const uint32_t kRays = 8;
    Buffer outBuf = CreateBuffer(dev, gpu, kRays * 16 * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memset(outBuf.mapped, 0, kRays * 16 * sizeof(uint32_t));

    // ── pipeline ──
    FILE* f = fopen("probe.spv", "rb");
    if (!f) { fprintf(stderr, "[FAIL] open probe.spv\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long spvSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> spv(spvSize);
    fread(spv.data(), 1, spvSize, f);
    fclose(f);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spv.size();
    smci.pCode = (const uint32_t*)spv.data();
    VkShaderModule shader;
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &shader));

    VkDescriptorSetLayoutBinding binds[5] = {};
    binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (int i = 1; i < 5; i++) {
        binds[i].binding = i; binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1; binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 5;
    dslci.pBindings = binds;
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPipelineLayout pl;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName = "main";
    cpci.layout = pl;
    VkPipeline pipeline;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = ps;
    VkDescriptorPool pool;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo biOut{outBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo biHeight{heightBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo biPatch{patchBuf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo biInst{instOffsetBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[5] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = dset; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;
    for (int i = 1; i < 5; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dset; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = (i == 1) ? &biOut : (i == 2) ? &biHeight : (i == 3) ? &biPatch : &biInst;
    }
    vkUpdateDescriptorSets(dev, 5, writes, 0, nullptr);

    // ── 命令录制 ──
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = qIndex;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool;
    VK_CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &cmdPool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));

    VkMemoryBarrier hostBar{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostBar.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostBar.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &hostBar, 0, nullptr, 0, nullptr);

    auto recordBLAS = [&](VkAccelerationStructureKHR as, Buffer& scratch,
                          VkAccelerationStructureGeometryKHR& geom, uint32_t prim) {
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geom;
        bi.dstAccelerationStructure = as;
        bi.scratchData.deviceAddress = scratch.addr;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = prim;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        pCmdBuildAS(cmd, 1, &bi, &pRange);
    };
    recordBLAS(blasTerrain, blasTerrainScratch, terrainGeom, PATCHES * PATCHES);
    recordBLAS(blasQuad, blasQuadScratch, quadGeom, 2);

    VkMemoryBarrier asBar{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    asBar.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    asBar.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &asBar, 0, nullptr, 0, nullptr);

    // TLAS build
    {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        g.geometry.instances.data.deviceAddress = instBuf.addr;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        bi.dstAccelerationStructure = tlas;
        bi.scratchData.deviceAddress = tlasScratch.addr;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = (uint32_t)tinst.size();
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        pCmdBuildAS(cmd, 1, &bi, &pRange);
    }

    VkMemoryBarrier asBar2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    asBar2.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    asBar2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &asBar2, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);

    VkMemoryBarrier outBar{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    outBar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outBar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &outBar, 0, nullptr, 0, nullptr);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    // ── 读回结果 ──
    const uint32_t* d = (const uint32_t*)outBuf.mapped;
    auto f32 = [&](uint32_t u) { float v; memcpy(&v, &u, 4); return v; };
    const char* cname[] = {"NONE", "TRIANGLE", "GENERATED", "AABB_CAND"};
    int fails = 0;
    auto expect = [&](int ray, const char* what, bool ok, const char* detail) {
        printf("  ray%d %-28s %s  (%s)\n", ray, what, ok ? "PASS" : "FAIL", detail);
        if (!ok) fails++;
    };

    printf("\n[result] 8 rays (cand, aabbCand, triCand, committedType, instanceId, primIndex, t, normalY)\n");
    for (int r = 0; r < (int)kRays; r++) {
        const uint32_t* o = d + r * 16;
        printf("  ray%d: cand=%u aabb=%u tri=%u type=%s inst=%u prim=%u t=%.4f nY=%.4f gen=%u genT=%.4f candPrim=%u candT=%.4f\n",
               r, o[0], o[1], o[2], cname[o[3] < 4 ? o[3] : 0], o[4], o[5], f32(o[6]), f32(o[7]), o[8], f32(o[9]), o[10], f32(o[11]));
        printf("          dbg: tEnter=%.4f tExit=%.4f tPrev=%.4f reason=%.0f\n", f32(o[11]), f32(o[12]), f32(o[13]), f32(o[14]));
    }

    printf("\n[checks]\n");
    {
        const uint32_t* o = d + 0 * 16;
        expect(0, "vertical hit flat terrain", o[3] == 2 && std::fabs(f32(o[6]) - 40.0f) < 0.05f, "expect t~40, type=GENERATED");
        expect(0, "AABB candidate delivered", o[1] >= 1, "expect >=1 AABB candidate");
        expect(0, "normal.y ~ 1 on flat", std::fabs(f32(o[7]) - 1.0f) < 0.02f, "expect nY~1");
    }
    {
        const uint32_t* o = d + 1 * 16;
        expect(1, "oblique hit terrain", o[3] == 2, "expect type=GENERATED");
    }
    {
        const uint32_t* o = d + 2 * 16;
        expect(2, "upward ray miss", o[3] == 0, "expect type=NONE");
    }
    {
        const uint32_t* o = d + 3 * 16;
        expect(3, "triangle quad hit", o[3] == 1 && o[4] == 2 && std::fabs(f32(o[6]) - 40.0f) < 0.01f,
               "expect type=TRIANGLE inst=2 t=40");
    }
    {
        const uint32_t* o = d + 4 * 16;
        expect(4, "terminate-on-first-hit", o[3] == 1, "expect type=TRIANGLE");
    }
    {
        const uint32_t* o = d + 5 * 16;
        expect(5, "bump hit with slope normal", o[3] == 2 && f32(o[7]) < 0.99f, "expect type=GENERATED, nY<0.99");
    }
    {
        const uint32_t* o = d + 6 * 16;
        expect(6, "FORCE_OPAQUE AABB instance", o[3] == 2 && o[4] == 1, "expect type=GENERATED inst=1");
    }
    {
        const uint32_t* o = d + 7 * 16;
        expect(7, "ray to empty space", o[3] == 0, "expect type=NONE");
    }

    printf("\n%s\n", fails == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return fails == 0 ? 0 : 2;
}












