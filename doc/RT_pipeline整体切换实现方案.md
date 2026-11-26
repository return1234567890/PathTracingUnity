# 整体切换到 Ray Tracing Pipeline —— 实现方案

> 前置文档：[RT_pipeline整体切换可行性调研](RT_pipeline整体切换可行性调研.md)（结论、证据、语义映射）
> 目标：把原生插件的路径追踪后端从 `VK_KHR_ray_query` + compute **整体切换**到 `VK_KHR_ray_tracing_pipeline`（raygen 迭代弹射 + 延迟着色 + SBT），**优先打通链路**。
> 非目标：不改 Unity/URP、不改 DLSS RR 链路、不改 BLAS/TLAS 构建、不做后处理（泛光/tonemap 另立项）、**不评估性能**、**不做与旧后端的等价性对比**。
> 环境：Unity 2022.3.62f3c1 + URP 14 + Vulkan SDK 1.4.313.2 + RTX 3070 Ti Laptop（无 SER）
> 编写时间：2026-11-21（第 3 版：去掉等价性对比，改为实验分支上的"打通"目标）

---

## 0. 已拍板的决策（2026-11-21）

| # | 决策 | 结论 | 对方案的影响 |
|---|---|---|---|
| D1 | 回退形态 | **纯整体切换，删除 RQ**，不做过渡期双后端 | 在实验性分支上直接改；回退 = git 分支/commit。旧后端代码**打通后才删**（先留着当参考与保险） |
| D2 | 着色器组织 | **Slang 单模块**（7 个 RT 入口共用一个模块） | 引入 `slangc` 工具链（+0.5 天）；运行时一个 `VkShaderModule` + 7 个 `pName`（与参考工程同做法） |
| D3 | 首帧编译 | **同步创建**；`VkPipelineCache` 本次**不做** | 不考虑性能 ⇒ 不做 cache 落盘、不做 deferred operation、不做耗时测量 |
| D4 | 验收方式 | **不求等价性，只求打通** | 不做逐像素 diff、不冻结旧构建做参考图；验收 = 画面正常 + 不崩 + 验证层干净（见 §7） |

> 关于性能与等价性：本次实现以"链路跑通、画面正确"为唯一目标。不采集基线、不做 A/B、不做逐像素回归；GPU 计时沿用现有链路（不新增落盘通道），也不为性能做任何取舍（例如不限制阴影层数、不裁剪着色器特性）。

---

## 1. 完成定义（DoD）

1. 全场景 PT 由 `vkCmdTraceRaysKHR` 生成：单 raygen 迭代弹射，CH/AH 只做命中记录与阴影透射，材质/BSDF/纹理全部在 raygen。
2. 设备扩展与 feature 由拦截器注入，能力查询、失败路径、日志齐备。
3. 画面"打通"：5 张输出图都有正确内容，材质/玻璃/体积/阴影/镂空/相机/ DLSS RR 均正常（判据见 §7）。
4. 设备丢失可恢复或明确报错；验证层无错误。
5. 构建链只剩一套执行路径：Slang → 一份 SPIR-V 头 → 一个 RT pipeline，`vpt_dispatch` 不再调用 compute 管线。
6. 旧后端代码与 `ray_query` 扩展注入**在打通确认后再删除** —— **已于 2026-11-22 完成**（见 §10.7）。
7. 文档、日志、README 硬件门槛同步更新。

---

## 2. 目标架构

### 2.1 命令流（前后对照）

```
现状（compute + ray query）
  ... BLAS refit / TLAS build（vpt_scene.cpp） ...
  barrier：UBO host→shader、5 张输出图 → GENERAL           dst stage = COMPUTE
  vkCmdBindPipeline(COMPUTE) + vkCmdBindDescriptorSets + vkCmdDispatch(w/8, h/8)
  barrier：5 张图 → SHADER_READ_ONLY                        src = COMPUTE
  Unity: MotionVector compute → NGX DLSS RR → Blit → FinalBlit

切换后（ray tracing pipeline）
  ... BLAS refit / TLAS build（同一套，不变） ...
  barrier：UBO host→shader、5 张输出图 → GENERAL           dst stage = RAY_TRACING_SHADER
  vkCmdBindPipeline(RAY_TRACING) + vkCmdBindDescriptorSets + vkCmdTraceRaysKHR(w, h, 1)
  barrier：5 张图 → SHADER_READ_ONLY                        src = RAY_TRACING_SHADER
  Unity: MotionVector compute → NGX DLSS RR → Blit → FinalBlit   ← 完全不变
```

**PT 段的输入输出契约不变**：输入 = 相机 UBO + 18 个 descriptor（TLAS、5 张图、6 SSBO、5 采样器、env SSBO），输出 = radiance + gbuffer0/1 + 2 张 albedo。这是"后面还能加自定义 pass（泛光等）"的前提（见 §9）。

### 2.2 着色器入口与 SBT（7 个入口 / 5 个 group）

