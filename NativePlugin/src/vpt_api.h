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

    /// 动态场景：仅更新实例变换（每帧调用，刚体运动，BLAS 不动）
    /// 写入常驻 host 缓冲并标记 TLAS 待重建，实际 build 在下一次 dispatch 前录制
    /// instances: 指向 VPT_InstanceData 数组（顺序必须与 VPT_UpdateScene 一致）
    /// instanceCount: 实例数（可小于初始容量，超出时内部扩容）
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_UpdateInstances(
        const VPT_InstanceData* instances, int32_t instanceCount);

    /// 骨骼动画（阶段2 GPU 蒙皮）：注册蒙皮 mesh 静态数据（一次性；场景重建后须重新注册）
    /// native 侧自统一顶点缓冲复制 bind-pose base 副本，上传权重/关节索引，
    /// 分配关节矩阵环形缓冲。blasID 须以 VPT_MESH_FLAG_DEFORMABLE 上传，
    /// vertexCount 须与首次上传一致；bindposes 不上传（C# 每帧折算进关节矩阵）。
    /// weights: vec4/顶点（boneWeights 权重）；joints: ivec4/顶点（骨骼索引）
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_RegisterSkinnedMesh(
        uint32_t blasID, const float* weights, const int32_t* joints,
        int32_t vertexCount, int32_t jointCount);

    /// 骨骼动画（阶段2 GPU 蒙皮）：每帧上传关节矩阵（jointCount 个 mat4，行主序）
    /// 写入 host-visible 环形槽位，dispatch 前 native 录制蒙皮 compute + BLAS refit。
    /// 须先 VPT_RegisterSkinnedMesh，否则忽略并告警。
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_UpdateSkinning(
        uint32_t blasID, const float* jointMatrices, int32_t jointCount);

    /// 骨骼动画（阶段3 refit 质量衰减对策）：设置变形 BLAS 全量 rebuild 间隔。
    /// 某 BLAS 累计 interval 次 refit 后改走一次 mode=BUILD 全量重建（恢复 BVH
    /// 遍历效率），随后计数清零；0=禁用（始终 refit）。
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_SetDeformableRebuildInterval(
        uint32_t interval);

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

    /// 查询设备 RT Core 支持状态
    /// 返回: 1=支持 VK_KHR_ray_query, 0=不支持
    UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API VPT_QueryRayQuerySupport();

    /// 设置 HDR 环境光贴图
    /// pixels: RGBA32F 数据（每像素 4 float，width*height 个像素）
    /// width/height: equirect 贴图尺寸
    /// 在原生侧创建 VkImage + alias-method importance 采样表（SSBO）
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API
    VPT_SetEnvironmentMap(const float* pixels, int32_t width, int32_t height);

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

    // ── DLSS NGX 接口（Ray Reconstruction） ──────────────

    /// 初始化 NGX DLSS Ray Reconstruction feature
    /// renderW/H: 渲染尺寸; outputW/H: 输出尺寸; qualityMode: 0=Q 1=B 2=P 3=UP
    /// 返回: 0=成功, -1=不可用, -2=RR 不支持
    UNITY_INTERFACE_EXPORT int32_t UNITY_INTERFACE_API VPT_DLSS_Init(
        int32_t renderW, int32_t renderH, int32_t outputW, int32_t outputH,
        int32_t qualityMode);

    /// 销毁 NGX DLSS feature
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API VPT_DLSS_Destroy();

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
