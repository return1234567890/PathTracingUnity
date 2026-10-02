# 半透明物体 BSDF 实现方案

> 参考实现：`D:\personalProject\MyEngineVulkanV2` 的 `gltf_pathtrace.slang` + `bsdf_functions.h.slang`
> 目标工程：`d:\unityHub\project\URPPT`（Unity URP 软件光追路径追踪）
> any-hit 模拟策略：路径 B（迭代式 closest-hit，不改现有遍历代码）

---

## 一、参考实现核心机制分析

Vulkan 参考实现的半透明能力来自四个层面：

### 1. Lobe 权重分层模型

`computeLobeWeights`（bsdf_functions.h.slang L167-L248）按 Clearcoat → Sheen → Metal → Specular → Transmission → Diffuse 的层叠顺序，用 Fresnel 衰减逐层分配权重，剩余能量在 Specular Transmission 和 Diffuse 间分配。

### 2. GGX-Smith BTDF

`btdf_ggx_smith_sample`（bsdf_functions.h.slang L686-L788）：VNDF 采样半向量 → Snell 定律折射 → TIR 检测 → thin-walled 伪 BTDF（反射方向翻转到背面）。

### 3. 体积衰减

`pathTrace`（gltf_pathtrace.slang L612-L616）：`isInside` 状态追踪 + Beer 定律 `throughput *= exp(-hitT * absorption_coeff)`。

### 4. 阴影半透明

`getShadowTransmission`（gltf_pathtrace.slang L283-L372）：阴影射线 any-hit 累积透射率，含 Fresnel + 颜色染色 + Beer 衰减 + 粗糙度/金属度衰减。

---

## 二、当前工程差距分析

当前项目 `PathTrace.compute` 的 BSDF（`evaluateBSDF` / `sampleBSDF`）仅含 diffuse + specular 两个反射 lobe，**完全没有透射能力**：

- `PathMaterial` 结构体无 `transmission`、`ior`、`thinWalled`、`attenuation` 等字段
- 路径追踪循环无 `isInside` 状态、无体积衰减
- 阴影射线 `OccludedTLAS` 是二值遮挡，不支持彩色透射阴影
- BLAS 遍历是硬遮挡（any-hit 即返回），不支持 alpha 混合

---

## 三、any-hit 模拟策略：路径 B（迭代式 closest-hit）

### 问题本质

硬件光追的 any-hit 是**遍历器层面的能力**：GPU 驱动的 BVH 遍历器在每次命中三角形时回调 shader，shader 可以决定 `IgnoreHit()`（跳过）或 `AcceptHitAndEndSearch()`（终止）。这是**遍历中途的回调**。

软件光追没有这个机制。`CastRayTLAS` / `OccludedTLAS` 是自写的遍历循环，要模拟 any-hit 有两条路：

- **路径 A（内联 any-hit）**：修改遍历循环，在叶子节点三角形遍历中内联透射判断。需重写 `OccludedTLAS` 为返回 `float3` 的穿透式遍历，改动大。
- **路径 B（迭代式 closest-hit）**：不改现有遍历代码，在外层循环中重复调用 `CastRayTLAS` 累积透射率。

### 本方案采用路径 B

**算法流程**：

```
totalTransmission = (1,1,1)   // 初始完全透过
剩余距离 = 光源距离

循环（最多 16 次）:
    1. 投射射线，找最近命中（CastRayTLAS）
    2. 如果 miss → 光线到达光源，返回 totalTransmission
    3. 如果命中不透明面 → 完全遮挡，返回 (0,0,0)
    4. 如果命中半透明面 → 计算该面的透射率 T
       totalTransmission *= T
       如果 totalTransmission 太暗 → 提前返回 (0,0,0)
    5. 从命中点继续向前投射（偏移到表面另一侧）
    6. 剩余距离 -= hit.t
```