| # | 入口 | 阶段 | 职责 | 对应现状 |
|---|---|---|---|---|
| 1 | `rgenMain` | raygeneration | 每像素循环：主光线 → 重建 HitInfo → 着色/NEE/BSDF/体积 → 阴影光线 → G-buffer 写出 | `pathtrace.glsl:904-1269` |
| 2 | `rmissMain` | miss | `hitT = INFINITE`（天空/环境） | `:930-950` |
| 3 | `rmissShadow` | miss | 空体：保留 AH 累积的透射率 | `:670` |
| 4 | `rchitMain` | closest-hit (group 0) | 只回写 `{instance, material, prim, bary, hitT}`，不做着色 | `getHitInfo()` `:717-763` |
| 5 | `rahitMain` | any-hit (group 0) | alpha cutout：`IgnoreHit()` | 主光线 candidate 循环 `:951-986` |
| 6 | `rchitShadow` | closest-hit (group 1) | 被不透明挡住：`totalTransmission = 0` | `:700-704` |
| 7 | `rahitShadow` | any-hit (group 1) | 逐层 Beer 吸收 + 透射材质透过率；`IgnoreHit()` / 耗尽时 `AcceptHitAndEndSearch()` | `traceShadowTransmission()` `:544-671` |

SBT：`maxPipelineRayRecursionDepth = 1`（阴影光线从 raygen 发出，不在 CH 里递归）；所有实例 `sbtOffset = 0`，由 raygen 侧传入的 `sbtOffset` 选 group（主路径 0 / 阴影 1），材质身份仍走 `instanceCustomIndex`。

模块形态（D2 = Slang）：一个 `pt.slang` 模块含 7 个 `[shader("...")]` 入口，编译出**一份** SPIR-V；运行时只创建一个 `VkShaderModule`，7 个 `VkPipelineShaderStageCreateInfo` 共用它、靠 `pName` 选入口（参考工程 `renderer_pathtracer.cpp:1333-1399` 就是这么做的，已在 NVIDIA 上验证）。好处是 `spv_to_header.ps1` 完全不用改，仍是一个数组。

Payload：主路径 ≤ 32 B，阴影 ~36 B（`isInside / prevHitT / extinction / totalTransmission`），hit attribute 用 `BuiltInTriangleIntersectionAttributes`（bary 8 B）。大结构一律回读 SSBO。

---

## 3. 阶段计划总览

| 阶段 | 内容 | 人天 | 闸门（G） |
|---|---|---|---|
| P0 | 扩展/feature/properties 注入与能力查询 | 0.5 | G0：能创建空 RT pipeline |
| P1 | Slang 着色器拆分 + payload/hitAttribute + 体积状态搬家 | 3.5–5.5 | G1：7 个入口编译链接通过 |
| P2 | SBT 生成器 | 1 | G2：句柄填充与 region 校验通过 |
| P3 | RT pipeline 创建 + 描述符 stage | 1–1.5 | G3：能出图 |
| P4 | 录制/barrier/设备重建对接 | 1–1.5 | G4：验证层无错误、不崩 |
| P5 | 打通验收（画面逐项过）+ 清理标注 | 1–1.5 | G5：§7 清单全部通过 |
| **合计** | | **8–11.5 人天 ≈ 1.5–2.5 周** | |

依赖关系：P1 是主风险路径；P2/P3 可在 P1 的公共头稳定后并行；P5 在 P3 出图后即可开始，边修边验。

---

## 4. 各阶段步骤

### P0 设备与能力（0.5 天）

落在 `vpt_intercept.cpp`（`InterceptedCreateDevice`，`:32-137`）与 `plugin_main.cpp`（能力查询，`:95-138`）。

1. 扩展注入：按依赖顺序追加 `VK_KHR_ray_tracing_pipeline`；同时显式加入 `VK_KHR_spirv_1_4` 与 `VK_KHR_shader_float_controls`（Vulkan 1.1 设备上 SPIR-V 1.4 需要前者）。
2. feature 注入：`VkPhysicalDeviceRayTracingPipelineFeaturesKHR{ rayTracingPipeline = VK_TRUE }` 挂进 pNext 链，复用现有的 `ChainHas` 去重逻辑（`:99-107`），避免与 Unity 原链重复 sType。
3. properties 查询：`vkGetPhysicalDeviceProperties2` 取 `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`，缓存 `shaderGroupHandleSize / shaderGroupHandleAlignment / shaderGroupBaseAlignment / maxShaderGroupStride / maxRayRecursionDepth / maxRayHitAttributeSize`（本机实测 32/32/64/…/31/32）。
4. **确认 Unity 请求的 Vulkan 版本**：打印 `VkPhysicalDeviceProperties.apiVersion` 与实例 `apiVersion`。这一步同时决定 slangc 的 `-profile sm_6_6+spirv_1_x`（1.1→1.4、1.2→1.5、1.3→1.6）；若实例是 1.1，`VK_KHR_spirv_1_4` 必须在启用列表里（步骤 1 已覆盖）。
5. 能力位：新增 `VPT_IsRayTracingPipelineSupported()`（`plugin_main.cpp:144` 旁），条件 = 扩展存在 && `rayTracingPipeline` feature && AS feature。**失败不是静默**：打印明确原因，C# 侧给明确报错。
6. 首次创建时打印一次 SBT 相关属性与管线创建日志（仅用于排查，不做耗时统计）。

闸门 G0：能用最小的 raygen-only 管线创建成功（无 SBT/无 traceRays），验证扩展与 feature 注入链正确。

### P1 Slang 着色器拆分（3.5–5.5 天，主风险）

1. 文件结构（Slang 单模块，D2）：
   - `pt.slang`：7 个 `[shader("...")]` 入口 + 主循环；`pt_common.slang`：结构体/材质读取/BSDF/BTDF/体积/环境采样/随机数/alpha cutout 辅助/HitInfo 重建（用 `import` 或 `#include`，第一天确认 slangc 的 include 路径行为）。
   - 入口名固定为 `rgenMain / rmissMain / rmissShadow / rchitMain / rahitMain / rchitShadow / rahitShadow`（与参考工程一致，便于对照）。
   - 所有资源用 `[[vk::binding(n, 0)]]` 显式标注，binding 编号与现有 18 个槽位一一对应。
