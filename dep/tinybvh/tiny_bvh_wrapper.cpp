// tiny_bvh_wrapper.cpp
// tinybvh 的 C 语言导出层，供 Unity P/Invoke 调用

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"

#include <cstring> // memcpy
#include <vector>

// 跨平台导出宏
#if defined(_WIN32)
    #define TBVH_EXPORT extern "C" __declspec(dllexport)
#else
    #define TBVH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// 不透明句柄，C# 侧用 IntPtr 持有
struct BVHHandle {
    tinybvh::BVH bvh;
};

// 射线命中结果（平铺结构，跨语言安全传递）
// 注意：instIdx 字段用于 TLAS 两层级求交时返回命中的实例索引。
// 旧的单 BLAS API 始终将 instIdx 填 0，保持向后兼容。
struct HitResult {
    int   hit;      // 0 = 未命中, 1 = 命中
    int   instIdx;  // 命中的实例索引（TLAS 模式），单 BLAS 模式恒为 0
    int   primIdx;  // 命中的三角形索引（BLAS 内）
    float t;        // 射线参数 t（距离）
    float u, v;     // 重心坐标
};

// ── 路径追踪数据导出结构体 ─────────────────────────────

// BLAS 描述表元素（按 blasID 索引）
struct BLASDescriptor {
    uint32_t blasNodeBase;   // 该 BLAS 节点在全局节点数组中的基地址
    uint32_t primIdxBase;    // 该 BLAS primIdx 在全局 primIdx 数组中的基地址
    uint32_t triBase;        // 该 BLAS 三角形在全局三角形数组中的基地址
    uint32_t vertBase;       // 该 BLAS 顶点在全局顶点数组中的基地址
};

// 实例表元素（按实例 ID 索引）
struct InstanceRecord {
    uint32_t materialID;
    uint32_t blasID;
    float    transform[16];     // 行主序 4x4（从 BLASInstance.transform 拷贝）
    float    invTransform[16];  // 行主序 4x4（从 BLASInstance.invTransform 拷贝）
};

// ── Scene 上下文 ───────────────────────────────────────
// TLAS Build 需要 BVHBase**（指针数组的指针），C# 侧无法直接管理，
// 因此在 native 侧维护此结构体，统一管理 BLAS 池、Instance 数组、指针数组。
struct SceneHandle {
    std::vector<tinybvh::BVH*>         blasPool;   // 每个 BLAS 一个 BVH*
    std::vector<tinybvh::BVHBase*>     blasPtrs;   // 指针数组，传给 TLAS Build
    std::vector<tinybvh::BLASInstance> instances;  // 实例数组
    tinybvh::BVH                       tlas;       // 顶层加速结构

    // ── 路径追踪数据支持 ──
    std::vector<uint32_t>              blasVertBase;     // 每个 BLAS 的 vertBase（调用方提供）
    std::vector<uint32_t>              instanceMaterials; // 每个实例的 materialID

    // 缓存的全局数组（assemble_globals 时构建）
    std::vector<tinybvh::BVH::BVHNode> globalBlasNodes;   // 拼接+重定位后的全局 BLAS 节点
    std::vector<uint32_t>              globalBlasPrimIdx; // 拼接后的全局 BLAS primIdx
    std::vector<BLASDescriptor>        blasDescriptors;  // BLAS 描述表
    bool                               globalsValid = false;
};

// ── 生命周期 ──────────────────────────────────────────

TBVH_EXPORT BVHHandle* tinybvh_create() {
    return new BVHHandle();
}

TBVH_EXPORT void tinybvh_destroy(BVHHandle* handle) {
    delete handle;
}

// ── 构建 BVH ──────────────────────────────────────────
// vertices: float 数组，每顶点 4 float (x,y,z,pad)，共 numTriangles*3 个顶点
TBVH_EXPORT void tinybvh_build(BVHHandle* handle, const float* vertices, int numTriangles) {
    handle->bvh.Build(reinterpret_cast<const tinybvh::bvhvec4*>(vertices), numTriangles);
}