**优点**：完全复用现有 `CastRayTLAS`，不改遍历逻辑，零风险。
**缺点**：每次穿透都要重新从 TLAS 根节点遍历，有重复遍历开销。但多数场景半透明层数少（1-3 层），开销可接受。

---

## 四、实现方案（分 5 个模块）

### 模块 1：PathMaterial 结构体扩展

当前 80B → 扩展到 96B，新增 16B 透射参数块。

#### HLSL 侧（PathTrace.compute）

```hlsl
// PathTrace.compute — PathMaterial 扩展（80B → 96B）
struct PathMaterial
{
    // ── 原有 80B 不变 ──
    float4 albedo;          // 16
    float  metallic;        // 4
    float  roughness;       // 4
    float  bumpScale;       // 4
    uint   flags;           // 4
    float4 emissionColor;   // 16
    uint   baseTexID;       // 4
    uint   metalRoughTexID; // 4
    uint   normalTexID;     // 4
    uint   emissiveTexID;   // 4
    float4 uvScaleOffset;   // 16
    // ── 新增 16B 透射参数 ──
    float  transmission;           // 4  镜面透射因子 [0,1]，0=不透明
    float  ior;                    // 4  折射率（默认1.5玻璃，1.0空气）
    float  diffuseTransmission;    // 4  漫透射因子 [0,1]（纸张/布料等）
    uint   thinWalled;             // 4  1=薄壁（无体积衰减），0=实体
};

#define MAT_IS_TRANSPARENT 16u  // flags bit4: 材质启用透射
```

#### C# 侧（ModelManager.cs）

```csharp
// ModelManager.cs — PathMaterial 扩展（80B → 96B）
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct PathMaterial
{
    // ── 原有 80B 不变 ──
    public Vector4 albedo;
    public float   metallic;
    public float   roughness;
    public float   bumpScale;
    public uint    flags;
    public Vector4 emissionColor;
    public uint    baseTexID;
    public uint    metalRoughTexID;
    public uint    normalTexID;
    public uint    emissiveTexID;
    public Vector4 uvScaleOffset;
    // ── 新增 16B ──
    public float   transmission;        // 镜面透射因子
    public float   ior;                 // 折射率
    public float   diffuseTransmission; // 漫透射因子
    public uint    thinWalled;          // 薄壁标志
}
// StrideMaterial: 80 → 96
```

#### 材质属性提取（GetMaterial 方法中新增）

```csharp
// ModelManager.cs — GetMaterial() 新增透射参数提取
// 在 return pm; 之前添加：

pm.transmission = 0f;
pm.ior = 1.5f;
pm.diffuseTransmission = 0f;
pm.thinWalled = 0u;

// 方式1：直接读取自定义属性（推荐，材质面板手动设置）
if (mat.HasProperty("_Transmission"))
    pm.transmission = Mathf.Clamp01(mat.GetFloat("_Transmission"));
if (mat.HasProperty("_IOR"))
    pm.ior = mat.GetFloat("_IOR");
if (mat.HasProperty("_DiffuseTransmission"))
    pm.diffuseTransmission = Mathf.Clamp01(mat.GetFloat("_DiffuseTransmission"));

// 方式2：从 URP _Surface 类型推断（Transparent=1 时自动启用透射）
if (mat.HasProperty("_Surface") && mat.GetFloat("_Surface") > 0.5f)
{
    if (pm.transmission <= 0f) pm.transmission = 0.5f;
    pm.thinWalled = 1u;
}

if (pm.transmission > 0f || pm.diffuseTransmission > 0f)
    flags |= 16u; // MAT_IS_TRANSPARENT

pm.flags = flags;
```

#### RenderBufferManager.cs 同步更新

```csharp
// RenderBufferManager.cs
public const int StrideMaterial = 96;  // 80 → 96
```

---

### 模块 2：BSDF Lobe 权重计算

参考 `computeLobeWeights` 的分层 Fresnel 模型，简化为 4 个 lobe（去掉 Clearcoat/Sheen/Iridescence）。

