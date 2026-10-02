# DLSS RR 亮区抖动根因分析（自适应累积 + 输入分布 OOD）

> 接续 `DLSS_抖动修复踩坑记录.md`「RR 亮处抖动排查」一节。根因 1-3（InJitterOffset 取负 / 移除 DepthInverted / 移除 AutoExposure）修复重启验证后仍有残余抖动，本文给出新根因分析、与官方样例代码（`MyEngineVulkanV2`）的逐层对比，以及下一步验证/修复计划。日期：2026-09-08。

## 现象

- 静止场景（当前仅一盏平行光）：**被光照到的像素持续抖动，暗处像素基本不抖**
- 调整光源强度无缓解
- 抖动与亮度强相关，与几何位置无关（边缘抖动已由根因 1/2 消除）

## 排除项

| 疑点 | 结论 |
|---|---|
| MV/矩阵/深度量纲 | 排除：几何对齐类问题症状应集中在边缘与深度突变处，与亮度无关 |
| 原次因「NEE 双峰跳变」 | 排除：双峰跳变需要 ≥2 盏亮度悬殊的灯；当前场景单灯时 `li = rand % 1 = 0` 恒定，不存在跳变 |
| 调光源强度无效的佐证 | 排除绝对量敏感类问题（曝光/值域）：若是，调强度必然改变抖动幅度 |
| jitter 公式 `pixel + 0.5 + jitter` 多加 0.5 | 排除：与样例代码逐项等价（见下文「jitter + 0.5 核对」）；且常数项不产生帧间差异，也无法解释亮/暗不对称 |

## 关键观察（用户方向，已确认成立）

**噪点分布与抖动分布负相关**：被光照到的部分噪点少（NEE 直接光确定）但抖动大；阴暗处噪点大（纯间接 1spp）反而几乎不抖。

## 根因分析

### 一句话结论

**RR transformer 的时域累积强度是按局部统计量自适应的：亮区（NEE 确定性直接光主导）相对方差低 → 网络判定"信号可信" → 弱累积、强细节保留 → 帧间差异直通输出，表现为抖动；暗区（纯间接 1spp）相对方差高 → 强累积（时域低通）→ 帧间差异被平均，表现为稳定但有颗粒感。**

### 1. 单平行光下，亮区直接光是"零方差"的

`NativePlugin/shaders/pathtrace.glsl` NEE 块（L828-861）：

```glsl
uint li = uint(rand(seed) * float(lightCount)) % lightCount;  // lightCount=1 → li 恒 0
```

- `sampleLightContribution`（L553-576）：directional 光 `toL`/`Lcolor` 逐帧恒定，无采样随机性
- `evaluateBSDF`（L192-213）：输入 N/L/V 全部确定
- `traceShadowTransmission`：确定性遍历（硬阴影）
- `* float(lightCount)` 补偿单灯时 = ×1

所以**亮区像素 = 确定性项 D（仅随 jitter 采样位置微变）+ 随机项 I（间接光）**；暗区 NEE 贡献为 0，只剩纯随机的 I。

### 2. 亮区输入的帧间差异分两类

**类型 A：jitter 相位驱动的结构化差异（主感知来源）**

```glsl
// pathtrace.glsl L688-689
vec2 jitter = cam.jitterNearFar.xy;
vec2 uv = (vec2(pixel) + 0.5 + jitter) / vec2(w, h);
```

Halton 8 相位循环（`PathTraceRendererFeature.cs` L152-156）下，每帧采样同一像素内**不同亚像素位置**的场景函数。亮区高频丰富（纹理、法线插值、阴影边界）时，采样值随相位系统性变化——非零均值、结构化，人眼感知为"抖动/爬动"。

**类型 B：间接光残留噪声（零均值）**

```glsl
// pathtrace.glsl L711
uint seed = xxhash32(uvec3(pixel.x, pixel.y, frameCount * spp + s));
```

