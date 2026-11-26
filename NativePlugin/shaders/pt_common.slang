// ══════════════════════════════════════════════════════════════
//  VulkanPathTracePlugin — pt_common.slang
//
//  路径追踪的公共部分（结构体 / 资源 / 材质 / BSDF / BTDF / 体积 /
//  环境采样 / 随机数 / 命中重建 / 阴影透射）。
//
//  迁移来源：shaders/pathtrace.glsl（GLSL 460 + GL_EXT_ray_query；该文件已于
//            2026-11-22 随旧后端一起删除，需要查历史请用 git）
//  迁移目标：VK_KHR_ray_tracing_pipeline（Slang 单模块 7 入口）
//
//  与 GLSL 版的差异（均为「换后端」必需，语义不变）：
//    · ray query 主光线/阴影 candidate 循环 → TraceRay + SBT hit group
//      （group 0 = 主路径 CH/AH，group 1 = 阴影 CH/AH）
//    · 着色仍在 raygen：CH 只回写 {hitT, instanceIndex, primitiveIndex, bary}
//    · 阴影体积状态机从局部变量搬进 ShadowPayload（any-hit 之间要保留）
//    · texture() → SampleLevel(..., 0)：ray tracing 阶段没有隐式 LOD，
//      显式 0 级 == GLSL compute 下的实际行为，且跨驱动确定
//    · mat4 * vec4 → mul(mat4, vec4)：Slang 的 row_major 定量为
//      「内存行主序」，与 GLSL 的 `layout(row_major) mat4` 一一对应
//
//  binding 编号与 C++ 侧 18 个槽位严格一一对应（见 vpt_dispatch.cpp）。
// ══════════════════════════════════════════════════════════════

#define PI 3.14159265359f
// 普通表面偏移 5mm：远离表面避免自交（对实体/不透明表面足够）
#define EPS_OFFSET 5e-3f
// 体积（Volume）边界偏移 0.1mm：12mm 玻璃用 5mm 会吃掉近一半内部路径，
// 薄玻璃（<5mm）甚至会直接跳过出射面。0.1mm 远大于 float 精度，不会自交。
#define EPS_VOLUME 1e-4f
// 未命中哨兵：rmissMain / rmissShadow 之外的路径只在「真命中」时改写 hitT
#define INFINITE_T 1e30f
// 无效实例索引哨兵
#define INVALID_INSTANCE 0xFFFFFFFFu

#define MAT_HAS_BASE       1u
#define MAT_HAS_MR         2u
#define MAT_HAS_NORMAL     4u
#define MAT_HAS_EMISS      8u
#define MAT_IS_TRANSPARENT 16u
#define MAT_IS_VOLUME      32u   // bit5: 有厚度玻璃，走 Beer-Lambert 体积分支
#define MAT_IS_THIN_WALLED 64u   // bit6: 零厚度薄壁（单面玻璃），保留原 double-tint 语义
#define MAT_ALPHA_TEST     128u  // bit7: alpha clipping / 镂空
#define TEX_NONE           0xFFFFFFFFu

#define LOBE_DIFFUSE_REFLECTION    0
#define LOBE_SPECULAR_REFLECTION   1
#define LOBE_SPECULAR_TRANSMISSION 2
#define LOBE_DIFFUSE_TRANSMISSION  3
#define LOBE_COUNT                 4

#define MIN_TRANSMISSION  0.01f
#define MAX_SHADOW_LAYERS 16

// ══════════════════════════════════════════════════════════════
//  Descriptor Set 0（binding 与 C++ 侧一致）
// ══════════════════════════════════════════════════════════════

[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas;

// 存储图必须显式声明格式（对应 GLSL 的 layout(rgba16f)/layout(rgba32f)）：
// 不声明会生成「Unknown 格式」的 OpTypeImage，需要 shaderStorageImageWriteWithoutFormat
// 设备特性；声明后 SPIR-V 带 ImageFormat，只需要基础特性。
// 格式必须与 Unity 侧 RenderTexture 一致（PathTraceRendererFeature.CreateTargets）：
//   outputRT / gbuffer1 / diffuseAlbedo / specularAlbedo = ARGBHalf (rgba16f)
//   gbuffer0 (worldPos + depth)                          = ARGBFloat (rgba32f)
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]]  RWTexture2D<float4> outputImg;
[[vk::image_format("rgba32f")]] [[vk::binding(2, 0)]]  RWTexture2D<float4> gbufferPosDepth;
[[vk::image_format("rgba16f")]] [[vk::binding(3, 0)]]  RWTexture2D<float4> gbufferNormalRough;
// DLSS RR albedo G-buffer（主光线首次命中的材质分解，输入分辨率）
[[vk::image_format("rgba16f")]] [[vk::binding(14, 0)]] RWTexture2D<float4> diffuseAlbedoImg;
[[vk::image_format("rgba16f")]] [[vk::binding(15, 0)]] RWTexture2D<float4> specularAlbedoImg;