2. 工具链（新增依赖，需固定版本）：
   - `slangc -target spirv -profile sm_6_6+spirv_1_4`（**SPIR-V 版本必须按 Unity 实际 Vulkan 版本定**：1.1→`spirv_1_4`、1.2→`spirv_1_5`、1.3→`spirv_1_6`；P0 已打印 apiVersion）；
   - 必加 `-fvk-use-entrypoint-name`，否则入口名被修饰、`pName` 找不到；
   - `-O0`（本工程约定让驱动优化）、`-g` 按需；建议加 `-emit-spirv-directly`（跳过中间 GLSL 路径）；
   - **不要加**参考工程的 `-force-glsl-scalar-layout`：它会改变常量缓冲布局，和现有 208 B 手工布局冲突；
   - 同一模块只编译一次、产出一份 SPIR-V（不需要逐入口编译）。
3. 数据通路：
   - 主路径 payload ≤ 32 B：`{hitT, instanceID/instCustomIndex, primID, bary(float2)}`，raygen 用实例/prim 索引回读 SSBO 重建 `HitInfo`（**不要把 72 B 的 HitInfo 塞进 hitAttribute**）。
   - 阴影 payload ~36 B：`{seed, prevHitT, isInside, extinction, totalTransmission}`。
   - hit attribute 直接用 `BuiltInTriangleIntersectionAttributes`。
4. 语义对照（逐条过）：
   - 主光线 `RAY_FLAG_FORCE_OPAQUE`，与现状 `gl_RayFlagsOpaqueEXT` 一致；透明表面仍由 CH 命中、BSDF 的 transmission 分支处理。
   - 阴影 `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH` + 实例级 `FORCE_OPAQUE`：不透明实例不触发 AH，直接 CH 置 0 并结束；透射实例进 AH。
   - alpha cutout 从"主光线 candidate 循环丢弃"改为"主路径 any-hit + `IgnoreHit()`"；语义等价，注意主路径 AH 必须存在（否则镂空材质变实心）。
   - 体积状态机从局部变量搬进阴影 payload（`isInside / extinction / prevHitT`）；主路径状态仍在 raygen 局部变量。
   - AH 到达顺序不保证：沿用现状的 `abs(hitT - prevHitT)` 近似（`:641`），记为已知近似，不算迁移 bug。
   - **payload 跨 `IgnoreHit()` 保留**：DXR/Vulkan 规范明确 any-hit 在 `IgnoreHit()` 前对 payload 的修改会保留（<https://learn.microsoft.com/en-us/windows/win32/direct3d12/any-hit-shader>），参考工程 `gltf_pathtrace.slang:1164-1208` 的生产代码也这么做；仍在 P1 首日用一个最小样例在本机驱动上验证一次。
   - 入口坐标：Slang 用 HLSL 风格内建 `DispatchRaysIndex()` / `DispatchRaysDimensions()`，取代 `gl_GlobalInvocationID`；Y-flip 输出逻辑保持不变（`:1267-1269`）。
   - **UBO 布局**：现有 `PathTraceCameraUBO` 是 208 B 的 std140 手工布局（`vpt_dispatch.cpp:104-117`）。Slang 侧保持 `float4x4 / float4` 字段类型，默认常量缓冲布局与 std140 对这类字段一致；但**必须做一次偏移核对**（`spirv-dis` 看 member offset，或临时把矩阵输出到颜色肉眼验证），避免相机全错这种"看起来像光线 bug"的假象。
5. 构建链：`slangc` 编译一次 → `spv_to_header.ps1` 转成一个数组（脚本**不用改**）→ `pathtrace_spv.h` → `cmake --build`。建议新增 `build_shaders.ps1` 串起两步（现状是手工三步），并把 `slangc` 版本写进脚本注释。
6. 校验：`spirv-val` 过一遍；确认 7 个入口名、payload/attribute 布局、binding 编号与 C++ 侧一致；`spirv-dis | findstr OpEntryPoint` 快速自检。

闸门 G1：7 个入口都能编译、能被 RT pipeline 链接（`pName` 与 group 对应）。

### P2 SBT（1 天）

1. `vkGetRayTracingShaderGroupHandlesKHR` 取 5 个 group 的句柄。
2. 对齐：`stride = align(handleSize, handleAlignment)`；region 起点与大小按 `baseAlignment`（本机 32/64）对齐；全部数值从 properties 取，禁止硬编码。
3. Buffer：`usage = SHADER_BINDING_TABLE_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT`，host-visible + mapped；**内存分配必须带 `VkMemoryAllocateFlagsInfo{ DEVICE_ADDRESS_BIT }`**，否则 `vkGetBufferDeviceAddress` 失败。
4. 4 个 `VkStridedDeviceAddressRegionKHR`（raygen / miss / hit / callable=空）；callable 用全零 region 传入。
5. 生命周期：SBT 与 pipeline 同生共死；与分辨率无关；创建后打印各 region 大小与偏移做校验。

闸门 G2：句柄非全零、region 布局与预期一致。

### P3 管线创建与描述符（1–1.5 天）

落在 `vpt_dispatch.cpp` 的管线创建段（`:124-181`）。

