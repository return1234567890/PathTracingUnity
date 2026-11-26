# Vulkan PT + DLSS RR 管线接入 ReSTIR GI / ReSTIR PT 的可行性调研

> 调研对象：当前工程（Vulkan Ray Tracing Pipeline 路径追踪 + DLSS Ray Reconstruction）能否、以何种形态接入 ReSTIR GI 或 ReSTIR PT。
> 日期：2026-11-26。范围：只做「接入形态 + 成本 + 风险 + 落地路线」的评估，**不含实现**；文中所有性能数字标注来源（实测 / 文献 / 推算）。
> 前置阅读：`doc/RT_pipeline整体切换可行性调研.md`（后端设计与性能口径）、`doc/DLSS_RR亮区抖动分析.md`（RR 对输入统计量的敏感性，本调研 §7 的核心依据）。

---

## 1. 结论速览

| 项 | 结论 |
|---|---|
| **技术上是否可行** | **可行**。不需要新设备扩展、不需要新 feature：只用 SSBO + push constant + 第二次 `vkCmdTraceRaysKHR`，都在现有 RT pipeline 框架内 |
| **推荐形态** | **ReSTIR GI（只替换漫反射间接光）**，保留现有镜面/透射弹射链路。**不建议 ReSTIR PT** |
| **最大收益** | 把「8 弹射」降到「1 弹射 + 复用」仍有多跳观感 → 每像素 `TraceRay` 从 ~8 次降到 ~2 次（含阴影约 16 → 6~7 次），顺带压掉 firefly |
| **最大风险** | **DLSS RR 明确要求输入采样在时空上「相关性最小」**，而 ReSTIR 的全部价值就来自制造时空相关。两者在原理上互相拆台；已有论文（I3D'23）实测「去掉 ReSTIR 相关性后 RR 质量更好，尽管原始噪声更大」 |
| **收益是否确定** | **不确定**。当前实测渲染分辨率只有 ~568×320（DLSS Balanced → 967×544），PT 段估计只有几毫秒；ReSTIR 省下的绝对毫秒数有限，而多一个全屏 pass + reservoir 带宽 + ALU 会把省下的吃掉一部分 |
| **建议决策** | 先做 **P0/P1 零代码实验**（量基线 + 扫 `maxDepth`），只有确认「PT 段是瓶颈」或「分辨率要上 1080p+/4K」时，才投入 ReSTIR GI |
| **工作量** | ReSTIR GI（diffuse-only，含 temporal+spatial）约 **1100~1700 行**改动，跨 6 个文件 + rt_probe；业余节奏 **2~4 周** |
| **一句话** | 「能做，且比想象中便宜；但在这个分辨率 + RR 已就位的前提下，它更像一个**为了更高分辨率/更多光源预留的性能与质量手段**，而不是当下立刻能变现的优化。」 |

---

## 2. 先对齐三个概念（否则后面的取舍讲不清）

### 2.1 ReSTIR 不是降噪器

这是本调研最重要的一句话，因为它直接决定了「RR 已经在了，还要不要 ReSTIR」：

- **RR 是滤波（filter）**：拿到一张 1spp 的噪声图，用神经网络在**图像空间**做时空重建。它降低的是「看得见的噪声」，代价是细节、lag、以及被网络误判时的鬼影。
- **ReSTIR 是估计器（estimator）**：在**样本空间**做重要性重采样，让每帧的「有效样本数 M」通过时空复用远大于实际发射的射线数。它降低的是**方差本身**——这是任何降噪器都做不到的（降噪器只能在已有方差上做取舍）。

所以两者**理论上互补**，不冲突。冲突发生在工程层面：RR 的训练分布假设输入是**独立白噪声**，而 ReSTIR 的输出是**时空强相关**的（相邻像素共享同一个次级采样点、同一像素的 reservoir 跨帧复用）。见 §7。

> 本项目历史上已经踩过一次「输入分布与训练分布错配」的坑：为了消除亮区抖动，最后是靠在 NEE 里加 50/50 灯/环境硬币**人为注入噪声**，让 RR 恢复强累积（`doc/DLSS_RR亮区抖动分析.md`，结论「注入逐帧蒙特卡洛噪声使 RR 恢复强累积」）。ReSTIR 做的事情方向正好相反——**它在降低方差**。这条历史是本调研对 ReSTIR 收益持保守态度的主要理由。

### 2.2 三个 ReSTIR 变体，成本差一个数量级

| | **ReSTIR DI** | **ReSTIR GI** | **ReSTIR PT** |
|---|---|---|---|
| 复用对象 | 光源样本（`(light, point)`） | 次级顶点 `x_s` + 一张「x_s 出射辐亮度」 | 整条路径（重连顶点 + RNG seed，靠 replay 重建） |
| 每像素存储 | ~4~8 B | ~48~72 B | **88 B/path**（GRIS 官方数字） |
| 需要重连 / Jacobian | 不需要 | 需要重连（**实现常省掉 Jacobian，有偏**） | 需要完整 shift mapping + 雅可比 + lobe tag |
| 覆盖的 lobe | 只直接光 | **只漫反射间接光** | 漫反射 + 光泽 |
| 相对 PT 的成本 | 更低 | ~1/2~1/3 射线数 | **~3×**（PT 4.4 ms → ReSTIR PT 13.8 ms，同场景对比） |
| 本项目价值 | 低（只有点/方向光，数量少） | **中~高** | 负 |