// ── 单条射线求交 ────────────────────────────────────────
TBVH_EXPORT HitResult tinybvh_intersect(
    BVHHandle* handle,
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float tMax)
{
    tinybvh::Ray ray(
        tinybvh::bvhvec3(ox, oy, oz),
        tinybvh::bvhvec3(dx, dy, dz),
        tMax
    );
    handle->bvh.Intersect(ray);

    HitResult result;
    result.hit     = (ray.hit.t < tMax) ? 1 : 0;
    result.instIdx = 0;  // 单 BLAS 模式无实例索引
    result.primIdx = ray.hit.prim;
    result.t       = ray.hit.t;
    result.u       = ray.hit.u;
    result.v       = ray.hit.v;
    return result;
}

// ── 批量射线求交（减少 P/Invoke 跨边界开销）────────────────
TBVH_EXPORT void tinybvh_intersect_batch(
    BVHHandle*  handle,
    const float* origins,    // float[n*3]
    const float* directions, // float[n*3]
    HitResult*  results,     // HitResult[n]
    int         count,
    float       tMax)
{
    for (int i = 0; i < count; i++) {
        int oi = i * 3, di = i * 3;
        tinybvh::Ray ray(
            tinybvh::bvhvec3(origins[oi], origins[oi+1], origins[oi+2]),
            tinybvh::bvhvec3(directions[di], directions[di+1], directions[di+2]),
            tMax
        );
        handle->bvh.Intersect(ray);
        results[i].hit     = (ray.hit.t < tMax) ? 1 : 0;
        results[i].instIdx = 0;  // 单 BLAS 模式无实例索引
        results[i].primIdx = ray.hit.prim;
        results[i].t       = ray.hit.t;
        results[i].u       = ray.hit.u;
        results[i].v       = ray.hit.v;
    }
}

// ════════════════════════════════════════════════════════════════
//  Scene / TLAS / BLAS 接口
//  用于构建两层级加速结构：TLAS（顶层）遍历多个 BLAS（底层）。
//  典型流程：create → add_blas(每个 mesh) → add_instance(每个实例) →
//            build_tlas → intersect / intersect_batch
// ════════════════════════════════════════════════════════════════

// ── Scene 生命周期 ──────────────────────────────────────

TBVH_EXPORT SceneHandle* tinybvh_scene_create() {
    return new SceneHandle();
}

TBVH_EXPORT void tinybvh_scene_destroy(SceneHandle* scene) {
    if (!scene) return;
    // 释放所有 BLAS
    for (tinybvh::BVH* blas : scene->blasPool) {
        delete blas;
    }
    delete scene;
}

// ── BLAS 管理 ──────────────────────────────────────────
// vertices: float 数组，每顶点 4 float (x,y,z,pad)，共 numTriangles*3 个顶点
// vertBase: 该 BLAS 顶点在全局顶点数组中的基地址（供路径追踪描述表使用）
TBVH_EXPORT int tinybvh_scene_add_blas(
    SceneHandle* scene,
    const float* vertices,
    int numTriangles,
    int vertBase)
{
    tinybvh::BVH* blas = new tinybvh::BVH();
    blas->Build(reinterpret_cast<const tinybvh::bvhvec4*>(vertices), numTriangles);
    scene->blasPool.push_back(blas);
    scene->blasVertBase.push_back((uint32_t)vertBase);
    scene->globalsValid = false;
    return (int)(scene->blasPool.size() - 1);
}