// DLSS RR specular guide（binding 18 → VPT_DLSS 的 pInSpecularRayDirectionHitDistance）
//   xyz = 镜面射线方向（世界空间，单位向量）
//   w   = 该射线自主表面出发命中的世界空间距离（米）；miss / 无镜面弹射 = 0
// 语义：RR 用它反推「虚拟命中点」 P_spec = P + dir * dist，从而对镜面分量做正确的
// 时域重投影（否则只能拿主表面 MV 硬套，运动时反射拖影）。
// 约定：只有 s==0 且 depth==0 选中【镜面反射】lobe 的像素写值，其余像素写 0。
//       写 0 等于「本帧没有镜面几何信息」→ RR 退回主表面重投影，不会算错虚拟点。
[[vk::image_format("rgba16f")]] [[vk::binding(18, 0)]] RWTexture2D<float4> specGuideImg;

// C++ 侧 PathTraceCameraUBO：208B / std140。
// row_major：C++ 传的是 Unity 的 Matrix4x4（行主序 16 float），
// 必须显式声明 row_major，否则会被当成列主序 → 相机全错。
struct CameraUBO
{
    row_major float4x4 viewInv;        // 0
    row_major float4x4 projInv;        // 64
    float4             posFov;         // 128  xyz = camera position, w = fov
    float4             jitterNearFar;  // 144  xy = jitter, z = near plane, w = far plane
    int4               frameMaxDepth;  // 160  x = frameCount, y = maxDepth, z = envReady(1=HDR 已上传)
    float4             ambientPad;     // 176  xyz = ambient color, w = pad
    uint               lightCount;     // 192
    uint               samplesPerPixel; // 196
    float              envIntensity;   // 200  HDR env intensity multiplier
    float              envRotation;    // 204  HDR env 绕 Y 轴旋转，单位弧度
};

[[vk::binding(4, 0)]] ConstantBuffer<CameraUBO> cam;

// ── SSBO 结构体（与 vpt_types.h 严格一致）──────────────────────
struct VPT_Vertex
{
    float3 pos;     float pad0;
    float3 normal;  float pad1;
    float4 tangent;
    float2 uv;      float2 pad2;
    float4 color;
};

struct VPT_Material
{
    float4 albedo;
    float  metallic;
    float  roughness;
    float  bumpScale;
    uint   flags;
    float4 emissionColor;
    uint   baseTexID;
    uint   metalRoughTexID;
    uint   normalTexID;
    uint   emissiveTexID;
    float4 uvScaleOffset;
    float  transmission;
    float  ior;
    float  diffuseTransmission;
    uint   thinWalled;
    // ── 96B ~ 128B：体积透射 ──
    float4 attenuationColor;     // 体积吸收色（线性 RGB，与 baseColor 相乘）
    float  attenuationDistance;  // 世界空间衰减距离（米）；<=0 无吸收
    uint   volumeFlags;          // 预留
    float  thickness;            // 世界空间厚度（米）；0 = 非体积
    float  alphaCutoff;          // alpha test 阈值（URP _Cutoff，默认 0.5）
};

struct VPT_Light
{
    float3 positionOrDir;
    float  range;
    float3 color;
    uint   type;
};

struct InstanceInfo
{
    float4 transformRow0;
    float4 transformRow1;
    float4 transformRow2;
    uint   vertexOffset;
    uint   indexOffset;
    uint   materialID;
    uint   pad;
};

[[vk::binding(5, 0)]] StructuredBuffer<VPT_Vertex>   vertices;
[[vk::binding(6, 0)]] StructuredBuffer<uint>         indices;
[[vk::binding(7, 0)]] StructuredBuffer<InstanceInfo> instances;
[[vk::binding(8, 0)]] StructuredBuffer<VPT_Material> materials;
[[vk::binding(9, 0)]] StructuredBuffer<VPT_Light>    lights;

// ── Texture Arrays（binding 10..13，combined image sampler）──
[[vk::binding(10, 0)]] Sampler2DArray<float4> baseColorTex;
[[vk::binding(11, 0)]] Sampler2DArray<float4> metallicRoughTex;
[[vk::binding(12, 0)]] Sampler2DArray<float4> normalTex;
[[vk::binding(13, 0)]] Sampler2DArray<float4> emissiveTex;

// ── HDR 环境贴图（binding 16/17）──────────────────────────────
[[vk::binding(16, 0)]] Sampler2D<float4> envHdrTex;
struct EnvAccel { uint alias; float q; };
[[vk::binding(17, 0)]] StructuredBuffer<EnvAccel> envAccel;

// ══════════════════════════════════════════════════════════════
//  Ray payload / hit attribute
// ══════════════════════════════════════════════════════════════

