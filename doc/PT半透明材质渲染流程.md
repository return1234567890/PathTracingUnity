# PT 半透明材质渲染流程（薄片 / 体积）

> **文档定位**：把当前 Vulkan 路径追踪里「半透明材质」从 Unity 材质资产到 compute shader 出图的完整链路梳理一遍，重点是两条分支——**薄片（thin-walled）** 与 **体积（volume）** 的差异、共同点、坑。
>
> **代码基线**：`f63fc40`（体积玻璃 / 两阶段 dispatch 合入）、`613b0b4`（alpha 采样与阴影透射修复），
> 以及其后「阴影遍历改硬件 any-hit」的改动（未提交，见 §3.4）。
> 本文所有行号以工作区当前代码为准，改动后请顺手校正。
>
> **核心文件**：
> - `NativePlugin/shaders/pathtrace.glsl` —— 全部着色逻辑（唯一真相源）
> - `Assets/Scripts/ModelManager.cs` —— 材质参数提取、体积参数表
> - `NativePlugin/src/vpt_types.h` —— 128B `VPT_Material` 布局（C#/C++ 对齐）
> - `NativePlugin/src/vpt_scene.cpp` —— TLAS/BLAS 构建、实例 opaque 标记、材质 SSBO 上传
>
> **最近改动（§3.4）**：阴影半透明遍历从「迭代式 closest-hit 伪 any-hit」改为
> 「ray query candidate 交点的硬件 any-hit」——整条阴影射线只下潜一次 BVH；
> 配套改了 BLAS 几何 opaque 标记与 TLAS 实例 `FORCE_OPAQUE`（只给不透明材质）。
>
> **历史说明**：仓库根目录曾有一份 `半透明BSDF实现方案.md`（最初面向已移除的 Unity/HLSL 软件光追链路的设计稿），其中体积吸收用 `albedo + ior` 推导 σt，与现实现不符，该文件已删除。现实现以本文为准：σt 由显式的 `attenuationColor / attenuationDistance` 决定，见 §5.1。

---

## 目录