// Indexed BLAS 构建：使用顶点+索引数组，支持共享顶点的 mesh
// vertices: bvhvec4 数组（每顶点 4 float: x,y,z,pad），按顶点 ID 索引
// indices: 每三角形 3 个顶点索引，共 numTriangles*3 个 uint32_t
// vertBase: 该 BLAS 顶点在全局顶点数组中的基地址
TBVH_EXPORT int tinybvh_scene_add_blas_indexed(
    SceneHandle* scene,
    const float* vertices,
    const uint32_t* indices,
    int numTriangles,
    int vertBase)
{
    tinybvh::BVH* blas = new tinybvh::BVH();
    blas->Build(
        reinterpret_cast<const tinybvh::bvhvec4*>(vertices),
        indices,
        (uint32_t)numTriangles);
    scene->blasPool.push_back(blas);
    scene->blasVertBase.push_back((uint32_t)vertBase);
    scene->globalsValid = false;
    return (int)(scene->blasPool.size() - 1);
}

TBVH_EXPORT void tinybvh_scene_update_blas(
    SceneHandle* scene,
    int blasIdx,
    const float* vertices,
    int numTriangles)
{
    if (blasIdx < 0 || blasIdx >= (int)scene->blasPool.size()) return;
    scene->blasPool[blasIdx]->Build(
        reinterpret_cast<const tinybvh::bvhvec4*>(vertices), numTriangles);
    scene->globalsValid = false;
}

// ── Instance 管理 ───────────────────────────────────────
// transform16: 行主序 4x4 矩阵（16 个 float），平移在 [3],[7],[11]
// materialID: 该实例的材质索引（供路径追踪着色使用）
TBVH_EXPORT int tinybvh_scene_add_instance(
    SceneHandle* scene,
    int blasIdx,
    const float* transform16,
    int materialID)
{
    if (blasIdx < 0 || blasIdx >= (int)scene->blasPool.size()) return -1;

    tinybvh::BLASInstance instance((uint32_t)blasIdx);
    // 将 float[16] 拷入 instance.transform（bvhmat4 内部为 float[16]）
    std::memcpy(&instance.transform, transform16, 16 * sizeof(float));
    // Update 计算逆变换 + 世界空间 AABB
    instance.Update(scene->blasPool[blasIdx]);

    scene->instances.push_back(instance);
    scene->instanceMaterials.push_back((uint32_t)materialID);
    return (int)(scene->instances.size() - 1);
}

TBVH_EXPORT void tinybvh_scene_update_instance(
    SceneHandle* scene,
    int instIdx,
    const float* transform16)
{
    if (instIdx < 0 || instIdx >= (int)scene->instances.size()) return;

    tinybvh::BLASInstance& instance = scene->instances[instIdx];
    std::memcpy(&instance.transform, transform16, 16 * sizeof(float));
    int blasIdx = (int)instance.blasIdx;
    if (blasIdx >= 0 && blasIdx < (int)scene->blasPool.size()) {
        instance.Update(scene->blasPool[blasIdx]);
    }
}

// swap-remove：O(1) 删除，但会改变最后一个 instance 的索引
TBVH_EXPORT void tinybvh_scene_remove_instance(
    SceneHandle* scene,
    int instIdx)
{
    if (instIdx < 0 || instIdx >= (int)scene->instances.size()) return;
    // 用末尾元素覆盖，再弹出
    int last = (int)scene->instances.size() - 1;
    if (instIdx != last) {
        scene->instances[instIdx] = scene->instances[last];
        scene->instanceMaterials[instIdx] = scene->instanceMaterials[last];
    }
    scene->instances.pop_back();
    scene->instanceMaterials.pop_back();
}

// ── 构建 TLAS ───────────────────────────────────────────
TBVH_EXPORT void tinybvh_scene_build_tlas(SceneHandle* scene) {
    // 刷新 blasPtrs 数组（BLAS 池可能已变化）
    scene->blasPtrs.resize(scene->blasPool.size());
    for (size_t i = 0; i < scene->blasPool.size(); i++) {
        scene->blasPtrs[i] = scene->blasPool[i];
    }
    // 构建 TLAS：遍历 BLASInstance 的世界空间 AABB
    uint32_t instCount = (uint32_t)scene->instances.size();
    uint32_t blasCount = (uint32_t)scene->blasPool.size();
    scene->tlas.Build(
        scene->instances.data(), instCount,
        scene->blasPtrs.data(), blasCount);
}