```hlsl
// PathTrace.compute — 新增 lobe 定义与权重计算

#define LOBE_DIFFUSE_REFLECTION    0
#define LOBE_SPECULAR_REFLECTION   1
#define LOBE_SPECULAR_TRANSMISSION 2
#define LOBE_DIFFUSE_TRANSMISSION  3
#define LOBE_COUNT                 4

// Schlick Fresnel（标量版，用于 lobe 权重分配）
float schlickFresnelScalar(float ior, float cosTheta)
{
    float r0 = (1.0 - ior) / (1.0 + ior);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - max(cosTheta, 0.0), 5.0);
}

// 计算 4-lobe 权重（参考 computeLobeWeights 简化版）
// 返回 float4: x=diffuse_refl, y=spec_refl, z=spec_trans, w=diff_trans
float4 computeLobeWeights(PathMaterial mat, float VdotN)
{
    float fr = schlickFresnelScalar(mat.ior, abs(VdotN));

    float weightBase = 1.0;
    float4 w = float4(0,0,0,0);

    // Layer 1: 金属反射
    w.y = weightBase * mat.metallic;
    weightBase *= (1.0 - mat.metallic);

    // Layer 2: 介电镜面反射（Fresnel）
    w.y += weightBase * fr;
    weightBase *= (1.0 - fr);

    // Layer 3: 镜面透射
    w.z = weightBase * mat.transmission;

    // Layer 4: 剩余在 diffuse refl 与 diffuse trans 间分配
    float remain = weightBase * (1.0 - mat.transmission);
    w.w = remain * mat.diffuseTransmission;
    w.x = remain * (1.0 - mat.diffuseTransmission);

    return w;
}

// 随机选择 lobe（参考 findLobe）
int findLobe(PathMaterial mat, float VdotN, float rndVal)
{
    float4 w = computeLobeWeights(mat, VdotN);
    float weight = 0.0;
    [unroll]
    for (int i = LOBE_COUNT - 1; i > 0; i--)
    {
        weight += w[i];
        if (rndVal < weight) return i;
    }
    return LOBE_DIFFUSE_REFLECTION;
}
```

---

### 模块 3：BTDF 实现（镜面透射）

参考 `btdf_ggx_smith_sample`，实现 GGX 微表面折射。