// 主路径 payload：只带「命中记录」，材质/BSDF/纹理全部在 raygen 回读 SSBO。
// 布局 20B：4 + 4 + 4 + 8 —— 远低于任何驱动的 payload 上限。
struct HitPayload
{
    float  hitT          = INFINITE_T;
    uint   instanceIndex = 0u;
    uint   primitiveIndex = 0u;
    float2 bary          = float2(0.0f);
};

// 阴影 payload：体积状态机（isInside / extinction / prevHitT）在 any-hit 之间保留。
// acceptMode：0 = 保留累积透射率（层数耗尽 / 空体穿透），1 = 完全遮挡（置 0）。
//   存在的理由：AcceptHitAndEndSearch() 之后 closest-hit 仍会执行，
//   而 rchitShadow 对「被不透明挡住」和「层数耗尽」需要不同结果。
struct ShadowPayload
{
    float  prevHitT          = 0.0f;
    uint   acceptMode        = 0u;
    uint   isInside          = 0u;
    uint   layerCount        = 0u;
    float3 extinction        = float3(0.0f);
    float3 totalTransmission = float3(1.0f);
};

// ══════════════════════════════════════════════════════════════
//  PRNG
// ══════════════════════════════════════════════════════════════

uint xxhash32(uint3 p)
{
    const uint P1 = 0x9E3779B1u, P2 = 0x85EBCA77u, P3 = 0xC2B2AE3Du, P5 = 0x165667B1u;
    uint h = P5 + (p.x + p.y) * P1;
    h ^= p.z; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    h ^= p.x; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    h ^= p.y; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

float rand(inout uint seed)
{
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) * (1.0f / 16777216.0f);
}

// ══════════════════════════════════════════════════════════════
//  Geometry helpers
// ══════════════════════════════════════════════════════════════

void createONB(float3 N, out float3 T, out float3 B)
{
    float3 up = abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

float3 cosineSampleHemisphere(float2 u, float3 T, float3 B, float3 N)
{
    float phi = 2.0f * PI * u.x;
    float r   = sqrt(u.y);
    float x   = r * cos(phi);
    float y   = r * sin(phi);
    float z   = sqrt(max(0.0f, 1.0f - u.y));
    return normalize(x * T + y * B + z * N);
}

// Transform a direction from object space to world space using InstanceInfo 3x4 row-major matrix
float3 transformDir(InstanceInfo info, float3 dir)
{
    return float3(
        dot(info.transformRow0.xyz, dir),
        dot(info.transformRow1.xyz, dir),
        dot(info.transformRow2.xyz, dir));
}

// ══════════════════════════════════════════════════════════════
//  GGX BSDF
// ══════════════════════════════════════════════════════════════

float D_GGX(float NdotH, float a2)
{
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d);
}

float G_SmithGGX(float NdotV, float NdotL, float a2)
{
    float g1v = 2.0f * NdotV / (NdotV + sqrt(a2 + (1.0f - a2) * NdotV * NdotV));
    float g1l = 2.0f * NdotL / (NdotL + sqrt(a2 + (1.0f - a2) * NdotL * NdotL));
    return g1v * g1l;
}

float3 F_Schlick(float3 F0, float cosTheta)
{
    return F0 + (1.0f - F0) * pow(1.0f - max(cosTheta, 0.0f), 5.0f);
}

float3 specularF0(float3 albedo, float metallic)
{
    return lerp(float3(0.04f), albedo, metallic);
}

// Evaluate BSDF for NEE (returns BSDF value, no NdotL factor)
float3 evaluateBSDF(VPT_Material mat, float3 N, float3 L, float3 V, float NdotL, float NdotV)
{
    float3 albedo    = mat.albedo.rgb;
    float  metallic  = mat.metallic;
    float  roughness = mat.roughness;
    float3 F0        = specularF0(albedo, metallic);

    float3 diffuse = (1.0f - metallic) * albedo / PI;
    diffuse *= (1.0f - mat.transmission);  // transparent: reduce reflect energy

    float3 H    = normalize(L + V);
    float  NdotH = max(dot(N, H), 0.0f);
    float  VdotH = max(dot(V, H), 0.0f);
    float  a     = roughness * roughness;
    float  a2    = a * a;
    float  D     = D_GGX(NdotH, a2);
    float  G     = G_SmithGGX(NdotV, NdotL, a2);
    float3 F     = F_Schlick(F0, VdotH);
    float3 specular = D * G * F / (4.0f * NdotV * NdotL + 1e-6f);

    return diffuse + specular;
}

