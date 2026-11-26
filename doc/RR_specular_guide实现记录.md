# DLSS RR specular guide（ray direction + hit distance）实现记录

> 目标：给 DLSS Ray Reconstruction 补上 `pInSpecularRayDirectionHitDistance`——一张 RGBA16F，
> xyz = 镜面射线方向（世界空间、单位向量），w = 该射线自主表面出发命中的世界空间距离（米）。
> 日期：2026-11-26。背景与风险评估见 [ReSTIR_GI_PT接入可行性调研.md](ReSTIR_GI_PT接入可行性调研.md) §12.1。

---

## 1. 结论速览

| 项 | 结果 |
|---|---|
| 状态 | **已实现并自检通过**（着色器编译 + 原生插件重建 + rt_probe `--trace` 全绿 + C# 全量重编通过） |
| 数据通路 | PT raygen 写第 6 张输出图（binding 18）→ NGX 用 `pInSpecularRayDirectionHitDistance` 接收 |
| 视觉影响 | 着色器本身**零额外射线、零额外 ALU**（方向和距离都已在寄存器里），只多一次 8B/像素写入 |
| ⚠ 但有一处**前置改动** | 见 §4：原来的 `pd = 1-metallic` 让**介质表面永远不采样镜面 lobe**，guide 会是空数据。已改为**无条件**的 Fresnel 加权（管线内基础行为，不暴露开关） |
| 未在 Unity 里验证 | 见 §6 的 A/B 清单；最需要确认的是 §3.3 的坐标系假设 |

---

## 2. 数据通路

```
pt.slang rgenMain
  ├─ 进主循环前：specGuideImg[像素] = 0            ← 每帧先清（防止残留上一帧的距离）
  ├─ depth 0：命中 p → NEE → sampleBSDF 选中镜面反射 lobe
  │            ⇒ specPending = true; specDir = newDir
  ├─ depth 1：TraceRay 拿到 hitT
  │            ⇒ specGuideImg[像素] = float4(specDir, hitT 或 0)
  └─ 之后照常弹射

vpt_dispatch.cpp  binding 18（STORAGE_IMAGE）→ s_OutputViews[5]（Unity RT: ARGBHalf）
  → dispatch 结束 barrier → SHADER_READ_ONLY_OPTIMAL

vpt_dlss.cpp  s_SpecGuideView → NVSDK_NGX_Resource_VK
  → rrEval.pInSpecularRayDirectionHitDistance = &specGuideRes
```

**为什么方向和距离打包在同一张图**：`pInSpecularRayDirectionHitDistance` 就是「方向 + 距离」的合并参数
（字段定义见 `third_party/ngx/include/nvsdk_ngx_helpers_dlssd_vk.h:83`）。
只给 `pInSpecularHitDistance` 的话，RR 得假设反射方向是「视线关于法线的镜面反射」才能反推虚拟命中点，
粗糙面必然偏；方向本来就在寄存器里，**多给 12 字节不花钱**，所以不采用「只给距离」的退化方案。

---

## 3. 语义约定（改之前先读）

### 3.1 单位与取值

| 字段 | 含义 | 约定 |
|---|---|---|
| xyz | 镜面射线方向 | 世界空间、单位向量 |
| w | 命中距离 | 世界空间欧氏距离（米），沿射线方向；**不是视空间 Z、不是深度值** |
| 整像素 | — | `float4(0,0,0,0)` = 本帧该像素没有镜面几何信息 |

**为什么 miss 写 0 而不是 `1e30` / `farPlane`**：0 让虚拟命中点退化到主表面本身，等价于
「退回主表面重投影」——这正是没接 guide 之前的旧行为，是最保守的失败姿势。
写一个大数会让 RR 把反射点投到无穷远，反而可能产生错误的重投影。

### 3.2 覆盖范围

- 只写 **s == 0**（与 G-buffer 一致）：`spp > 1` 时 guide 只反映第 0 个 sample 的镜面弹射。
- 只写 **depth == 0 → depth 1** 这一跳：即「主表面出发的第一次镜面反射」。
  `maxDepth == 1` 时循环到不了 depth 1，**guide 恒为 0**（等于没接）。
- **透射材质（玻璃）整体排除**：折射方向不是镜面反射方向，RR 会按镜面反射去反推虚拟点 → 反射错位。
  这类像素保持 0。

### 3.3 坐标系是**假设**（唯一需要实测的语义）

NGX 头文件与集成指南都没有写 `pInSpecularRayDirection` 的坐标系。本次按 **world space** 实现。
若 RR 实际期望 view space，症状是**反射整体错位/撕裂**（不是「略糊」）。
真机若发现该症状，改动只有一处：

```slang
// pt_common.slang / pt.slang 里写 guide 的地方
specGuideImg[c] = float4(mul((float3x3)cam.viewMatrix, specDir), dist);
```

（当前 UBO 只传了 `viewInv`，需要再加一个 `viewMatrix` 字段，或直接用 `transpose((float3x3)cam.viewInv)`。）

---