数据来源：GRIS 的 88 B/path 与 3× 成本见 [GRIS, TOG 41(4) 2022](http://cemyuksel.com/research/papers/sig22_GRIS.pdf) 与 [Area ReSTIR, SIGGRAPH 2024](https://research.nvidia.com/labs/rtr/publication/zhang2024area/zhang2024area.pdf) 的同场景对比表；ReSTIR GI 的 1spp 收益（MSE 降 9.3×~166×）见 [ReSTIR GI, HPG 2021](https://onlinelibrary.wiley.com/doi/10.1111/cgf.14378)。

**引用更正（重要）**：常被写成 "ReSTIR PT, Lin et al., SIGGRAPH 2023" 的引用是错的。ReSTIR PT 首次出现在 **GRIS: "Generalized Resampled Importance Sampling: Foundations of ReSTIR", Lin, Kettunen, Bitterli, Pantaleoni, Yuksel, Wyman, ACM TOG 41(4), SIGGRAPH 2022**。后续文献一律引作 "ReSTIR PT [Lin et al. 2022]"。见 [作者论文页](https://dqlin.xyz/publications/index.html)。

### 2.3 「ReSTIR GI 只有一跳，为什么能有多跳观感」

因为复用把**别的像素的次级点**借过来当自己的间接样本。一个像素本帧只打了 1 条次级射线，但它的 reservoir 里可能装着从邻居（以及邻居的邻居、以及历史）传过来的、来自完全不同路径的 `x_s`。有效样本数 M 靠时间累积可以到几十，等效「几十条间接路径」——观感接近 2~3 跳，但**不是**真的多跳（真正的多跳仍需 ReSTIR PT）。

这一点决定了下面的设计原则：**ReSTIR GI 不能替代弹射，它是「用复用换掉 2~N 跳的漫反射部分」**。镜面链、透射链、体积链仍要走自己的弹射。

---

## 3. 项目现状：决定改动成本的那几条事实

### 3.1 一帧的完整数据流（当前）

```
C#  PathTraceRenderPass.Execute (Assets/Scripts/PathTraceRendererFeature.cs:355)
 ├─ EnsureRTs(w,h,outW,outH)                          低分辨率 RT（DLSS 倍率精确匹配）
 ├─ 相机 UBO 打包（viewInv/projInv/pos/jitter/near-far/frame/maxDepth/ambient/env）
 ├─ NativeBridge.PrepareDispatch(...)                  Phase 1：TLAS + 18 binding 描述符 + UBO
 │      └─ cmd.IssuePluginEvent(EventPathTrace)
 ├─ MotionVector compute（DlssAux.compute）            gbuffer0 → MV + 线性深度
 ├─ NativeBridge.PrepareDLSSRRDispatch(...) + EventDlss NGX RR Evaluate
 └─ cmd.Blit(_dlssOutputRT → cameraColorTarget)

Native RenderCallback (vpt_dispatch.cpp:946)
 ├─ RecordGPUSkinningIfNeeded / RecordBLASRefitIfNeeded / RecordTLASUpdateIfNeeded
 ├─ Barrier 1：UBO host→shader，5 张图 → GENERAL
 ├─ vkCmdTraceRaysKHR  ← 一次调用跑完整张图的完整弹射循环
 └─ Barrier 2：5 张图 → SHADER_READ_ONLY
```

raygen `rgenMain`（`NativePlugin/shaders/pt.slang:37`）**一人包办全部**：相机射线 → 弹射循环 → 材质/纹理/BSDF/BTDF/体积/环境采样 → NEE → 写 5 张输出图。CH 只回写 `{hitT, instance, prim, bary}`（延迟着色），阴影走 group 1 的 any-hit 累积透射率。

### 3.2 与 ReSTIR 直接冲突/缺失的六件事

| # | 现状 | ReSTIR 需要什么 | 改动面 |
|---|---|---|---|
| 1 | **描述符集固定 18 binding**，池按 1/5/1/6/5 精确配额（`vpt_dispatch.cpp:192-217`、`:312-324`） | +3~4 个 SSBO（reservoir×2~3、primary surf） | 布局、poolSizes、`writes[]`、`kRtDescriptorStages` 四处必须同步改 |
| 2 | **UBO 只有 208B**，没有 `prevViewProj`（`vpt_common.slang:76-89` / `vpt_dispatch.cpp:129-142`） | 上一帧 world→clip 矩阵做重投影 | UBO 扩到 ~304B，C++/Slang 两侧逐字段对齐（std140，mat4 需 16B 对齐） |
| 3 | **没有 push constant**（pipeline layout 无 `VkPushConstantRange`，`vpt_dispatch.cpp:220-225`） | 一次模块、多次 TraceRays，用 pass 索引切换阶段 | 新增 pipeline layout 的 push constant range + Slang `[[vk::push_constant]]` |
| 4 | **SBT 只有 1 条 raygen 记录**（`vpt_rt.cpp:163-172`，`raygenRegionSize = raygenStride`） | 要么多 raygen 入口，要么单入口 + push constant | 推荐后者：SBT 完全不动 |
| 5 | **没有「上一帧主表面」G-buffer**：`gbuffer0/1` 每帧被整图覆写，且被 RR 同帧消费 | 历史校验（位置/法线/材质/深度） | 新增持久 SSBO 或让 reservoir 自带 owner 身份（见 §5.4） |
| 6 | **没有任何 reset 通路到 native**：`_hasPrev` 只喂给 DLSS 的 `InReset`（`PathTraceRendererFeature.cs:589`） | 场景重建/相机切换/分辨率变化时必须清 reservoir | `VPT_CameraData` 加 reset 字段；native 侧监听 w/h 变化 |

**唯一的好消息**：插件已经具备自建 device-local buffer 的能力与先例（`vpt_scene.cpp:151 CreateBufferResource`），并且已经有 4 槽环形缓冲的成熟模式（TLAS ring `vpt_scene.cpp:95`、描述符 ring `vpt_dispatch.cpp:36`、关节矩阵 ring）。reservoir 的环形缓冲可以照抄这套。

### 3.3 分辨率与性能口径（决定收益量级）

`doc/RT_pipeline整体切换可行性调研.md:323` 记录：**渲染分辨率约 568×320（DLSS Balanced 1.7× → 967×544）**，PT 段「估计只有几毫秒（无实测基线）」。

- 568×320 = **18.2 万像素**。按 `maxDepth=8`、spp=1 计，每像素 8 次弹射 `TraceRay` + 8 次 NEE 阴影 `TraceRay` ≈ **16 次/像素** → 全屏 ≈ **290 万次 TraceRay/帧**。
- 这个量级下，PT 段大概率在 2~4 ms（推算，非实测）。ReSTIR 省 2/3 的射线 → 大概省 **1~3 ms**，但同时增加：1 次全屏 `TraceRays` 启动（spatial pass）、每像素 ~64B×2 的 reservoir 读写（18 万像素 → ~24 MB/帧带宽，可忽略）、reservoir 数学 ALU。
- **结论：在 568×320 下，收益与新增固定开销在同一量级，净收益可能接近 0。** 分辨率越高越划算（见 §6 的表）。

> ⚠ 口径提醒：本工程**从未跑出过 `[PT-GPU]` 分段日志**（`Logs/VPT-trace-csharp.log` 里 0 条），即 PT 段的真实毫秒数至今未知。P0 第一件事就是把它量出来，否则整个收益判断都是空的。

### 3.4 RR 的输入契约（现状）

`vpt_dlss.cpp:852-887` 当前喂给 RR 的：color / depth(线性) / motion(MVLowRes, 已×w,h 转像素空间) / normals+roughness(packed) / diffuseAlbedo / specularAlbedo / jitter(取负) / view·proj 矩阵 / 曝光固定 1。

**没有喂 specular hit distance，也没有 specular motion vectors。** 而 [RR 集成指南 §4.1.8/§4.1.9](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md) 把「specular MV **或** specular hit distance」列为二选一的**必需项**。当前能跑，但高光稳定性天然吃亏（见 §12 的顺带建议：这一项的性价比远高于 ReSTIR）。

另外 `gbuffer1` 存的是 **geoNormal**（`pt.slang:190`），不是扰动后的着色法线；RR 拿到的是几何法线。这不影响 ReSTIR，但重连算 BSDF 时要明确用哪个 N。

---

## 4. 接入形态对比（先选形态，再谈实现）

| 形态 | 做法 | 射线数/像素 | 观感 | 与 RR 的冲突 | 工作量 | 结论 |
|---|---|---|---|---|---|---|
| **A. 只降 `maxDepth`** | 改 Inspector（8 → 2/3），零代码 | 8→~4 | 多跳感变弱，暗部偏死 | 无（反而更噪 → RR 更爱） | **0** | **必做**，是其它方案的对照基线 |
| **B. ReSTIR DI** | 只重采样直接光 | 不变 | 多光源/大光源下噪声↓ | 有（相关样本） | 中（~600 行） | 本项目只有点/方向光，**价值低**；但可当基础设施验证 |
| **C. ReSTIR GI（diffuse-only）** | 漫反射间接走 reservoir，镜面/透射保留弹射 | 16→6~7 | 间接噪声大降，可用更少弹射 | **强冲突**（§7） | 中大（1100~1700 行） | **推荐形态** |
| **D. ReSTIR PT** | 整路径复用 + GRIS shift | 16→~8 但每样本贵 3× | 最正确、支持光泽 | 冲突 + 实现复杂度爆炸 | 极大（3000+ 行） | **不建议** |
| **E. DDGI / 辐照度探针** | 世界空间探针网格，非屏幕空间 | 1（仅主射线） | 低频 GI，会漏光 | 弱（探针数据是低频的，RR 喜欢） | 大（自己一套探针管线） | 备选，另一条路 |

**为什么 D 不建议**：ReSTIR PT 需要 (a) 随机数 replay 复现子路径、(b) 重连雅可比、(c) lobe/technique tag 让路径空间与 primary sample space 一一对应；reservoir 只存「一个重连顶点 + RNG seed + 入射辐亮度」，其余靠 replay。这套东西要嵌进当前「raygen 内联弹射、不留任何路径状态」的结构里，等于重写着色器架构。而且成本是 PT 的 3 倍——在一个 PT 段只有几毫秒的项目里，省射线才是目的，D 反而更贵。

**为什么选 C 而不是 B**：本项目的 NEE 光源只有点/方向光（`LightManager.cs:87-99`，Spot/Area 都退化成点），且**自发光几何体完全不参与光源采样**——只有被光线打中才贡献。ReSTIR DI 只能优化「光源列表里那几盏」，收益上限低。而 ReSTIR GI 的复用会**自动发现自发光路径**（某个像素的 reservoir 打到了自发光面，邻居就免费得到这个贡献），这对教堂场景（烛光/灯箱/彩色玻璃窗透光）是实打实的质量提升。

---

## 5. 技术设计：ReSTIR GI 落到这套管线里长什么样

### 5.1 管线结构：两种 pass 切法

ReSTIR GI 的标准流程是四步：**initial candidates → temporal reuse → spatial reuse → resolve**。其中 spatial reuse 要读**邻居本帧刚写出的** reservoir，所以必须在 temporal 之后加一道 barrier。

两种实现切法：

| 切法 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **多 raygen 入口** | SBT 扩成 3 条 raygen 记录，`vkCmdTraceRaysKHR` 传不同 raygen 区域基址 | 每个 pass 是独立着色器，寄存器压力低、代码清晰 | 要改 `vpt_rt.cpp` 的 SBT 布局（`raygenRegionSize` 从 1 条变 3 条） |
| **单 raygen + push constant（推荐）** | 1 条 raygen，push constant 里放 `pass`，`TraceRays` 调 2 次 | **SBT 完全不改**；单模块单入口的既有惯例不变 | raygen 内多一层分支；寄存器压力叠加 |

推荐后者。理由：SBT 是这个项目里最容易出「句柄/对齐/区域大小」玄学问题的模块（`vpt_rt.cpp` 里有一整套校验打印），能不动就不动；而 push constant 只是一个 16B 的 pipeline layout 增量。

```
Pass A (init + temporal)：每像素
  1) 主射线（复用现有逻辑）→ p, N, 材质, hitT
  2) 直接光 NEE（保留现有实现，含 MIS 硬币）        ← 不动
  3) 余弦采样方向 → 次级射线 → x_s（若打中透射面/镂空面则本帧无候选）
  4) 在 x_s 做 NEE（灯 + 环境）→ L_o(x_s)           ← 复用现有 sampleLightContribution/environmentSample
  5) 初始化候选 reservoir（M=1, wSum=p̂/p_src）
  6) 重投影到上一帧 + 历史校验 → 合并历史 reservoir
  7) 写 reservoirScratch[x,y] + primarySurf[x,y]（给 pass B 用）
  [Barrier：SSBO 写 → 读写]

Pass B (spatial + resolve)：每像素
  1) 读 primarySurf[x,y]
  2) 在随机旋转的圆盘内取 K 个邻居（4~16），逐个：校验法线/深度 → 用 p̂_cur/p̂_neighbor 加权合并
  3) resolve：contribution = ŵ/M × V(p↔x_s)
     → 写 outputImg（和现有的直接光/镜面贡献相加）
  4) 写 reservoirHistory[curSlot][x,y]（给下一帧 pass A 当历史）
```

### 5.2 与现有弹射循环的接缝：漫反射 / 镜面拆分

这是设计里最容易出错的地方，必须写明白。

当前循环（`pt.slang:82-366`）在第 0 跳用 `sampleBSDF`（`pt_common.slang:292`）按 `pd = 1-metallic` 概率在漫反射/镜面之间选一个 lobe，然后继续往下弹。**ReSTIR GI 只能接管漫反射间接**，因为它假设「从 p 看 x_s 的方向」是可用余弦分布重采样的；光泽 lobe 的 BSDF 在重连方向上会掉到接近 0（课程笔记明确写 "Glossy materials need ReSTIR in half-vector space"，即需要 ReSTIR PT 的 hybrid shift）。

推荐拆分方式（按侵入性递增）：

1. **优先**：第 0 跳把 BSDF 拆成两段——
   - 漫反射间接 → ReSTIR（不再对这个 lobe 发弹射射线）
   - 镜面间接 → 照常弹射，但弹射深度只留 1~2（`specularDepth`，与 ReSTIR 的 `maxDepth` 解耦）
   - 之后所有跳（镜面链上）继续用现有 `sampleBSDF`，因为那些路径不需要复用
2. **退一步（P2a 阶段够用）**：直接判定「该像素的漫反射权重是否占优」（`luminance(diffuse)·(1-metallic)` 与镜面权重比），占优则 ReSTIR 接管间接、不弹射；否则整像素走老循环。

代价账（每像素 `TraceRay` 次数，`spp=1`）：

| 配置 | 主射线 | 次级 | NEE@p | NEE@x_s | 可见性 | 镜面链 | 合计 |
|---|---|---|---|---|---|---|---|
| 现状 `maxDepth=8` | 8（含弹射） | — | 8 | — | — | — | **16** |
| A：`maxDepth=2` | 2 | — | 2 | — | — | — | **4** |
| C：ReSTIR GI + 镜面 1 跳 | 1 | 1 | 1 | 1 | 1 | 1+1 | **7** |
| C'：ReSTIR GI + 无镜面链（纯漫反射场景） | 1 | 1 | 1 | 1 | 1 | — | **5** |

> 注意：`maxDepth=8` 时最后一跳仍然做 NEE 再采样 BSDF 然后退出循环（俄罗斯轮盘在 `pt.slang:359-365` 才启用，`depth>3`），所以「8 弹射」实打实是 8 次 `TraceRay` + 8 次阴影 `TraceRay`。这也意味着**形态 A（`maxDepth=2`）能把射线数从 16 砍到 4，成本为零**——这是 ReSTIR 必须先打败的基线。

### 5.3 Reservoir 数据结构（64B/像素）

```slang
// 与 C++ 侧无对应结构（纯 GPU 内部），但 C++ 需要按同样 sizeof 分配 buffer
struct ReservoirGI
{
    float3 xPos;        // 12B  次级采样点世界坐标 x_s
    uint   xNormalPk;   //  4B  x_s 法线，八面体编码 2×16bit
    float3 radiance;    // 12B  L_o(x_s)：x_s 朝 p 的出射辐亮度（含 x_s 的 NEE + 自发光）
    float  pHat;        //  4B  被选中样本在本像素的目标函数值（下一帧算 p̂_cur/p̂_prev 的分子分母都要）
    float  wSum;        //  4B  Σ w_i
    uint   M;           //  4B  有效样本数（工程上应封顶 16~32）
    uint   flags;       //  4B  bit0=valid, bit1=次级点在透射面上, bit2=经历空间复用
    uint   ownerSurfId; //  4B  本像素主表面的身份（instanceIndex×N + primIndex，或 materialID 哈希）
    float  ownerDepth;  //  4B  本像素主表面的视空间线性深度（历史校验用）
    float  pad[2];      //  8B  对齐到 64B
};
// sizeof = 64 B
```

**为什么需要 `ownerSurfId/ownerDepth`**：重投影时的历史校验要回答「上一帧这个像素看到的表面，是不是当前帧这个像素看到的同一个表面」。把「reservoir 自己所属像素的表面身份」写进 reservoir，就不用再维护一张独立的「上一帧主表面」G-buffer —— 省一个 binding、省一次带宽。代价是 8B/像素。

**`pHat` 为什么必须存**：跨像素/跨帧复用时的标准权重是
`w_prev = ŵ_prev / M_prev × p̂_cur(x_s) / p̂_prev(x_s)`。
`p̂_prev(x_s)` 是「上一帧那个像素对同一个 x_s 的目标函数值」，不可能重算（上一帧的材质/法线已经不在了），**只能存**。

显存账（64B/像素，含 scratch + 历史环）：

| 渲染分辨率 | 对应输出（Balanced 1.7×） | 单槽 | 3 槽（最小） | 5 槽（与现有 ring 4 惯例对齐） |
|---|---|---|---|---|
| 568×320（当前实测） | 967×544 | 11.6 MB | **35 MB** | 58 MB |
| 1129×635 | 1920×1080 | 45.9 MB | **138 MB** | 230 MB |
| 2258×1270 | 3840×2160 | 183.5 MB | **551 MB** | 918 MB |

> 4K 下 64B/像素**不可接受**。真上 4K 时要么压到 32B（`xPos` 用 fp16 相对相机坐标、`radiance` 用 RGB9E5、`M` 压 uint16），要么把 reservoir 放到**半分辨率**（间接光是低频的，半分辨率 reservoir + 双线性 gather 是常见做法）。这条在 P0 阶段就要定，因为它影响 §5.1 的 pass 结构。

### 5.4 重投影与历史校验

```slang
// 1) 世界坐标 → 上一帧像素
float4 prevClip = mul(cam.prevViewProj, float4(P, 1.0));
if (prevClip.w <= 0.0) → 无历史
float2 prevUV = prevClip.xy / prevClip.w * 0.5 + 0.5;
if (任何分量越界) → 无历史
uint2 prevPix = uint2(prevUV * float2(w, h));        // 注意：不是 storeCoordFromPixel
```

⚠ **Y 翻转陷阱**：所有写 Unity RT 的地方都要 `storeCoordFromPixel`（`pt.slang:27-30`）翻 Y；但 reservoir 是 **SSBO，按 `DispatchRaysIndex().xy` 索引，绝对不能翻**。这个坑一旦踩了，表现为「静止时画面正常，一动就整片鬼影/漏光」，且极难定位。建议在 reservoir 索引处写一行注释 + 在 rt_probe 里加一条「静态相机两帧 reservoir 完全一致」的断言。

校验条件（全部通过才复用）：

| 条件 | 阈值起点 | 说明 |
|---|---|---|
| `ownerSurfId` 相同 | 严格相等 | 不同物体直接拒 |
| 深度相对差 | `abs(dPrev - dCur) / dCur < 0.1` | 起点宽松，按场景调 |
| 法线点积 | `dot(nPrev, nCur) > 0.9` | 需要当前像素存法线（pass A 里有） |
| `M_prev > 0` 且 `flags.valid` | — | 空历史直接跳过 |

**注意 3.4 的坑**：`gbuffer1` 里存的是 geoNormal；而 reservoir 的 `xNormal` 应该是**着色法线**（重连时算 BSDF 要用扰动后的 N）。两者不要混用。

### 5.5 目标函数、源 pdf、权重（写清楚，避免实现时各写各的）

设当前像素主表面 `p`（位置、着色法线 `n_p`、材质），reservoir 里的次级点 `x_s`。

**目标函数**（不含可见性，可见性在 resolve 时才验）：

```
p̂(x_s) = f_s(p, ω=p→x_s) · |cos θ_p| · L_o(x_s)
```

**源 pdf**（候选的生成分布）：方向用余弦半球采样，转成「对 x_s 的立体角」：

```
p_src = cos θ_p / π
```

（若改成 BSDF 采样则 `p_src` 换成 BSDF pdf，但会削弱空间复用的稳定性——ReSTIR GI 原论文也建议 GI 用余弦/bounded pdf。）

**候选权重**：`w_i = p̂(x_s,i) / p_src,i`

**合并（无论是初始、时间、空间）**：加权 reservoir 采样
```
wSum = Σ w_i ;  M = Σ M_i ;  W = wSum / (M · p̂_chosen)
```

**最终着色贡献**：
```
contribution = (wSum / M) · V(p ↔ x_s)
```
`V` 用现有的 `traceShadowTransmission`（`pt_common.slang:617`），它会自动处理玻璃透射率——见 §5.7 的偏差说明。

**光晕/萤火虫抑制**：对 `W` 或对 `p̂/p_src` 设上界。这等价于给雅可比设上界，**必然引入能量损失**（文献已明确指出这一点）；建议把上界做成 UBO 参数，P0/P3 阶段用能量对照实验定值。

### 5.6 有偏性：必须诚实写在文档里

ReSTIR GI 在**跨像素复用 + 重连**时省略了 shift mapping 的雅可比 `|∂ω_y/∂ω_x|`，因此：

- 它是**有偏**的；
- 偏差在「相邻像素几何相近」时小，在**大平面掠射角、曲面边缘、深度不连续处**大；
- 典型表现：能量偏暗（样本被「搬」到不合适的立体角上）、接触阴影变浅；
- GRIS 的结论更狠：这类复用可能「无偏但收敛到错误结果」。

**验收必须包含能量对照**：纯漫反射 Cornell-box 式场景 + 单点光，8 弹射 PT 高 spp 参考 vs ReSTIR GI 收敛后，逐区域平均亮度偏差 **< 3%** 才算过（§10）。

### 5.7 与现有材质的边界（本项目特有）

当前 PT 有一套相当复杂的透射/体积状态机（`pt.slang:174-198`、`:299-350`，`pt_common.slang:434-569`）。ReSTIR GI 的假设（单一半球、从 p 直线看得到 x_s）在这些情况下不成立：

| 情况 | 处理 | 理由 |
|---|---|---|
| 主表面是 `MAT_IS_TRANSPARENT` / `MAT_IS_VOLUME` | **该像素退出 ReSTIR**，整像素走老循环 | 透射 lobe 不可用余弦重采样；且 p 可能在玻璃内部（`isInsideVolume`） |
| 次级点 x_s 落在透射/体积面 | **丢弃该候选**（`flags` 不置 valid） | 从 p 到 x_s 的直线会穿过玻璃，而真实路径是折射的，重连几何上错误 |
| 次级点 x_s 落在镂空（alpha cutout）面 | 允许（any-hit 已保证是接受过的微面） | 但 resolve 的可见性射线会重新走一遍 alpha 判定，可能得到不一致结果 → 建议在 §10.2 的伪影清单里列一条 |
| 主表面是 alpha cutout | **允许**（这是漫反射表面） | — |
| 自发光面 | 作为 x_s 时 `L_o` 含 emission；作为 p 时照常 | 这正是 ReSTIR GI 的价值点之一（自发光不参与 NEE，靠复用扩散） |

**玻璃透射率进入可见性射线**（`traceShadowTransmission` 返回的是直线路径上的彩色透射率）在物理上近似「薄玻璃」，会给 ReSTIR 引入额外偏亮（漏光）。项目现有阴影就已经是这套近似，**不算新增偏差**，但要写进文档避免以后误判。

### 5.8 走一遍：一个像素在 ReSTIR GI 下经历什么

以渲染分辨率 568×320、像素 `(x,y) = (200,120)`、第 137 帧、材质是哑光墙面为例。

```
C# 侧
  · jitter = Halton(137%8+1) 的两个分量
  · prevViewProj = 上一帧 proj*view（本来就在算，给 MV 用，PathTraceRendererFeature.cs:402）
  · 打包 UBO（新增 prevViewProj / restirParams / reset=0）
  · PrepareDispatch → 描述符指向 reservoir 历史环的槽位 (137 % 2)

Pass A（reservoir 槽 hist[1] 读、scratch 写）
  1. 主射线：origin=相机，dir=由 jitter 抖动后的像素中心决定
     → 命中墙：p=(3.2, 1.1, -5.4), Ngeo=N=(0,0,1), 材质 albedo=0.62/rough=0.8/metallic=0
  2. 直接光 NEE：硬币掷出「灯」→ 采样 1 盏点光 → 阴影射线（被柱子挡一半）
     → radiance += 0.31（这是现有逻辑，不动）
  3. 余弦采样方向 ω_s → 次级射线
     → 命中地板 x_s=(2.1, 0.0, -4.2)，法线 (0,1,0)，材质 albedo=0.7
     → 若命中的是玻璃 → flags 不置 valid，本帧该像素无候选（退回老循环）
  4. 在 x_s 做 NEE：采样灯 → 阴影射线 → L_o(x_s) = 0.44
  5. 初始候选：p̂ = f_s(p,ω_s)·cosθ_p·L_o = 0.19 ; p_src = cosθ/π = 0.27
     → wSum = 0.70, M = 1, p̂_chosen = 0.19
  6. 重投影：prevClip = prevViewProj·p → prevUV → prevPix=(199,120)
     校验 hist[1][199,120]：ownerSurfId 相同、深度差 0.8%、法线点积 0.999 → 通过
     w_prev = ŵ_prev/M_prev · p̂_cur/p̂_prev = ... 合并后 wSum = 12.4, M = 18
  7. 写 scratch[x,y]（含 ownerSurfId / ownerDepth）+ primarySurf[x,y]

  [VkBufferMemoryBarrier：SHADER_WRITE → SHADER_READ|WRITE，RAY_TRACING → RAY_TRACING]

Pass B（scratch 读、hist[0] 写）
  8. 邻居 gather：以 (x,y) 为中心、半径 12px、旋转角由 xxhash32(x,y,137) 决定，
     取 8 个邻居；每个做同样的法线/深度校验 → 再合并 → wSum = 15.1, M = 24
  9. resolve：可见性射线 p→x_s（穿过半边阴影）→ V=1.0
     → contribution = wSum/M · V = 0.63
     → outputImg[x,y] += 0.63（与步骤 2 的直接光相加）
 10. 写 hist[0][x,y] 作为下一帧的历史

C# 侧（不变）
  · MotionVector compute 读 gbuffer0（主表面位置，与 reservoir 无关）
  · RR Evaluate：color=outputImg（现在是「直接光 + ReSTIR 间接光 + 镜面链」）
```

几点值得注意：

- 步骤 6 的 `p̂_prev` 用的是 **hist 里存的** `pHat`，不是重算——这就是 §5.3 里那个字段存在的理由。
- 步骤 9 的可见性射线是**每帧每像素一次**，无法跨帧复用（x_s 虽不变，但 p 变了）。
- 整个流程里 **`outputImg` 的语义没变**（还是一张 radiance 图），RR 的接口一行都不用改；变的是「这张图里的间接光是怎么估出来的」。这是本方案侵入性可控的关键。
- 如果 `restirStrength=0`，步骤 3~9 全部跳过，走老循环——**这就是可回滚性的实现方式**。

### 5.9 与其它调研的关系

- 与 `doc/PT体积雾接入可行性调研.md`：ReSTIR GI 与体积散射**正交但互斥**——体积介质的路径不能被「从 p 直线连到 x_s」重连，所以形态 C 里「打在雾里的路径」必须走老循环。如果将来上体积雾，ReSTIR 的覆盖率会进一步下降（雾遮挡的像素越多，可复用的比例越低），这一点在做收益评估时要算进去。
- 与 `doc/RT_pipeline整体切换实现方案.md`：ReSTIR 的第二道 `TraceRays` 会继承该方案里已经踩过的 SBT/描述符环/barrier 经验，**不需要重新趟一遍**。
- 与 `doc/DLSS_RR集成踩坑记录.md`：RR 的输入尺寸/格式/flag 约束对 ReSTIR 完全不变（因为 5 张输出图的语义不变）。

---

## 6. 性能分析（推算，需 P0 实测）

### 6.1 射线数账（已经算过，见 §5.2 表）

现状 16 → 形态 C 约 7 → 形态 C' 约 5。**形态 A（`maxDepth=2`）就已经到 4。**

这个对比很关键：**ReSTIR 的 7 次比 `maxDepth=2` 的 4 次还多**，它换来的是「多跳观感」。所以 ReSTIR 的正确对标不是「现在的 8 弹射」，而是「`maxDepth=2` 的画面」——你得证明前者明显好于后者，才值得多花 40% 的射线 + 1200 行代码。

### 6.2 固定开销

| 项 | 量级 | 说明 |
|---|---|---|
| 第二次 `vkCmdTraceRaysKHR` 启动 | ~0.05~0.2 ms | 全屏 dispatch 的固定成本，与分辨率弱相关 |
| 两道 buffer barrier | 可忽略 | `VkMemoryBarrier` + 一次 `vkCmdPipelineBarrier` |
| reservoir 带宽 | 568×320 下 ~24 MB/帧 | L2/显存带宽的零头 |
| reservoir ALU | 每像素 ~50~100 flop × 2 pass | 与 1 次 BVH 遍历同量级，不能忽略 |
| push constant + pipeline layout 变更 | 一次 | 不影响每帧 |

### 6.3 什么分辨率下才划算

| 输出分辨率 | 渲染分辨率（Balanced） | PT 段推算 | 形态 A 省 | 形态 C 省 | 形态 C 净收益 |
|---|---|---|---|---|---|
| 967×544（当前实测） | 568×320 | 2~4 ms | ~1.5~3 ms | ~1~2.5 ms | **≈0，可能为负** |
| 1920×1080 | 1129×635 | 8~16 ms | ~6~12 ms | ~4~10 ms | **正，值得** |
| 3840×2160 | 2258×1270 | 32~64 ms | ~24~48 ms | ~16~40 ms | **显著正，但显存要压** |

（PT 段推算公式：按 568×320 的 2~4 ms 线性外推到像素数。这是极其粗略的线性假设，实际有 BVH 遍历的相干性变化。）

### 6.4 显存

见 §5.3 表。**568×320 下 35~58 MB 完全无压力；4K 下必须压缩或半分辨率。**

---

## 7. 与 DLSS RR 的交互：本次调研的核心风险

这一节是全文档最重要的部分。结论先说：**ReSTIR 与 RR 在原理上互相拆台，本项目的特殊情况让它更容易拆台。**

### 7.1 官方要求：输入采样必须「时空相关性最小」

[DLSS-RR Integration Guide §3.5](https://media.githubusercontent.com/media/NVIDIA-RTX/Streamline/refs/heads/main/docs/DLSS-RR%20Integration%20Guide.pdf) 原文（转述）：

> DLSS RR **assumes independent samples**，要求采样在**空间与时间上相关性最小**；推荐高质量哈希（xxhash32）+ 白噪声；输入应当能收敛到无噪的 ground truth。**应避免**：棋盘渲染、相关性采样的哈希、屏幕空间抖动、**在算法之间共享采样模式**。

ReSTIR 的每一个设计决定，都是「制造时空相关性」：

- 相邻像素共享同一个 `x_s`（空间复用）；
- 同一像素跨帧复用（时间复用）；
- 一个像素的最终样本来自别的像素的重连——**它就是「共享采样模式」**。

### 7.2 已有实测证据：去相关后 RR 更好，**尽管噪声更大**

Kettunen et al., I3D'23（NVIDIA 参与）明确报告：**去掉 ReSTIR 的相关性后，DLSS-RR 的质量更好**，即使原始噪声更大。([ACM](https://dl.acm.org/doi/fullHtml/10.1145/3610548.3618245))

这条对我们是坏消息：它说明「ReSTIR 降低方差」这件事，在 RR 后面**不一定**转化为画质提升——RR 本来就在做方差缩减（通过时空重建），而我们交给它的是一张「方差已降但结构相关」的图，正好落在它训练分布的边界外。

### 7.3 工业界的真实答案：互相抢饭吃，靠工程手段调停

CDPR + NVIDIA 的 SIGGRAPH 2023 课程里有一页直接叫 **"ReSTIR and Denoiser Eat Each Other's Lunch – But How?"**，给出的对策是：([slides](https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Cyberpunk_2077_Integration.pdf))

- ReSTIR 只做 diffuse（镜面另走 path-traced reflections）；
- 时空复用循环上限 SPP≈65,536（即 **M 必须封顶**，不允许无限收敛）；
- disocclusion boost（新出现区域不能只靠历史）；
- NRD 侧 **pre-blur** 压 ReSTIR 的 outlier。

而 CDPR 的 2.1 补丁说明则从正面描述收益：「Introduced ReSTIR GI – further improves path-traced lighting quality ... **minimizes ghosting**」。([Steam 公告](https://store.steampowered.com/news/posts/?feed=steam_community_announcements&enddate=1701714718))

**所以答案是：可行，但必须「调停」，而且调停的旋钮不在 ReSTIR 侧，在「把 RR 当什么用」这件事上。**

### 7.4 本项目特有的放大器：亮区抖动的历史

`doc/DLSS_RR亮区抖动分析.md` 的结论是：**RR 的时域累积强度按局部统计量自适应**——相对方差低 → 判定信号可信 → 弱累积 → 结构化帧间差异直通输出（亮区抖动）；相对方差高 → 强累积 → 稳定但颗粒。

ReSTIR 恰好会**把间接光的相对方差压下去**。这可能带来两种结果：

1. **好**：间接光本来噪，RR 强累积 → lag/糊；ReSTIR 降方差后 RR 可以少累积、多保细节。
2. **坏**：方差降下去之后，若 ReSTIR 留下的是**结构化残差**（空间复用的 5×5 相关性、disocclusion 处的 M=1 块状、重连偏差的接触阴影缺失），RR 会判定「信号可信」而**原样透传**——表现为**斑块状 blotch、boiling、边界处「非噪声的抖动」**。这类伪影比噪声难看得多，也更难修。

当前工程为了修亮区抖动，已经在 NEE 里放了 50/50 硬币制造方差（`pt.slang:213-215`）。如果 ReSTIR 又把间接光的方差压下去，**这两套机制会在同一个信号上互相抵消**：一个在造噪、一个在去噪、RR 在中间按局部统计量做判断。

### 7.5 三种可能的结局与预判

| 结局 | 概率（主观） | 判定信号 |
|---|---|---|
| ReSTIR 明显更好（间接光更干净、可用更少弹射、RR lag 减少） | 35% | A/B 中把 `maxDepth` 从 8 降到 2，画面无退化且 PT 段 ms 明显下降 |
| 持平（省了射线，但 RR 输出看不出差别） | 40% | PT 段 ms 下降但画质 A/B 分不出 → 只在分辨率提升时才有意义 |
| 更差（斑块/boiling，怎么调都不如现在的 1spp 纯噪） | 25% | 关掉 ReSTIR 反而更耐看 |

**预判 40% 概率是「持平」**，这正是建议先做 P0/P1 的原因：如果 P1 显示「`maxDepth=2` 的画质已经能接受」，那 ReSTIR 的价值就只剩「更高分辨率」，可以推迟。

### 7.6 如果要降风险，具体调什么

- **不要追求收敛**：`M` 封顶 16~24，让输出保留一层白噪声底（喂 RR 的「可信度」信号）。
- **空间复用半径小 + 每像素随机旋转**：半径 10~15 px、4~8 个邻居，旋转用 `xxhash32(pixel, frame)` 而不是固定图案（固定图案正是指南里点名要避免的）。
- **保留直接光的 MIS 硬币**，不要「顺手把直接光也 ReSTIR 掉」——那会让输入更平滑。
- **加一个 `restirStrength`（0~1）混合旋钮**：`contribution = lerp(classicPT, restir, strength)`，用于 A/B 和「某些场景 RR 打架时一键关掉」。
- **reservoir reset 与 RR 的 `InReset` 同步**，但**不要**因为 reservoir reset 就 reset RR（会闪）。

---

## 8. 工作量估算

| 文件 | 改动 | 行数（估） |
|---|---|---|
| `NativePlugin/shaders/pt_common.slang` | `ReservoirGI` 结构、目标函数、重投影/校验辅助、UBO 新字段、八面体法线编解码 | +150~200 |
| `NativePlugin/shaders/pt.slang` | pass 分支、初始候选、temporal、spatial、resolve、漫反射/镜面拆分 | +400~600 |
| `NativePlugin/src/vpt_dispatch.cpp` | reservoir 环形 buffer 创建/重建/销毁、3~4 个 binding、poolSizes、push constant range、第二次 TraceRays、buffer barrier、尺寸变化检测 | +250~350 |
| `NativePlugin/src/vpt_api.{h,cpp}` | 相机结构新增 `prevViewProj` / `reset` / ReSTIR 参数透传 | +60~100 |
| `Assets/Scripts/NativeBridge.cs` | `VPT_CameraData` 扩字段、`PrepareDispatch` 签名、可选 `VPT_RestirSetParams` | +40~80 |
| `Assets/Scripts/PathTraceRendererFeature.cs` | `prevViewProj` 已在（`:402`，给 MV 用）→ 复用；reset 触发条件（场景重建/相机切换/分辨率）；Inspector 参数 | +50~90 |
| `Tools/rt_probe/rt_probe.cpp` | 新入口链接、reservoir ping-pong、静态场景方差收敛断言、能量对照 | +150~250 |
| 文档/调试 | 本文 + 实现记录 + bug 记录 | — |
| **合计** | | **1100~1700 行** |

按本项目历史节奏（RT pipeline 整体切换是一次同等量级的改造），业余节奏 **2~4 周**，其中**一半时间会花在与 RR 的调停上**（这部分不可预估）。

---

## 9. 落地路线（可回滚、每阶段有闸门）

### P0：基线（**必做，零风险**）

1. 打开 `logGpuTiming`，把 `[PT-GPU]` 分段跑出来 —— **本工程至今没有这个数据**（`Logs/VPT-trace-csharp.log` 里 0 条）。要记：`pathtrace` / `motion` / `dlss` / `total` 四项的平均值与占比。
2. 记录当次分辨率、`dlssQualityMode`、`maxDepth`、光源数、实例数。
3. 存一帧参考图 + 一段 8 帧（一个 Halton 周期）的录屏，作为后续 A/B 的共同基准。

**闸门 G0**：PT 段占比 < 30% → **直接停在这里，不值得做 ReSTIR**（瓶颈在别处）。

### P1：`maxDepth` 扫描（**零代码**）

对 `maxDepth ∈ {1, 2, 3, 4, 8}`，同一相机位置、同一分辨率，各存 8 帧平均图 + PT 段 ms。

**闸门 G1**：若 `maxDepth=2` 与 `8` 的差异在主观上不可辨 → **ReSTIR 的价值只剩「高分辨率」**，推迟到有 1080p+ 需求时再做。

### P2a：只做时间复用（**单 TraceRays，最小可回滚**）

范围刻意收窄：

- 不做 spatial reuse（不需要第二个 pass、不需要 push constant、不需要 primarySurf buffer）；
- 只加：`prevViewProj` 进 UBO、2 个 SSBO binding（reservoir 历史环 + scratch）、reservoir 环形 buffer、reset 通路；
- 漫反射间接换成「1 条次级射线 + NEE@x_s + 时间复用 + resolve」；
- 镜面链保持现状（`specularDepth` 独立参数）。

**闸门 G2**：静止 3 秒后间接光方差显著下降（rt_probe 里可量），且 RR 输出**没有**斑块/boiling。

### P2b：加空间复用（第二个 pass）

- 加 push constant + primarySurf SSBO + 第二道 barrier + 邻居 gather。

**闸门 G3**：A/B（`strength=0` vs `1`）在 5 个场景 × 2 分辨率下，每场景记录「更好/持平/更差」+ PT 段 ms。

### P3：与 RR 调停

按 §7.6 的旋钮逐个试；重点是 `M` 上限、空间半径、`restirStrength`。这一步没有明确的终止条件——**建议设一个时间盒（例如 3 个晚上），到点没结论就按 G3 的结果做取舍**。

### P4（可选，不建议）

ReSTIR DI（多光源/自发光采样）或 ReSTIR PT。真要做，先补「自发光三角形光源采样」这个前置（现在完全没有）。

---

## 10. 验收与实验计划

### 10.1 rt_probe 扩展（离线、不需要 Unity）

现有 `Tools/rt_probe/rt_probe.cpp` 已经能：建设备 → 链 7 个入口 → 建 SBT → `traceRays` → 检查 radiance 有限非零。需要加：

| 编号 | 检查 | 判据 |
|---|---|---|
| R0 | 新 entry/pass 能链接、push constant 生效 | 不报错 |
| R1 | 两帧 ping-pong：reservoir 的 M 单调递增、`wSum` 有限 | 无 NaN/Inf |
| R2 | **静态相机两帧 reservoir 完全一致**（除 M/wSum） | 逐字节 diff = 0（专抓 §5.4 的 Y 翻转坑） |
| R3 | 静态场景 1 帧 vs 32 帧的方差比 | 下降 ≥ 4× |
| R4 | 能量对照：纯漫反射场景 vs 高 spp PT 参考 | 平均亮度偏差 < 3% |
| R5 | 重置：`reset` 后 M 归 1 | 生效 |

### 10.2 Unity 侧 A/B

| 维度 | 取法 |
|---|---|
| 画质 | 5 个场景（教堂、Cornell-box 式、玻璃为主、动态物体多、暗部多）× `strength ∈ {0, 0.5, 1}` × `maxDepth ∈ {2, 8}`，各存 8 帧平均图 + 录屏 |
| 性能 | `[PT-GPU]` 的 `pathtrace` 段 ms（需要给 ReSTIR 的两个 pass 各加一个 `VkCmdWriteTimestamp`，`vpt_gpu_timer.h` 已有机制） |
| 伪影清单 | 斑块 / boiling / 接触阴影缺失 / 掠射角漏光 / disocclusion 拖影 / 玻璃边缘错误 / 镂空边缘闪烁 |
| 回归 | 关掉 ReSTIR 必须与当前版本逐像素一致（可回滚性验证） |

---

## 11. 风险清单（按严重度）

| # | 风险 | 严重度 | 证据 | 缓解 |
|---|---|---|---|---|
| 1 | **RR 要求时空去相关，ReSTIR 制造相关 → 质量反而下降** | **致命** | RR 指南 §3.5；Kettunen I3D'23 | P2a 小步验证；`strength` 旋钮；M 封顶 |
| 2 | 双时域累积 → 移动光源/开门时 lag、ghosting | 高 | CP2077「抢饭吃」页；RR 内置时域 | 限制 M；disocclusion 丢弃历史而非 boost；场景切换 reset |
| 3 | ReSTIR GI 重连有偏（缺雅可比）→ 能量偏暗、接触阴影变浅 | 高 | GRIS Fig.2；ReSTIR GI 论文 | 能量对照（R4）；权重上界做成参数 |
| 4 | 历史校验不足 → 漏光/拖影（动态物体、TLAS ring 切换） | 高 | — | ownerSurfId + 深度 + 法线三重校验；TLAS 重建帧强制 reset |
| 5 | **Y 翻转陷阱**（reservoir 是 SSBO，不能翻） | 中 | `pt.slang:27-30` 与 `:189` 的既有约定 | R2 断言 |
| 6 | 描述符 18→21 改动漏改 poolSizes | 中 | 当前池是精确配额 `vpt_dispatch.cpp:312-324` | 池配额按 binding 数算，不写死；开验证层跑一遍 |
| 7 | 透明/体积/镂空材质边界处理错 | 中 | §5.7 | 该像素退出 ReSTIR；R1~R5 之外再加玻璃/镂空场景 A/B |
| 8 | 显存（4K 下 64B/px 不可接受） | 中 | §5.3 | 压到 32B 或半分辨率 reservoir |
| 9 | 多 pass 的 barrier 写错（srcStage/dstStage 用 RT shader） | 中 | 现有 barrier 只处理 image，没有 buffer 先例 | 照抄 `vpt_dispatch.cpp:1117-1162` 的结构，加 `VkBufferMemoryBarrier` |
| 10 | 与「已删掉自研时域累积」的历史决定冲突，维护成本上升 | 中 | `README.md:17`、`DlssAux.compute:8-9` | 文档里明确「ReSTIR 是估计器不是降噪器」；代码注释同 |
| 11 | 分辨率/场景重建后 reservoir 未重置 → 闪一下 | 低 | — | 尺寸变化检测 + reset 通路 |
| 12 | Push constant 与 RT pipeline 的 stageFlags 配错 | 低 | 无先例 | `VK_SHADER_STAGE_RAYGEN_BIT_KHR` |

---

## 12. 顺带建议：两处比 ReSTIR 便宜得多的改进

调研过程中发现两件事，**性价比明显高于 ReSTIR**，建议无论做不做 ReSTIR 都先做：

### 12.1 补 specular hit distance（**已实现 2026-11-26**）

> 已落地为「ray direction + hit distance」打包版本，实现与验收记录见 [RR_specular_guide实现记录.md](RR_specular_guide实现记录.md)。
> 关键发现：原 `pd = 1-metallic` 让介质表面**永远不采样镜面 lobe**，guide 会是空数据；已改为无条件的 Fresnel 加权。

RR 集成指南 §4.1.8/§4.1.9 把「specular MV **或** specular hit distance」列为二选一必需项；但 NGX 头文件把这两个字段归在 `/* OPTIONAL - only for research purposes */` 区（`nvsdk_ngx_helpers_dlssd_vk.h:60-83`）——所以实情是「**不阻塞，但缺了 RR 只能拿主表面 MV 去凑镜面**」，高光/反射的时域稳定性打折。当前 `vpt_dlss.cpp:852-887` 两个都没给。补法很便宜：

- 在 `pt.slang` 里把「镜面那一跳的命中距离」写进一张新的 `rgba16f` 图（或复用现有图的某个通道）；
- C++ 侧加一个 binding + 一个 `VPT_DLSS_PrepareRRDispatch` 参数；
- `rrEval.pInSpecularHitDistance = &hitDistRes;`（字段名见 `third_party/ngx/include/nvsdk_ngx_helpers_dlssd_vk.h:79`；同族还有 `pInSpecularRayDirection` / `pInSpecularRayDirectionHitDistance`，可一并给）。也可直接给 specular motion vectors——指南说那条更好，但要多一张 RG 图 + 一套镜面重投影。

预期收益：高光/反射的时间稳定性提升（这是 RR 标称的能力，现在没用上）。成本约 **150~250 行**，是 ReSTIR 的 1/6~1/8。

### 12.2 灯采样改成按功率 importance

`pt.slang:220` 现在是 `uint li = rand(seed) * lightCount` —— **均匀选灯**，不考虑亮度/距离。灯多且亮度悬殊时，方差与「双峰跳变」都会恶化（`doc/DLSS_RR亮区抖动分析.md` 的「排除项」里提到过这个风险）。改成按功率（或按 `luminance(radiance_at_p)` 的近似）建立 CDF 别名表，是 ~100 行的活，且**降低方差**（对 RR 有利）。

> 讽刺的是：12.2 降低方差的方向，和 7.4 里「RR 需要噪声」是反的。这也正说明为什么本项目任何「降方差」的改动都必须过 A/B —— **这里的质量目标不是「更干净」，而是「RR 输出更稳、更锐、更不闪」**。

---

## 13. 参考资料

### 13.1 本仓库

- `NativePlugin/shaders/pt.slang` — raygen 主循环、NEE、BSDF/BTDF 采样、G-buffer 写出
- `NativePlugin/shaders/pt_common.slang` — 结构体、UBO、BSDF、`traceShadowTransmission`、`reconstructHitInfo`
- `NativePlugin/src/vpt_dispatch.cpp` — 18 binding 布局、描述符环、TraceRays、barrier
- `NativePlugin/src/vpt_rt.cpp` — SBT（1 raygen + 2 miss + 2 hit group）
- `NativePlugin/src/vpt_dlss.cpp` — RR create/evaluate 参数
- `NativePlugin/src/vpt_scene.cpp:151` — `CreateBufferResource`（device-local buffer 的现成写法）
- `Assets/Scripts/PathTraceRendererFeature.cs` — 帧编排、jitter、MV、RR 调度
- `Assets/Shaders/DlssAux.compute` — MV kernel（重投影参考实现）
- `doc/DLSS_RR亮区抖动分析.md` — RR 对输入统计量的敏感性（§7 的核心依据）
- `doc/RT_pipeline整体切换可行性调研.md` — 性能口径与分辨率基线

### 13.2 公开资料

- ReSTIR GI: Ouyang, Liu, Kettunen, Pharr, Pantaleoni, *HPG 2021* — [Wiley](https://onlinelibrary.wiley.com/doi/10.1111/cgf.14378)、[PDF](https://sgvr.kaist.ac.kr/~sungeui/ICG_F21/Students/ReSTIR%20GI_%20Path%20Resampling%20for%20Real-Time%20Path%20Tracing.pdf)
- GRIS / ReSTIR PT: Lin, Kettunen, Bitterli, Pantaleoni, Yuksel, Wyman, *TOG 41(4), SIGGRAPH 2022* — [PDF](http://cemyuksel.com/research/papers/sig22_GRIS.pdf)、[DOI](https://dl.acm.org/doi/10.1145/3528223.3530158)
- ReSTIR PT Enhanced (I3D 2026) — [NVIDIA RTR](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/)
- Area ReSTIR (成本对比数据) — [PDF](https://research.nvidia.com/labs/rtr/publication/zhang2024area/zhang2024area.pdf)
- ReSTIR 课程笔记（2023） — [intro-to-restir](https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Notes.pdf)、[Cyberpunk 集成页](https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Cyberpunk_2077_Integration.pdf)
- DLSS-RR Integration Guide — [PDF](https://media.githubusercontent.com/media/NVIDIA-RTX/Streamline/refs/heads/main/docs/DLSS-RR%20Integration%20Guide.pdf)
- DLSS-RR Programming Guide — [GitHub](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md)
- Kettunen et al. I3D'23（去相关后 RR 更好） — [ACM](https://dl.acm.org/doi/fullHtml/10.1145/3610548.3618245)
- RTXDI（ReSTIR DI/GI/PT 参考实现，DX12） — [GitHub](https://github.com/NVIDIA-RTX/RTXDI)
- ReSTIR_PT（GRIS 作者实现，Falcor/DX12） — [GitHub](https://github.com/DQLin/ReSTIR_PT)
- vk_gltf_renderer（Vulkan RT + DLSS RR 参考，**无 ReSTIR**） — [GitHub](https://github.com/nvpro-samples/vk_gltf_renderer)
- Cyberpunk 2077 2.1 补丁说明（ReSTIR GI 原文） — [Steam](https://store.steampowered.com/news/posts/?feed=steam_community_announcements&enddate=1701714718)

> **公开实现现状**：没有找到「Vulkan Ray Tracing Pipeline + ReSTIR + DLSS RR」三件套的完整开源参考。RTXDI（ReSTIR，DX12）与 vk_gltf_renderer（RR，Vulkan）需要自行拼合，这是本调研判断「工作量在中大区间」的另一条依据。

---

## 14. 一句话总结

**ReSTIR GI 接进来在工程上是「够得着」的（不需要新扩展，SBT 都不用动），真正的难点不在图形学而在两处：一是这个项目的分辨率太低、RR 已经把事情做完了，收益可能为零；二是 RR 明确要求时空去相关的输入，而 ReSTIR 的价值恰恰来自制造相关——这两件事在一个已经为「喂 RR 噪声」打过补丁的管线里相遇，结果只能靠实测，不能靠推理。**
**所以：先花一个晚上量基线 + 扫 `maxDepth`（零代码），再花一百五十行补 specular hit distance；ReSTIR GI 留到「要上 1080p+」或「P1 证明深度确实是瓶颈」时再动。**