// Sample BSDF for indirect bounce (returns BSDF value and pdf)
// lobeOut：本次选中的 lobe（LOBE_DIFFUSE_REFLECTION / LOBE_SPECULAR_REFLECTION）。
//   raygen 用它判断「这一跳是不是镜面反射」，从而写 DLSS RR 的 specular guide。
float3 sampleBSDF(VPT_Material mat, float3 N, float3 V,
                  inout uint seed, out float3 wi, out float pdf, out uint lobeOut)
{
    float3 albedo    = mat.albedo.rgb;
    float  metallic  = mat.metallic;
    float  roughness = mat.roughness;
    float3 F0        = specularF0(albedo, metallic);
    float  a         = roughness * roughness;
    float  a2        = a * a;

    // ⚠ 随机数的消费顺序必须保持稳定（r 先取，再算 pd），否则整条路径的
    //   RNG 流会平移，画面全变、与历史版本无法逐像素对比。
    float r  = rand(seed);

    float3 T, B;
    createONB(N, T, B);

    float NdotV = max(dot(N, V), 0.0f);

    // ── 漫反射 / 镜面 lobe 的选择概率 pd ──
    // 金属恒走镜面；介质按 Fresnel 加权（F0 = lerp(0.04, albedo, metallic)）。
    // 不用 pd = 1-metallic：那样介质表面永远走余弦，镜面 lobe 一次都不采 ——
    // 镜面反射只能靠余弦方向碰巧落进 GGX 瓣（噪声大），而且 RR 的 specular guide
    // 永远没有镜面弹射可写（该通路会变成死代码）。
    // 改 pd 只改方差、不改均值：下方 pdf 就是与选样策略一致的混合分布。
    float f0    = dot(F0, float3(1.0f / 3.0f));
    float fr    = f0 + (1.0f - f0) * pow(1.0f - NdotV, 5.0f);
    float pSpec = max(metallic, (1.0f - metallic) * clamp(fr, 0.0f, 1.0f));
    float pd    = 1.0f - clamp(pSpec, 0.0f, 1.0f);

    if (r < pd)
    {
        wi      = cosineSampleHemisphere(float2(rand(seed), rand(seed)), T, B, N);
        lobeOut = LOBE_DIFFUSE_REFLECTION;
    }
    else
    {
        float r1 = rand(seed), r2 = rand(seed);
        float phi      = 2.0f * PI * r1;
        float cosTheta = sqrt(max(0.0f, (1.0f - r2) / (1.0f + (a2 - 1.0f) * r2)));
        float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
        float3 H = normalize(sinTheta * cos(phi) * T
                           + sinTheta * sin(phi) * B
                           + cosTheta * N);
        wi      = reflect(-V, H);
        lobeOut = LOBE_SPECULAR_REFLECTION;
    }

    float NdotL = max(dot(N, wi), 0.0f);
    if (NdotL <= 0.0f) { pdf = 0.0f; return float3(0.0f); }

    float3 H     = normalize(wi + V);
    float  NdotH = max(dot(N, H), 0.0f);
    float  VdotH = max(dot(V, H), 0.0f);
    float  D     = D_GGX(NdotH, a2);
    float  G     = G_SmithGGX(NdotV, NdotL, a2);
    float3 F     = F_Schlick(F0, VdotH);
    float3 specular = D * G * F / (4.0f * NdotV * NdotL + 1e-6f);
    float3 diffuse  = (1.0f - metallic) * albedo / PI;
    diffuse *= (1.0f - mat.transmission);  // transparent: reduce reflect energy
    float3 bsdf = diffuse + specular;

    float pdf_diffuse  = NdotL / PI;
    float pdf_specular = (VdotH > 0.0f) ? (D * NdotH / (4.0f * VdotH)) : 0.0f;
    pdf = pd * pdf_diffuse + (1.0f - pd) * pdf_specular;

    return bsdf;
}

// ══════════════════════════════════════════════════════════════
//  Transparent BSDF: lobe weights / BTDF
// ══════════════════════════════════════════════════════════════

// Schlick Fresnel (scalar, for lobe weight allocation)
float schlickFresnelScalar(float ior, float cosTheta)
{
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    return r0 + (1.0f - r0) * pow(1.0f - max(cosTheta, 0.0f), 5.0f);
}

// Compute 4-lobe weights: x=diffuse_refl, y=spec_refl, z=spec_trans, w=diff_trans
float4 computeLobeWeights(VPT_Material mat, float VdotN)
{
    float fr         = schlickFresnelScalar(mat.ior, abs(VdotN));
    float weightBase = 1.0f;
    float4 w         = float4(0.0f);

    // Layer 1: metallic reflection
    w.y = weightBase * mat.metallic;
    weightBase *= (1.0f - mat.metallic);

    // Layer 2: dielectric specular (Fresnel)
    w.y += weightBase * fr;
    weightBase *= (1.0f - fr);

    // Layer 3: specular transmission
    w.z = weightBase * mat.transmission;

    // Layer 4: remaining split between diffuse refl and diffuse trans
    float remain = weightBase * (1.0f - mat.transmission);
    w.w = remain * mat.diffuseTransmission;
    w.x = remain * (1.0f - mat.diffuseTransmission);

    return w;
}

