#pragma once

#include "vpt_types.h"
#include <vulkan/vulkan.h>
#include <vector>

// ── 场景资源管理命名空间 ──────────────────────────────────
namespace VPTScene {

    /// 加载 Vulkan 扩展函数指针（vkGetAccelerationStructureBuildSizesKHR 等）
    void LoadExtensionFunctions(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkDevice device);

    /// 推送场景数据（CPU 深拷贝，主线程可安全调用；不碰 Vulkan 设备/队列）。
    /// 真正的 buffer 创建 + BLAS/TLAS 构建在 RecordSceneUpdateIfNeeded 中
    /// 录进 Unity 的命令缓冲（Plant B 事件内、渲染线程上），因此这里只会
    /// 拷贝调用方的指针数据 —— 返回后调用方可以立即释放/复用那些内存。
    void PushScene(const VPT_SceneUpdate& su);

    /// 是否有待处理的场景推送（供 dispatch 决定本帧是否需要取命令缓冲）
    bool IsSceneUpdatePending();

    /// 若存在待处理场景推送，向命令缓冲录制：等 GPU 空闲 → 拆旧资源 →
    /// 上传 buffer → 构建 BLAS/TLAS。
    /// 返回 true 表示本帧发生了场景重建（旧 TLAS 已销毁、descriptor 已过期，
    /// 调用方本帧不得再 dispatch）。
    bool RecordSceneUpdateIfNeeded(VkCommandBuffer cmd);

    /// 若存在待处理的蒙皮 mesh 注册，向命令缓冲录制 staging 上传 + base 副本拷贝。
    /// 必须在场景构建之后调用（依赖统一顶点缓冲的内容）。
    void RecordSkinRegistrationIfNeeded(VkCommandBuffer cmd);

    /// 取走"重建后需要做一次 GPU 同步探针"的标记。
    /// 重建帧录制的命令由 Unity 提交，插件自己看不到 VkResult；下一帧在事件里
    /// 做一次 vkDeviceWaitIdle 就能把"重建帧把 GPU 搞死了"这件事变成显式的
    /// VK_ERROR_DEVICE_LOST（否则会静默带着死设备继续跑几十秒再卡死）。
    bool TakeGpuSyncProbe();

    /// 动态场景：仅更新实例变换（每帧调用，刚体运动）
    /// 写入常驻 host 缓冲并标记 TLAS 待重建，实际 build 在下一次 dispatch 前录制
    void UpdateInstances(const VPT_InstanceData* instances, int32_t instanceCount);

    /// 若本帧有实例更新，向命令缓冲录制：InstanceInfo staging→device 拷贝 + TLAS 重建 + barrier。
    /// 由 dispatch 路径（RenderCallback / 遗留 dispatch / debug depth）在绑定管线前调用。
    /// 返回 true 表示实例容量扩容、TLAS 环形资源被重建（descriptor 里的旧 buffer
    /// 句柄已失效，调用方本帧不得 dispatch）。
    bool RecordTLASUpdateIfNeeded(VkCommandBuffer cmd);

    /// 若本帧有蒙皮更新，向命令缓冲录制：BLAS 原地 refit（mode=UPDATE）+ barrier 链。
    /// 必须在 RecordTLASUpdateIfNeeded 之前调用（TLAS 引用 BLAS 内容时 refit 需先行）。
    void RecordBLASRefitIfNeeded(VkCommandBuffer cmd);

    // ── 骨骼动画（阶段2 GPU 蒙皮，对齐参考项目 AnimationVk）──

    /// 注册蒙皮 mesh 的静态数据（一次性；场景重建后须重新注册）。
    /// 从统一顶点缓冲复制 bind-pose base 副本（蒙皮 compute 读 base 写统一缓冲，
    /// 避免在已变形数据上反复蒙皮），上传权重/关节索引，分配关节矩阵环形缓冲。
    /// blasID 须以 VPT_MESH_FLAG_DEFORMABLE 上传，vertexCount 须与首次上传一致。
    /// bindposes 不上传：C# 每帧折算进关节矩阵（worldToLocal * bone.localToWorld * bindpose）。
    /// weights: vec4/顶点；joints: ivec4/顶点。
    void RegisterSkinnedMesh(uint32_t blasID,
                             const float* weights, const int32_t* joints,
                             uint32_t vertexCount, uint32_t jointCount);

    /// 每帧上传关节矩阵（jointCount 个 mat4，行主序 vec4 行）。
    /// 写入常驻 host-visible 环形槽位，标记待蒙皮；dispatch 前由
    /// RecordGPUSkinningIfNeeded 录制 compute dispatch，BLAS refit 由
    /// RecordBLASRefitIfNeeded 统一处理。
    /// 须先 RegisterSkinnedMesh，否则忽略并告警。
    void UpdateSkinning(uint32_t blasID, const float* jointMatrices, uint32_t jointCount);

    /// 若本帧有关节矩阵更新，向命令缓冲录制：host barrier + 蒙皮 compute dispatch +
    /// compute 写→AS build 读 barrier。必须在 RecordBLASRefitIfNeeded 之前调用
    /// （refit 引用蒙皮后的新顶点），dispatch 点须以
    /// RecordGPUSkinningIfNeeded → RecordBLASRefitIfNeeded → RecordTLASUpdateIfNeeded 顺序调用。
    void RecordGPUSkinningIfNeeded(VkCommandBuffer cmd);

    // ── 骨骼动画（阶段3 refit 质量衰减对策）──

    /// 设置变形 BLAS 全量 rebuild 间隔（C# Inspector 暴露，随参数变化调用）。
    /// refit 是原地包络微调，大幅变形多帧后 BVH 遍历效率下降；某变形 BLAS
    /// 累计 refit 达到 interval 次后，下一次改走 mode=BUILD 全量重建（复用
    /// dstAS/scratch），随后计数清零继续 refit。interval=0 禁用（始终 refit）。
    /// 计数在 RecordBLASRefitIfNeeded 中按 BLAS 累计，场景重建时随资源一起清零。
    void SetDeformableRebuildInterval(uint32_t interval);

    /// 销毁所有场景资源（VkBuffer/VkDeviceMemory/VkAccelerationStructureKHR）
    void DestroyResources();

    /// 获取 TLAS 句柄（供 dispatch 使用）
    VkAccelerationStructureKHR GetTLAS();

    /// 是否已就绪（TLAS 构建完成）
    bool IsReady();

    /// 扩展函数是否加载成功（用于 VPT_Initialize 检查）
    bool AreExtensionsReady();

    // ── Getter（供 dispatch 绑定 SSBO）──
    VkBuffer GetUnifiedVertexBuffer();
    VkBuffer GetUnifiedIndexBuffer();
    VkBuffer GetInstanceInfoBuffer();
    /// 当前环形槽位在 InstanceInfo SSBO 中的字节偏移（动态场景每帧变化）
    VkDeviceSize GetInstanceInfoOffset();
    VkBuffer GetMaterialBuffer();
    VkBuffer GetLightBuffer();

    // ── 供其他模块复用的工具函数 ──
    /// 查找匹配 typeFilter + 属性的内存类型索引
    uint32_t FindMemoryType(VkPhysicalDevice gpu, uint32_t typeFilter, VkMemoryPropertyFlags props);

} // namespace VPTScene