seed 依赖 frameCount，首跳后的 BSDF 采样每帧独立随机。亮区同样叠加此项（绝对幅度与暗区同量级），RR 去噪后有残留，但零均值，感知为"噪点/闪烁"而非"抖动"。

### 3. RR 输入通路无任何前置平滑

`PathTraceRendererFeature.cs` L746-749：RR 模式跳过全部自研时域/空域/AntiFirefly，`_outputRT`（原始 1spp）直接作为 RR 的 color 输入。其余 G-buffer（MV 静态时=0、线性深度、normal+rough、albedo 分解）帧间高度稳定 → **RR 输出在亮区的帧间变化只能来源于 color 输入的帧间变化**（类型 A + B）。

### 4. 为什么亮抖暗稳

- **暗区**：相对方差大 → 网络强累积 ≈ 多帧滑动平均。类型 B 零均值，平均后期望稳定 → "噪点大但不抖"（静态颗粒感 ≠ 结构化位移）
- **亮区**：相对方差小（D 主导）→ 网络判定信号可信 → 弱累积、保留/锐化高频。类型 A 的相位差异和类型 B 的残留**不被平均，直接进入输出** → "噪点少但抖"

SR 模式同一 MotionVector kernel、同一 jitter 却不抖，正是因为 SR 输入是 à-trous 空域滤波后的图像——类型 A 的高频差异已被空域滤波压掉。RR 拿到高频完整的原始 1spp，失去了这层保护。

### 5. 为什么调光源强度无效（最有力的佐证）

间接光能量来自直接光弹射，调强度时 D 与 I **近似等比缩放**，两区的相对方差结构（σ/μ）不变。RR 的累积决策基于相对统计量而非绝对值 → 网络行为不变 → 抖动不变。

### 6. 深层原因：输入分布与训练分布错配（OOD）

官方 RR 训练数据中的路径追踪器，直接光区域**同样带 1spp 噪声**（多灯随机选灯、面积光/太阳角锥采样、环境 importance 采样等，见下文样例代码梳理）。网络学到的策略是"直接光区域也有噪声 → 需要累积"。URPPT 把直接光做得比训练分布"干净得多"（单平行光确定性 NEE + 硬阴影），等于在亮区喂了一个网络没见过的分布——它按"低方差=可信信号"处理，弱累积 + 细节保留，而 DLSS 超分固有的相位依赖高频合成在官方管线里正是靠"输入有噪声 → 强累积"压住的，这个保护在亮区失效。

## 与样例代码对比

### jitter + 0.5 核对（已排除的疑点）

对比 `MyEngineVulkanV2/VulkanPathTracer/shaders/gltf_pathtrace.slang`：`getRay` 的 `offset` 参数不含 0.5，但 0.5 被折叠进调用方传入的 offset。调用链展开：

```slang
// L779-787 processPixel（DLSS 路径）
float2 subpixelJitter = float2(0.5f, 0.5f);                 // 非 DLSS：以 0.5 为基准
if(pushConst.frameCount > 0)
  subpixelJitter += ANTIALIASING_STANDARD_DEVIATION * sampleGaussian(...);
if(pushConst.useDlss == 1)
  subpixelJitter = pushConst.jitter + float2(0.5f, 0.5f);   // ★ DLSS：jitter + 0.5

// L725 → L112-114 getRay
RayDesc ray = getRay(samplePos, subpixelJitter, ...);
const float2 clipCoords = (samplePos + offset) / imageSize * 2.0 - 1.0;
```

实际采样位置 = `samplePos + (jitter + 0.5)` = `pixel + 0.5 + jitter`，**与 URPPT pathtrace.glsl L689 逐项等价**。0.5 的语义是把坐标从像素左上角移到像素中心，`InJitterOffset` 的定义就是相对像素中心的偏移，两边配对才自洽。

三方配对完整核对：