```hlsl
// PathTrace.compute — 新增 BTDF 采样与评估

// Snell 折射：返回折射方向，TIR 时返回 float3(0,0,0)
float3 refractDir(float3 I, float3 N, float eta, float cosI, out bool tir)
{
    tir = false;
    float k = 1.0 - eta * eta * (1.0 - cosI * cosI);
    if (k < 0.0) { tir = true; return float3(0,0,0); }
    return eta * I + (eta * cosI - sqrt(k)) * N;
}

// ── BTDF 采样（镜面透射）──
void btdfSample(PathMaterial mat, float3 N, float3 T, float3 B,
                float3 k1, inout uint seed,
                out float3 k2, out float pdf, out float3 bsdf_over_pdf)
{
    bsdf_over_pdf = float3(0,0,0);
    pdf = 0.0;
    k2 = float3(0,0,0);

    bool thinWalled = (mat.thinWalled != 0u);
    float ior1 = 1.0;
    float ior2 = mat.ior;
    float VdotN = dot(k1, N);
    bool entering = (VdotN > 0.0);
    float eta = entering ? (ior1 / ior2) : (ior2 / ior1);

    float nk1 = abs(VdotN);
    float3 k10 = float3(dot(k1, T), dot(k1, B), nk1);

    // VNDF 采样半向量 H
    float a = mat.roughness * mat.roughness;
    float a2 = a * a;
    float r1 = rand(seed), r2 = rand(seed);
    float phi = 2.0 * PI * r1;
    float cosThetaH = sqrt(max(0.0, (1.0 - r2) / (1.0 + (a2 - 1.0) * r2)));
    float sinThetaH = sqrt(max(0.0, 1.0 - cosThetaH * cosThetaH));
    float3 h0 = normalize(float3(sinThetaH * cos(phi), sinThetaH * sin(phi), cosThetaH));
    float3 H = h0.x * T + h0.y * B + h0.z * N;

    float k1h = dot(k1, H);
    if (k1h <= 0.0) return;

    bool tir = false;
    if (thinWalled)
    {
        // 薄壁伪 BTDF：反射后翻转到背面
        k2 = reflect(-k1, H);
        k2 = normalize(k2 - 2.0 * N * dot(k2, N));
    }
    else
    {
        // 实体折射
        float3 refN = entering ? H : -H;
        k2 = refractDir(-k1, refN, eta, k1h, tir);
    }

    if (tir)
    {
        k2 = reflect(-k1, H);  // TIR → 作为镜面反射处理
    }

    float nk2 = abs(dot(k2, N));
    if (nk2 <= 1e-6 || isnan(k2.x)) return;

    float fresnel = schlickFresnelScalar(mat.ior, k1h);
    float transProb = 1.0 - fresnel;

    float G = G_SmithGGX(nk1, nk2, a2);
    float D = D_GGX(max(dot(N, H), 0.0), a2);

    if (!thinWalled && !tir)
    {
        // 折射 PDF
        float k2h = abs(dot(k2, H));
        float tmp = k1h * ior1 - k2h * ior2;
        if (abs(tmp) > 1e-6)
            pdf = D * max(dot(N,H),0.0) * G * k1h * k2h / (nk1 * max(dot(N,H),1e-6) * tmp * tmp);
    }
    else
    {
        // 反射 PDF（薄壁或 TIR）
        pdf = D * max(dot(N,H),0.0) * G * 0.25 / (nk1 * max(dot(N,H),1e-6));
    }

    if (pdf <= 1e-6) return;

    bsdf_over_pdf = mat.albedo.rgb * G * transProb;
    if (thinWalled || tir)
        bsdf_over_pdf = mat.albedo.rgb * G;
}

// ── BTDF 评估（用于 NEE）──
float3 btdfEval(PathMaterial mat, float3 N, float3 T, float3 B,
                float3 k1, float3 k2, float NdotV, float NdotL)
{
    bool thinWalled = (mat.thinWalled != 0u);
    float ior1 = 1.0, ior2 = mat.ior;

    bool backside = (dot(k2, N) < 0.0);
    if (!backside) return float3(0,0,0);

    float3 H = normalize(k1 + k2);
    if (thinWalled) H = normalize(k1 - k2);

    float NdotH = max(dot(N, H), 0.0);
    float k1h = max(dot(k1, H), 0.0);
    float k2h = abs(dot(k2, H));
    if (NdotH <= 0.0 || k1h <= 0.0) return float3(0,0,0);

    float a = mat.roughness * mat.roughness;
    float a2 = a * a;
    float D = D_GGX(NdotH, a2);
    float G = G_SmithGGX(NdotV, NdotL, a2);
    float fresnel = schlickFresnelScalar(mat.ior, k1h);

    if (!thinWalled)
    {
        float tmp = k1h * ior1 - k2h * ior2;
        if (abs(tmp) < 1e-6) return float3(0,0,0);
        float bsdf = (1.0 - fresnel) * G * D * k1h * k2h / (NdotV * NdotH * tmp * tmp);
        return mat.albedo.rgb * bsdf;
    }
    else
    {
        float bsdf = (1.0 - fresnel) * G * D * 0.25 / (NdotV * NdotH);
        return mat.albedo.rgb * bsdf;
    }
}
```

---

### 模块 4：路径追踪循环改造

参考 `pathTrace` 中的 `isInside` 状态追踪和体积衰减。