// ── TLAS 射线求交（两层级遍历）──────────────────────────
TBVH_EXPORT HitResult tinybvh_scene_intersect(
    SceneHandle* scene,
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float tMax)
{
    tinybvh::Ray ray(
        tinybvh::bvhvec3(ox, oy, oz),
        tinybvh::bvhvec3(dx, dy, dz),
        tMax
    );
    // BVH::Intersect 在 isTLAS() 时自动走 IntersectTLAS 两层级路径
    scene->tlas.Intersect(ray);

    HitResult result;
    result.hit     = (ray.hit.t < tMax) ? 1 : 0;
    result.instIdx = (int)ray.hit.inst;  // 命中的实例索引
    result.primIdx = (int)ray.hit.prim;   // BLAS 内的三角形索引
    result.t       = ray.hit.t;
    result.u       = ray.hit.u;
    result.v       = ray.hit.v;
    return result;
}

TBVH_EXPORT void tinybvh_scene_intersect_batch(
    SceneHandle*  scene,
    const float* origins,    // float[n*3]
    const float* directions, // float[n*3]
    HitResult*   results,    // HitResult[n]
    int          count,
    float        tMax)
{
    for (int i = 0; i < count; i++) {
        int oi = i * 3, di = i * 3;
        tinybvh::Ray ray(
            tinybvh::bvhvec3(origins[oi], origins[oi+1], origins[oi+2]),
            tinybvh::bvhvec3(directions[di], directions[di+1], directions[di+2]),
            tMax
        );
        scene->tlas.Intersect(ray);
        results[i].hit     = (ray.hit.t < tMax) ? 1 : 0;
        results[i].instIdx = (int)ray.hit.inst;
        results[i].primIdx = (int)ray.hit.prim;
        results[i].t       = ray.hit.t;
        results[i].u       = ray.hit.u;
        results[i].v       = ray.hit.v;
    }
}

// ════════════════════════════════════════════════════════════════
//  路径追踪数据导出接口
//  用于将 tinybvh 内部数据导出为 Shader 可消费的全局数组。
//  典型流程：add_blas/add_blas_indexed → add_instance →
//            build_tlas → assemble_globals → 查询各数组
// ════════════════════════════════════════════════════════════════

// ── 全局数组组装 ──────────────────────────────────────
// 在所有 BLAS 构建完成、build_tlas 之后调用。
// 负责拼接全局 BLAS 节点数组（重定位子节点索引）、
// 全局 BLAS primIdx 数组、BLAS 描述表。
TBVH_EXPORT void tinybvh_scene_assemble_globals(SceneHandle* scene) {
    if (!scene) return;

    size_t blasCount = scene->blasPool.size();

    // 1. 计算各 BLAS 的基地址（累计）
    scene->blasDescriptors.resize(blasCount);
    uint32_t nodeBase = 0, primBase = 0, triBase = 0;
    for (size_t i = 0; i < blasCount; i++) {
        tinybvh::BVH* blas = scene->blasPool[i];
        scene->blasDescriptors[i].blasNodeBase = nodeBase;
        scene->blasDescriptors[i].primIdxBase  = primBase;
        scene->blasDescriptors[i].triBase      = triBase;
        scene->blasDescriptors[i].vertBase    = scene->blasVertBase[i];
        nodeBase += blas->usedNodes;
        primBase += blas->idxCount;
        triBase  += blas->triCount;
    }

    // 2. 拼接全局 BLAS 节点数组 + 重定位子节点索引
    scene->globalBlasNodes.clear();
    scene->globalBlasNodes.reserve(nodeBase);
    for (size_t i = 0; i < blasCount; i++) {
        tinybvh::BVH* blas = scene->blasPool[i];
        uint32_t myNodeBase = scene->blasDescriptors[i].blasNodeBase;
        uint32_t myPrimBase = scene->blasDescriptors[i].primIdxBase;
        for (uint32_t n = 0; n < blas->usedNodes; n++) {
            tinybvh::BVH::BVHNode node = blas->bvhNode[n];
            if (node.triCount > 0) {
                // 叶子节点：leftFirst 是 primIdx 起始，加 primIdxBase
                node.leftFirst += myPrimBase;
            } else {
                // 内部节点：leftFirst 是子节点索引，加 blasNodeBase
                node.leftFirst += myNodeBase;
            }
            scene->globalBlasNodes.push_back(node);
        }
    }

    // 3. 拼接全局 BLAS primIdx 数组（值不变，shader 侧加 triBase）
    scene->globalBlasPrimIdx.clear();
    scene->globalBlasPrimIdx.reserve(primBase);
    for (size_t i = 0; i < blasCount; i++) {
        tinybvh::BVH* blas = scene->blasPool[i];
        for (uint32_t j = 0; j < blas->idxCount; j++) {
            scene->globalBlasPrimIdx.push_back(blas->primIdx[j]);
        }
    }

    scene->globalsValid = true;
}