// Randomly select a lobe based on weights
int findLobe(VPT_Material mat, float VdotN, float rndVal)
{
    float4 w4 = computeLobeWeights(mat, VdotN);
    // 动态下标：标量数组允许在 function 作用域里按变量下标读（SPIR-V 允许，
    // 向量类型的动态分量索引则要靠 select 链，没必要冒这个风险）
    float w[LOBE_COUNT] = { w4.x, w4.y, w4.z, w4.w };
    float weight = 0.0f;
    for (int i = LOBE_COUNT - 1; i > 0; i--)
    {
        weight += w[i];
        if (rndVal < weight) return i;
    }
    return LOBE_DIFFUSE_REFLECTION;
}

// Snell refraction; returns refracted dir, TIR returns float3(0)
float3 refractDir(float3 I, float3 N, float eta, float cosI, out bool tir)
{
    tir = false;
    float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
    if (k < 0.0f) { tir = true; return float3(0.0f); }
    return eta * I + (eta * cosI - sqrt(k)) * N;
}

// ── Volume（有厚度玻璃）──────────────────────────────────────────
// thickness > 0 且置了 MAT_IS_VOLUME：颜色由体积吸收承担，表面只乘透射量/Fresnel。
// 与薄壁材质（MAT_IS_THIN_WALLED）互斥，薄壁保留原 double-tint 语义。
bool materialIsVolume(VPT_Material mat)
{
    return ((mat.flags & MAT_IS_VOLUME) != 0u) && (mat.thickness > 0.0f);
}

// 表面附近的偏移量：体积玻璃用更小的 offset，保证 12mm 内部路径不被吃掉。
float surfaceOffset(VPT_Material mat)
{
    return materialIsVolume(mat) ? EPS_VOLUME : EPS_OFFSET;
}

// 体积吸收系数 σt = -ln(透射色) / 衰减距离（Beer-Lambert）
// tint = 逐像素 baseColor；再乘材质 attenuationColor。attenuationDistance <= 0 表示无吸收。
float3 volumeSigmaT(VPT_Material mat, float3 tint)
{
    if (mat.attenuationDistance <= 0.0f)
        return float3(0.0f);
    float3 a = clamp(tint * mat.attenuationColor.rgb, float3(1e-4f), float3(1.0f));
    return -log(a) / mat.attenuationDistance;
}

// ── BTDF sampling (specular transmission) ──
// Outputs raw BSDF and pdf for the transmitted lobe.
void btdfSample(VPT_Material mat, float3 N, float3 T, float3 B,
                float3 k1, inout uint seed, bool insideVolume,
                out float3 k2, out float pdf, out float3 bsdf)
{
    bsdf = float3(0.0f);
    pdf  = 0.0f;
    k2   = float3(0.0f);

    bool  thinWalled = (mat.thinWalled != 0u);
    bool  isVolume   = materialIsVolume(mat);
    float ior1       = 1.0f;
    float ior2       = mat.ior;
    // 体积边界：从介质内部射出时 IOR 反向（玻璃→空气），否则出射折射方向会失真
    if (isVolume && insideVolume) { ior1 = mat.ior; ior2 = 1.0f; }
    float VdotN   = dot(k1, N);
    bool  entering = (VdotN > 0.0f);
    float eta     = entering ? (ior1 / ior2) : (ior2 / ior1);

    float nk1 = abs(VdotN);

    // VNDF sample half-vector H
    float a  = mat.roughness * mat.roughness;
    float a2 = a * a;
    float r1 = rand(seed), r2 = rand(seed);
    float phi       = 2.0f * PI * r1;
    float cosThetaH = sqrt(max(0.0f, (1.0f - r2) / (1.0f + (a2 - 1.0f) * r2)));
    float sinThetaH = sqrt(max(0.0f, 1.0f - cosThetaH * cosThetaH));
    float3 h0 = normalize(float3(sinThetaH * cos(phi), sinThetaH * sin(phi), cosThetaH));
    float3 H  = h0.x * T + h0.y * B + h0.z * N;

    float k1h = dot(k1, H);
    if (k1h <= 0.0f) return;

    bool tir = false;
    if (thinWalled)
    {
        k2 = reflect(-k1, H);
        k2 = normalize(k2 - 2.0f * N * dot(k2, N));
    }
    else
    {
        float3 refN = entering ? H : -H;
        k2 = refractDir(-k1, refN, eta, k1h, tir);
    }

    if (tir)
        k2 = reflect(-k1, H);

    float nk2 = abs(dot(k2, N));
    if (nk2 <= 1e-6f || isnan(k2.x)) return;

    // ── gnk2 侧向校验（参考工程 bsdf_functions.h.slang L730-752）──
    // 折射必须真正穿过表面到几何法线另一侧；TIR 反射必须留在入射侧。
    // 粗糙度使 H 偏离 N 时，少量采样会落到错误一侧 → 直接丢弃（absorb），
    // 避免光线带着错误的「内外状态」继续传播。
    if (!thinWalled)
    {
        float side = dot(k2, N);
        if (tir ? (side <= 0.0f) : (side >= 0.0f)) return;
    }

    float fresnel   = schlickFresnelScalar(mat.ior, k1h);
    float transProb = 1.0f - fresnel;

    float G = G_SmithGGX(nk1, nk2, a2);
    float D = D_GGX(max(dot(N, H), 0.0f), a2);

    if (!thinWalled && !tir)
    {
        float k2h = abs(dot(k2, H));
        float tmp = k1h * ior1 - k2h * ior2;
        if (abs(tmp) > 1e-6f)
            pdf = D * max(dot(N, H), 0.0f) * G * k1h * k2h / (nk1 * max(dot(N, H), 1e-6f) * tmp * tmp);
    }
    else
    {
        pdf = D * max(dot(N, H), 0.0f) * G * 0.25f / (nk1 * max(dot(N, H), 1e-6f));
    }

    if (pdf <= 1e-6f) return;

    // 体积材质的表面透射 tint 用中性白：颜色只由内部 Beer-Lambert 吸收承担，
    // 否则前后两个表面各乘一次 baseColor → T_total = T_layer²。
    float3 transTint = isVolume ? float3(1.0f) : mat.albedo.rgb;

    if (thinWalled || tir)
        bsdf = transTint * G * pdf;
    else
        bsdf = transTint * G * transProb * pdf;
}