```hlsl
// PathTrace.compute — PathTrace kernel 循环改造（关键差异标注 ★）

[numthreads(8, 8, 1)]
void PathTrace(uint3 id : SV_DispatchThreadID)
{
    // ... 前置光线生成不变 ...

    float3 radiance = float3(0,0,0);
    float3 throughput = float3(1,1,1);
    bool  isInside = false;  // ★ 新增：内部状态追踪

    [loop]
    for (int depth = 0; depth < (int)MaxDepth; depth++)
    {
        // ... CastRayTLAS 不变 ...

        if (hit.instIndex == MISS_INDEX) {
            // ... miss 处理不变 ...
            break;
        }

        float3 N = getHitWorldNormal(hit);
        float3 Ngeo = getHitGeoNormal(hit);
        float3 P = ray.origin + ray.direction * hit.t;
        PathMaterial mat = getHitMaterial(hit);
        // ... 纹理采样不变 ...

        bool frontFace = dot(ray.direction, N) < 0.0;

        // ★ 体积衰减（Beer 定律）—— 在翻转法线之前应用
        if (isInside && mat.thinWalled == 0u && mat.transmission > 0.0)
        {
            float3 absorbColor = max(mat.albedo.rgb, float3(0.001,0.001,0.001));
            float3 absCoeff = -log(absorbColor) / max(mat.ior * 2.0, 0.1);
            throughput *= exp(-hit.t * absCoeff);
        }

        // 面朝向修正
        if (frontFace) {
            N = -N; Ngeo = -Ngeo;
        }

        // ... 法线贴图、纹理采样不变 ...

        float3 V = -ray.direction;
        bool isTransparent = ((mat.flags & MAT_IS_TRANSPARENT) != 0u);

        // ── NEE 直接光照 ──
        [loop]
        for (uint li = 0; li < LightCount; li++)
        {
            // ... 光源采样不变 ...

            float NdotL = dot(N, toL);
            float NdotL_smooth = dot(N_flat, toL);

            // ★ 透射材质允许背面光照
            bool lightValid = false;
            float3 bsdf = float3(0,0,0);

            if (isTransparent && NdotL <= 0.0)
            {
                float absNdotL = abs(NdotL);
                bsdf = btdfEval(mat, N, T, B, V, toL, NdotV, absNdotL);
                lightValid = (dot(N_flat, toL) < 0.0 || NdotL_smooth > 0.0);
                NdotL = absNdotL;
            }
            else if (NdotL > 0.0)
            {
                bsdf = evaluateBSDF(mat, N, toL, V, NdotL, NdotV);
                lightValid = (NdotL_smooth > 0.0);
            }

            if (!lightValid) continue;

            Ray shadowRay;
            shadowRay.origin = P + Ngeo * EPS_OFFSET;
            shadowRay.direction = toL;

            // ★ 阴影：透射材质用半透明阴影，否则二值遮挡
            if (isTransparent) {
                float3 shadowTrans = traceShadowTransmission(shadowRay, dist);
                radiance += throughput * bsdf * NdotL * Lcolor * shadowTrans;
            } else {
                if (OccludedTLAS(shadowRay, dist)) continue;
                radiance += throughput * bsdf * NdotL * Lcolor;
            }
        }

        // ── BSDF 采样（间接弹射）──
        float3 newDir; float pdf; float3 bsdf;

        if (isTransparent)
        {
            int lobe = findLobe(mat, dot(N, V), rand(seed));
            if (lobe == LOBE_SPECULAR_TRANSMISSION || lobe == LOBE_DIFFUSE_TRANSMISSION)
            {
                if (lobe == LOBE_SPECULAR_TRANSMISSION)
                    btdfSample(mat, N, T, B, V, seed, newDir, pdf, bsdf);
                else
                {
                    // 漫透射：余弦半球采样到背面
                    newDir = cosineSampleHemisphere(float2(rand(seed),rand(seed)), T, B, -N);
                    pdf = abs(dot(newDir, N)) / PI;
                    bsdf = mat.albedo.rgb * mat.diffuseTransmission;
                }
                // ★ 切换 isInside 状态
                if (mat.thinWalled == 0u)
                    isInside = !isInside;
            }
            else
            {
                bsdf = sampleBSDF(mat, N, V, seed, newDir, pdf);
            }
        }
        else
        {
            bsdf = sampleBSDF(mat, N, V, seed, newDir, pdf);
        }

        float NdotL_new = max(dot(N, newDir), 0.0);
        if (isTransparent) NdotL_new = abs(dot(N, newDir));
        if (pdf <= 0.0 || NdotL_new <= 0.0) break;
        throughput *= bsdf * NdotL_new / pdf;

        // ★ 射线偏移：透射时沿光线方向偏移，反射时沿法线偏移
        bool isTransmission = (dot(newDir, Ngeo) < 0.0);
        if (isTransmission)
            ray.origin = P - Ngeo * EPS_OFFSET;
        else
            ray.origin = P + Ngeo * EPS_OFFSET;
        ray.direction = newDir;

        // ... 俄罗斯轮盘不变 ...
    }
}
```