- [0. 速查](#0-速查)
- [1. 数据流：一次 PT 帧里半透明数据怎么走](#1-数据流一次-pt-帧里半透明数据怎么走)
- [2. 分支判定：一条材质走薄片还是体积](#2-分支判定一条材质走薄片还是体积)
- [3. 公共基座（两条分支共用）](#3-公共基座两条分支共用)
- [4. 薄片（thin-walled）流程](#4-薄片thin-walled流程)
- [5. 体积（volume）流程](#5-体积volume流程)
- [6. 两条流程逐项对照](#6-两条流程逐项对照)
- [7. 能量与近似清单](#7-能量与近似清单)
- [8. 已知限制与坑](#8-已知限制与坑)
- [9. 调参手册](#9-调参手册)
- [10. 改代码与构建流程](#10-改代码与构建流程)
- [11. 代码索引](#11-代码索引)

---

## 0. 速查

| | 薄片 thin-walled | 体积 volume |
|---|---|---|
| 材质条件 | `_Surface=Transparent` 且 `thickness<=0`（或总开关关闭） | `_Surface=Transparent` 且 `thickness>0` 且 `useVolumeGlass=on` |
| flags | bit4 `MAT_IS_TRANSPARENT` + bit6 `MAT_IS_THIN_WALLED` | bit4 `MAT_IS_TRANSPARENT` + bit5 `MAT_IS_VOLUME` |
| `thinWalled` 字段 | 1 | 0 |
| 出射方向 | 微表面反射方向**按法线镜像到背面**（伪折射，零厚度） | GGX 微表面**真折射**（Snell，含 TIR） |
| 表面 tint | 每命中一次乘一次 `baseColor.rgb`（double-tint） | 表面 **中性白**，颜色全部由内部 Beer 吸收承担 |
| 体积吸收 | 无 | `σt = -ln(tint·attenuationColor) / attenuationDistance`，逐段 `exp(-d·σt)` |
| 内外状态 | 不追踪 | `isInsideVolume` + `volumeSigma` 状态机 |
| 射线偏移 | `EPS_OFFSET = 5mm` | `EPS_VOLUME = 0.1mm` |
| 掠射角变暗 | 不会（缺 `(1-F)` 因子） | 会（`(1-F)` 进权重） |
| 典型用途 | 单面/薄壳窗玻璃、叶子、纸、纱 | 12mm 教堂彩色玻璃、玻璃块、有色透光实体 |

---

## 1. 数据流：一次 PT 帧里半透明数据怎么走

### 1.1 调用链

```
Unity 材质资产 (.mat)
   │  _BaseColor(rgba) / _BaseMap(rgba) / _Surface / _Smoothness / _Metallic
   ▼
ModelManager.GetMaterial()                 Assets/Scripts/ModelManager.cs:635-832
   │  组 PathMaterial（128B），写 flags（bit4/5/6）、thickness、attenuationColor/Distance
   │  体积参数来自 Inspector 的 volumeGlassMaterials 表（按 Material 引用查表）
   ▼
原生插件 VPT_UpdateScene                   NativePlugin/src/vpt_scene.cpp:1374-1376
   │  VPT_Material[] 拷进 s_PendingMaterials，上传为 SSBO（binding 8）
   │  材质 flags 另存快照 s_MaterialFlagsSnapshot（:1507），供 TLAS 实例判 opaque
   ▼
pathtrace.glsl  MaterialBuffer             NativePlugin/shaders/pathtrace.glsl:110-112
   │  TLAS 的 instanceCustomIndex = materialID（vpt_scene.cpp:718）
   │  InstanceInfo.materialID 同源（vpt_scene.cpp:741），供 shader 取顶点属性
   ▼
main() 里按 flags 分派：不透明 / 薄片 / 体积
```

### 1.2 材质结构（128B，三侧严格一致）

`NativePlugin/shaders/pathtrace.glsl:60-82` / `vpt_types.h:41-72` / `ModelManager.cs:54-85`：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `albedo` (vec4) | `_BaseColor`，**alpha 参与 transmission 推导** |
| 16 | `metallic` / `roughness` / `bumpScale` / `flags` | roughness = `1 - _Smoothness`，钳到 `[0.02, 1]` |
| 32 | `emissionColor` | `_EmissionColor`（HDR 钳到 1e4） |
| 48 | `baseTexID` / `metalRoughTexID` / `normalTexID` / `emissiveTexID` | Texture2DArray 的 slice 索引，`0xFFFFFFFF` = 无 |
| 64 | `uvScaleOffset` | xy = tiling，zw = offset |
| 80 | `transmission` | **运行期被 shader 覆盖**（见 §3.1），C# 写入值仅供参考 |
| 84 | `ior` | C# 硬编码 1.5（`ModelManager.cs:826`），材质属性不读 |
| 88 | `diffuseTransmission` | C# 恒 0 → 漫透射 lobe 永不启用 |
| 92 | `thinWalled` | 1 = 薄片，0 = 实体/体积 |
| 96 | `attenuationColor` (vec4) | 体积吸收色（线性 RGB），与逐像素 baseColor 相乘 |
| 112 | `attenuationDistance` | 世界空间衰减距离（米）；**≤0 = 无吸收（纯清玻璃）** |
| 116 | `volumeFlags` | 预留，未使用 |
| 120 | `thickness` | 世界空间厚度（米）；**只当开关用**，见 §8-① |
| 124 | `padVolume` | 对齐填充 |

> 另注：`bumpScale` 目前只写入结构体，shader 的法线扰动没乘这个系数（`pathtrace.glsl:994-999` 直接使用 normal map 的完整强度），所以玻璃表面的法线强度只能靠贴图本身调。

### 1.3 flags 位

```c
#define MAT_HAS_BASE       1u   // bit0
#define MAT_HAS_MR         2u   // bit1
#define MAT_HAS_NORMAL     4u   // bit2
#define MAT_HAS_EMISS      8u   // bit3
#define MAT_IS_TRANSPARENT 16u  // bit4：启用半透明 BSDF（决定"是否走透射分支"）
#define MAT_IS_VOLUME      32u  // bit5：有厚度玻璃 → Beer-Lambert 分支
#define MAT_IS_THIN_WALLED 64u  // bit6：零厚度薄壁（C# 统计/阅读用）
```

> 注意：shader 的薄片判定读的是 **`mat.thinWalled` 字段**，不是 bit6；bit6 目前只在 `ModelManager` 的构建统计里被计数（`ModelManager.cs:564-568`）。两者由 C# 同步写入，实际等价。

---

## 2. 分支判定：一条材质走薄片还是体积

### 2.1 C# 侧是唯一判定点（`ModelManager.cs:806-829`）

```csharp
if (mat.HasProperty("_Surface") && mat.GetFloat("_Surface") > 0.5f)  // URP Transparent
{
    flags |= MAT_IS_TRANSPARENT;
    if (useVolumeGlass && thickness > 0f)
    {
        flags |= MAT_IS_VOLUME;
        pm.thinWalled = 0u;
    }
    else
    {
        flags |= MAT_IS_THIN_WALLED;
        pm.thinWalled = 1u;
    }
}
```

判定矩阵：

| `_Surface` | `thickness` | `useVolumeGlass` | 结果 |
|---|---|---|---|
| Opaque | 任意 | 任意 | 不透射（但 `transmission` 可能非 0，见 §8-④） |
| Transparent | ≤ 0 | 任意 | **薄片** |
| Transparent | > 0 | on | **体积** |
| Transparent | > 0 | off | **薄片**（快速回退路径） |

`thickness` 的来源优先级（`ModelManager.cs:783-799`）：

1. `ModelManager.volumeGlassMaterials` 表（**优先**，按 `Material` 引用匹配，`ModelManager.cs:312-321`）
2. 材质上的 `_Thickness` / `_AttenuationColor` / `_AttenuationDistance` 属性（URP Lit 未声明这些属性，`HasProperty` 为 false，实测读不到）
3. 默认 `thickness = 0` → 薄片

> 所以想让某块玻璃走体积分支，必须把材质拖进 `ModelManager` 的 `volumeGlassMaterials` 列表并填 `thickness > 0`。构建日志会打印 `体积材质=N, 薄壁材质=M`（`ModelManager.cs:571-576`），这是最快的自检手段。

### 2.2 shader 侧判定（`pathtrace.glsl:1004-1006`、`:362-365`）

```glsl
bool isTransparent    = ((mat.flags & MAT_IS_TRANSPARENT) != 0u);
bool materialIsVolume = ((mat.flags & MAT_IS_VOLUME) != 0u) && (mat.thickness > 0.0);
bool thinWalled       = (mat.thinWalled != 0u);
```

`materialIsVolume()` 是**双重保险**：即使 flags 说体积，`thickness <= 0` 时仍按薄片处理。

### 2.3 互斥与优先级

设计上 bit5 / bit6 互斥（C# 保证）。若两者同时为真：

- `btdfSample` / `btdfEval` 里 `if (thinWalled)` **先命中** → 方向按薄片走；
- `transTint = isVolume ? vec3(1.0) : mat.albedo.rgb` → 颜色按体积中性化。

即"方向薄片 + 颜色体积"的混合态，属于未定义组合。改 C# 时不要破坏这个互斥。

---

## 3. 公共基座（两条分支共用）

### 3.1 transmission 的真相：来自 base color alpha

C# 侧（`ModelManager.cs:824-825`）先写 `transmission = 1 - _BaseColor.a`，但 shader 在透明材质上**会重算覆盖**：

```glsl
// pathtrace.glsl:524-530
vec4 sampleBaseColor(VPT_Material mat, vec2 uvT)
{
    vec4 c = mat.albedo;
    if ((mat.flags & MAT_HAS_BASE) != 0u && mat.baseTexID != TEX_NONE)
        c *= texture(baseColorTex, vec3(uvT, float(mat.baseTexID)));
    return c;                       // RGBA 全乘（alpha 也乘）
}

// pathtrace.glsl:976-977, 999-1001
vec4 baseCol = sampleBaseColor(mat, finalUV);
mat.albedo = baseCol;
...
if (isTransparent)
    mat.transmission = 1.0 - mat.albedo.a;
```

要点：

- **透射率 = 1 − (_BaseColor.a × _BaseMap.a)**，逐像素。
- 这一点是 `613b0b4` 修的：之前只乘 RGB，`mat.albedo.a` 恒为 1 → `transmission` 恒 0 → 玻璃全黑不透明。
- 因此**美术调透明度就是调 base color 的 alpha**；`PathMaterial.transmission` 字段本身是被覆盖的死值。
- `transmission` 的三个消费点：
  1. lobe 选择（`w.z = weightBase * transmission`，§3.2）
  2. 漫反射衰减（`evaluateBSDF`/`sampleBSDF` 里 `diffuse *= (1 - mat.transmission)`，`:219`/`:280`）
  3. 阴影层透射率（`trans = 1 - base.a`，`:610`；阴影遍历自己重算，不读字段）

### 3.2 四 lobe 权重与选择（`pathtrace.glsl:294-348`）

```glsl
// 层叠分配：金属 → 介电镜面 Fresnel → 镜面透射 → 剩余给漫反射/漫透射
w.y  = metallic;                                  // 金属反射
weightBase *= (1 - metallic);
w.y  += weightBase * fr;                          // 介电镜面反射（fr = Schlick(ior, |V·N|)）
weightBase *= (1 - fr);
w.z  = weightBase * transmission;                 // 镜面透射
remain = weightBase * (1 - transmission);
w.w  = remain * diffuseTransmission;              // 漫透射（C# 恒 0 → 永不选中）
w.x  = remain * (1 - diffuseTransmission);        // 漫反射
```

返回 `vec4(diffuse_refl, spec_refl, spec_trans, diff_trans)`。

`findLobe` 从 i=3 倒序累加，`rndVal < 累计权重` 即选中；**选择概率本身充当能量权重**（pdf 里没有再除 lobe 概率）。

清玻璃（`metallic=0, ior=1.5, fr≈0.04, transmission=1`）的实际分布：

| lobe | 权重 | 实际概率 |
|---|---|---|
| 3 漫透射 | 0 | 0% |
| 2 镜面透射 | 0.96 | 96% |
| 1 镜面反射 | 0.04 | 4% |
| 0 漫反射 | ~0 | ~0% |

调 alpha（→ transmission）会同时改变"透射 vs 反射"的比例：alpha=0.5 的玻璃有一半采样走漫反射 → 观感偏乳白/毛玻璃。这是当前唯一的"毛玻璃"控制手段。

### 3.3 NEE 结构（灯 / 环境 50-50 MIS，`pathtrace.glsl:1044-1130`）

```glsl
bool sampleLights = (rand(seed) <= 0.5);   // 硬币：本着色点只用一种光源类型做 NEE
```

| 分支 | 采样 | 权重补偿 | 阴影 |
|---|---|---|---|
| 灯 | 随机选 1 盏（`:1055`） | `× lightCount × 2.0` | `traceShadowTransmission(origin, toL, dist, ...)` |
| 环境 | HDR alias importance（`:1099`） | `× 2.0` | 同上，`maxT = 1e34`（环境在无穷远，等价 AO/半影） |

透明材质的特殊处理（`:1061-1076`、`:1107-1118`）：

```glsl
if (isTransparent && NdotL <= 0.0)            // 光线来自背面 → 走透射 BSDF
{
    float absNdotL = abs(NdotL);
    bsdf_nee = btdfEval(mat, N, T, B, V, toL, NdotV, absNdotL, isInsideVolume);
    lightValid = (dot(N_flat, toL) < 0.0 || NdotL > 0.0);
    NdotL = absNdotL;                          // 允许背面光照，用 |N·L|
}
else if (NdotL > 0.0) { bsdf_nee = evaluateBSDF(...); }
```

**所有材质的 NEE 遮挡都走 `traceShadowTransmission`**（`:1082`、`:1124`）。不透明材质由硬件在第一个不透明命中处 commit 并立即终止遍历（等价且快于旧的 `testShadow`）；半透明材质则在同一次遍历里逐层累积。

### 3.4 阴影半透明遍历（硬件 any-hit，`pathtrace.glsl:532-671`）

ray query 有 **candidate 相交**：几何标成 non-opaque 后，遍历器会把每个命中当作 candidate 交回 shader，由 shader 决定确认（commit）还是丢弃——这就是硬件 any-hit，**不需要 ray tracing pipeline / SBT**。

两侧配合：

| 侧 | 做法 | 位置 |
|---|---|---|
| BLAS | 几何级不再标 `VK_GEOMETRY_OPAQUE_BIT_KHR`（否则实例级无法覆盖成 non-opaque） | `vpt_scene.cpp:490` |
| TLAS | 只有**真不透明**实例才 `FORCE_OPAQUE`；半透明实例 non-opaque → 会出 candidate | `vpt_scene.cpp:727-733` |
| 阴影 query | 只加 `gl_RayFlagsTerminateOnFirstHitEXT`，**不加** `Opaque` | `pathtrace.glsl:556-558` |
| 主光线 query | 仍传 `gl_RayFlagsOpaqueEXT`（强制按不透明处理）→ 主路径零变化 | `pathtrace.glsl:921-924` |

于是：不透明实例被硬件直接 commit，`TerminateOnFirstHit` 当场结束遍历（比 closest-hit 求最近更快）；半透明实例以 candidate 形式逐个回到 shader，不 confirm 就被丢弃、遍历继续往下走。

```
totalTransmission = (1,1,1)
prevT = 0
rayQuery(TerminateOnFirstHit)                  // 整条阴影射线只下潜一次

while (Proceed()):
    0. 非三角形 candidate       → 忽略
    1. 层数 > 16               → break（返回已累积值）
    2. 上一段在介质内           → total *= exp(-|hitT - prevT| · sigmaT)   ← 体积分支
    3. 命中不透明（无 bit4）     → 返回 (0,0,0)   ← 正常到不了：硬件已 commit 并终止
    4. baseColor → trans = 1 - base.a；<= 0.01 → 返回 (0,0,0)
    5. layerT = 下面 4~5 个因子相乘
    6. 体积命中 → 翻转 isInsideVolume / 捕获或清空 sigmaT
    7. total *= layerT；最大分量 <= 0.01 → 返回 (0,0,0)
    8. 不 confirm → 该层被丢弃，遍历自动继续

结束后：committed 命中存在 → 返回 (0,0,0)（被不透明实例挡住）
       否则              → 返回 totalTransmission
```

一次遍历的收益：穿过 1 片玻璃（前后 2 面）从「3 次 closest-hit」降到「1 次 any-hit」；不透明遮挡也从「closest-hit 求最近」变成「命中即停」。

**代价（近似）**：candidate 不保证按 `t` 排序，所以 Beer 段长只能取 `|hitT - prevT|`——对**单层凸体精确**，多层玻璃是近似（参考工程同样如此处理）。见 §7-⑨。

每层透射率的因子：

| 因子 | 公式 | 薄片 | 体积 |
|---|---|---|---|
| A 基础透过 | `1 - base.a` | ✓ | ✓ |
| B Fresnel 反射损失 | `1 - Schlick(ior, |dir·Ngeo|)` | ✓ | ✓ |
| C 颜色染色 | `× base.rgb` | ✓（每面各乘一次） | ✗（由 Beer 承担） |
| D 体积段吸收 | `exp(-d · σt)` | ✗ | ✓ |
| E 粗糙度/金属衰减 | `(1-metallic) × mix(0.65, 1, 1-r²)` | ✓ | ✓ |

**关于 C 因子（double-tint）**：薄片材质每命中一个面就乘一次 `base.rgb`。

- 若薄片是**单面几何**（一个 plane）→ 乘一次，正确；
- 若薄片是**闭合薄壳**（盒状窗玻璃，光线穿过两个面）→ 乘两次 → `tint²`，明显偏暗。

这就是 `f63fc40` 里体积分支要"表面中性化"的根因：体积玻璃同样有前后两个面，如果两面都乘 tint，颜色深度就是平方关系，怎么调都比预期暗。

---

## 4. 薄片（thin-walled）流程

物理模型：**零厚度薄片**。光线打上去，一部分按 Fresnel 反射，其余"穿过"，但穿过后**不改变传播介质**、不做任何距离衰减，只在穿过的瞬间被染一次色。几何上等价于把微表面反射方向按法线镜像到另一侧。

### 4.1 主路径逐步骤

```
depth 循环（pathtrace.glsl:914）
├─ rayQuery 最近命中（tmin = depth==0 ? nearPlane : 0，:919）
├─ miss → HDR 环境 / 常数 ambient，break
├─ getHitInfo 重建 P / N / Ngeo / uv / tangent（:717-763）
├─ 法线朝向修正：dot(rayDir, N) >= 0 时翻转 N 与 Ngeo（:960-965）
├─ UV 变换 + 纹理采样（base / MR / emissive / normal map，:968-999）
├─ transmission = 1 - albedo.a（:1004-1006）
├─ 【跳过】体积段吸收（isInsideVolume 恒 false，:1011-1012 不执行）
├─ G-buffer / DLSS RR albedo 写出（仅 depth==0 && s==0，:1015-1029）
├─ emissive 累加（:1032-1035）
├─ NEE（灯/环境，含 btdfEval 背面透射 + 透射阴影，:1050-1130）
├─ 间接弹射：findLobe → btdfSample（薄片分支）/ sampleBSDF（:1137-1160）
├─ throughput *= bsdf · |N·newDir| / pdf（:1162-1166）
├─ 射线偏移：沿几何法线 +EPS_OFFSET(5mm) 侧（:1188-1193）
├─ 【跳过】体积边界状态机（materialIsVolume 为 false，:1174-1186 不执行）
└─ 俄罗斯轮盘（depth>3，:1196-1201）
```

关键：整条路径**没有 `isInsideVolume` 状态**，薄片永远"在空气中"。

### 4.2 直接光：btdfEval（`:477-518`）

当 NEE 的入射方向在面背面（`NdotL <= 0`）时评估透射 BSDF：

```glsl
vec3 H = normalize(k1 + k2);
if (thinWalled) H = normalize(k1 - k2);      // 薄片用"差向量"构造半向量
if (dot(k2, N) >= 0.0) return 0;             // 必须是背面方向
float b = (1 - fresnel) * G * D * 0.25 / (NdotV * NdotH);
return mat.albedo.rgb * b;                   // 薄片：乘 tint
```

- `H = normalize(k1 - k2)` 是薄片透射的经典构造（入射与出射在法线两侧）；
- 要求 `dot(k2, N) < 0`，即光确实来自表面背后；
- 分子只有 `0.25`（无折射 Jacobian），因为不涉及介质改变。

### 4.3 间接光：btdfSample（`:385-474`，薄片分支 `:419-423`）

```glsl
// 1. VNDF 采样半向量 H（GGX，a = roughness²）
float cosThetaH = sqrt((1 - r2) / (1 + (a2 - 1) * r2));
vec3 H = h0.x*T + h0.y*B + h0.z*N;

// 2. 反射方向 + 按法线镜像到背面 = "穿透"
k2 = reflect(-k1, H);
k2 = normalize(k2 - 2.0 * N * dot(k2, N));   // ← 薄片的核心：零厚度伪折射

// 3. pdf：按反射 PDF 近似
pdf = D * max(dot(N,H),0) * G * 0.25 / (nk1 * max(dot(N,H),1e-6));

// 4. bsdf：把 pdf 直接烘进 bsdf（后续 div pdf 会解析抵消）
bsdf = transTint * G * pdf;     // transTint = mat.albedo.rgb（薄片）
```

代入 `throughput *= bsdf · |N·newDir| / pdf` 得**薄片等效透过权重**：

```
throughput *= tint · G · |N · k2|          （pdf 完全抵消）
```

数值特征：

- **没有 `(1 - Fresnel)`**：掠射角不会像真实玻璃那样变暗（薄片近似）；
- `G` 是 Smith 阴影项（≤1），提供掠射角下的轻微能量衰减；
- `D` 与 `pdf` 抵消，所以**粗糙度只通过 `G` 和采样分布影响**，不改变总量级。

### 4.4 阴影：double-tint（§3.4 的薄片列）

阴影遍历每层乘 `(1-a) · (1-F) · tint · (1-metallic) · mix(0.65,1,1-r²)`。

- 单面薄片穿过一次 = 一次 tint，符合直觉；
- 闭合盒状薄壳穿过两个面 = 两次 tint → 偏暗（要提前把这算进美术预期，或改用体积分支）。

### 4.5 适用与不适用

| 适合 | 不适合 |
|---|---|
| 单面几何的窗玻璃 / 贴片 | 有明确厚度的玻璃块（无折射位移，边缘无厚度感） |
| 叶子、纸、纱、旗帜（薄、透光） | 需要"厚玻璃内部染色随距离变化"的效果 |
| 快速原型：只要透光、不在乎折射 | 需要 TIR / 全内反射的棱镜、水块 |

---

## 5. 体积（volume）流程

物理模型：**有厚度的电介质**。表面是 GGX 微表面 + Fresnel，内部是均匀吸收介质（Beer-Lambert）。颜色由内部传播距离决定，表面本身不染色。

### 5.1 σt：体积吸收系数（`:375-381`）

```glsl
vec3 volumeSigmaT(VPT_Material mat, vec3 tint)
{
    if (mat.attenuationDistance <= 0.0) return vec3(0.0);        // 无吸收（纯清玻璃）
    vec3 a = clamp(tint * mat.attenuationColor.rgb, 1e-4, 1.0);  // 逐像素 baseColor × 材质吸收色
    return -log(a) / mat.attenuationDistance;                    // σt = -ln(T) / d
}
```

语义：**在介质内走 `attenuationDistance` 米，透射率恰好等于 `tint × attenuationColor`**。

- `tint` 是命中点的逐像素 baseColor（彩色玻璃马赛克能逐格着色）；
- `attenuationColor = white` 时直接用贴图颜色；单色玻璃可以填一个颜色；
- `attenuationDistance <= 0` → `σt = 0` → 完全透明的清玻璃（无颜色，但仍有折射与 Fresnel）。

段的透射率：`exp(-d · σt)`，`d` 是实际几何距离（`hitT`），**不是** `thickness` 字段。

### 5.2 介质状态机（主路径 `:909-912`、`:1011-1012`、`:1171-1186`）

每个 sample 独立初始化：

```glsl
bool isInsideVolume = false;
vec3 volumeSigma    = vec3(0.0);
```

三个交互点：

```glsl
// ① 命中后、着色前：先结算"上一表面 → 本次命中"这一段
if (isInsideVolume)
    throughput *= exp(-hit.hitT * volumeSigma);     // :1011-1012

// ② 采样出射方向后：只有"透射"才翻转状态，反射不翻转
if (isTransmission && materialIsVolume(mat))
{
    if (!isInsideVolume) { volumeSigma = volumeSigmaT(mat, baseCol.rgb); isInsideVolume = true; }  // 进入
    else                 { isInsideVolume = false; volumeSigma = vec3(0.0); }                     // 离开
}
```

状态图：

```
空气 ──[进入面: 透射]──▶ 玻璃内部 (捕获 σt)
   ▲                        │
   │                        ├─[内部反射 / TIR]─▶ 仍在内部（状态不变，段吸收继续生效）
   │                        │
   └──[出射面: 透射]────────┘  (清空 σt)
```

要点：

- **段吸收在"下一次命中"时才结算**，所以第一段（相机 → 第一个面）不吸收 —— 正确，因为那一段在空气里；
- 若在介质内 **miss**（数值漏光/几何不闭合），最后一段不补吸收（`:663-665` 的注释说明了这一取舍：补吸收会把漏光变成黑点）；
- 单介质假设：只有 1 个 `bool + σt`，不支持嵌套（玻璃内的水、双层玻璃），见 §8-②。

### 5.3 表面中性化（`btdfSample` / `btdfEval` 的 `transTint`）

```glsl
vec3 transTint = isVolume ? vec3(1.0) : mat.albedo.rgb;   // :468 / :504
```

体积玻璃的**表面不染色**，颜色只由 §5.1 的 Beer 吸收在内部段乘一次。

为什么必须这样（`f63fc40` 修掉的根因）：

```
错误（表面染色）： T_total = T_surface_front × T_beer × T_surface_back
                            = tint × exp(-d·σ) × tint  = tint² × exp(-d·σ)
正确（中性表面）： T_total = exp(-d·σ)
```

即"前后两个面各乘一次 baseColor"会让颜色变成平方关系，红玻璃怎么调都是暗红/发黑。

### 5.4 折射方向、IOR、TIR（`btdfSample` `:393-444`）

```glsl
bool isVolume = materialIsVolume(mat);
float ior1 = 1.0, ior2 = mat.ior;
if (isVolume && insideVolume) { ior1 = mat.ior; ior2 = 1.0; }   // 从介质内部射出：IOR 反向

float VdotN   = dot(k1, N);
bool  entering = (VdotN > 0.0);
float eta = entering ? (ior1 / ior2) : (ior2 / ior1);

// VNDF 采样 H → Snell 折射
vec3 refN = entering ? H : -H;
k2 = refractDir(-k1, refN, eta, k1h, tir);

// gnk2 侧向校验：折射必须真的穿到法线另一侧
if (!thinWalled) {
    float side = dot(k2, N);
    if (tir ? (side <= 0.0) : (side >= 0.0)) return;   // 丢样本（等效吸收）
}
```

几个容易看错的点：

- **`entering` 恒为 true**。因为主路径在命中时已经把 `N` 翻到面向光线（`:960-965`），`dot(k1, N)` 永远 ≥ 0。真正决定"进/出介质"的是 `insideVolume`，它把 `ior1/ior2` 对调。这是体积分支能正确出射折射的前提（否则出射方向会失真）。
- **TIR（全内反射）**：`refractDir` 返回 `tir = true` 时，`k2 = reflect(-k1, H)`；此时 `bsdf = transTint * G * pdf`（走薄片式权重）。因为 `dot(k2, Ngeo) > 0`，`isTransmission` 为 false → **状态机不翻转**，光线继续在介质内传播。正确。
- **侧向校验丢样本**：粗糙度让 `H` 偏离 `N` 时，少量采样会折射到错误一侧。直接丢弃（返回 pdf=0 → 该 sample 的路径终止），避免带着错误的"内外状态"继续传播。代价是轻微能量损失（表现为粗糙玻璃偏暗一点），但比状态错乱安全。

### 5.5 偏移与 tmin：12mm 玻璃的两个必备修正

| 修正 | 值 | 不改会怎样 |
|---|---|---|
| `EPS_VOLUME`（体积表面偏移） | `1e-4`（0.1mm） | 用通用的 `5mm` 会把 12mm 玻璃的内部路径吃掉近一半；< 5mm 的薄玻璃甚至直接跳过出射面 |
| 逐 bounce `tmin` | `depth==0 ? nearPlane : 0` | 相机近裁剪面 0.3m，而玻璃只有 12mm → 后表面永远落在 tmin 之前，**永远命中不到** |

```glsl
// :919
float tmin = (depth == 0) ? nearPlane : 0.0;
// :368-371
float surfaceOffset(VPT_Material mat)
{
    return materialIsVolume(mat) ? EPS_VOLUME : EPS_OFFSET;
}
```

`surfaceOffset` 同时被主路径的射线偏移、NEE 阴影起点、阴影遍历的步进距离复用。

### 5.6 体积的阴影（§3.4 的体积列）

阴影遍历额外接收起点的介质状态（`:1082-1083`）：

```glsl
// 主路径位于玻璃内部时（例如从玻璃内侧表面做 NEE），阴影射线整段都要按 σt 吸收
traceShadowTransmission(shadowOrigin, toL, dist, isInsideVolume, volumeSigma);
```

内部顺序（`:578-583`）：

```glsl
// 先把"上一表面 → 本次命中"这一段按当前 σt 吸收（与主路径完全对称）
if (isInsideVolume) totalTransmission *= exp(-hitT * sigmaT);
```

然后是每层因子（不乘 `base.rgb`），最后在命中体积材质时切换状态：

```glsl
if (isVolume) {
    if (!isInsideVolume) { sigmaT = volumeSigmaT(mat, base.rgb); isInsideVolume = true; }  // 进入：捕获
    else                 { isInsideVolume = false; sigmaT = vec3(0.0); }                    // 离开：清空
}
```

于是：

```
灯 ──[出射面]──(空气段: 无吸收)──[入射面: (1-a)(1-F)(1-metallic)·rough]──(玻璃内: exp(-d·σt))──▶ 地面
```

彩色玻璃的"地上被染色的光斑"就来自玻璃内部那段 `exp(-d · σt)`，`σt` 由逐像素 baseColor 决定 → 每一格窗玻璃投出自己颜色的光。

### 5.7 与薄片差异一览

| 维度 | 薄片 | 体积 |
|---|---|---|
| `btdfSample` 方向 | `reflect` 后按 N 镜像 | `refractDir` + TIR 回退 |
| IOR 方向 | 不用 eta | `insideVolume` 决定 `ior1/ior2` |
| 侧向校验 | 跳过 | 必须（否则状态错乱） |
| pdf | 反射 PDF `D·G/(4·nk1)` | 折射 PDF `D·cosH·G·k1h·k2h/(nk1·cosH·tmp²)` |
| bsdf | `tint·G·pdf` | `G·(1-F)·pdf`（tint 中性） |
| 间接权重 | `tint·G·|N·k2|` | `G·(1-F)·|N·k2|` |
| 掠射变暗 | 否 | 是 |
| 段吸收 | 无 | `exp(-d·σt)` |
| 状态 | 无 | `isInsideVolume` + `volumeSigma` |
| 偏移 | 5mm | 0.1mm |
| 阴影 tint | 每面乘一次 | 表面不乘，颜色来自段吸收 |

---

## 6. 两条流程逐项对照

同一次 `main()` 循环里，两条分支只在下面这些点分叉（其余代码完全共享）：

| 行号 | 分叉点 | 薄片 | 体积 |
|---|---|---|---|
| `:362` | `materialIsVolume()` | false | true |
| `:1011` | 段吸收 | 跳过 | `throughput *= exp(-hitT·σ)` |
| `:1025` | DLSS RR diffuse albedo | 同（未按 transmission 衰减，见 §8-⑦） | 同 |
| `:1068` | NEE 透射评估 | `btdfEval(..., insideVolume=false)` | 传入真实 `insideVolume` |
| `:1082` / `:1124` | 阴影 | 起点 `insideVolume=false` | 起点带介质状态 |
| `:1143` | 间接透射采样 | 薄片分支（镜像伪折射） | 折射 + TIR |
| `:1149` | 漫透射 lobe | `albedo·diffuseTransmission`（恒 0 权重，不会进） | 同 |
| `:1174` | 状态机 | 不进入 | 进入/离开翻转 |
| `:1188` | 偏移 | 5mm | 0.1mm |

---

## 7. 能量与近似清单

**物理正确的部分**

- 4-lobe 层叠权重（金属 → 介电 Fresnel → 镜面透射 → 剩余）能量不超 1；
- GGX 的 D/G、Schlick Fresnel；
- 折射方向、TIR、折射 PDF 的 Jacobian（`(η₁k₁·h − η₂k₂·h)²`）；
- Beer-Lambert 段吸收；
- 阴影的逐层透射与介质段吸收。

**近似 / 已知偏差**（判断效果问题时先看这里）

| # | 近似 | 影响 | 备注 |
|---|---|---|---|
| ① | 薄片透射缺 `(1-F)` | 掠射角不比正视暗，玻璃边缘不"发亮/变实" | 体积分支有 |
| ② | `btdfEval`（NEE）分母用 `NdotV·NdotH` | 与采样侧权重不完全一致，高光/透射的 NEE 能量略有偏差 | 采样侧是 `k1h·k2h/(nk1·cosH·tmp²)` |
| ③ | lobe 选择概率不进 pdf | 选择概率被当作能量权重使用；反射两 lobe 与透射两 lobe 的归一化不完全统一 | 观感基本正常，严格 MIS 需重算权重 |
| ④ | 阴影粗糙度衰减 `mix(0.65, 1, 1-r²)` | 经验值，非物理 | 用于模拟粗糙表面的散射损失 |
| ⑤ | 阴影 Fresnel 用几何法线、不看 normal map | 法线贴图不影响彩色阴影的 Fresnel 项 | 有意简化（阴影遍历不采样 normal map） |
| ⑥ | 无漫透射 | `diffuseTransmission` 恒 0，纸张/布料近似不可用 | 结构体已预留 |
| ⑦ | 无 IOR 色散、无 absorption 波长相关 | 彩虹边、真实色散不出现 | `KHR_materials_dispersion` 未实现 |
| ⑧ | 单介质 | 玻璃内套玻璃/水会状态错乱 | 只用一个 bool + 一个 σt |
| ⑨ | candidate 不保证按 `t` 排序 | Beer 段长用 `\|hitT - prevT\|`：单层凸体精确，多层玻璃可能过吸收；不同色玻璃多片时有串色风险 | 参考工程同样如此；要精确就收集 `(t, instance, prim, bary)` 排序后再算 |

---

## 8. 已知限制与坑

### ① `thickness` 只是开关，不参与吸收距离

`materialIsVolume()` 只判 `thickness > 0`；真正进 Beer 的 `d` 是几何 `hitT`。所以：

- 把 `thickness` 从 12mm 改成 100mm **不会**让玻璃更浓，只会改变"是否走体积分支"；
- 想让颜色更深，改 `attenuationDistance`（变小 → 更浓）或 `attenuationColor`（更暗 → 更浓）；
- 几何本身就是单面薄片却配了 `thickness > 0` → 光线进入后永远没有出射面 → 状态一直"在内部"，后续所有段都持续吸收，表现会非常怪。**体积分支要求几何是闭合实体。**

### ② 单介质假设

只有一套 `isInsideVolume + volumeSigma`，不支持嵌套介质。碰到"玻璃里装水""双层玻璃"会得到错误结果（且不会报错）。注释在 `:909-912` 已明确。

### ③ 薄片 / 体积互斥

同时置位时是未定义组合（方向薄片 + 颜色体积）。由 C# 保证互斥，改 C# 判定逻辑时不要破坏。

### ④ Opaque 材质的 `_BaseColor.a < 1` 会削弱漫反射

`ModelManager.cs:825` **无条件**写 `pm.transmission = 1 - _BaseColor.a`；shader 只在 `isTransparent` 时才覆盖它（`:1004-1006`）。于是：

- 一个 `_Surface=Opaque` 但 base color alpha = 0.3 的材质 → `transmission = 0.7` → `evaluateBSDF` 里 `diffuse *= (1-0.7)` → **PT 里比光栅端暗 70%**；
- 光栅端完全忽略不透明材质的 alpha，所以两边对不上。

自检：PT 里某物体莫名发暗、且材质 alpha 非 1 → 先把 alpha 拉回 1。

### ⑤ 阴影层数上限 16，超出会**偏亮**

循环跑完 16 次后直接返回已累积的 `totalTransmission`（`:670`），后面没走完的层就漏乘了。多层玻璃幕墙场景要注意（16 层够用，但别指望更多）。

### ⑥ 介质内 miss 不补最后一段吸收

数值漏光（几何不闭合、浮点误差）时，返回的是"已离开介质"的结果而非黑点。取舍原因写在 `:663-665`：补吸收会把漏光变成黑点，更难查。

### ⑦ DLSS RR 把首次命中当不透明表面

`:1015-1029` 写 G-buffer 时：

```glsl
imageStore(gbufferNormalRough, ..., vec4(Ngeo, mat.roughness));       // 几何法线，非 shading normal
imageStore(diffuseAlbedoImg, ..., vec4(albedo * (1-metallic), 1.0));  // 未按 transmission 衰减
imageStore(specularAlbedoImg, ..., vec4(EnvBRDFApprox(F0, ...), 1.0));
```

玻璃表面的 depth/normal 是"实体表面"，但它的实际外观是**背后物体的透射图像**。RR 拿到的是"这个表面有 albedo 和粗糙度"，容易在玻璃区域产生模糊/拖影/边缘糊。若出现玻璃区域整体发糊，先确认不是 RR 的语义问题（可临时关 RR 对比）。

### ⑧ `ior` 硬编码 1.5

`ModelManager.cs:826` 直接写 `pm.ior = 1.5f`，不读材质属性。想做水（1.33）、钻石（2.4）必须改代码，或扩展 `VolumeGlassMaterialParams` 加一个 `ior` 字段。

### ⑨ `testShadow` 已删除（并入 any-hit 遍历）

旧代码里有个 `terminate-on-first-hit` 的二值遮挡函数 `testShadow`，但一直没有调用点（NEE 遮挡统一走 `traceShadowTransmission`）。改成 candidate any-hit 之后**两种行为合成了一次遍历**：不透明实例被硬件 commit 并立即终止（≈ 原来的 `testShadow`），半透明实例作为 candidate 逐层累积。所以该函数已删除，不要再按旧文档去找它。

> 若哪天要临时关掉玻璃阴影（纯二值遮挡）做性能对照，把阴影 query 改成 `gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT` 即可，一行的事。

### ⑩ `MAT_IS_THIN_WALLED`（bit6）在 shader 里没有被读取

shader 的薄片判定用 `mat.thinWalled` 字段。bit6 只用于 C# 统计日志。两者由 C# 同步写入，目前等价；若将来只改其中一个，行为会不一致。

### ⑪ `PathMaterial.transmission` 字段是"死值"

C# 写入的值会被 shader 覆盖（透明材质）。不要指望从 C# 直接设 transmission 来调透明度 —— 唯一入口是 base color alpha。

### ⑫ 构建只在 `Start()` 做一次

改 `volumeGlassMaterials`、`useVolumeGlass` 后必须重进 Play（`ModelManager.cs:151-154` 的注释）。Play 中改 Inspector 不会重建，也不会生效。

### ⑬ 半透明实例必须保持 non-opaque（否则玻璃阴影全黑）

阴影 any-hit 依赖「半透明实例在 TLAS 里 non-opaque」。如果谁给实例无条件加上
`VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR`（或把 BLAS 几何改回 `VK_GEOMETRY_OPAQUE_BIT_KHR`），
硬件会把玻璃当实心遮挡直接 commit → 彩色玻璃阴影变成**纯黑剪影**，而主路径画面看不出任何异常。
这是这套机制唯一容易踩的静默失败，改 `vpt_scene.cpp` 的实例/几何 flags 时留意。

---

## 9. 调参手册

| 想要的效果 | 改什么 | 位置 |
|---|---|---|
| 更透 / 更实（整体不透明度） | `_BaseColor.a`（越小越透） | 材质 |
| 玻璃染色（薄片） | `_BaseColor.rgb`（每面乘一次，盒状薄壳注意 double-tint） | 材质 |
| 玻璃染色（体积） | `attenuationColor`（白 = 用贴图颜色） | `volumeGlassMaterials` |
| 颜色浓度随距离 | `attenuationDistance`（越小越浓） | `volumeGlassMaterials` |
| 走薄片还是体积 | `thickness > 0` + `useVolumeGlass` | `volumeGlassMaterials` / ModelManager |
| 表面粗糙（磨砂） | `_Smoothness`（→ roughness） | 材质 |
| 折射率 | 暂不可配，改 `pm.ior = 1.5f` | `ModelManager.cs:826` |
| 减少玻璃噪点 | 提高 `samplesPerPixel` / 让 RR 多累积 | `PathTraceRendererFeature` |
| 阴影层数上限 | `MAX_SHADOW_LAYERS`（默认 16） | `pathtrace.glsl:301` |
| 透射提前终止阈值 | `MIN_TRANSMISSION`（默认 0.01） | `pathtrace.glsl:300` |
| 体积边界偏移 | `EPS_VOLUME`（默认 0.1mm） | `pathtrace.glsl:17` |

**推荐起步配置**（12mm 彩色玻璃）：

```
_BaseColor        = (1, 1, 1, 0)      // alpha=0 → transmission=1
_Smoothness       ≈ 0.9              // roughness ≈ 0.1
volumeGlassMaterials:
    thickness           = 0.012
    attenuationDistance = 0.012      // 走满一个厚度后的透射率 ≈ attenuationColor
    attenuationColor    = 白（贴图自带颜色）
```

---

## 10. 改代码与构建流程

shader 是**内嵌 SPIR-V**（`src/pathtrace_spv.h`），改 `.glsl` 后不做下面三步，Unity 跑的还是旧二进制：

```powershell
# 1. 重编 SPIR-V（ray query 需要 vulkan1.2）
glslangValidator -V -S comp --target-env vulkan1.2 `
    -o NativePlugin/shaders/pathtrace.spv NativePlugin/shaders/pathtrace.glsl

# 2. 重生成内嵌头（变量名必须是 g_PathTraceSpv）
#    若执行策略禁止运行脚本，加 -ExecutionPolicy Bypass
powershell -NoProfile -ExecutionPolicy Bypass -File NativePlugin/shaders/spv_to_header.ps1 `
    -spvPath    NativePlugin/shaders/pathtrace.spv `
    -headerPath NativePlugin/src/pathtrace_spv.h

# 3. 重建 DLL（CMake POST_BUILD 会自动拷到 Assets/Plugins/x86_64/）
cmake --build NativePlugin/build --config Release --target VulkanPathTracePlugin
```

**校验不变量**：

- `pathtrace_spv.h` 里的 `g_PathTraceSpv_size` == `pathtrace.spv` 文件字节数（当前 86120B）；
- `spirv-val --target-env vulkan1.2 pathtrace.spv` 通过；
- `Assets/Plugins/x86_64/VulkanPathTracePlugin.dll` 与 `NativePlugin/build/bin/Release/` 产物 SHA256 一致。

**注意**：Unity 运行时会锁 DLL，复制前需关闭编辑器（手工拷贝时）；通过 CMake POST_BUILD 也会因为占用而失败，同样要关编辑器。

改 C# 侧（`ModelManager`）不需要重建 DLL，但**必须重进 Play**。

---

## 11. 代码索引

### shader（`NativePlugin/shaders/pathtrace.glsl`）

| 行 | 内容 |
|---|---|
| 13-17 | `EPS_OFFSET`(5mm) / `EPS_VOLUME`(0.1mm) 与注释（12mm 玻璃的取舍） |
| 19-26 | flags 定义 |
| 60-82 | `VPT_Material`（128B） |
| 187-232 | GGX：`D_GGX` / `G_SmithGGX` / `F_Schlick` / `evaluateBSDF` |
| 235-288 | `sampleBSDF`（反射侧采样，含 `diffuse *= 1-transmission`） |
| 294-348 | 4-lobe 权重 + `findLobe` |
| 351-357 | `refractDir`（TIR 检测） |
| 362-371 | `materialIsVolume` / `surfaceOffset` |
| 375-381 | `volumeSigmaT` |
| 385-474 | `btdfSample`（薄片 + 体积 + TIR + 侧向校验） |
| 477-518 | `btdfEval`（NEE 用） |
| 524-530 | `sampleBaseColor`（RGBA 全乘） |
| 532-671 | `traceShadowTransmission`（硬件 any-hit：candidate 遍历） |
| 717-763 | `getHitInfo`（属性重建） |
| 909-912 | 每个 sample 的介质状态初值 |
| 914-1202 | 主路径循环 |
| 1004-1006 | `transmission = 1 - albedo.a` |
| 1011-1012 | 主路径段吸收 |
| 1050-1130 | NEE（灯 / 环境，透明背面透射） |
| 1137-1160 | 间接弹射（lobe 分派） |
| 1171-1186 | 体积状态机 |
| 1188-1193 | 射线偏移 |

### C#（`Assets/Scripts/ModelManager.cs`）

| 行 | 内容 |
|---|---|
| 54-85 | `PathMaterial`（128B） |
| 98-114 | `VolumeGlassMaterialParams`（体积参数表条目） |
| 116-127 | `MaterialFlags` |
| 146-155 | `useVolumeGlass` / `volumeGlassMaterials`（Inspector） |
| 312-321 | 材质 → 体积参数 的字典构建 |
| 564-568 | 构建统计：体积/薄壁材质计数 |
| 635-832 | `GetMaterial`（全部参数提取） |
| 783-804 | thickness / attenuation 优先级 |
| 806-829 | flags 与 transmission 的最终写入 |

### native（`NativePlugin/src/`）

| 位置 | 内容 |
|---|---|
| `vpt_types.h:41-72` | `VPT_Material` + `static_assert(128)` |
| `vpt_types.h:94-95` | `VPT_MAT_FLAG_TRANSPARENT`（bit4，与 C# / GLSL 一一对应） |
| `vpt_scene.cpp:490` | BLAS 几何**不再**标 `VK_GEOMETRY_OPAQUE_BIT_KHR`（`geom.flags = 0`） |
| `vpt_scene.cpp:718-733` | `instanceCustomIndex = materialID`；实例 flags：只给不透明材质 `FORCE_OPAQUE`，半透明保持 non-opaque |
| `vpt_scene.cpp:1507-1513` | 材质 flags 快照（动态场景每帧写实例时要用） |
| `vpt_scene.cpp:1488-1496` | 材质 SSBO 上传（binding 8） |
| `vpt_dispatch.cpp:14, 166-167` | 内嵌 `g_PathTraceSpv` 创建 shader module |

---

## 附：一眼看懂两条分支

```
                        ┌────────────────────────────────────────────┐
  命中半透明面 ─────────▶│ transmission = 1 - base.a  (逐像素 alpha)   │
                        └───────────────┬────────────────────────────┘
                                        │
                    ┌───────────────────┴───────────────────┐
                    │ flags & MAT_IS_VOLUME && thickness>0 ? │
                    └───────┬───────────────────────┬───────┘
                         否 │                       │ 是
                            ▼                       ▼
              ┌──────────────────────┐   ┌──────────────────────────────┐
              │ 薄片 thin-walled      │   │ 体积 volume                   │
              │                      │   │                              │
              │ 方向: reflect→镜像N   │   │ 方向: refract(eta) / TIR      │
              │ 表面: tint 染色       │   │ 表面: 中性白                   │
              │ 内部: 无吸收          │   │ 内部: exp(-d·σt)              │
              │ 状态: 无              │   │ 状态: isInsideVolume+σt       │
              │ 偏移: 5mm             │   │ 偏移: 0.1mm                   │
              │ 阴影: tint^层数       │   │ 阴影: exp(-d·σt) 段吸收        │
              └──────────────────────┘   └──────────────────────────────┘
```