## 4. 实现中发现的问题：介质表面根本不会采样镜面 lobe

这是本次改动最值得记的一条。

原 `sampleBSDF`（`pt_common.slang`）用：

```slang
float pd = 1.0f - metallic;   // pd = 选中"漫反射"的概率
if (r < pd) 余弦采样; else GGX 采样;
```

- `metallic = 1` → `pd = 0` → 永远镜面（金属正常）；
- `metallic = 0` → `pd = 1` → **永远漫反射**。介质表面（本项目绝大多数材质）**一次都不会采样镜面 lobe**。

后果有两个：

1. 镜面反射只能靠「余弦方向碰巧落进 GGX 瓣」来估计 → 低粗糙度表面噪声极大（firefly 源）；
2. **RR 的 specular guide 没有任何镜面弹射可写**——照着原样实现，guide 会恒为 0，等于死代码。

### 处理方式

`pd` 直接改为 **无条件的** Fresnel 加权（没有开关：这属于管线内的基础行为）：

```slang
float f0    = dot(specularF0(albedo, metallic), float3(1.0f / 3.0f));
float fr    = f0 + (1.0f - f0) * pow(1.0f - NdotV, 5.0f);
float pSpec = max(metallic, (1.0f - metallic) * clamp(fr, 0.0f, 1.0f));
float pd    = 1.0f - clamp(pSpec, 0.0f, 1.0f);   // 选中"漫反射"的概率
```

**为什么这是安全的**：`pd` 只是混合分布的**选样概率**，而 pdf 用的正是同一个混合分布
（`pdf = pd*pdf_diffuse + (1-pd)*pdf_specular`），所以

> 改 `pd` 只改**方差**，不改**均值**。

它不会让画面变亮/变暗，也不会破坏无偏性；它改变的是「噪声分布长什么样」——而这恰好是本项目
最敏感的东西（见 `doc/DLSS_RR亮区抖动分析.md`），所以这次是无条件启用（不暴露 Inspector 开关）。

**顺手保住的细节**：随机数的**消费顺序**没变（`r = rand(seed)` 仍在 `createONB` 之前、仍在 `pd` 计算之前）。
否则整条路径的 RNG 流会平移，画面会整体变化，与历史版本无法逐像素对比。

> 遗留的正式解法（未做）：主顶点拆成「漫反射路径 + 镜面射线」两条独立路径。
> 那能让 guide **每帧每像素**都有值（当前介质只在 ~4% 概率下命中镜面 lobe，guide 是稀疏的），
> 但它会重写主顶点的着色结构，风险和本次改动不是一个量级。见 §7。

---

## 5. 改动清单

| 文件 | 改动 |
|---|---|
| `NativePlugin/shaders/pt_common.slang` | binding 18 `specGuideImg`；`CameraUBO.ambientPad.w` 语义注释；`sampleBSDF` 增加 `out uint lobeOut` + Fresnel 加权 `pd` |
| `NativePlugin/shaders/pt.slang` | 每帧先写 0；`specPending/specDir` 状态；depth 1 写 guide；`bsdfLobe` 传递 |
| `NativePlugin/src/vpt_dispatch.cpp` | 输出图 5→6（`s_OutputViews` / `kOutputFormats` / `s_LoggedFormat` / `s_ViewRecreateCount` / `s_OutThrottle` / `s_PtrChangeCount`）；`PrepareDispatch` 多一个 `specGuidePtr`；layout 18→19 binding + pool 配额 5→6；`writes[]` 18→19（guide 占下标 16，env 移到 17/18，保持「已填条目连续」）；两道 barrier 5→6 |
| `NativePlugin/src/vpt_dlss.cpp` | `s_SpecGuideView` 缓存 + `EnsureDLSSImageView` + NGX resource + `pInSpecularRayDirectionHitDistance` |
| `NativePlugin/src/vpt_api.{h,cpp}`、`vpt_dispatch.h`、`vpt_dlss.h` | 两处签名 +1 参数 |
| `Assets/Scripts/NativeBridge.cs` | 两个 P/Invoke + 两个 wrapper；纹理指针缓存槽位重编号（PT 0..9 / DLSS 10..17，数组 16→18） |
| `Assets/Scripts/PathTraceRendererFeature.cs` | `_specGuideRT`（无条件创建，同 albedo 的理由）；`pad` = 标志位；两处传参；Inspector 开关 |
| `Tools/rt_probe/rt_probe.cpp` | 19 binding + 第 6 张图 + 池配额 + 回读与断言；材质改 `metallic=1` 以确定性覆盖镜面路径 |

**规模**：+299 / −113 行（净 +186），不含重新生成的 `pathtrace_spv.h`；其中功能代码约 220 行，其余是 `rt_probe` 的自检（+77）与注释。

---

## 6. 自检结果（命令行，不需要 Unity）

### 6.1 着色器

```
[build_shaders] entry points OK: rgenMain, rmissMain, rmissShadow, rchitMain, rchitShadow, rahitMain, rahitShadow
[build_shaders] SPIR-V : pt.spv (70440 bytes)     ← 原 68388，+2052
OpDecorate %specGuideImg Binding 18               ← spirv-dis 确认
```