| | 样例 (MyEngineVulkanV2) | URPPT | 一致？ |
|---|---|---|---|
| jitter 值域 | `halton(i) - float2(0.5,0.5)` ∈ [-0.5, 0.5]（`dlss_util.h` L128-132） | `Halton(i) - 0.5f` ∈ [-0.5, 0.5]（`PathTraceRendererFeature.cs` L140-156） | ✓ |
| 采样公式 | `pixel + 0.5 + jitter` | `pixel + 0.5 + jitter` | ✓ |
| 传 NGX | `InJitterOffset = -jitter`（`dlss_wrapper.cpp` L390-391） | RR 分支 `-jitter`（`vpt_dlss.cpp` L859-860） | ✓ |

反证：0.5 是每帧每像素相同的常量，若错只会产生恒定半像素错位（整图稳定偏移），帧差分中直接消掉，不可能产生帧间抖动，更不可能产生亮/暗不对称。

### 样例 processPixel 的噪声结构梳理（无"人为加噪声"语句，但全链路天然全噪）

样例中找不到任何对颜色注入噪声的语句，其噪声全部来自蒙特卡洛采样本身，分布在每一层：

**第一层：processPixel（gltf_pathtrace.slang L757-834）**

| 位置 | 代码 | 随机性 |
|---|---|---|
| L775 | `seed = xxhash32(uint3(pixel, frameCount))` | 每帧独立种子 |
| L779-781 | `0.5 + 0.4246 × sampleGaussian(rand,rand)` | 非 DLSS 路径：高斯亚像素随机抖动做 AA；DLSS 路径下替换为确定性 Halton |
| L805-807 | `if(first_frame \|\| useDlss==1) → replace` | **DLSS 模式下每帧覆盖、绝不累积** → 喂给 DLSS 的天然是逐帧独立的 1spp 噪声帧 |

**第二层：samplePixel（L715-751）**

- L727-738：DoF 光圈采样（`aperture>0` 时随机出瞳位置）
- L743-748：firefly clamp（确定性操作）

**第三层：pathTrace 直接光（L599-698）—— 与 URPPT 差异最大**

NEE 走 `sampleLights`（L129-227），随机性有四重：

1. **灯/环境 50/50 硬币**（L144-159）：`bool sampleLights = (rand(seed) <= 0.5)`——Veach one-sample MIS。只要环境开着，每帧每个着色点都掷硬币决定本帧采不采这盏灯，选中时能量除以 0.5 补偿 → 单帧值在"2×贡献"和"0/环境值"间跳变
2. **随机选灯**（L172）：`rand(seed) * numLights`（URPPT 同款，但样例场景常驻多灯）
3. **面积光角锥采样**（L174 → `nvpro_core2/nvshaders/light_contrib.h.slang` L122-141）：`radius>0` 或角尺寸>0 的灯，`randVal` 在光源立体角内采随机入射方向 → 软阴影 + 半影噪声。nvpro 注释原话："using random sampling will result in more realistic but potentially **noisy** illumination"
4. **环境 NEE importance 采样**（L188-206）：物理天空 `samplePhysicalSky(rand,rand)` 或 HDR `environmentSample(rand3)`

外加第五处：L636 `bsdfEvaluate` 的 `xi = float3(rand,rand,rand)`——nvpro 分层 BSDF 库连"评估 NEE 方向上的 BSDF"都消耗随机数。

### 直接光随机性对比表

| | 样例 (nvpro) | URPPT |
|---|---|---|
| 单灯时选灯 | 多灯随机（demo 常驻多灯+环境） | li 恒 0，确定 |
| 灯/环境选择 | 50/50 逐帧硬币 | 无（ambient 是常数，走 miss 分支，不参与 NEE） |
| 平行光方向 | 角尺寸>0 时角锥内随机采样 | 恒定 |
| NEE 的 BSDF 评估 | 带 `xi` 随机数 | `evaluateBSDF` 纯确定 |
| 直接光结果 | **逐帧独立随机变量** | 常数（仅随 jitter 相位微变） |