1. `VkRayTracingPipelineCreateInfoKHR`：7 个 stage、5 个 group（GENERAL ×3 + TRIANGLES_HIT_GROUP ×2）、`maxPipelineRayRecursionDepth = 1`、pipeline layout 沿用现有 18 binding 的 setLayout。
2. 描述符 `stageFlags`：全部 binding 改为 `RAYGEN_BIT_KHR | CLOSEST_HIT_BIT_KHR | ANY_HIT_BIT_KHR | MISS_BIT_KHR`（先用并集保证正确）。
3. 创建策略（D3）：**同步创建**，不传 `VkPipelineCache`、不做 deferred operation、不测量耗时。
4. 失败路径：创建失败/链接失败要打印具体 VkResult 与日志，返回错误码给 C#，避免每帧重试。

闸门 G3：能出图（哪怕画面不对）。

### P4 录制 / 同步 / 设备重建（1–1.5 天）

1. 录制段（`:849-877`）替换：`vkCmdBindPipeline(RAY_TRACING_KHR)` → `vkCmdBindDescriptorSets(RAY_TRACING_KHR)` → `vkCmdTraceRaysKHR(cmd, w, h, 1)`（直接用像素尺寸，不再 `(w+7)/8`）。
2. Barrier 1：dst stage 改 `RAY_TRACING_SHADER_BIT_KHR`，dstAccess 保持 `SHADER_WRITE`。
3. Barrier 2：src stage 改 `RAY_TRACING_SHADER_BIT_KHR`，dst 保持 `COMPUTE_SHADER | FRAGMENT_SHADER`（MotionVector / NGX / Blit 都在后面读）。
4. **AS 屏障补消费阶段**（可行性调研没提，实际会踩）：`vpt_scene.cpp` 里所有"AS 写完 → shader 读"的屏障都用 `COMPUTE_SHADER_BIT` 当消费者，需补上 `PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR`（`SHADER_READ` 已有）。至少这些位置：`:774-778`、`:804-808`、`:913-917`、`:1320-1325`、`:1546-1552`。漏掉会读到未完成的 TLAS（花屏/假阴影/验证层报错）。
5. GPU 计时：沿用现有 7 段，T4 位置不变（语义从 dispatch 结束变成 traceRays 结束）；不新增统计与落盘。
6. 设备丢失（TDR）：pipeline + SBT + descriptor 一起销毁重建；复用 `VPT_IsDeviceLost` / `VPT_MarkDeviceLost`（`plugin_main.cpp:150-164`）与 C# 侧的 `VPT_GetDeviceStatus` 轮询（`PathTraceRendererFeature.cs:180-192`）。
7. C# 侧：能力日志与错误提示更新（`NativeBridge.IsRayQuerySupported` → 新接口）；GPU 分段名字保持不变，避免文档/日志口径变化。

闸门 G4：验证层开一帧无错误；连续跑 10 分钟不崩；切分辨率、切 DLSS 质量档、切场景不花。

### P5 打通验收 + 清理标注（1–1.5 天）

> 目标不是"和旧版逐像素一致"，而是"画面是对的"。逐项走 §7 清单，边看边修。

1. 按 §7 清单逐项过一遍（相机/材质/玻璃/阴影/镂空/体积/G-buffer/RR/稳定性）。
2. 发现问题时优先用"最小场景"定位：单材质球 → 玻璃球 → 教堂整场，避免一次性在复杂场景里调。
3. 把旧后端相关代码标注 `TODO(remove-after-rt)` 或移到一个不参与编译的分支文件；确认打通后另起一个 commit 删除，不阻塞本次验收。
4. 更新文档：可行性调研的"现状"段落、DLSS 文档里对 PT 后端的描述、README 硬件门槛（RTX 20 系起，且必须有 `VK_KHR_ray_tracing_pipeline`）。

闸门 G5：§7 清单全部通过。

---

## 5. 决策点：已拍板与仍待确认

### 5.1 已拍板（2026-11-21）

见 §0：D1 纯整体切换 / D2 Slang 单模块 / D3 同步创建（不做 PipelineCache）/ D4 只求打通、不做等价性对比。

### 5.2 剩余待确认（先按推荐默认值执行，不改也能开工）

| # | 决策 | 选项 | 默认（推荐） | 若改的影响 |
|---|---|---|---|---|
| D5 | 无 RT pipeline 设备的处理 | A 初始化失败 + 明确报错，PT 关闭；B 自动回退光栅渲染；C 运行时回退 RQ | **A** | 项目 README 已要求 RTX；B 要恢复 `PathTraceCameraGate` 的全部抑制项（工作量翻倍）；C 与 D1 冲突 |
| D6 | 是否预留 SER/OMM 接口点 | A 只留能力探测 + 注释；B 现在就把 SBT/pipeline 做成可插拔 | **A** | B 是过度设计，且本机无 SER 无法验证 |

已定、无需再讨论的项（沿用可行性调研）：命中着色留在 raygen（CH 只回写记录）；材质不映射 hit group；`maxPipelineRayRecursionDepth = 1`；实例 `sbtOffset = 0`；`instanceCustomIndex = materialID` 语义不变；DLSS RR 链路完全不变；本次不接 SER/OMM。

---

## 6. 风险与坑（相对于可行性调研，新增的工程级风险）

> 只列影响**正确性/稳定性**的项；性能相关（多层玻璃 any-hit 开销、首帧编译耗时等）本次不评估。