### 6.2 rt_probe `--trace`

```
[5] Descriptor set layout (19 bindings)           [ OK ]
[6] vkCreateRayTracingPipelinesKHR (7 entries)    [ OK ]
[8] specGuide: dirPixels=29/64 badDir=0 badDist=0 nonzeroDist=0
    [ OK ] specGuide is finite (no NaN/Inf)
    [ OK ] specGuide direction is unit-length when nonzero
    [ OK ] specGuide distance is non-negative
    [ OK ] specGuide 有方向写出（镜面 lobe 路径打通）
    [ OK ] specGuide 距离在 miss 时为 0（本场景无二次命中）
=== PASS ===   (validation errors: 0)
```

29/64 是「命中三角形并成功发出镜面射线」的像素数（单三角形场景，镜面射线反射出去必然 miss，
所以距离全为 0——这正是 §3.1 约定的 miss 行为）。

> 排查记录：第一次跑出来 `dirPixels=0`，原因是**探针把 RGBA16F 当 float32 读**（4 个 half 被
> 两个 float 解释）。改成 `uint16 + halfToFloat` 后正常。这个坑值得记住：新增半精度回读时
> 一定要走 `halfToFloat`，否则会误判成「着色器没写」。

### 6.3 C# 与原生插件

- `cmake --build NativePlugin\build --config Release` → DLL 重建并自动拷到 `Assets/Plugins/x86_64/`。
- `MSBuild Assembly-CSharp.csproj /t:Rebuild` → 通过（用 Unity 安装目录里的 241 个托管引用做类型检查）。

---

## 7. Unity 侧验收清单（**尚未执行**）

### 7.1 先确认没坏

| 检查 | 判据 |
|---|---|
| 启动日志 | `[VPT] PathTrace RT pipeline initialized (19 bindings, ...)`，且 `shader SPV = 70440`（换过 DLL 要重启 Unity，否则打印的还是旧值） |
| 格式 | 不应出现 `view slot 5 format ... << MISMATCH!` |
| RR | `EvaluateFeature` 仍返回 `0x1`，无 `0xbad0000a`（MissingInput） |
| 画面 | 关掉 `specularGuideFresnelLobe` 后，**静态画面**应与改动前基本一致（差异只在噪声分布；若亮度明显变了 → `pd` 与 pdf 不同步，回退） |

### 7.2 再看有没有用

- **动起来看**：左右平移相机 / 转身，观察**镜面地板、金属、玻璃上的反射**是否还拖影。
  静止时几乎看不出差别——这个改动的收益全在运动时。
- **失败信号**：
  - 反射**整体错位 / 撕裂** → §3.3 的坐标系假设错了（改用 view space）；
  - 高光「粘」在物体上不随视角移动 → 距离/方向搞反了或写成了视空间 Z；
  - 出现新的斑块/boiling → 可能是 Fresnel 开关改变了 RR 的噪声输入，试着关掉对比。
- **RenderDoc**：抓 `_specGuideRT`，检查天空区域 w 为大值、近处物体 w 小、无 NaN；
  方向分量应满足 `|xyz| ≈ 1`。

### 7.3 需要同时记录的数据

`[PT-GPU]` 分段（尤其 `pathtrace` 与 `dlss`）改动前后的对比；开关 `specularGuideFresnelLobe`
开/关两组各录一段 8 帧（一个 Halton 周期）的素材。

---

## 8. 回退方式

| 想撤销什么 | 怎么做 |
|---|---|
| 只撤销「介质采样镜面 lobe」 | 把 `pt_common.slang` 的 `pd` 改回 `1.0f - metallic`（需重编 shader + DLL） |
| 只撤销 guide 本身 | `vpt_dlss.cpp` 里注释掉 `pInSpecularRayDirectionHitDistance` 那一行（RR 回到用主表面 MV） |
| 全部撤销 | `git checkout` 这 8 个文件 + 重跑 `build_shaders.ps1` + 重编 DLL |

---

## 9. 未做 / 下一步

1. **guide 稀疏性**：介质表面只有 ~4% 概率选中镜面 lobe，所以 guide 是“偶尔有值”的。
   RR 的时域累积能吸收这种稀疏，但要让 **每帧每像素** 都有 guide，需要主顶点拆双路径（§4 末尾）。
   这一次先按体检分更小的方案落地。
2. **specular motion vectors**：官方指南认为它比 hit distance 更好（能覆盖折射/曲面镜）。
   代价是多一张 RG16F + 一套镜面命中点的上一帧重投影。等 §7 的 A/B 有结论再决定。
3. **`pInDiffuseHitDistance`**：同一个参数族里的漫反射版本。漫反射是低频信号，收益小，
   但成本也低（复用同一套管线，只多一个通道/一张图），可顺手做。
4. **坐标系实测**：等 Unity A/B 结果，把 §3.3 的假设变成结论。