即使构造最退化场景（单平行光、角尺寸 0、无环境），样例直接光也会因无 NEE 可采（`totalWeight==0` 直接 return）而完全依赖 BSDF 路径命中自发光——噪声反而更大。**样例实现中不存在"亮区比暗区干净"的路径，被照亮区域永远以噪声形态出现**——这把 OOD 推断钉成了代码实锤。

## 下一步计划

### 第一步：验证实验（按序执行，定位贡献比例）

| # | 实验 | 做法 | 状态 | 结果 / 若根因成立的预期 |
|---|---|---|---|---|
| 1 | 砍间接光 | `maxDepth = 1`（PathTraceRendererFeature 上直接改） | ✅ 已验证（2026-09-08） | **实测：亮区抖动仍在，肉眼无法判断是否加剧；暗区退化为纯黑**。解读见下——类型 A 被确认为充分来源 |
| 2 | NEE 注入噪声 | one-sample MIS（灯 50% / 环境 50%），环境=常数偏蓝。详见「修复实现 B」 | ✅ 已验证（2026-09-25） | **实测：亮区抖动明显减轻，但仍有轻微残余**。方向正确——常数环境方差有限，换成真正的 HDR 环境贴图后 importance 采样方差更大，预期残余抖动进一步消除 |
| 3 | 关 jitter | 勾选 `debugDisableJitter`（PathTraceRendererFeature 调试实验区），`GetJitter` 旁路返回零向量 | ✅ 已验证（2026-09-25，maxDepth=1 + 关 jitter） | **实测：亮区几乎不抖；取消勾选后抖动立即恢复**。确认抖动 100% 来自 jitter 相位重采样（类型 A），根因闭环 |

实验 2 实现注意：方向扰动后 shadow ray 必须用同一扰动方向（`traceShadowTransmission(shadowOrigin, toL, dist)` 的 `toL` 即扰动值），保证光照与遮挡一致；扰动幅度可从 0.5°-2° 起步（对应样例 `angularSize` 量级）。

#### 实验 1 结果分析（2026-09-08 已验证）

**实测现象**：`maxDepth = 1` 后亮区抖动仍在；肉眼无法区分是否比 `maxDepth = 8` 时更剧烈；暗区（阴影/背光几何）退化为纯黑。

比「是否加剧」信息量更大的是**「仍在」**：

1. **maxDepth=1 下亮区输入已 100% 无蒙特卡洛噪声**：单灯 NEE 全链路确定（li=0、`evaluateBSDF` 确定、shadow 确定性遍历）、间接光为 0；首次迭代内 BSDF 采样虽仍消耗随机数，但采样方向不会再有第二次 trace，不影响输出。帧间唯一变化源只剩**类型 A（jitter 相位重采样）**
2. **抖动仍在 → 类型 A 单独足以产生亮区抖动，类型 B（间接噪声）不是必要条件**。若抖动主要来自类型 B，砍掉间接光后亮区抖动应基本消失——实测没有。根因链收窄为：确定性输入（零方差）→ RR 最弱累积 → 相位差异全部透传
3. **「看不出是否加剧」的原因**：移除间接光同时大幅改变图像外观（暗区归零、对比度重分布），主观 A/B 失去共同基准，原预测「显著加重」无法肉眼证实或证伪——后续对比须用定量手段（亮区帧间 RMS 差，见「判断标准」）
4. **暗区纯黑符合预期（修正本文档原预测）**：原预测「暗区退化为纯 ambient 常数」不准确——ambient 仅在 miss（天空）分支生效，阴影/背光几何像素 NEE=0、无间接、无 emissive → radiance 恒 0。纯黑区域零方差零信号，不构成有效对照（本就不抖）

**实验 3 升级说明**：在 maxDepth=1 基础上关 jitter，输入将完全逐帧恒定（每像素每帧同值、G-buffer 恒定、MV=0）。此时若 RR 输出仍抖 → 问题在 RR 内部时域稳定性或其未知内部假设，需另开方向排查；若完全不抖 → 确认抖动 100% 来自 jitter 相位重采样，与实验 2（注入噪声恢复累积）一扣一合，根因闭环。