| # | 问题 | 后果 | 验证方式 |
|---|---|---|---|
| 1 | AS 屏障仍用 `COMPUTE_SHADER` 当消费者（`vpt_scene.cpp` 5 处） | 读到未完成的 TLAS：假阴影/花屏/验证层报错；排查时容易误判成"着色器 bug" | 验证层 + TLAS 每帧重建场景（动态物体） |
| 2 | SBT buffer 缺 `SHADER_BINDING_TABLE_BIT_KHR` 或分配缺 `DEVICE_ADDRESS_BIT` | `vkGetBufferDeviceAddress` / traceRays 失败或设备丢失 | G2 校验 + 验证层 |
| 3 | Unity 实例 Vulkan 版本是 1.1 | SPIR-V 1.4 模块被拒（`vkCreateShaderModule` 失败） | P0 打印 apiVersion；确认 `VK_KHR_spirv_1_4` 在启用列表 |
| 4 | 描述符 stageFlags 漏 RT 阶段 | 验证层报"descriptor not accessible"，或驱动静默读错 | G4 验证层一帧 |
| 5 | payload / hitAttribute 超限 | 管线创建失败或越界写 | G1/G3；payload ≤32 B、attribute 仅 bary |
| 6 | AH 顺序不保证导致阴影透射近似误差 | 与现状同源，不引入新错误但也不修好 | 玻璃特写人工看：无全黑/全白/硬边 |
| 7 | 设备丢失后 pipeline、SBT、descriptor 重建顺序错乱 | 二次崩溃 | 复用现有 device-lost 流程；G4 连续 10 分钟 |
| 8 | Slang 产出的 SPIR-V 版本高于 Unity 设备支持的 Vulkan 版本（`sm_6_6+spirv_1_6` 是参考工程的默认值，本机可能只到 1.1/1.2） | `vkCreateShaderModule` 直接失败，看起来像"扩展没启用" | P0 先打印 apiVersion，再定 `spirv_1_x` |
| 9 | Slang 常量缓冲布局与现有 208 B `PathTraceCameraUBO`（std140 手工布局）不一致 | 相机矩阵/参数全错，表现为"光线方向乱飞" | P1 首日核对 `spirv-dis` 的 member offset，或临时把矩阵输出到颜色验证 |
| 10 | 未加 `-fvk-use-entrypoint-name` | SPIR-V 入口名被修饰，`pName` 匹配失败、管线链接报错 | G1 用 `spirv-dis \| findstr OpEntryPoint` 自检 |
| 11 | `slangc` 版本漂移导致 SPIR-V/优化结果变化（参考工程依赖目录里是 2025.13.1） | 重构后行为改变、难以二分 | 固定版本写进 `build_shaders.ps1` 与文档 |
| 12 | raygen 里一次性写出 5 张图 + 大循环，Slang 寄存器压力可能比 GLSL 更极端 | 驱动拒绝创建管线或周期性花屏 | 最小场景先跑通；必要时按材质特性用 specialization/macro 裁剪（不阻塞打通） |

---

## 7. 打通验收清单

> 全部通过即 G5。**不与旧版逐像素对比**；每项只需"肉眼正确 + 无异常"。

| # | 项 | 判据 |
|---|---|---|
| 1 | 输出图有内容 | radiance 非全黑/全白；gbuffer0 深度渐变合理；gbuffer1 法线朝向正确；两张 albedo 有材质颜色（否则 RR 会崩） |
| 2 | 相机正确 | 移动/旋转相机画面跟随；无镜像、无偏移、无缩放异常（UBO 布局与 Y-flip 都对） |
| 3 | 材质正确 | 教堂整场：纹理、法线贴图、金属/粗糙度、自发光、颜色符合美术预期 |
| 4 | 玻璃 | 能透过玻璃看到对面；彩色玻璃有颜色；无全黑、无全白、无硬阴影 |
| 5 | 体积 | 有厚玻璃/介质处出现吸收着色（Beer-Lambert），无突然截断 |
| 6 | 阴影 | 不透明物体有阴影；玻璃后方无硬阴影；半影正常 |
| 7 | 镂空（若有） | 镂空边缘无实心化、无漏光 |
| 8 | DLSS RR | 开/关都能出图；无异常条纹、鬼影、色块（1spp 轻微噪点可接受） |
| 9 | 稳定性 | 验证层无错误；连续跑 10 分钟无设备丢失/花屏；切分辨率、切质量档、切 DLSS 开关、切场景不崩 |
| 10 | 失败路径 | 故意关掉扩展（或用不支持的设备模拟）：日志有明确原因，不崩溃、不无限重试 |

---

## 8. 回退方案

1. **实验分支上直接改**：回退 = `git checkout` 切换分支或 `git reset` 到切换前的 commit（D1 无过渡期双后端）。
2. **代码级**：不保留后端开关；旧后端代码在打通前保留，必要时可临时把 import 切回去做对照（但不作为验收标准）。
3. **触发回退条件**（任一）：G4 之前连续 2 天完全无画面且定位不到方向；出现系统性的花屏/设备丢失无法定位；Slang 在 Unity 的 Vulkan 版本下无法产出可用的 SPIR-V 且无法在 2 天内解决。

---

## 9. 后续工作（明确不在本次范围）

- **后处理（泛光 / tonemap）**：PT 的输出契约不变（5 张图 + 相机颜色 Blit 时序），所以泛光可以在打通后按任一路径接入：
  - URP 侧：新增 Bloom RendererFeature，pass 放在 `AfterRenderingPostProcessing`（600）并排在 `PathTraceRendererFeature` 之后；取源用 `_dlssOutputRT`（RR 之后）或 camera color；
  - native 侧：在 `vkCmdTraceRaysKHR` 与 Barrier 2 之间插 compute 链（此时 barrier/stageFlags 已经改好，正好复用）。
  - 注意当前链路**没有 tonemap**（相机后处理被 `PathTraceCameraGate` 关闭、PT renderer 的 `postProcessData` 为空），泛光上线时要一并决定 tonemap 的位置。
