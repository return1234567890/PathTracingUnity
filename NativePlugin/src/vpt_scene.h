#pragma once

#include "vpt_types.h"
#include <vulkan/vulkan.h>
#include <vector>

// ── 场景资源管理命名空间 ──────────────────────────────────
namespace VPTScene {

    /// 加载 Vulkan 扩展函数指针（vkGetAccelerationStructureBuildSizesKHR 等）
    void LoadExtensionFunctions(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkDevice device);

    /// 更新场景：上传 buffer + 构建 BLAS/TLAS（静态拓扑变化时调用，低频路径）
    void UpdateScene(const VPT_SceneUpdate& su);

    /// 动态场景：仅更新实例变换（每帧调用，刚体运动）
    /// 写入常驻 host 缓冲并标记 TLAS 待重建，实际 build 在下一次 dispatch 前录制
    void UpdateInstances(const VPT_InstanceData* instances, int32_t instanceCount);

    /// 若本帧有实例更新，向命令缓冲录制：InstanceInfo staging→device 拷贝 + TLAS 重建 + barrier。
    /// 由 dispatch 路径（RenderCallback / 遗留 dispatch / debug depth）在绑定管线前调用。
    void RecordTLASUpdateIfNeeded(VkCommandBuffer cmd);

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
