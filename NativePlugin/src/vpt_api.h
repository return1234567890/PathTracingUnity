#pragma once

#include "IUnityInterface.h"  // UNITY_INTERFACE_EXPORT / UNITY_INTERFACE_API 宏
#include "vpt_types.h"
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

    /// 初始化 Native 路径追踪插件（由 C# 在 Awake 中调用）
    /// 返回: 0=成功, 1=Vulkan 设备获取失败, 2=RT Core 扩展不支持
    UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API VPT_Initialize();

    /// 销毁插件资源（由 C# 在 OnDestroy 中调用）
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_Destroy();

    /// 更新场景数据
    /// sceneUpdate: 指向 VPT_SceneUpdate 结构的指针
    ///   - meshes: VPT_MeshData 数组（每个 mesh 描述顶点/索引/材质）
    ///   - materials: VPT_Material 数组（96B/元素）
    ///   - lights: VPT_Light 数组（32B/元素）
    ///   - instances: VPT_InstanceData 数组（含 blasID + 3x4 变换矩阵）
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_UpdateScene(
        const VPT_SceneUpdate* sceneUpdate);

    /// 执行路径追踪 dispatch（阶段4）
    /// outputPtr/gbuf0Ptr/gbuf1Ptr: Unity RenderTexture 的原生指针（输出/G-Buffer）
    /// diffuseAlbedoPtr/specularAlbedoPtr: DLSS RR albedo G-buffer RT 指针
    /// baseColorPtr/metallicRoughPtr/normalPtr/emissivePtr: Unity Texture2DArray 的原生指针
    /// width/height: 渲染尺寸
    /// cameraData: 指向 VPT_CameraData 结构的指针
    /// lightCount: 场景光源数量（NEE 用）
    /// samplesPerPixel: 每帧采样数（SPP）
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_DispatchPathTrace(
        void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
        void* diffuseAlbedoPtr, void* specularAlbedoPtr,
        void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
        int32_t width, int32_t height,
        const VPT_CameraData* cameraData,
        int32_t lightCount, int32_t samplesPerPixel);

    /// 执行 debug 深度图 dispatch（阶段2.6验证用）
    /// 写入 Unity RenderTexture 的原生 VkImage，无 CPU 回读
    /// nativeTexturePtr: Unity RenderTexture 的原生指针
    /// width/height: 渲染尺寸
    /// cameraData: 指向 VPT_CameraData 结构的指针
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_DispatchDebugDepth(
        void* nativeTexturePtr, int32_t width, int32_t height,
        const VPT_CameraData* cameraData);

    /// 查询设备 RT Core 支持状态
    /// 返回: 1=支持 VK_KHR_ray_query, 0=不支持
    UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API VPT_QueryRayQuerySupport();

    // ── Plan B: Two-phase dispatch (eliminates vkQueueWaitIdle) ──────────

    /// Phase 1: 准备路径追踪 dispatch（在 IssuePluginEvent 之前调用）
    /// 访问 Unity 纹理、填充 UBO、更新描述符集，不提交命令缓冲
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_PrepareDispatch(
        void* outputPtr, void* gbuf0Ptr, void* gbuf1Ptr,
        void* diffuseAlbedoPtr, void* specularAlbedoPtr,
        void* baseColorPtr, void* metallicRoughPtr, void* normalPtr, void* emissivePtr,
        int32_t width, int32_t height,
        const VPT_CameraData* cameraData,
        int32_t lightCount, int32_t samplesPerPixel);

    /// Phase 2: Unity 渲染事件回调（由 IssuePluginEvent 在命令缓冲执行期间触发）
    /// 通过 CommandRecordingState 获取 Unity 的 VkCommandBuffer，录制 dispatch
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_RenderCallback(int32_t eventID);

    /// 获取渲染回调函数指针（供 C# IssuePluginEvent 使用）
    UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API VPT_GetRenderCallback();

    // ── DLSS NGX 接口（阶段3 SR + 阶段4 RR） ──────────────

    /// 初始化 NGX DLSS feature（SR 或 RR）
    /// renderW/H: 渲染尺寸; outputW/H: 输出尺寸; qualityMode: 0=Q 1=B 2=P 3=UP
    /// mode: 0=SR(超分), 1=RR(光线重建)
    /// 返回: 0=成功, -1=不可用, -2=RR不支持
    UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API VPT_DLSS_Init(
        int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
        int32_t qualityMode, int32_t mode);

    /// 销毁 NGX DLSS feature
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_DLSS_Destroy();

    /// Phase 1: 准备 DLSS SR Evaluate dispatch
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_DLSS_PrepareDispatch(
        void* colorLowPtr, void* motionLowPtr, void* depthLowPtr, void* outputHighPtr,
        int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
        float jitterX, float jitterY, int32_t reset);

    /// Phase 1: 准备 DLSS RR Evaluate dispatch
    /// 额外绑定 normalRoughPtr (GBuffer1) + 线性深度 + albedo G-buffer + 视图/投影矩阵
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_DLSS_PrepareRRDispatch(
        void* colorLowPtr, void* motionLowPtr, void* linearDepthPtr, void* outputHighPtr,
        void* normalRoughPtr,
        void* diffuseAlbedoPtr, void* specularAlbedoPtr,
        int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
        float jitterX, float jitterY, int32_t reset,
        const float* viewMatrix, const float* projMatrix);

#ifdef __cplusplus
}
#endif