- **SER**：需 Ada+；本机不可用，触发条件 = 目标硬件换成 RTX 40/50。
- **OMM**：已决定暂不接（可行性调研 §10.4）。
- **删除旧后端**：打通确认后单独一次 commit（本方案不强制在 P5 内完成）。
- **性能**：本次不涉及；将来若要，需要先补基线（实验分支上没有留对照，得从 git 历史重建旧构建）。

---

## 10. 实施记录（2026-11-21）

> 本节是落地时的实际状态，与上文方案有出入的地方都在 §10.2 逐条说明。

### 10.1 各阶段与闸门状态

| 阶段 | 代码 | 闸门 | 结果 |
|---|---|---|---|
| P0 扩展/feature/properties | ✅ | G0 能建 RT 管线 | ✅ **离线通过**（`Tools/rt_probe`） |
| P1 Slang 着色器拆分 | ✅ | G1 7 入口编译链接 | ✅ **离线通过** |
| P2 SBT | ✅ | G2 句柄/region 校验 | ✅ **离线通过** |
| P3 RT pipeline + 描述符 | ✅ | G3 能出图 | ✅ **通过**（画面正常，无花屏/无错位） |
| P4 录制/barrier/重建 | ✅ | G4 验证层干净 + 10 分钟稳定 | ✅ **通过**：11 分钟 / 54k 帧稳定；验证层开启后**本插件零报错**（仅剩 10 条 Unity 自己 swapchain 信号量的告警） |
| P5 打通验收 + 清理标注 | ✅ | G5 §7 清单 | ✅ **通过**（画面逐项确认）；旧后端已按 DoD #6 清理（见 §10.7） |

离线的 G0/G1/G2 由 `Tools/rt_probe` 覆盖（本机 RTX 3070 Ti Laptop + Vulkan SDK 1.4.313.2 实测）：

```
vkCreateDevice(RT 扩展 + feature 注入)            OK
vkCreateShaderModule(pt.spv)                      OK     SPIR-V 1.4 被驱动接受
vkCreateRayTracingPipelinesKHR(7 入口 / 5 group)  OK
vkGetRayTracingShaderGroupHandlesKHR              OK     句柄非全零
SBT region 对齐                                   OK     raygen{addr,stride=64,size=64} miss/hit 各 64B
traceRays 冒烟（1 三角形 BLAS/TLAS）              OK     radiance 有限、非零，命中像素数符合几何覆盖
UBO 矩阵方向（非对称 viewInv）                     OK     未转置
```

`rt_probe` 顺带补掉一个「方案没写但会踩」的坑：**BLAS 构建 → TLAS 构建之间必须有屏障**，否则
TLAS 读到未完成的 BLAS，表现为「一条光线都打不中」。`vpt_scene.cpp:1609` 本来就有这道屏障（注释
已标 CRITICAL），离线工具里一开始漏了，症状正好复现了一次。

### 10.2 与方案的偏差（均为落地必需，语义不变）

| # | 方案原文 | 实际实现 | 原因 |
|---|---|---|---|
| 1 | §2.2 「主光线 `RAY_FLAG_FORCE_OPAQUE`」 | 主光线**不加** `FORCE_OPAQUE`（`RAY_FLAG_NONE`） | `FORCE_OPAQUE` 会让**所有** any-hit 都不执行，镂空材质直接变实心；现状 GLSL 也已明确不加（`pathtrace.glsl:956` 注释）。不透明实例仍靠实例级 `FORCE_OPAQUE` 直接 commit，这正是 §4.2 说的「主路径 AH 必须存在」 |
| 2 | §P1.2 按 Unity 的 Vulkan 版本选 `spirv_1_4/1_5/1_6` | **固定 `sm_6_6+spirv_1_4`** | 1.4 是 RT pipeline 的最低要求；Vulkan 1.1 靠注入的 `VK_KHR_spirv_1_4`，1.2/1.3 核心即允许 1.4 ⇒ 一份产物覆盖全部版本，少一个「按版本换 profile」的坑 |
| 3 | §2.2 阴影 payload ~36 B | 40 B（多 `layerCount` / `acceptMode`） | `AcceptHitAndEndSearch()` 之后 closest-hit 仍会执行，必须区分「被不透明挡住（置 0）」和「层数耗尽（保留已累积值，沿用旧行为）」，否则 §2.2 的 `MAX_SHADOW_LAYERS` 语义丢失 |
| 4 | （未提）存储图格式 | `[[vk::image_format("rgba16f"/"rgba32f")]]` | 不声明会生成 `Unknown` 格式的 `OpTypeImage`，需要 `shaderStorageImageWriteWithoutFormat` 设备特性；声明后 SPIR-V 只带基础能力，且与 Unity RT 格式一一对上 |
| 5 | （未提）纹理采样 | 全部 `SampleLevel(..., 0.0f)` | ray tracing 阶段没有隐式 LOD；显式 0 级 == GLSL compute 下的实际行为，跨驱动确定 |
| 6 | §P4.7 新增能力接口 | `VPT_QueryRayTracingPipelineSupport` + `VPT_Initialize` 返回码 3 | 旧 `VPT_QueryRayQuerySupport` 保留为诊断接口；返回 3 = 管线创建失败（SPIR-V 被拒/链接失败/SBT 失败），避免每帧重试 |
| 7 | （未要求） | 新增 `Tools/rt_probe` | 把 G0/G1/G2 变成一条命令，不必每次开 Unity；也顺手验证 UBO 矩阵方向与 traceRays 录制 |