---

### 模块 5：半透明阴影（路径 B 实现）

参考 `getShadowTransmission`，实现迭代式累积透射。

```hlsl
// PathTrace.compute — 新增半透明阴影遍历（路径 B：迭代式 closest-hit）

#define MIN_TRANSMISSION 0.01
#define MAX_SHADOW_LAYERS 16

// 透射式阴影遍历：穿过所有半透明表面，累积透射率
// 返回 rgb 透射率: (1,1,1)=完全透过, (0,0,0)=完全遮挡
float3 traceShadowTransmission(Ray ray, float maxT)
{
    float3 totalTransmission = float3(1.0, 1.0, 1.0);
    float3 origin = ray.origin;
    float3 dir = ray.direction;
    bool isInside = false;

    [loop]
    for (int iter = 0; iter < MAX_SHADOW_LAYERS; iter++)
    {
        // ── 1. 投射最近命中射线 ──
        RayHit hit;
        hit.t = maxT;
        hit.u = 0.0; hit.v = 0.0;
        hit._pad0 = 0;
        hit.triIndex = MISS_INDEX;
        hit.instIndex = MISS_INDEX;
        hit._pad1 = 0; hit._pad2 = 0;

        Ray currentRay;
        currentRay.origin = origin;
        currentRay.direction = dir;
        currentRay._pad0 = 0.0;
        currentRay._pad1 = 0.0;

        CastRayTLAS(currentRay, hit);

        // ── 2. miss：光线到达光源，返回累积透射率 ──
        if (hit.instIndex == MISS_INDEX)
        {
            return totalTransmission;
        }

        // ── 3. 获取命中点材质与几何 ──
        float3 Ngeo = getHitGeoNormal(hit);
        PathMaterial mat = getHitMaterial(hit);
        float3 P = origin + dir * hit.t;

        // ── 4. 不透明材质：完全遮挡，终止 ──
        if ((mat.flags & MAT_IS_TRANSPARENT) == 0u || mat.transmission <= MIN_TRANSMISSION)
        {
            return float3(0.0, 0.0, 0.0);
        }

        // ── 5. 计算本层透射率（4 因素）──

        // 因素 A: 基础透射因子
        float3 layerT = mat.transmission;

        // 因素 B: Fresnel 反射损失
        float cosTheta = abs(dot(dir, Ngeo));
        float fresnel = schlickFresnelScalar(mat.ior, cosTheta);
        layerT *= (1.0 - fresnel);

        // 因素 C: 颜色染色
        layerT *= mat.albedo.rgb;

        // 因素 D: 体积吸收（Beer 定律）—— 仅在物体内部传播时
        if (mat.thinWalled == 0u && isInside)
        {
            float3 absorbColor = max(mat.albedo.rgb, float3(0.001, 0.001, 0.001));
            float3 absCoeff = -log(absorbColor) / max(mat.ior * 2.0, 0.1);
            layerT *= exp(-hit.t * absCoeff);
            isInside = !isInside;
        }
        else if (mat.thinWalled == 0u)
        {
            isInside = !isInside;
        }

        // 因素 E: 粗糙度/金属度衰减
        layerT *= (1.0 - mat.metallic);
        float roughnessEffect = 1.0 - (mat.roughness * mat.roughness);
        layerT *= lerp(0.65, 1.0, roughnessEffect);

        // ── 6. 累积透射率 ──
        totalTransmission *= layerT;

        // ── 7. 透射率过低 → 提前终止 ──
        float maxComp = max(max(totalTransmission.r, totalTransmission.g), totalTransmission.b);
        if (maxComp <= MIN_TRANSMISSION)
        {
            return float3(0.0, 0.0, 0.0);
        }

        // ── 8. 从命中点继续向前投射 ──
        float facing = dot(dir, Ngeo);
        origin = P + Ngeo * sign(facing) * EPS_OFFSET;
        maxT -= hit.t;

        if (maxT <= EPS_OFFSET) break;
    }

    return totalTransmission;
}
```