#### 实验 3 结果分析（2026-09-25 已验证）

**实测现象**：`maxDepth=1` + `debugDisableJitter` 勾选后，亮区几乎不抖；取消勾选（恢复 Halton jitter）后抖动立即回来。

**结论**：根因闭环成立——
1. 关 jitter 后亮区输入逐帧恒定（零方差确定性像素 + G-buffer 恒定 + MV=0），RR 无帧间差异可透传 → 输出稳定
2. 恢复 jitter 后类型 A 相位重采样回归 → 抖动回归
3. 与实验 1（maxDepth=1 仍抖）叠加：**抖动的唯一必要充分条件是 jitter 相位重采样（类型 A）**，类型 B（间接噪声）非必要、RR 内部时域稳定性无问题

**对根因分析的最终印证**：亮区因 NEE 直接光确定性 → 零方差 → RR 弱累积 → 类型 A 相位差异全部透传为抖动。下一步直接进入修复——给平行光加角锥采样使直接光重获蒙特卡洛噪声（同时实现实验 2 的验证与正式修复）。

#### 实验 3 实现说明（2026-09-25 代码已就绪）

- **改动文件**：`Assets/Scripts/PathTraceRendererFeature.cs`
- **改动内容**：在 DLSS 超分区下新增 `debugDisableJitter`（bool，默认 false）调试开关；勾选后 L374 处局部变量 `jitter` 强制为 `Vector2.zero`。
- **覆盖范围**：该局部变量是 jitter 唯一来源，下游全部同源置零——
  1. 原生 pathtrace 采样（camData.jitter → `pathtrace.glsl` `cam.jitterNearFar.xy`，L618）
  2. MotionVector kernel（`JitterOffset`，L666）
  3. 传给 NGX 的 `InJitterOffset`（RR/SR evaluate，L697/L706/L805/L814）
  三者同步为零，保证「采样位置无相位变化」与「NGX 收到零 jitter」自洽配对，不会因错配引入新伪影。
- **运行步骤**：
  1. Inspector 中将 `maxDepth` 改为 1（实验 1 已验证的设置）
  2. 勾选 `debugDisableJitter`
  3. 保持 `dlssMode = RR`、相机静止
  4. 观察亮区是否仍抖
- **回归保护**：开关默认 false，不影响正常管线；SR 分支与 MotionVector kernel 代码未改，SR 静止不抖作为回归基线。验证完成后记得取消勾选。

### 第二步：修复方向（实验确认后择一）

1. ~~**平行光角锥采样**~~：已实现并验证（2026-09-25）→ **实测仍有抖动，已回退**。角锥采样仅在锥内扰动方向，亮区方差注入不足（锥半角 1.5° 时方向扰动幅度小，NdotL/阴影变化有限），RR 未充分恢复累积。回退改动见下。
2. **灯/环境 one-sample MIS**：按样例 L144-159 做 50/50 硬币。环境=常数偏蓝（无环境贴图退化）。 ✅ 已实现（2026-09-25，见下「修复实现」）
3. **多灯场景顺带项**（原次因方案仍然有效，与本根因不冲突）：power-based 加权选灯、NEE/BSDF 贡献 clamp 压 firefly——多灯场景下这些能消除双峰跳变，但对当前单灯抖动无效

#### 修复实现 A（已回退）：方向光角锥采样（2026-09-25）

**实测**：角锥采样后亮区仍有抖动。原因推测：角锥半角需很小（1.5°）才不破坏硬阴影观感，但小锥内方向扰动产生的 NdotL/BSDF 变化幅度有限，亮区帧间方差注入不足，RR 累积未充分恢复。该方案在「不改变硬阴影观感」约束下方差注入太弱；放大角度则软阴影观感变化大。已全部回退（LightManager / pathtrace.glsl / PathTrace.compute 恢复原状）。

#### 修复实现 B（当前）：灯/环境 one-sample MIS（2026-09-25）