### 10.3 改动文件清单

| 文件 | 内容 |
|---|---|
| `NativePlugin/src/vpt_intercept.cpp` | 注入 `VK_KHR_spirv_1_4` / `VK_KHR_shader_float_controls` / `VK_KHR_ray_tracing_pipeline` + `VkPhysicalDeviceRayTracingPipelineFeaturesKHR` |
| `NativePlugin/src/plugin_main.cpp` | apiVersion 打印、RT properties 缓存（handleSize/alignment/baseAlignment/maxStride/recursion/hitAttr）、`VPT_IsRayTracingPipelineSupported()` |
| `NativePlugin/shaders/pt_common.slang` | 公共代码（资源/材质/BSDF/BTDF/体积/环境/命中重建/阴影透射），UTF-8 |
| `NativePlugin/shaders/pt.slang` | 7 个 RT 入口 + raygen 主循环 |
| `NativePlugin/shaders/build_shaders.ps1` | slangc → spv → `pathtrace_spv.h`，含入口名自检 |
| `NativePlugin/src/pathtrace_spv.h` | 重新生成的 SPIR-V（68 KB） |
| `NativePlugin/src/vpt_rt.h/.cpp` | RT 扩展函数加载 + SBT（句柄/对齐/DEVICE_ADDRESS/region 校验） |
| `NativePlugin/src/vpt_dispatch.cpp/.h` | RT 管线创建（7 stage / 5 group / recursion 1）、描述符 RT 阶段、`vkCmdTraceRaysKHR`、barrier 换阶段、SBT 生命周期、失败不重试 |
| `NativePlugin/src/vpt_scene.cpp` | 5 处「AS 写完 → shader 读」屏障补 `RAY_TRACING_SHADER_BIT_KHR` |
| `NativePlugin/src/vpt_api.cpp/.h` | 能力判定（D5=A：不支持就明确报错）、返回码 3、新查询接口 |
| `Assets/Scripts/NativeBridge.cs` / `PathTraceRendererFeature.cs` | 新能力接口与错误提示、注释口径 |
| `README.md` | 硬件门槛（RTX 20 系 + `VK_KHR_ray_tracing_pipeline`）、构建与自检命令、目录结构 |
| `Tools/rt_probe/*` | 离线自检工具（见 10.1） |

### 10.4 G3 首轮排查记录（2026-11-22）

**症状**：进 Play 约 2 秒后 Unity 无响应，~45 s 后被崩溃处理器接管（栈在 `nvoglv64` 内，
由 `VPT_RenderCallback` 调入）。`GfxDeviceVK` 渲染线程卡在驱动里，主线程跟着卡死。

**定位**：`Logs/VPT-trace-native.log` 里 watchdog 打出最后阶段 = `dispatch:record`：

```
[frame 2] gpu-sync probe: vkDeviceWaitIdle after rebuild ... returned 0
WATCHDOG: 主线程已 8.0 s 没有推进，最后阶段 = dispatch:record（frame=2）
```

即管线建好、描述符写好、TLAS 就绪，卡在 **第一次 `vkCmdTraceRaysKHR`**。

**根因**：把 `vkCmdDispatch` 换成 `vkCmdTraceRaysKHR` 时，整块替换把
`vkCmdBindPipeline` / `vkCmdBindDescriptorSets` 一起删掉了 —— traceRays 用了 **Unity 当时
绑定的管线** 配 **我们的 SBT**，驱动直接挂死。这是「录制代码重复」的典型代价：离线
`rt_probe` 里自己写了一份「bind + traceRays」，所以没暴露。

**修复**：补回 bind（`VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR`），并把录制段拆成细粒度
stage 标记，下次卡在哪一步一眼可见：

```
dispatch:gpu-skinning → blas-refit → tlas-update → barrier-1-images-to-general
  → bind-pipeline → trace-rays → trace-rays-recorded → barrier-2-images-to-sro
```

### 10.5 观察：输出图句柄每帧变化（已确认为「RT 每帧重建」的结果）

> **2026-11-22 结论**：这条现象**不是** Unity 在稳定 RT 上换图，而是 C# 侧
> `EnsureRTs`/`ReleaseRTs` 的顺序 bug 让**全部 7~8 张 RenderTexture 每帧被 Recreate**，
> 新建的 RT 自然是新 VkImage。已在 `PathTraceRendererFeature.cs` 修掉，验证数据见
> [bug追踪](bug追踪.md) **BUG-004**。下面的记录保留了当时的现象与判据，作为排查同类问题的入口。

G3 通过后查日志（3 个 Play 会话、16.9k 帧、无任何 ERROR/WARN/device lost）时发现：

```
view slot 0 image=0x...ED36D0
view slot 0 image=0x...ED00D0
view slot 0 image=0x...ED18D0
...   （连续 12 帧 12 个不同句柄）
```

即 **Unity 的 `AccessTexture()` 对同一张 RenderTexture 每帧返回不同的 `VkImage`**。后果：

| 影响 | 说明 |
|---|---|
| 每帧 5 次 destroy+create ImageView | 句柄变了就必须重建 view（`EnsureImageView` 的既有逻辑）。CPU 开销很小，但 `s_OutputViews[].layout` 每帧被重置成 `UNDEFINED` |
| 「不每帧丢弃上一帧内容」的优化实际失效 | barrier 每帧都是 `UNDEFINED → GENERAL`（丢弃）。对 PT 无害：raygen 每帧整图覆写 |
| 潜在风险（**切换前就存在**） | 被销毁的 ImageView 可能仍被在飞帧的命令缓冲引用。实测 16.9k 帧未出事；若将来出现随机崩溃/花屏，优先查这里 —— 正确做法是 descriptor ring（每帧换一套 descriptor set）+ ImageView 延迟 N 帧销毁 |