---

## 五、阴影半透明物理模型详解

### 每层透射率的 5 个衰减因素

#### 因素 A：材质透射因子

```
T_base = mat.transmission  // [0,1]，0=完全不透，1=完全透过
```

`transmission=0` 的材质等价于不透明，直接返回 `(0,0,0)`。

#### 因素 B：Fresnel 反射损失

```
cosTheta = |dot(rayDir, Ngeo)|  // 光线与面法线的夹角余弦
F = schlickFresnel(ior, cosTheta)  // Schlick 近似 Fresnel 反射率
T *= (1 - F)
```

物理意义：掠射角（光线几乎平行于表面）时 Fresnel 反射率趋近 1，几乎不透过。

```
正入射（cosTheta≈1）：F≈0.04（玻璃），96% 透过
掠射角（cosTheta≈0）：F≈1.0，几乎 0 透过
```

#### 因素 C：颜色染色

```
T *= mat.albedo.rgb  // base color 作为透射颜色
```

红色玻璃 `albedo=(1, 0.1, 0.1)` → 透射光被染红。穿过越多层，颜色越浓（乘法叠加）。

#### 因素 D：体积吸收（Beer 定律）

```
Beer 定律: I(d) = I₀ * exp(-σ * d)
  σ = 吸收系数（per channel）
  d = 在介质内传播的距离
```

```
// 吸收系数 σ = -ln(attenuationColor) / attenuationDistance
float3 absorbance = -log(max(mat.attenuationColor, 0.001)) / max(mat.attenuationDistance, 0.001);
// 衰减 = exp(-距离 × 吸收系数)
attenuation.r = exp(-hitT * absorbance.r);
attenuation.g = exp(-hitT * absorbance.g);
attenuation.b = exp(-hitT * absorbance.b);
```

`isInside` 状态追踪：只有光线在物体内部传播时才应用 Beer 定律。

```
空气 ──命中面1──→ 玻璃内部（isInside=true，Beer衰减）──命中面2──→ 空气（isInside=false）
```

#### 因素 E：粗糙度/金属度衰减

```
金属度衰减: T *= (1 - metallic)      // 金属完全不透光
粗糙度衰减: T *= lerp(0.65, 1.0, 1-roughness²)  // 粗糙表面散射光
```

### isInside 状态追踪图示

以从地面 P 指向光源的阴影射线穿过红色玻璃球为例：

```
 光源 ☀
  ↑
  │  (miss → 到达光源, totalTrans = 最终累积值)
  │
  ├── 面B: 玻璃→空气 (isInside: true→false)
  │     Beer衰减: exp(-d_内部 × σ_红)  ← 玻璃内传播距离 d
  │     Fresnel: (1-F_B)
  │     染色: ×(1, 0.1, 0.1)
  │
  ╔═══════════╗  (玻璃内部, isInside=true)
  ║  红色玻璃  ║
  ╚═══════════╝
  │
  ├── 面A: 空气→玻璃 (isInside: false→true)
  │     Fresnel: (1-F_A)
  │     染色: ×(1, 0.1, 0.1)
  │
  │
  P (地面) ─────────────────────→ 阴影射线方向
```

