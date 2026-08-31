#pragma once

#include "vpt_types.h"
#include <vulkan/vulkan.h>
#include <vector>

// ── 场景资源管理命名空间 ──────────────────────────────────
namespace VPTScene {

    /// 加载 Vulkan 扩展函数指针（vkGetAccelerationStructureBuildSizesKHR 等）
    void LoadExtensionFunctions(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkDevice device);

    /// 更新场景：上传 buffer + 构建 BLAS/TLAS
    void UpdateScene(const VPT_SceneUpdate& su);

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
    VkBuffer GetMaterialBuffer();
    VkBuffer GetLightBuffer();

} // namespace VPTScene