已做的处理（不改行为，只降噪）：格式日志改成「格式变化时才打」，并新增每 120 帧一行
`view recreate /120 frames: out=...,...,... texArray=...`，用来确认重建频率与纹理数组是否也在旋转。
（Editor.log 之前被这 5 行/帧撑到 7 MB。）

### 10.6 下一步（需要 Unity 侧验证）

**已验证（2026-11-22）**

- G3 能出图 ✅：教堂整场画面正常（材质/法线/自发光/玻璃/体积/阴影/RR 均未出现花屏或错位）
- G4 稳定性 ✅：11 分钟 / 54,414 帧 / 1129×635 + DLSS RR，无崩溃、无 device lost、无看门狗、退出干净
- 资源侧健康度 ✅：`view recreate out=0`、`ptrChanged=0`、`queryInterval=600`、`Editor.log` 422 KB
  （修复前 240 MB / 每帧 7 张 RT 重建，见 [bug追踪](bug追踪.md) BUG-004）

**还剩一件事**

**G5 的 §7 清单逐项过**：相机/材质/玻璃/体积/阴影/镂空/RR 开关，以及切分辨率、切 DLSS 质量档、
切场景不花（BUG-002 的 ImageView 部分仍是 P2，但触发频率已从「每帧」降到「只在重建时」）。

**验证层结果（2026-11-22，`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`，Play 40 s / 3.4k 帧）**

| 发现 | 归属 | 处理 |
|---|---|---|
| 缺 `VK_EXT_descriptor_indexing`（AS 的规范依赖） | 拦截器 | 已补注入 |
| KHR/EXT 两个 BDA 别名同时启用 | 拦截器 vs Unity | 已摘掉 Unity 表里的 EXT，只留 KHR |
| 对无 `SHADER_DEVICE_ADDRESS_BIT` 的 buffer 查地址（5 处） | `vpt_scene.cpp` | 已加 usage 判断 |
| `vkUpdateDescriptorSets` 改写仍在 pending 的 set | `vpt_dispatch.cpp`（BUG-002） | 已加描述符集环形缓冲（4 套） |
| `vkQueueSubmit-pSignalSemaphores-00067`（10 条） | **Unity 自己的 swapchain** | 与本插件无关（VVL 也在 10 条后按重复上限停报） |

修复后复跑：**只剩 Unity swapchain 那 10 条**，插件的创建/录制/描述符全部干净。

> 逐条的「现象原文 / 根因 / 改法 / 复验 / 教训」记录在
> [bug追踪](bug追踪.md) **BUG-005**（章节标题：验证层一轮扫出的 4 个规范违背），
> 包括我中途改错一次（把 BDA 的 KHR 别名关掉反而让 feature 没启用）的经过。

### 10.7 旧后端清理（2026-11-22 完成，DoD #6）

G5 画面清单确认通过后，按 DoD #6 做了纯清理提交（无行为变化）：

| 删除内容 | 说明 |
|---|---|
| `NativePlugin/shaders/pathtrace.glsl` + `pathtrace.spv` | 旧 ray query + compute 着色器与其 SPIR-V（仅作参考，从未参与构建） |
| `VK_KHR_RAY_QUERY` 扩展注入 + `VkPhysicalDeviceRayQueryFeaturesKHR` | `vpt_intercept.cpp`：不再向 `vkCreateDevice` 注入 ray query |
| `VPT_IsRayQuerySupported()` / `VPT_QueryRayQuerySupport()` | native 内部访问器 + 导出接口一并删除 |
| `NativeBridge.IsRayQuerySupported()` / `VPT_QueryRayQuerySupport` P/Invoke / `InitResult.RayQueryNotSupported` | C# 侧旧接口（`PathTraceRendererFeature` 早已改用 `IsRayTracingPipelineSupported`） |
| 注释里的 `pathtrace.glsl` / `rayQuery` / `candidate 循环` 表述 | 改为 `pt.slang` / `any-hit`，避免后来人按旧架构理解 |

**保留**：`vpt_scene.cpp` 的 AS 函数加载（RT pipeline 同样依赖 acceleration structure）、
`pathtrace_spv.h`（现在由 `pt.slang` 生成）、`skinning.comp.glsl`（骨骼蒙皮，与 PT 后端无关）。

**验证**：重建 DLL → 开验证层自动跑一局（Play 40 s）→ `VPT_Initialize` 成功、PT 正常出帧
（`out=0`、`queryInterval=600`）、`rt_probe` 全绿、验证层只剩 Unity 自身的 swapchain 告警。
回退点 = 本次清理前的 commit。

**清理时额外修掉的一条验证层问题**：删掉 `rayQuery` 注入之后，规范那条
"rayQuery 未启用时，含 `ACCELERATION_STRUCTURE_READ` 的 barrier 的 dstStageMask 不能含
非 RT 的 shader 阶段"才生效，于是暴露出 5 处 AS barrier 还留着旧 compute 后端的
`COMPUTE_SHADER_BIT`（`VUID-vkCmdPipelineBarrier-dstAccessMask-06257`）。已把这些 barrier 的
dstStageMask 收敛为 `AS_BUILD | RAY_TRACING_SHADER`，复跑验证层归零。详见
[bug追踪](bug追踪.md) **BUG-005 §5.6**。