// ── BTDF evaluation (for NEE) ──
float3 btdfEval(VPT_Material mat, float3 N, float3 T, float3 B,
                float3 k1, float3 k2, float NdotV, float NdotL, bool insideVolume)
{
    bool  thinWalled = (mat.thinWalled != 0u);
    bool  isVolume   = materialIsVolume(mat);
    float ior1       = 1.0f, ior2 = mat.ior;
    // 与 btdfSample 一致：体积边界按介质内外方向确定 IOR
    if (isVolume && insideVolume) { ior1 = mat.ior; ior2 = 1.0f; }

    bool backside = (dot(k2, N) < 0.0f);
    if (!backside) return float3(0.0f);

    float3 H = normalize(k1 + k2);
    if (thinWalled) H = normalize(k1 - k2);

    float NdotH = max(dot(N, H), 0.0f);
    float k1h   = max(dot(k1, H), 0.0f);
    float k2h   = abs(dot(k2, H));
    if (NdotH <= 0.0f || k1h <= 0.0f) return float3(0.0f);

    float a  = mat.roughness * mat.roughness;
    float a2 = a * a;
    float D  = D_GGX(NdotH, a2);
    float G  = G_SmithGGX(NdotV, NdotL, a2);
    float fresnel = schlickFresnelScalar(mat.ior, k1h);

    // 体积材质表面中性透射（颜色由内部吸收承担）
    float3 transTint = isVolume ? float3(1.0f) : mat.albedo.rgb;

    if (!thinWalled)
    {
        float tmp = k1h * ior1 - k2h * ior2;
        if (abs(tmp) < 1e-6f) return float3(0.0f);
        float b = (1.0f - fresnel) * G * D * k1h * k2h / (NdotV * NdotH * tmp * tmp);
        return transTint * b;
    }
    else
    {
        float b = (1.0f - fresnel) * G * D * 0.25f / (NdotV * NdotH);
        return transTint * b;
    }
}

// ── Base color sampling (tint * texture, RGBA) ──
// URP Lit final base color is _BaseColor * _BaseMap, including alpha.
// Both the main path and the transparent shadow traversal use this so that
// opacity (transmission) and glass tint stay consistent.
float4 sampleBaseColor(VPT_Material mat, float2 uvT)
{
    float4 c = mat.albedo;
    if ((mat.flags & MAT_HAS_BASE) != 0u && mat.baseTexID != TEX_NONE)
        c *= baseColorTex.SampleLevel(float3(uvT, float(mat.baseTexID)), 0.0f);
    return c;
}

// ── Alpha cutout（镂空）辅助 ──────────────────────────────
// 三角形重心坐标插值 object-space UV（object space，未做材质 uvScaleOffset）。
float2 triangleHitUV(float2 bary, InstanceInfo inst, uint primitiveIndex)
{
    uint i0 = indices[inst.indexOffset + primitiveIndex * 3 + 0];
    uint i1 = indices[inst.indexOffset + primitiveIndex * 3 + 1];
    uint i2 = indices[inst.indexOffset + primitiveIndex * 3 + 2];
    VPT_Vertex v0 = vertices[inst.vertexOffset + i0];
    VPT_Vertex v1 = vertices[inst.vertexOffset + i1];
    VPT_Vertex v2 = vertices[inst.vertexOffset + i2];
    return v0.uv * (1.0f - bary.x - bary.y) + v1.uv * bary.x + v2.uv * bary.y;
}