最终地面接收的光 = 原始光强 × `(1-F_A) × (1,0.1,0.1) × exp(-d×σ) × (1-F_B) × (1,0.1,0.1)`

---

## 六、性能考量

| 场景 | OccludedTLAS（现有） | traceShadowTransmission |
|------|---------------------|------------------------|
| 全不透明场景 | 1 次遍历 | 1 次遍历（第一次命中即返回0） |
| 穿过 1 层玻璃 | 1 次遍历（错误遮挡） | 2 次遍历 + 透射计算 |
| 穿过 3 层玻璃 | 1 次 | 4 次遍历 |
| 穿过 16+ 层 | 1 次 | 16 次遍历（上限截断） |

**优化策略**：
1. 仅对半透明命中点才进入 `traceShadowTransmission`——对不透明材质仍用 `OccludedTLAS`
2. `MAX_SHADOW_LAYERS=16` 可按需调低（多数场景 4-6 层足够）
3. 透射率低于 `MIN_TRANSMISSION=0.01` 时提前终止
4. 薄壁材质（`thinWalled=1`）跳过 Beer 计算

---

## 七、与参考实现的关键差异

| 方面 | Vulkan 参考 | 本方案 | 理由 |
|------|------------|--------|------|
| Lobe 数量 | 7（含 Clearcoat/Sheen/Iridescence） | 4（Diffuse/SpecRefl/SpecTrans/DiffTrans） | URP Lit 无 Clearcoat/Sheen，简化降低计算量 |
| 半向量采样 | VNDF（可见法线分布） | 等面积 NDF 采样 | 当前项目已有此实现，VNDF 可后续升级 |
| 体积吸收 | `attenuationColor` + `attenuationDistance` 独立字段 | 复用 `albedo.rgb` + `ior` 推导 | 节省结构体空间，后续可独立扩展 |
| 阴影半透明 | 硬件 any-hit + `IgnoreHit()` | 软件循环 `CastRayTLAS` 最多 16 层（路径 B） | 软件光追无 any-hit，用循环替代 |
| 色散 | `KHR_materials_dispersion` 波长采样 | 不支持 | 过于复杂，非核心需求 |
| 折射方向 | `refract()` 内置 | 自实现 `refractDir()` | HLSL compute shader 无内置 refract |

---

## 八、实施顺序与依赖关系

```
模块1 (PathMaterial 扩展)
  ├── C# ModelManager.GetMaterial 提取透射属性
  ├── C# RenderBufferManager StrideMaterial 96B
  └── HLSL PathMaterial 结构体 + MAT_IS_TRANSPARENT flag
        │
        ▼
模块2 (Lobe 权重计算) ← 独立函数，无前置依赖
        │
        ▼
模块3 (BTDF 采样/评估) ← 依赖模块2 的 lobe 选择
        │
        ▼
模块4 (路径追踪循环) ← 依赖模块1+2+3
        │
        ▼
模块5 (半透明阴影) ← 可与模块4 并行开发，但需最终集成
```

---

## 九、验证策略

1. **纯透射玻璃球**：`transmission=1, ior=1.5, roughness=0.02, thinWalled=0` → 验证折射方向正确、背景不翻转
2. **薄壁透射**：`transmission=0.8, thinWalled=1` → 验证无体积吸收、背面可见
3. **彩色玻璃**：`albedo=(1,0.2,0.2), transmission=0.9` → 验证红色染色阴影
4. **磨砂玻璃**：`roughness=0.5, transmission=0.8` → 验证粗糙透射散射
5. **半透明布料**：`diffuseTransmission=0.5, transmission=0` → 验证漫透射（SSS 效果近似）