**原理**：根因是亮区 NEE 直接光零方差 → RR 弱累积 → jitter 相位差异透传。方案 B 在 NEE 引入 50/50 硬币（采灯 vs 采环境）制造强帧间方差：亮区像素每帧要么得到「直接光（亮）」、要么得到「环境光（暗）」→ 双峰跳变 → RR 判定高方差 → 强累积 → 抖动被时域平均压平。环境光=常数偏蓝 `ambient`（无环境贴图的退化；有贴图后替换为 importance 采样）。这是样例 `gltf_pathtrace.slang` L144-159 的同款机制，直接回到训练分布内。

**实现要点（GLSL 原生路径为主，C# 调参）**：

| 文件 | 改动 |
|---|---|
| `NativePlugin/shaders/pathtrace.glsl` | NEE 块改为 one-sample MIS：`bool sampleLights = rand(seed)<=0.5`；灯分支原逻辑 + MIS 权重 ×2；新增环境分支（uniform 半球采样方向 + BSDF 评估 + shadow ray/AO + `ambient` 辐亮度，乘子 ×4π = MIS×2 × 1/pdf(2π)） |
| `Assets/Scripts/PathTraceRendererFeature.cs` | `ambientColor` 默认改为偏蓝 (0.05,0.10,0.20)，Tooltip 说明其 NEE 环境辐亮度双重用途 |
| `Assets/Shaders/PathTrace.compute` | **未改**（HLSL fallback 保留原确定方向 NEE；RR 用原生 GLSL 路径） |
| `Assets/Scripts/LightManager.cs` | **未改**（已回退角锥采样） |

**关键设计**：
- **环境分支无偏性**：uniform 半球采样 pdf=1/(2π)；乘子 4π = MIS 权重 2 × 1/pdf(2π)。对漫反射：E[环境分支贡献|选中] = (albedo/π)·ambient·E[NdotL]·4π = (albedo/π)·ambient·(1/2)·4π = 2·albedo·ambient；硬币期望 0.5×2·albedo·ambient = albedo·ambient = 真实天空辐照度 ✓
- **shadow ray 一致性**：环境方向同时用于 BSDF 评估与 `traceShadowTransmission(shadowOrigin, toL, 1e34)` → 自然产生 AO/环境遮蔽 + 遮挡噪声。
- **环境辐亮度来源**：复用 `cam.ambientPad.xyz`（即 inspector `ambientColor`），与 miss 分支同源（同为天空辐亮度，语义一致）。
- **影响范围**：环境分支在每个着色点（含间接弹射）触发 → 暗区会被 `ambient` 点亮（此前暗区仅靠 miss 分支获得天空）。这是向样例训练分布靠拢的正确性提升，但需调小 `ambient` 以免暗区过曝。

**调参指引**：
- `ambientColor` 是唯一调参钮：值越大 → 环境分支方差越大（RR 累积越强、抖动越轻）但暗区越亮；值越小 → 暗区越暗但抖动可能残留。在「亮区抖动消失」与「暗区不过曝」间平衡。
- 起步：默认 (0.05, 0.10, 0.20)；若亮区仍抖 → 增大；若暗区过曝 → 减小。
- `ambientColor` 为 inspector 序列化值，改默认不影响已有场景——**需在 inspector 手动设为偏蓝值**。
- 实验完成后记得取消勾选 `debugDisableJitter`（实验 3 诊断开关）。

**部署**：GLSL 需重编译 SPIR-V → 重生 `pathtrace_spv.h` → 重建 DLL（已执行：`glslc` 编译 + `spv_to_header.ps1` 重生 header + MSBuild Release 构建）。新 DLL 在 `NativePlugin/build/bin/Release/VulkanPathTracePlugin.dll`，需复制到 `Assets/Plugins/x86_64/`（Unity 运行时会锁 DLL，需关闭编辑器后复制）。