// 采样命中点的 baseColor alpha（已应用材质的 uvScaleOffset）。
float sampleHitAlpha(float2 bary, InstanceInfo inst, VPT_Material mat, uint primitiveIndex)
{
    float2 uv  = triangleHitUV(bary, inst, primitiveIndex);
    float2 uvT = uv * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;
    return sampleBaseColor(mat, uvT).a;
}

// ══════════════════════════════════════════════════════════════
//  阴影透射（RT pipeline 版：group 1 的 CH/AH 承担）
//
//  与 GLSL 版「candidate 循环」的语义对照：
//    · 不透明实例带实例级 FORCE_OPAQUE → 硬件直接 commit，不触发 AH，
//      CH 把 totalTransmission 置 0（= 任一遮挡即停）；
//    · 半透明/镂空实例触发 AH：玻璃按透射率累积，镂空 alpha < cutoff 的
//      微面 IgnoreHit() 直接跳过（洞），实心部分完全遮挡。
//    · 层数上限 / 透射率耗尽 → AcceptHitAndEndSearch()（区别见 acceptMode）。
//
//  startInsideVolume / startSigmaT：阴影射线起点所在的介质状态（主路径位于玻璃
//  内部时为 true），让「表面 → 光源」整段用与主路径相同的 σt 做 Beer-Lambert 吸收。
// ══════════════════════════════════════════════════════════════
float3 traceShadowTransmission(float3 origin, float3 dir, float maxT,
                               bool isInsideVolume, float3 sigmaT)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = maxT;

    ShadowPayload sp;
    sp.prevHitT          = 0.0f;
    sp.acceptMode        = 0u;
    sp.isInside          = isInsideVolume ? 1u : 0u;
    sp.layerCount        = 0u;
    sp.extinction        = sigmaT;
    sp.totalTransmission = float3(1.0f);

    // sbtOffset = 1 → hit group 1（rchitShadow/rahitShadow）；missIndex = 1 → rmissShadow
    TraceRay(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 1, 0, 1, ray, sp);

    return sp.totalTransmission;
}

// ══════════════════════════════════════════════════════════════
//  Light sampling
// ══════════════════════════════════════════════════════════════

void sampleLightContribution(VPT_Light L, float3 P,
                             out float3 toL, out float dist, out float3 Lcolor)
{
    if (L.type == 1u) // directional
    {
        toL    = L.positionOrDir;
        dist   = 1e34f;
        Lcolor = L.color;
    }
    else // point
    {
        float3 d     = L.positionOrDir - P;
        float  dist2 = dot(d, d);
        dist         = sqrt(dist2);
        toL          = d / max(dist, 1e-6f);
        float atten  = 1.0f;
        if (L.range < 1e33f)
        {
            float rr = dist / L.range;
            atten = max(0.0f, 1.0f - rr * rr);
        }
        Lcolor = L.color * atten / max(dist2, 1e-6f);
    }
}

// ══════════════════════════════════════════════════════════════
//  Hit point attribute reconstruction from SSBOs
// ══════════════════════════════════════════════════════════════

struct HitInfo
{
    float3 worldPos;
    float3 normal;      // interpolated world normal
    float3 geoNormal;   // geometry face normal (world)
    float2 uv;
    float4 tangent;     // world-space tangent
    uint   materialID;
    float  hitT;
    bool   hasHit;
};