// ── TLAS 数据查询（直接返回 native 指针，TLAS rebuild 后失效）──

TBVH_EXPORT const void* tinybvh_scene_get_tlas_nodes(
    SceneHandle* scene, int* outCount)
{
    if (!scene || !outCount) return nullptr;
    *outCount = (int)scene->tlas.usedNodes;
    return scene->tlas.bvhNode;
}

TBVH_EXPORT const uint32_t* tinybvh_scene_get_tlas_primidx(
    SceneHandle* scene, int* outCount)
{
    if (!scene || !outCount) return nullptr;
    *outCount = (int)scene->tlas.idxCount;
    return scene->tlas.primIdx;
}

// ── 全局 BLAS 数据查询（从缓存返回，assemble_globals 后有效）──

TBVH_EXPORT const void* tinybvh_scene_get_global_blas_nodes(
    SceneHandle* scene, int* outCount)
{
    if (!scene || !outCount) return nullptr;
    *outCount = (int)scene->globalBlasNodes.size();
    return scene->globalBlasNodes.data();
}

TBVH_EXPORT const uint32_t* tinybvh_scene_get_global_blas_primidx(
    SceneHandle* scene, int* outCount)
{
    if (!scene || !outCount) return nullptr;
    *outCount = (int)scene->globalBlasPrimIdx.size();
    return scene->globalBlasPrimIdx.data();
}

// ── BLAS 描述表查询 ──

TBVH_EXPORT const BLASDescriptor* tinybvh_scene_get_blas_descriptors(
    SceneHandle* scene, int* outCount)
{
    if (!scene || !outCount) return nullptr;
    *outCount = (int)scene->blasDescriptors.size();
    return scene->blasDescriptors.data();
}

// ── 实例表查询（按需组装：BLASInstance + materialID）──
// 返回的指针指向 thread_local 缓存，仅在下次调用本函数时失效
TBVH_EXPORT const InstanceRecord* tinybvh_scene_get_instance_table(
    SceneHandle* scene, int* outCount)
{
    if (!scene || !outCount) return nullptr;
    static thread_local std::vector<InstanceRecord> cache;
    size_t n = scene->instances.size();
    cache.resize(n);
    for (size_t i = 0; i < n; i++) {
        cache[i].materialID = scene->instanceMaterials[i];
        cache[i].blasID     = scene->instances[i].blasIdx;
        std::memcpy(cache[i].transform,    &scene->instances[i].transform,    16 * sizeof(float));
        std::memcpy(cache[i].invTransform, &scene->instances[i].invTransform, 16 * sizeof(float));
    }
    *outCount = (int)n;
    return cache.data();
}