**验证结果（2026-09-25）**：勾掉 `debugDisableJitter`、恢复 `maxDepth=8`、`ambientColor` 设为偏蓝 (0.05,0.10,0.20) → **亮区抖动比修复前明显减轻，但仍有轻微残余抖动**。方向判定正确——MIS 硬币在亮区制造了「直接光/环境」双峰方差，RR 累积增强。残余抖动原因：常数环境分支的方差仍偏弱——uniform 半球采样 + 常数 `ambient` 使环境分支期望固定，方差仅来自硬币选择与 NdotL/shadow 随机性；而样例训练分布中环境是 HDR 贴图，importance 采样在不同方向返回不同辐亮度，方差显著更大。**下一步：接入真正的 HDR 环境贴图**（见下「第三步」）。

### 第三步：接入 HDR 环境光贴图（下一步计划）

**目标**：用真正的 HDR 环境贴图替换常数 `ambient`，在 NEE 环境分支做 importance 采样 → 环境分支方差大幅上升（不同方向返回不同辐亮度）→ RR 累积更强 → 消除残余抖动。同时获得真实的天空照明、IBL 反射与软阴影半影，彻底回到 RR 训练分布内。

**参考工程**：`D:\personalProject\MyEngineVulkanV2\vk_gltf_renderer`（nvpro_core2 体系）。关键实现位点：

| 参考文件 | 作用 |
|---|---|
| `nvpro_core2/nvvk/hdr_ibl.cpp` `createEnvironmentAccel()` | **CPU 端 alias method**：遍历 HDR 每个纹素，计算 luminance×solidAngle，构建 `EnvAccel{alias, q}` 数组（Veach alias method，L239-293）。返回 `vector<EnvAccel>` 上传为 SSBO |
| `nvpro_core2/nvshaders/hdr_io.h.slang` `EnvAccel` | 结构体 `{uint alias; float q}`，每纹素一个 |
| `nvpro_core2/nvshaders/hdr_env_sampling.h.slang` `environmentSample()` | **shader 端 importance 采样**：`rand3` → alias 选纹素 → 返回 `vec4(radiance, pdf)` + 方向。亮方向被采更频、pdf 正确 |
| `vk_gltf_renderer/shaders/gltf_pathtrace.slang` `sampleLights()` L129-215 | **one-sample MIS**：`lightWeight/envWeight=0.5/0.5`，硬币选灯或环境；两分支均计算对方 pdf 以便 MIS 权重（比当前 URPPT 的简化版更严格） |
| `nvpro_core2/nvshaders/hdr_dome.slang` / `hdr_prefilter_*.slang` | 后续可选：dome 可视化、diffuse/glossy 预滤波 IBL |

**URPPT 落地拆解（建议顺序）**：

1. **HDR 资源接入**
   - **默认贴图**：`Assets/Textures/hdr/default.hdr`（已存在，equirectangular RGBE，≈26MB）。C# 侧加 HDR 贴图引用字段（如 `PathTraceRendererFeature` 或 `LightManager` 上 `[SerializeField] Texture2D` / 或直接传路径），默认指向该文件；空时回退到当前常数 `ambientColor`。
   - 运行时解码为 RGBA32F（可用 Unity `Texture2D.LoadImage` 或 stb_image；`.hdr` 需 RGBE→float 解码）。
   - 经 `NativeBridge` 把像素 + 尺寸传给原生插件（类似现有 `TextureArrayManager` 上传路径）。
   - 原生插件创建 `VkImage`（equirect 2D，RGBA16F/32F）+ `VkImageView`，挂到一个空闲 descriptor binding（当前 pathtrace.glsl binding 0-15 已用，新增一个 binding，如 16）。

2. **EnvAccel 构建（CPU alias method）**
   - 在原生插件移植 `createEnvironmentAccel`（`hdr_ibl.cpp` L330-）：遍历纹素，`solidAngle = 2π/height × cos(θ)`，`luminance = dot(rgb, vec3(0.2126,0.7152,0.0722))`，`pdf_i = lum_i × solidAngle_i / total`，构建 alias 表。
   - 上传 `EnvAccel[]` 为 SSBO（binding 17）。
   - 输入：HDR 像素；输出：`vector<EnvAccel>` + `average`/`integral`（用于 tonemapping/调强）。