// raygen 侧用 payload 的 {instanceIndex, primitiveIndex, bary, hitT} 回读 SSBO 重建。
// （closest-hit 只做命中记录，不做着色 —— 与参考工程一致）
HitInfo reconstructHitInfo(HitPayload payload)
{
    HitInfo info;
    info.hasHit = false;

    uint instanceIndex  = payload.instanceIndex;
    uint primitiveIndex = payload.primitiveIndex;
    float2 bary         = payload.bary;

    InstanceInfo inst = instances[instanceIndex];
    info.materialID = inst.materialID;
    info.hitT       = payload.hitT;

    uint i0 = indices[inst.indexOffset + primitiveIndex * 3 + 0];
    uint i1 = indices[inst.indexOffset + primitiveIndex * 3 + 1];
    uint i2 = indices[inst.indexOffset + primitiveIndex * 3 + 2];

    VPT_Vertex v0 = vertices[inst.vertexOffset + i0];
    VPT_Vertex v1 = vertices[inst.vertexOffset + i1];
    VPT_Vertex v2 = vertices[inst.vertexOffset + i2];

    float w0 = 1.0f - bary.x - bary.y;
    float w1 = bary.x;
    float w2 = bary.y;

    // Interpolate object-space attributes
    float3 objPos    = v0.pos * w0 + v1.pos * w1 + v2.pos * w2;
    float3 objNormal = v0.normal * w0 + v1.normal * w1 + v2.normal * w2;
    float2 uv        = v0.uv * w0 + v1.uv * w1 + v2.uv * w2;
    float4 objTang   = v0.tangent * w0 + v1.tangent * w1 + v2.tangent * w2;

    // Transform to world space
    info.worldPos  = float3(
        dot(inst.transformRow0.xyz, objPos) + inst.transformRow0.w,
        dot(inst.transformRow1.xyz, objPos) + inst.transformRow1.w,
        dot(inst.transformRow2.xyz, objPos) + inst.transformRow2.w);
    info.normal    = normalize(transformDir(inst, objNormal));
    info.geoNormal = normalize(transformDir(inst,
        normalize(cross(v1.pos - v0.pos, v2.pos - v0.pos))));
    info.uv        = uv;
    info.tangent   = float4(normalize(transformDir(inst, objTang.xyz)), objTang.w);
    info.hasHit    = true;

    return info;
}

// ══════════════════════════════════════════════════════════════
//  DLSS RR specular albedo：预积分镜面反射率近似
//  （Ray Tracing Gems Ch.32 / UE4 Karis 移动近似，a = roughness^2）
// ══════════════════════════════════════════════════════════════

float3 EnvBRDFApprox(float3 F0, float a, float NoV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r     = a * c0 + c1;
    float  a004  = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    float2 AB    = float2(-1.04f, 1.04f) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// ══════════════════════════════════════════════════════════════
//  HDR 环境光 importance 采样（移植自 nvpro hdr_env_sampling.h.slang）
//  alias method：rand3 → 选纹素/alias → 球面方向 + float4(radiance, pdf)
// ══════════════════════════════════════════════════════════════

float4 environmentSample(Sampler2D<float4> hdrTex, float3 randVal, out float3 toLight)
{
    float xi_x = randVal.x;
    float xi_y = randVal.y;
    float xi_z = randVal.z;

    uint tw, th;
    hdrTex.GetDimensions(tw, th);
    uint width  = tw;
    uint height = th;
    uint size   = width * height;

    // Uniformly pick a texel index
    uint idx = min(uint(xi_x * float(size)), size - 1u);

    // Fetch alias table data for that texel
    EnvAccel sampleData = envAccel[idx];
    uint     envIdx;

    if (xi_y < sampleData.q)
    {
        envIdx = idx;
        xi_y /= sampleData.q;
    }
    else
    {
        envIdx = sampleData.alias;
        xi_y = (xi_y - sampleData.q) / (1.0f - sampleData.q);
    }

    // 2D integer coordinates of the texel
    uint px = envIdx % width;
    uint py = envIdx / width;

    // Uniformly sample the solid angle subtended by the texel
    float u      = float(px + xi_y) / float(width);
    float phi    = u * 2.0f * PI - PI;
    float sinPhi = sin(phi);
    float cosPhi = cos(phi);

    float stepTheta = PI / float(height);
    float theta0    = float(py) * stepTheta;
    float cosTheta  = cos(theta0) * (1.0f - xi_z) + cos(theta0 + stepTheta) * xi_z;
    float theta     = acos(clamp(cosTheta, -1.0f, 1.0f));
    float sinTheta  = sin(theta);
    float v         = theta / PI;

    // Direction in Cartesian coordinates
    toLight = float3(cosPhi * sinTheta, cosTheta, sinPhi * sinTheta);

    // Apply envRotation (rotate around Y axis)
    if (abs(cam.envRotation) > 1e-6f)
    {
        float cr = cos(cam.envRotation);
        float sr = sin(cam.envRotation);
        toLight = float3(cr * toLight.x + sr * toLight.z, toLight.y,
                         -sr * toLight.x + cr * toLight.z);
    }

    // Lookup the environment value (returns rgb=pixel color, a=pdf)
    float4 texel    = hdrTex.SampleLevel(float2(u, v), 0.0f);
    float3 radiance = texel.rgb * cam.envIntensity;
    float  pdf      = texel.a;  // PDF stored in alpha channel by alias builder

    return float4(radiance, pdf);
}

// ══════════════════════════════════════════════════════════════
//  方向 → equirect UV（用于 miss 分支 HDR 查找）
// ══════════════════════════════════════════════════════════════
float2 sphericalUV(float3 dir)
{
    float phi = atan2(dir.z, dir.x);
    // Apply envRotation
    phi += cam.envRotation;
    float u = (phi + PI) / (2.0f * PI);
    float v = acos(clamp(dir.y, -1.0f, 1.0f)) / PI;
    return float2(u, v);
}
