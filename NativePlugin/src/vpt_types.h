#pragma once

// ──────────────────────────────────────────────────────────
// VPT 共享数据结构（C#/C++ 跨语言，内存布局严格对齐）
//
// C# 侧对应:
//   PathMaterial  → VPT_Material  (96B)
//   PathLight     → VPT_Light     (32B)
//   VertexGpuData → VPT_Vertex    (80B)
//   InstanceRecord → VPT_Instance  (136B + padding → 144B)
// ──────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>

// ── 前向声明: Vulkan 类型（避免在此头文件中引入 Vulkan 头文件）──
#include <vulkan/vulkan.h>

// ═══════════════════════════════════════════════════════════
//  GPU 数据结构（与 C# StructLayout.Sequential 对齐）
// ═══════════════════════════════════════════════════════════

#pragma pack(push, 1)

/// 顶点结构（80B，与 RenderBufferManager.VertexGpuData 一致）
struct VPT_Vertex
{
    float    pos[3];       // 12B
    float    pad0;         // 4B
    float    normal[3];    // 12B
    float    pad1;         // 4B
    float    tangent[4];   // 16B (xyz + sign)
    float    uv[2];        // 8B
    float    pad2[2];      // 8B
    float    color[4];     // 16B
    // 合计 80B
};
static_assert(sizeof(VPT_Vertex) == 80, "VPT_Vertex must be 80 bytes");

/// 材质结构（96B，与 PathMaterial 一致）
struct VPT_Material
{
    float    albedo[4];         // 16B - base color tint (rgba)
    float    metallic;          // 4B
    float    roughness;         // 4B
    float    bumpScale;         // 4B - normal map strength
    uint32_t flags;             // 4B - bit0:hasBaseMap, bit1:hasMetalRough, bit2:hasNormal, bit3:hasEmissive, bit4:transparent
    // -- 32B --
    float    emissionColor[4];  // 16B - emissive tint (rgba)
    uint32_t baseTexID;        // 4B - BaseColorArray slice (0xFFFFFFFF=none)
    uint32_t metalRoughTexID;  // 4B
    uint32_t normalTexID;      // 4B
    uint32_t emissiveTexID;    // 4B
    // -- 32B --
    float    uvScaleOffset[4];  // 16B - xy=scale, zw=offset
    // -- 16B --
    float    transmission;      // 4B
    float    ior;               // 4B
    float    diffuseTransmission;// 4B
    uint32_t thinWalled;        // 4B
    // -- 16B --
    // 合计 96B
};
static_assert(sizeof(VPT_Material) == 96, "VPT_Material must be 96 bytes");

/// 光源结构（32B，与 PathLight 一致）
struct VPT_Light
{
    float    positionOrDir[3]; // 12B
    float    range;            // 4B
    float    color[3];         // 12B
    uint32_t type;             // 4B - 0=point, 1=directional
    // 合计 32B
};
static_assert(sizeof(VPT_Light) == 32, "VPT_Light must be 32 bytes");

#pragma pack(pop)

// ═══════════════════════════════════════════════════════════
//  P/Invoke 传输结构（C# → Native 的数据描述）
// ═══════════════════════════════════════════════════════════

/// 网格数据描述（C# 侧 GCHandle.Pinned 后传入指针）
struct VPT_MeshData
{
    const VPT_Vertex* vertices;    // 顶点数据指针（80B/vertex）
    uint32_t          vertexCount; // 顶点数量
    const uint32_t*   indices;     // 索引数据指针
    uint32_t          indexCount;  // 索引数量（=三角形数*3）
    uint32_t          materialID;  // 材质 ID
};

/// 实例数据（Transform 矩阵 + BLAS/Material 关联）
struct VPT_InstanceData
{
    uint32_t materialID;
    uint32_t blasID;
    // 3x4 行主序变换矩阵（与 VkAccelerationStructureInstanceKHR 兼容）
    // transform[0..2]  = row0, transform[3..5]  = row1, transform[6..8]  = row2
    float    transform[12];
};

/// 相机数据（通过 P/Invoke 传入）
struct VPT_CameraData
{
    float    viewInv[16];     // 4x4 逆视图矩阵（行主序）
    float    projInv[16];     // 4x4 逆投影矩阵（行主序）
    float    position[3];     // 相机世界坐标
    float    fov;             // 视场角（弧度）
    float    jitter[2];       // 像素抖动偏移
    float    nearPlane;       // 近裁剪面
    float    farPlane;        // 远裁剪面
    int32_t  frameCount;      // 帧序号（用于时域累积）
    int32_t  maxDepth;        // 最大弹射深度
    float    ambientColor[3]; // 环境光颜色
    float    pad;             // 对齐填充
};

// ═══════════════════════════════════════════════════════════
//  场景更新参数（VPT_UpdateScene 的打包形式）
// ═══════════════════════════════════════════════════════════

/// C# 侧将所有数据打包为一个结构传入
struct VPT_SceneUpdate
{
    const VPT_MeshData*     meshes;         // mesh 数组
    int32_t                 meshCount;
    const VPT_Material*     materials;     // 材质数组
    int32_t                 materialCount;
    const VPT_Light*        lights;         // 光源数组
    int32_t                 lightCount;
    const VPT_InstanceData* instances;     // 实例数组
    int32_t                 instanceCount;
};