3. **shader 端 importance 采样**
   - 移植 `environmentSample` 到 `pathtrace.glsl`（GLSL 化 `hdr_env_sampling.h.slang`）：`rand3` → alias 选纹素 → 球面坐标 → 方向 + radiance/pdf。
   - 改写 NEE 环境分支：用 `environmentSample(envTex, envAccel, rand3, toL)` 替换当前 uniform 半球 + 常数 ambient；`radiance += throughput × bsdf × NdotL × (radiance_over_pdf) × shadowTrans × misWeight`。
   - 灯分支同步补 envPdf 计算（gltf_pathtrace.slang L194-215 的 else 分支），实现严格 MIS（当前简化版未做灯分支的 envPdf，常数环境可忽略；HDR 下需补全）。

4. **miss 分支统一**
   - 光线未命中时，用 `texturesHdr[envTex].SampleLevel(sphericalUV(dir), 0)` 取 HDR 辐亮度替换当前常数 `ambient`（含 envRotation 旋转）。

5. **调参与验证**
   - `envIntensity` 调环境整体强度；`envRotation` 旋转贴图。
   - 验证：亮区残余抖动是否消除；暗区照明是否自然；AO/软阴影是否合理。
   - 回归：SR 分支不受影响。

**预估工作量**：中等偏大——主要在原生插件（EnvAccel CPU 构建 + SSBO + GLSL 采样函数移植），C# 仅资源接入与 bridge。可复用现有 `TextureArrayManager`/`NativeBridge` 上传模式。风险点：alias method 实现正确性（pdf 归一化）、HDR 解码（RGBE/float）、equirect 球面映射精度。

**与当前常数 MIS 的关系**：本次常数 MIS 是 HDR 环境贴图的「退化先行验证」——已证明方向正确。接入 HDR 后，环境分支从「uniform 半球 + 常数」升级为「importance 采样 + 变量辐亮度」，方差注入量级提升，预期残余抖动消除。`ambientColor` 字段可保留为「无 HDR 时的 fallback 常数」。

### 判断标准

- 主观：静止场景录像逐帧对比亮区（灯照直射面）抖动幅度。局限：实验 1 已证明图像外观大改时主观对比不可靠（失去共同基准），仅适用于外观不变的 A/B（如实验 2 前后）
- 客观（可选）：RenderDoc 抓连续 8 帧（一个 Halton 周期），对亮区像素求帧间 RMS 差；实验前后对比
- 回归确认：实验/修复改动仅限 RR 路径输入侧，SR 分支与 MotionVector kernel 不动，SR 静止不抖作为回归基线

## 相关文件

- `NativePlugin/shaders/pathtrace.glsl` —— NEE/seed/jitter（L688-689、L711、L828-861）
- `Assets/Scripts/PathTraceRendererFeature.cs` —— jitter 生成（L140-156）、RR 输入直通（L746-749）
- `NativePlugin/src/vpt_dlss.cpp` —— RR flags/evaluate（L485-495、L859-860）
- `Assets/Shaders/PathTrace.compute` —— MotionVector kernel（L1715-1770，已确认无回归）
- 样例：`D:\personalProject\MyEngineVulkanV2\VulkanPathTracer\shaders\gltf_pathtrace.slang`、`nvpro_core2\nvshaders\light_contrib.h.slang`、`vk_gltf_renderer\src\dlss_wrapper.cpp`
- HDR 环境贴图参考：`D:\personalProject\MyEngineVulkanV2\vk_gltf_renderer`——`build/_deps/nvpro_core2/nvvk/hdr_ibl.cpp`（`createEnvironmentAccel` alias method）、`nvshaders/hdr_env_sampling.h.slang`（`environmentSample`）、`nvshaders/hdr_io.h.slang`（`EnvAccel`）、`shaders/gltf_pathtrace.slang`（`sampleLights` MIS L129-215）
