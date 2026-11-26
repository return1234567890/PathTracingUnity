# 整体切换 Ray Tracing Pipeline 可行性调研

> 📌 **本文已成历史结论（2026-11-21）**：调研结论是"技术可行但性能大概率负收益，建议双后端"；
> 决策层最终仍选择**纯整体切换**（只求打通，不评估性能、不做等价性对比），实施见
> [RT_pipeline整体切换实现方案](RT_pipeline整体切换实现方案.md)（末尾 §10 是实施记录与偏差）。
> 下文描述的是**迁移前**的现状（`pathtrace.glsl` + ray query），保留作为对照资料。

> 调研对象：URPPT（Unity 2022.3 + URP 14 + Vulkan Ray Query 1spp 路径追踪 + DLSS RR）
> 参考实现：`D:\personalProject\MyEngineVulkanV2\vk_gltf_renderer`（独立 Vulkan 引擎，同一份路径追踪同时提供 **Ray Query compute** 与 **RT pipeline** 两种后端）
> 调研时间：2026-11-16
> 范围：只评估「把原生插件里的路径追踪后端从 `VK_KHR_ray_query` + compute 整体换成 `VK_KHR_ray_tracing_pipeline`」的可行性、改动面与性能风险。**不涉及** Unity/URP 自身改成光追（Unity 2022.3 没有 RT pipeline API，URP 侧仍是光栅化 + Blit）、不涉及地形/AABB 新特性。

---

## 1. 结论速览

**一句话：技术上完全可行，参考工程已经把同一套路径追踪在两种后端上跑通；但「整体切换」在当前项目、当前机器（RTX 3070 Ti Laptop，无 SER）上大概率是性能负收益，不建议为了性能而切。建议保留 Ray Query 作为默认后端，把 RT pipeline 做成可选后端（双后端），只有明确要 SER / Slang 统一 / 未来 OMM 时才值得投入。**

| 维度 | 判定 | 依据 |
|---|---|---|
| 技术可行性 | ✅ 高 | 参考工程用 `IRaytracer` 接口把同一份 `pathTrace()` 同时跑在 Ray Query compute 与 RT pipeline 上（`shaders/raytracer_interface.h.slang:60-67`、`shaders/gltf_pathtrace.slang:1054-1087`） |
| Unity 集成 | ✅ 可行 | 项目已用 `vkCreateDevice` 拦截注入 RT 扩展（`vpt_intercept.cpp:32-137`），再追加 `VK_KHR_ray_tracing_pipeline` 与 feature 即可；命令仍录进 Unity 的 `VkCommandBuffer`，不需要自建队列提交 |
| 语义映射 | ✅ 基本一一对应 | 主光线 → raygen + closest-hit；阴影透射 candidate 循环 → any-hit + `IgnoreHit()`；miss → miss；体积状态机 → payload（参考工程 `rahitShadow` 就是这么做的，`gltf_pathtrace.slang:1164-1208`） |
| 代码改动量 | ⚠️ 中 | 粗估 **2.5–3.5 周**（含等价性回归），与 `doc/程序化几何体加速结构接入Terrain可行性调研.md:509-517` 的 2–4 周估算一致 |
| 性能收益 | ❌ 大概率没有 | 参考工程实测：**表面/室内/透射场景 Ray Query 比 RT pipeline（开 SER）还快 10–30%，关 SER 差距更大**（`docs/benchmarking.md:232`）；RT pipeline 的 2× 优势只出现在体积/强发散 scatter 场景，且依赖 SER |
| 本机 SER | ❌ 不可用 | RTX 3070 Ti Laptop：`VK_EXT_ray_tracing_invocation_reorder` 存在，但 `rayTracingInvocationReorderReorderingHint = NONE`（本机 `vulkaninfo` 实测），即 RT pipeline 拿不到 SER 这个最大杠杆 |
| 严重风险 | 中低 | 不会出现数量级/不可用级崩塌（遍历硬件与 BVH 完全不变）；但十几到几十个百分点的 PT 回退是现实的，首帧多一次 0.5–2 s 的 RT pipeline 编译，属于可感知的回归 |
| 兼容性 | ⚠️ 整体切换会丢退路 | RT pipeline 需要 RTX 20 系以上；若遇到设备/驱动问题，没有 Ray Query 回退。双后端可以按 GPU/场景切换 |

**关键数字（先看这三条，后面有出处）**

| 内容 | 参考工程实测（1080p，RTX PRO 4500 Blackwell） |
|---|---|
| Scatter（体积/强发散） | RT pipeline 约 **2×** 快于 Ray Query |
| Surfaces（不透明/透射室内、地形） | Ray Query 比 RT pipeline **+SER** 还快 **10–30%**；关 SER 差距更大 |
| SER 收益 | Scatter **2.3–2.8×**；Surfaces **20–25%** |

> 出处：`vk_gltf_renderer/docs/benchmarking.md:224-234`。注意这三条是同一工程、同一套着色代码的 A/B，不是跨引擎的传闻。

**给当前项目的直接判断**

- 教堂/室内/彩绘玻璃这类**表面型 + 透射**内容，正是参考工程里「Ray Query 更快」的那一档；本机又没有 SER。
- 因此整体切换的预期是：**PT 段变慢（量级十到数十个百分点），不是变快**。会不会「严重」取决于 PT 占帧时间的比例：
  - 当前 1spp + DLSS Balanced（渲染约 568×320 → 输出 967×544，日志实测），PT 估计只有几毫秒（无实测基线，需 P0 确认），慢 20–30% 约等于总帧 +0.3–1 ms，**不致命但可感知**；
  - 若把渲染分辨率拉到 1080p 以上或关 DLSS，PT 占比上升，回退会放大。
- 首帧/换管线时多一次 RT pipeline 编译（含 raygen 大着色器），是另一个独立风险，可用延迟编译/管线缓存压掉。

---

## 2. 两个工程的架构对照

| 维度 | URPPT（现状） | vk_gltf_renderer（参考） |
|---|---|---|
| 宿主 | Unity 2022.3 + URP；VkDevice 由 Unity 创建，插件通过 `InterceptInitialization` 注入扩展 | 独立 Vulkan 应用，自建设备/交换链 |
| PT 后端 | **只有** compute 8×8 + Ray Query（`pathtrace.glsl:131`、`:951-986`） | compute Ray Query 与 RT pipeline **二选一**，运行时可切换（`ptTechnique`，`docs/user-guide.md:44`） |
| 着色器 | GLSL 460，单文件 1212 行、单入口 `main()`、SPIR-V 86 KB | Slang，单模块 7 个 RT 入口 + 1 个 compute 入口，共享 `pathTrace()` |
| 命中着色 | 全部内联在 compute kernel | **raygen 延迟着色**：closest-hit 只回写命中记录，材质/BSDF/纹理都在 raygen |
| 透明阴影 | candidate 循环，不 confirm，靠实例 `FORCE_OPAQUE` 产生 committed 遮挡（`pathtrace.glsl:568-706`） | any-hit + `IgnoreHit()` 累积透射率（`gltf_pathtrace.slang:1164-1208`） |
| 加速结构 | 每 mesh 一个 BLAS，730 BLAS/730 实例；TLAS 每帧全量 rebuild（`vpt_scene.cpp:750-808`） | 每 primitive BLAS，TLAS 增量 update/rebuild；支持 OMM |
| 降噪 | DLSS RR（唯一） | DLSS RR / OptiX / OIDN |
| 场景规模（日志实测） | 730 mesh、730 实例、49 材质、1 光源 | 单场景通常几十到几百 render node |
| 目标硬件 | RTX（README 要求），本机 3070 Ti Laptop 8 GB | RTX；AMD 上参考工程直接禁用 Ray Query（`renderer_pathtracer.cpp:133-136`） |

**结论**：两者都是「1spp + 降噪」的实时路径追踪，参考工程只是把后端抽象出来了；迁移的**算法与数据层几乎不变**，变的是「谁遍历、谁着色、状态怎么回传」。

---

## 3. 参考工程是怎么把同一个 PT 跑在两种后端上的

这一节是可行性的直接证据：不是「理论上可以」，而是「已经有人用同一份着色代码跑通了」。

### 3.1 入口点与职责

| 阶段 | 入口 | 做什么 | URPPT 里对应什么 |
|---|---|---|---|
| raygeneration | `rgenMain`（`gltf_pathtrace.slang:1074-1087`） | 每像素跑完整 `pathTrace()`；内部迭代弹射，不递归 | `pathtrace.glsl:904-1269` 的 `main()` |
| compute（RQ） | `computeMain`（`:1054-1069`） | 同一个 `processPixel`，只是换 `RayQueryRaytracer` | 现状 |
| miss 0 | `rmissMain`（`:1115-1119`） | `hitT = INFINITE` | `:930-950` 的 miss 分支 |
| miss 1 | `rmissShadow`（`:1121-1127`） | 空体：保留 any-hit 累积的透射率 | `:670` 返回 `totalTransmission` |
| closest-hit 0 | `rchitMain`（`:1092-1100`） | **只写** hitT / instance / prim / bary，不做着色 | `getHitInfo()`（`:717-763`）+ `:952-957` |
| closest-hit 1 | `rchitShadow`（`:1102-1109`） | 阴影被挡：`totalTransmission = 0` | `:700-704` committed → 返回 0 |
| any-hit 0 | `rahitMain`（`:1132-1161`） | alpha test，`IgnoreHit()` | 当前 alpha cutout 走 Ray Query candidate；迁 RT pipeline 后由这个 any-hit 承担 |
| any-hit 1 | `rahitShadow`（`:1164-1208`） | 逐层 Beer 吸收 + 透射材质透过率；耗尽时 `AcceptHitAndEndSearch()` | `traceShadowTransmission()`（`:544-671`） |
| intersection | 无 | 全三角形 BLAS，没有 procedural AABB | 现状无 AABB（地形探针除外） |

关键设计：**着色留在 raygen，closest-hit 只做「命中记录回写」**。这样：
- 只有一套材质/BSDF/纹理代码，不需要为每个材质写 hit shader；
- SBT 只需要 2 个 hit group（主路径、阴影），不需要 per-material SBT；
- 代价是 raygen 仍然是「大着色器」，寄存器压力不会因为拆阶段而自动消失。

### 3.2 SBT 与管线创建

参考工程只有 **5 个 shader group**（`renderer_pathtracer.cpp:1362-1398`）：

1. raygen（GENERAL）
2. miss 0（GENERAL）
3. miss 1（GENERAL）
4. hit group 0：closest-hit + any-hit（主路径）
5. hit group 1：closest-hit + any-hit（阴影）

- `maxPipelineRayRecursionDepth = 2`（`:1435`）：阴影光线从 raygen 发出，**不在 CH 里递归**；迭代弹射在 raygen 的 `while` 里（`gltf_pathtrace.slang:620-661`）。
- SBT 用 `nvvk::SBTGenerator` 按运行时查到的 `shaderGroupHandleSize / shaderGroupBaseAlignment` 生成，不硬编码（`:1522-1543`）。
- 材质**不映射到 hit group**：所有实例 `instanceShaderBindingTableRecordOffset = 0`（`gltf_scene_rtx.cpp:339`），命中身份走 `InstanceIndex()` / `instanceCustomIndex` + SSBO 查材质。
- 主光线 `TraceRay(..., sbtOffset=0, sbtStride=0, missIndex=0)`；阴影 `(1, 0, 1)`（`raytracer_interface.h.slang:233,283`）。也就是说 **sbtOffset 直接选 hit group**，不需要按实例改 SBT 偏移。

### 3.3 Payload 与 hit attribute

- 主路径 `HitPayload`：`seed, hitT, rnodeID, rprimID, primitiveID, bary`，字段合计约 **28 B**（`raytracer_interface.h.slang:36-47`）。
- 阴影 `ShadowPayload`：`seed, prevHitT, isInside, extinction(float3), totalTransmission(float3)`，字段合计约 **36 B**（`:50-57`）。参考工程实际可用，说明至少这个尺寸没问题。
- hit attribute 只用 `BuiltInTriangleIntersectionAttributes`（`barycentrics` float2，8 B），远小于本机 `maxRayHitAttributeSize = 32`（`vulkaninfo` 实测）。
- **不要把 URPPT 的 `HitInfo`（约 72 B，`pathtrace.glsl:741-750`）塞进 hitAttribute**。正确做法：payload 只带 `instance/material/prim/bary/hitT`（≤ 32 B），raygen 用这些索引回读 SSBO 重建 `HitInfo`——这正是参考工程 `getHitState()` 的做法。

### 3.4 与 URPPT 现状的「同构」程度

| URPPT 现状 | RT pipeline 对应物 | 是否需要重写逻辑 |
|---|---|---|
| `rayQueryInitializeEXT` 主光线（`:920-925`） | `traceRayEXT` + hit group 0 | 否，换 API |
| `getHitInfo(rq)`（`:717-763`） | CH 回写 payload + raygen 重建 | 否，换数据通路 |
| `traceShadowTransmission()` candidate 循环（`:544-671`） | AH + `inout` payload + `IgnoreHit()` | **逻辑要搬家**，公式不变 |
| committed 遮挡判断（`:700-704`） | 阴影 CH 置 0 / miss 返回透射率 | 否 |
| 体积状态机（`:632-644`、`:1174-1186`） | payload 里带 `isInside/sigmaT/prevT` | 是，状态从局部变量变 payload |
| G-buffer / RR albedo 写出（`:1015-1029`） | raygen 里同一段代码 | 否 |
| NEE / RR / 纹理 / BSDF（`:1044-1201`） | raygen 里同一段代码 | 否 |
| `instanceCustomIndex = materialID`（`vpt_scene.cpp:718`） | CH 里 `gl_InstanceCustomIndexEXT` | 否，语义正好对上 |
| 实例 `FORCE_OPAQUE`（`vpt_scene.cpp:731-733`） | 不透明实例跳过 any-hit；透射实例进 AH | 否，语义正好对上 |

**一句话：算法/材质/阴影公式全部复用，改的是「入口 + 状态回传 + SBT/管线创建」。**

---

## 4. 迁到 URPPT 的具体工作项

### 4.1 设备与扩展（`vpt_intercept.cpp`）

现状只注入 `deferred_host_operations / buffer_device_address / acceleration_structure / ray_query`（`vpt_intercept.cpp:64-68`、`:83-85`）。迁 RT pipeline 需要追加：

| 项 | 内容 | 备注 |
|---|---|---|
| 扩展 | `VK_KHR_ray_tracing_pipeline` | 依赖 `VK_KHR_acceleration_structure`、`VK_KHR_deferred_host_operations`（已有） |
| 依赖扩展 | `VK_KHR_spirv_1_4`、`VK_KHR_shader_float_controls` | Vulkan 1.2+ 上多为核心，但启用 RT pipeline 扩展时建议显式加入，避免验证层/驱动依赖检查 |
| Feature | `VkPhysicalDeviceRayTracingPipelineFeaturesKHR{ rayTracingPipeline = VK_TRUE }` | 按现有模式挂到 pNext 链，注意别和 Unity 原链重复 sType |
| Properties | `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`（`vkGetPhysicalDeviceProperties2`） | 取 `shaderGroupHandleSize`、`shaderGroupBaseAlignment`、`maxShaderGroupStride`、`maxRayRecursionDepth` |

本机实测（`vulkaninfo`）：

```
shaderGroupHandleSize          = 32
shaderGroupBaseAlignment       = 64
maxRayRecursionDepth           = 31
maxRayHitAttributeSize         = 32
maxRayDispatchInvocationCount  = 1073741824
```

> KHR 结构里**没有** `maxRayPayloadSize` 查询；参考工程 36 B 的 `ShadowPayload` 已实际可用。稳妥设计仍是 payload ≤ 32 B、大结构走 SSBO。

### 4.2 着色器拆分

两套方案，按是否愿意引入 Slang 选：

| 方案 | 做法 | 优点 | 代价 |
|---|---|---|---|
| **A. GLSL 多入口**（改动小） | 把现有 `pathtrace.glsl` 的公共函数抽到 `pathtrace_common.glsl`；新增 `pt_raygen.glsl` / `pt_miss.glsl` / `pt_chit.glsl` / `pt_shadow_chit.glsl` / `pt_shadow_ahit.glsl`，每个入口一个 `main()` | 工具链不变（`glslangValidator` 已支持 `GL_EXT_ray_tracing`）；改动可控 | 6 个入口（raygen / miss×2 / CH×2 / AH×1）、6 段 SPIR-V；公共代码靠 include |
| **B. Slang 单模块**（对齐参考） | 像参考工程一样，一个 `.slang` 里 `[shader("raygeneration")]` 等入口，共享 `pathTrace()`；`slangc` 预编译成嵌入头 | 与参考工程结构一致，后续双后端/SER/OMM 更好扩展 | 引入 Slang 构建依赖；`spv_to_header.ps1` / CMake 要改 |

**推荐先做 A**（如果只是验证 RT pipeline 性能），**要做双后端/长期维护再上 B**。参考工程的 `IRaytracer` 抽象（`raytracer_interface.h.slang:60-67`）在 Slang 里很自然，在 GLSL 里则要靠 `#ifdef` + 两套 `traceRay/traceRayInline` 包装，维护性差一些。

拆分后的入口职责（与 §3.1 对应）：

```
raygen:
  for each pixel:
    for spp:
      for depth:
        traceRayEXT(hitgroup 0, miss 0, payload0)   // 主光线，RAY_FLAG_FORCE_OPAQUE
        if miss -> env/ambient, break
        reconstruct HitInfo from payload0 + SSBO
        shading / NEE / BSDF / volume state / G-buffer
        if NEE:
          traceRayEXT(hitgroup 1, miss 1, payload1) // 阴影，ACCEPT_FIRST_HIT_AND_END_SEARCH
          radiance += contribution * payload1.totalTransmission

closest-hit 0:  payload0 = {instance, material, prim, bary, hitT}   // 不 shading
any-hit 0:      （alpha cutout；迁 RT pipeline 后必须保留，当前 Ray Query 版走 candidate）
closest-hit 1:  payload1.totalTransmission = 0                       // 被不透明挡住
any-hit 1:      逐层 Beer + 透射率；IgnoreHit() / 耗尽时 AcceptHitAndEndSearch()
miss 0:         payload0.hitT = INFINITE
miss 1:         （空体，保留累积透射率）
```

注意几个语义点：

1. **主光线用 `RAY_FLAG_FORCE_OPAQUE`**：与现状 `gl_RayFlagsOpaqueEXT` 一致，透明表面仍会被 CH 命中，然后由 BSDF 的 transmission 分支处理；只是不触发 any-hit。
2. **阴影用 `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`**，配合实例级 `FORCE_OPAQUE`：不透明实例没有 AH、直接 CH 置 0 并结束；透射实例进 AH，`IgnoreHit()` 继续。与现状 candidate 循环语义等价（参考工程就是这么写的）。
3. **AH 的到达顺序不保证**：现状已经用 `abs(hitT-prevT)` 近似段长（`pathtrace.glsl:641`），RT pipeline 同样不保证顺序，所以**不会引入新的正确性问题**，但也不会修好这个近似。
4. **体积状态机**要放进 `ShadowPayload`（`isInside / extinction / prevHitT`），这正是参考工程 `ShadowPayload` 的字段（`raytracer_interface.h.slang:50-57`）。主路径的状态仍在 raygen 局部变量里，不需要 payload。
5. **payload location**：主/阴影用两个不同 location（如 0 和 1），CH/AH 里用 `rayPayloadInEXT` 对应声明；hitAttribute 用 `vec2`（8 B）。
6. **Alpha cutout**：当前 Ray Query 版在主光线 candidate 循环里按 `alpha < cutoff` 丢弃微面（`pathtrace.glsl:951-986`）；迁 RT pipeline 后改用 any-hit + `IgnoreHit()`，语义等价；后续可接 OMM 把大部分判定前移到硬件。

### 4.3 SBT 生成

参考工程用 nvpro 的 `SBTGenerator`（`renderer_pathtracer.cpp:1522-1543`），URPPT 没有 nvpro，需要自己写 ~100–200 行：

```
1. vkGetRayTracingShaderGroupHandlesKHR(pipeline, 0, groupCount, ...)
2. handleSize = props.shaderGroupHandleSize (32)
   handleAlignment = props.shaderGroupHandleAlignment (通常 32)
   baseAlignment = props.shaderGroupBaseAlignment (64)
3. stride = align(handleSize, handleAlignment)              // 32
4. raygenOffset = 0；raygenSize = align(stride, baseAlignment) // 64
5. missOffset = raygenOffset + raygenSize；missSize = align(2*stride, baseAlignment)
6. hitOffset = ...；hitSize = align(2*stride, baseAlignment)
7. buffer: usage = SHADER_BINDING_TABLE_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT,
   host-visible + mapped；按 region 拷贝 group handle
8. vkGetBufferDeviceAddress → VkStridedDeviceAddressRegionKHR
9. vkCmdTraceRaysKHR(cmd, &raygen, &miss, &hit, &callable(空), w, h, 1)
```

本机 handle 32 / base 64，两个 hit group + 2 miss + 1 raygen，SBT 总量 < 1 KB，显存可忽略。

### 4.4 管线、描述符、同步、计时、DLSS

| 位置 | 现状 | 迁移后 |
|---|---|---|
| 描述符 stageFlags | 全部 `VK_SHADER_STAGE_COMPUTE_BIT`（`vpt_dispatch.cpp:130-149`） | 改为 `RAYGEN_BIT_KHR \| CLOSEST_HIT_BIT_KHR \| ANY_HIT_BIT_KHR \| MISS_BIT_KHR`（或 `ALL`） |
| 管线创建 | `vkCreateComputePipelines`（`:173-181`） | `vkCreateRayTracingPipelinesKHR`；`maxPipelineRayRecursionDepth = 1`（raygen 只发一次，不递归；取 2 更保险）；可选 `VkDeferredOperationKHR` 异步编译 |
| 录制 | `vkCmdBindPipeline(COMPUTE)` + `vkCmdDispatch`（`:851-857`） | `vkCmdBindPipeline(RAY_TRACING)` + `vkCmdBindDescriptorSets` + `vkCmdTraceRaysKHR` |
| Barrier 1 | dst stage `COMPUTE_SHADER`（`:841-847`） | dst stage `RAY_TRACING_SHADER_KHR` |
| Barrier 2 | src/dst `COMPUTE_SHADER`（`:868-871`） | src `RAY_TRACING_SHADER_KHR`，dst 保持 `COMPUTE_SHADER`/`FRAGMENT_SHADER`（NGX/Blit 读） |
| GPU timer | T4 标在 compute dispatch 后（`:877`） | 位置不变，语义变成 traceRays 结束；可再加子段（可选） |
| DLSS RR | PT 输出 5 张图 → MV compute → NGX evaluate（`vpt_dlss.cpp:745-774`） | **完全不变**；只要 barrier 的 stage 改对，输入图内容/格式不变 |
| Unity 事件 | `CommandRecordingState` + `IssuePluginEvent`（`vpt_api.cpp:98-114`） | 不变；`vkCmdTraceRaysKHR` 可以录进 Unity 的 primary/secondary command buffer |

### 4.5 不需要改的部分

- BLAS/TLAS 构建、refit、TLAS 环形缓冲（`vpt_scene.cpp`）——RT pipeline 用同一套 AS。
- 实例 `FORCE_OPAQUE` / `TRIANGLE_FACING_CULL_DISABLE` 标记（`vpt_scene.cpp:731-733`）——语义正好对上。
- `instanceCustomIndex = materialID`（`:718`）——CH 可直接取。
- C# 侧调度、MotionVector kernel、RR 参数、Blit——不变。
- 输出图格式（rgba16f/rgba32f）与 Y-flip——不变。

### 4.6 工作量估算

| 阶段 | 内容 | 人天 |
|---|---|---|
| P0 | 打开 GPU timer 落盘，固定相机路径测 RQ 基线（PT / DLSS / 总帧） | 0.5–1 |
| P1 | interceptor 追加 RT pipeline 扩展/feature/properties | 0.5 |
| P2 | 着色器拆分 + payload/hitAttribute + 体积状态搬家 | 3–5 |
| P3 | SBT 生成器 + buffer + 设备地址 | 1 |
| P4 | RT pipeline 创建、描述符 stage、延迟编译/缓存 | 1–1.5 |
| P5 | 录制/barrier/timer/DLSS 对接 | 1–1.5 |
| P6 | 等价性回归（玻璃/体积/阴影/G-buffer/RR 逐像素 diff） | 3–5 |
| P7 | 性能 A/B（3 场景 × 2 后端 × 分辨率/spp） | 1.5–2 |
| **合计** | | **11.5–17.5 人天 ≈ 2.5–3.5 周** |

双后端（保留 RQ + 加 RT pipeline + 运行时开关）在上述基础上 **+2–3 天**；如果要 SER，再 **+1–2 周**（仅 Ada+ 有意义）。

---

## 5. 性能分析：会不会产生严重问题

### 5.1 先明确「什么变了、什么没变」

**没变**：
- 同一块 RT core、同一棵 BVH、同样的射线数量与弹射深度；
- 同样的材质/BSDF/纹理采样/NEE 计算量；
- TLAS/BLAS 构建成本、DLSS RR 成本。

**变了**：
- 遍历状态从「shader 里的 ray query 对象」变成「固定功能遍历 + 硬件栈」；
- 每次命中从「inline 处理」变成「SBT 间接 + CH/AH 调用 + payload 读写」；
- 阴影透射从「一个循环里累积」变成「每层一次 any-hit 调用」；
- 不再受 compute workgroup（8×8）调度，raygen 按 invocation 调度；
- 有 SER 的 GPU 上，RT pipeline 还能把发散 invocation 重排序（本机没有）。

所以**不存在「换管线导致遍历慢一个数量级」的物理原因**；差异只可能在几个百分比到几十个百分点，方向由内容发散度和是否有 SER 决定。

### 5.2 参考工程的实测（最有价值的证据）

`vk_gltf_renderer/docs/benchmarking.md:224-234`，1920×1080，RTX PRO 4500 Blackwell：

| 参数 | 效果 |
|---|---|
| `--ptTechnique` | Ray Query（compute）vs RT pipeline。**Scatter：RT pipeline 约 2× 更快。Surfaces：Ray Query 比 RT pipeline（开 SER）快 10–30%，关 SER 差距更大** |
| `--ptUseSER` | RT pipeline 专用。**Scatter 2.3–2.8×；Surfaces 20–25%** |
| `--ptOptimalShader` | 按场景裁剪材质特性宏。Scatter 14–20%（SER on）/ ~32%（SER off）；Surfaces pipeline ~20%，Ray Query 更多（主要省寄存器） |

其中：
- **scatter** = 体积/强发散样例；**surfaces** = 不透明/透射室内 + 地形（含约 100 材质的房间、FlightHelmet 等）。
- 这组数字说明：**RT pipeline 的优势主要来自「发散负载 + SER」，不是「RT pipeline 本身遍历更快」**。

### 5.3 本机没有 SER，这是关键约束

本机 `vulkaninfo`：

```
VK_EXT_ray_tracing_invocation_reorder : revision 2
VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT:
    rayTracingInvocationReorderReorderingHint = RAY_TRACING_INVOCATION_REORDER_MODE_NONE_EXT
```

即：RTX 3070 Ti Laptop 上 RT pipeline **可以创建**，但**没有 SER**。参考工程最大的两个收益（scatter 2.3–2.8×、surfaces 20–25%）在本机直接归零。剩下的只有「SBT/CH/AH 的固定开销」和「raygen 拆分可能带来的寄存器收益」。

### 5.4 按参考数据粗算「无 SER 的 RT pipeline」

> 下面是**同一参考工程、同一内容**下，用 `--ptTechnique` 与 `--ptUseSER` 两行做的量级推算，**不是本机实测**；跨内容不能直接套用，只用于判断方向。
> 推算假设 `--ptTechnique` 那次测量是**开 SER** 的：参考工程的 benchmark 脚本在 load 段就写了 `--ptTechnique 1` + `--ptUseSER 1`（`docs/benchmarking.md:201-211`）。若该行本身未开 SER，则 scatter 的 2× 就是无 SER 结果，下表第二行不成立；但 surfaces 档「RQ 更快」的结论不受影响。

设 Ray Query 时间为 1.0：

| 场景 | RT pipeline + SER（相对 RQ） | SER 收益 | RT pipeline 无 SER（推算） |
|---|---|---|---|
| Surfaces | 1.10–1.30（慢 10–30%） | 20–25% → 时间 ×1.25–1.33 | **约 1.4–1.7×（慢 40–70%）** |
| Scatter | 0.50（快 2×） | 2.3–2.8× → 时间 ×2.3–2.8 | **约 1.15–1.4×（慢 15–40%）** |

这张表想说明的是：**在参考工程的内容上，一旦没有 SER，RT pipeline 连 scatter 都不一定赢，surfaces 更是明显落后**。本机正好是无 SER 的 3070 Ti。

> 注意：URPPT 的场景（730 实例、彩绘玻璃、单光源、1spp）与参考工程的测试场景不完全一样；URPPT 的 shader 是「全特性单文件」，寄存器压力可能比参考工程的 RQ 路径更大，这**可能**缩小差距甚至反转。所以最终必须用本项目的 A/B 实测说话，不能只看这张表。

### 5.5 URPPT 的负载特征

| 特征 | 数值/事实 | 对迁移的含义 |
|---|---|---|
| 渲染分辨率 | 约 568×320（DLSS Balanced 1.7× → 967×544，日志实测） | 每帧主射线 ~18 万，PT 估计只有几毫秒（无实测基线）；固定开销占比相对高，RT pipeline 的 SBT/launch 开销更不划算 |
| spp | 1（`PathTraceRendererFeature.cs:52`） | 没有足够样本摊薄固定开销 |
| maxDepth | 8（`:49`） | 与参考工程相当 |
| 场景 | 730 mesh / 730 实例 / 49 材质 / 1 光源 | 实例多但材质少；一个 hit group + materialID 足够 |
| 透明/体积 | 薄壁玻璃 + 有厚度玻璃（Beer-Lambert，单介质）；阴影透射最多 16 层（`pathtrace.glsl:302`） | 阴影 AH 可能被多层玻璃反复调用，是 RT pipeline 的主要潜在回退点 |
| 主光线相干性 | 相机射线高度相干；漫反射弹射一般；阴影射线多数相干 | 属于参考工程的「surfaces」档，Ray Query 的主场 |
| 着色器 | 1212 行、86 KB SPIR-V、全特性编译 | 寄存器压力可能高；拆阶段/裁剪特性都可能带来收益，但不一定是 RT pipeline 独有 |

### 5.6 量化：什么程度算「严重」

假设 PT 段在 3070 Ti 上为 **2–4 ms**（568×320、1spp、8 弹射；没有实测，需 P0 基线确认）：

| 情形 | PT 变化 | 总帧 16.7 ms 下的影响 | 体感 |
|---|---|---|---|
| 乐观（寄存器压力主导，RT 反而更快） | −10% ~ −20% | −0.2 ~ −0.8 ms（改善） | 几乎无感 |
| 中性（与参考 surfaces 档接近） | +20% ~ +30% | +0.4–1.2 ms | 可感知，不致命 |
| 悲观（无 SER + 多层玻璃 AH 开销） | +40% ~ +70% | +0.8–2.8 ms | 明显回退，高分辨率下可能掉帧 |
| 首帧/换管线编译 | 一次性 0.5–2 s | 启动卡顿 | 可用延迟编译/缓存压掉 |

**结论**：
- 不会出现「RT pipeline 一换，帧率砍半/不可用」的原理性灾难；遍历硬件没变。
- 但在**当前机器 + 当前内容**上，最可能的区间是 **PT 慢 20–40%**，属于「明显但不致命」；只有在高分辨率/PT 占比大时才可能变成「严重」。
- 更重要的是：**它大概率不会让当前项目更快**。如果目标是性能，迁移不是正确工具。

### 5.7 什么情况下 RT pipeline 才可能真的更快

1. **RTX 40/50 + SER**：发散射线重排序，参考工程实测 scatter/体积场景约 2×；alpha/OMM 场景原理上也受益，但需实测。
2. **大量 alpha-test / OMM**：RT pipeline 的 any-hit/OMM 路径更自然；参考工程用 OMM 把 alpha 成本转成硬件。
3. **多材质、复杂 hit shader**：RT pipeline 支持 per-material hit group/着色器特化；但 URPPT 目前 49 材质、单套着色，用不上。
4. **未来要 procedural AABB / 地形 intersection shader**：RT pipeline 更自然；但 Ray Query 也支持 AABB（见地形调研 §6.3），不是充分理由。
5. **AMD 兼容**：参考工程在 AMD 上直接禁用 Ray Query（驱动崩溃），RT pipeline 是唯一路径；URPPT 目前只要求 NVIDIA，暂不构成理由。

---

## 6. 风险清单（按严重度）

| # | 风险 | 影响 | 规避/验证 |
|---|---|---|---|
| 1 | 无 SER 的 3070 Ti 上 RT pipeline 变慢 | PT 回退 20–40%+ | 先做双后端 A/B；默认 RQ；只在 Ada+ 且实测更快的场景启用 RT |
| 2 | RT pipeline 首帧编译/链接耗时 | 启动或切场景卡顿 0.5–2 s | `VkDeferredOperationKHR` 异步编译；`VkPipelineCache` 落盘；启动时预编译 |
| 3 | 多层玻璃阴影 any-hit 调用开销 | 彩绘玻璃/多层透射场景 PT 变慢 | A/B 时单独测「玻璃特写」；必要时限制 `MAX_SHADOW_LAYERS` 或对玻璃走近似 |
| 4 | 扩展/feature 依赖漏注入 | `vkCreateDevice` 失败或验证层报错 | 追加 `VK_KHR_spirv_1_4` / `VK_KHR_shader_float_controls`；开验证层跑一帧 |
| 5 | Barrier stage 沿用 COMPUTE | 图像布局/读写竞争，花屏或卡死 | Barrier 1/2 改用 `RAY_TRACING_SHADER_KHR`；检查 NGX 前的 stage |
| 6 | SBT 对齐/句柄大小硬编码 | 花屏/崩溃/设备丢失 | 全部从 `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` 取；按 64 对齐 region |
| 7 | Payload/HitAttribute 超限 | 管线创建失败或越界 | payload ≤ 32 B；hitAttribute 只用 `vec2`；大结构走 SSBO |
| 8 | 阴影 AH 顺序不保证 | 体积段长近似误差（现状已存在） | 与现状同口径用 `abs()`；记录为已知近似，不当作迁移 bug |
| 9 | 整体切换后没有回退路径 | 某些 GPU/驱动上不可用 | 双后端；按设备能力选择 |
| 10 | GLSL 多入口工具链改动 | SPIR-V 生成/嵌入脚本要改 | 扩展 `spv_to_header.ps1` 支持多数组；或改用 Slang 单模块 |
| 11 | 调试工具支持差异 | RenderDoc/Nsight 对 RT pipeline 的调试体验不同 | 保留 RQ 后端作为对照；用验证层 + 逐像素 diff |
| 12 | Unity 设备重建设备丢失 | RT pipeline/SBT 句柄失效 | 复用现有 `VPT_IsDeviceLost` / 重建流程；SBT 与 pipeline 一起销毁重建 |

---

## 7. 建议路线

### 方案 A（推荐）：双后端，RQ 默认，RT pipeline 可选

| 阶段 | 内容 | 产出 |
|---|---|---|
| A0 | 用现有 GPU timer 记录 RQ 基线（PT / DLSS / 总帧，固定相机路径） | 性能基线表 |
| A1 | 抽公共着色器代码，保留 `computeMain`，新增 RT pipeline 入口（GLSL 多文件或 Slang） | 两套后端可编译 |
| A2 | interceptor + SBT + `vkCreateRayTracingPipelinesKHR` + `vkCmdTraceRaysKHR` | RT 后端可跑 |
| A3 | 等价性回归：radiance / gbuffer / albedo 逐像素 diff；玻璃/体积/阴影/RR 对比 | 无视觉差异 |
| A4 | A/B：3 个场景 × 2 后端 × 2 分辨率；记录 PT ms、首帧编译、显存 | 决策数据 |
| A5 | 运行时开关：默认 RQ；Ada+ 且实测更快的场景切 RT（未来可接 SER） | 不丢性能、保留扩展性 |

**收益**：不牺牲当前性能；保留未来 SER/OMM/Slang 的可能性；出问题可回退。
**成本**：比整体切换多 2–3 天。

### 方案 B：整体切换（只在以下目标明确时）

- 目标是**统一到参考工程的 Slang + SBT + SER 架构**，愿意接受短期 PT 回退；
- 或者目标硬件明确是 **RTX 40/50 + 发散/体积内容**，且实测 RT pipeline 更快；
- 或者需要 **AMD 兼容**（参考工程在 AMD 上禁用 RQ）。

必须配套：P0 基线、P6 等价回归、首帧延迟编译、失败回退计划。

### 方案 C：不迁移（如果目标只是「更快」）

- 继续 Ray Query；
- 把精力放在**着色器特性裁剪**（参考工程 `--ptOptimalShader` 在 surfaces 上 pipeline ~20%、RQ 更多，主要省寄存器）、阴影层数/偏移、DLSS RR 输入分辨率上；
- 这些是低风险、可能立刻见效的优化，不需要动后端。

### 决策矩阵

| 你的目标 | 建议 |
|---|---|
| 提高当前帧率 | **C**（不迁），先做着色器裁剪/基线测量 |
| 为未来 SER / OMM / Slang 统一铺路 | **A**（双后端） |
| 与参考工程代码结构完全对齐 | **A → 视 A/B 结果决定是否把 RT 设为默认** |
| 目标硬件是 RTX 40/50 且内容发散 | **A/B 都可**，但必须实测 |
| 需要 AMD 兼容 | **B**（参考工程 AMD 上只能走 RT pipeline） |
| 只想少维护一套后端 | **B**，但接受回退；不建议 |

---

## 8. 验收与实验计划（迁移前先做）

### 8.1 基线（P0）

- 固定相机路径（避免视角差异），`spp=1`，DLSS 关闭（隔离 PT），分别测 568×320 与 967×544。
- 用 `vpt_gpu_timer.h` 的 7 段计时，把 `STAGE_PATHTRACE`、`STAGE_DLSS`、`STAGE_TOTAL` 落到 `VPT-trace` 日志（当前只采集不落盘，`vpt_gpu_timer.h:196-204`）。
- 场景至少 3 个：教堂室内（现状）、玻璃特写（多层透射）、室外/远景（相干主光线）。

### 8.2 迁移后 A/B

| 指标 | 通过标准 |
|---|---|
| PT 段 ms | 目标场景 ≤ +5%（否则不切默认）；记录 ±% |
| 总帧 ms | 不劣化超过 5% |
| 首帧编译 | 记录 pipeline creation 时间；用延迟编译/缓存后 < 100 ms 卡顿 |
| 视觉 | radiance/gbuffer/albedo 逐像素 diff，无结构差异；玻璃/体积/阴影人工对比 |
| 稳定性 | 验证层无错误；连续跑 10 分钟无设备丢失/花屏 |
| 显存 | SBT + pipeline 增量记录（预计 < 10 MB） |

### 8.3 建议的 A/B 矩阵

| 场景 | RQ | RT pipeline | 备注 |
|---|---|---|---|
| 教堂室内 | ✅ | ✅ | 主判据（表面型） |
| 玻璃特写（多层） | ✅ | ✅ | 看 AH 开销 |
| 室外远景 | ✅ | ✅ | 看主光线相干性 |
| （可选）体积雾/散射 | ✅ | ✅ | 看是否接近参考工程的 scatter 收益 |

---

## 9. 证据与参考

### 9.1 参考工程（vk_gltf_renderer）

| 内容 | 位置 |
|---|---|
| 同一 PT 双后端抽象 | `shaders/raytracer_interface.h.slang:60-67`；RQ 实现 `:70-196`；RT 实现 `:199-290` |
| RT 入口点 | `shaders/gltf_pathtrace.slang:1054-1208`（raygen/compute/CH/miss/AH） |
| raygen 延迟着色 | `gltf_pathtrace.slang:1074-1087`；CH 只回写 `:1092-1100`；AH 阴影 `:1164-1208` |
| payload 定义 | `raytracer_interface.h.slang:36-57`（HitPayload ~28 B，ShadowPayload ~36 B） |
| shader group / pipeline | `src/renderer_pathtracer.cpp:1362-1398`（5 组）；`:1427-1454`（recursion=2）；`:1522-1543`（SBT） |
| `vkCmdTraceRaysKHR` | `src/renderer_pathtracer.cpp:2337` |
| 实例 SBT offset / custom index | `src/gltf_scene_rtx.cpp:337-339` |
| 性能实测 | `docs/benchmarking.md:224-234` |
| 用户手册的管线说明 | `docs/user-guide.md:44`（两种管线结果一致，RQ 避免部分工作负载的管线开销） |
| AMD 禁用 RQ | `src/renderer_pathtracer.cpp:133-136` |

### 9.2 本仓库（URPPT）

| 内容 | 位置 |
|---|---|
| 当前 compute/ray query 主循环 | `NativePlugin/shaders/pathtrace.glsl:904-1269` |
| 主光线 ray query + alpha cutout candidate 循环 | `pathtrace.glsl:951-986` |
| 阴影透射 candidate 循环（含镂空分支） | `pathtrace.glsl:568-706` |
| Alpha cutout 辅助（candidate UV / alpha） | `pathtrace.glsl:533-553` |
| 命中重建 | `pathtrace.glsl:752-798` |
| G-buffer / RR albedo | `pathtrace.glsl:1072-1086` |
| compute 管线/描述符 | `NativePlugin/src/vpt_dispatch.cpp:124-181` |
| dispatch/barrier/timer | `vpt_dispatch.cpp:804-877` |
| 设备扩展注入 | `NativePlugin/src/vpt_intercept.cpp:32-137` |
| BLAS/TLAS、实例标记 | `NativePlugin/src/vpt_scene.cpp:468-558`、`:679-746`、`:750-808` |
| Alpha cutout 材质 flags / cutoff | `Assets/Scripts/ModelManager.cs:80-86`、`:827-839`；`NativePlugin/src/vpt_types.h:94-97` |
| DLSS RR 回调 | `NativePlugin/src/vpt_dlss.cpp:745-774` |
| GPU 分段计时 | `NativePlugin/src/vpt_gpu_timer.h:8-22`、`:196-204` |
| 迁移工作量旧估算 | `doc/程序化几何体加速结构接入Terrain可行性调研.md:509-517` |
| 本机 RT 属性实测 | `vulkaninfo`（`shaderGroupHandleSize=32`、`shaderGroupBaseAlignment=64`、`maxRayRecursionDepth=31`、`maxRayHitAttributeSize=32`、`maxRayDispatchInvocationCount=1G`） |
| 本机 SER 不可用 | `vulkaninfo`：`rayTracingInvocationReorderReorderingHint = NONE_EXT` |
| 本机 OMM 可用 | `vulkaninfo`：`VK_KHR_opacity_micromap` 存在，`micromap = true` |

### 9.3 规范/公开资料

- Vulkan `VK_KHR_ray_tracing_pipeline` / `VK_KHR_ray_query` / `VK_KHR_acceleration_structure` 规范（Khronos）。
- NVIDIA, *Improving Ray Tracing Performance with Shader Execution Reordering*（SER 的原理与收益；参考工程 `docs/user-guide.md:45` 引用）。
- 参考工程 `docs/benchmarking.md` 的测试方法（固定相机、spp、`ptOptimalShader`、`ptTechnique`、`ptUseSER`）。

---

## 10. 附录：SER 与 OMM 是什么

> 这两个都是 NVIDIA RTX 的硬件加速特性，但解决的问题完全不同：**SER 解决「着色发散/调度」，OMM 解决「alpha test 镂空」**。两者都不是新的光追遍历算法，也不会改变 BVH 或射线数量。

### 10.1 SER — Shader Execution Reordering（着色器执行重排序）

**是什么**：NVIDIA 在 Ada 架构（RTX 40 系）引入的硬件调度能力。它把「求交」和「着色」拆开：先只拿到 `HitObject`，再由 `ReorderThread` 让硬件按材质、命中点、方向等 key 把 invocation 重新分组，最后统一着色。

**解决什么问题**：路径追踪是 SIMT 执行。一个 warp 的 32 个 lane 打到不同材质/不同方向的表面后，会走不同分支、采样不同纹理，产生严重 divergence——部分 lane 空转、纹理 cache 命中率低。SER 不改变遍历，只让**后续着色**的 warp 更「整齐」。

**API**：

- Vulkan：`VK_EXT_ray_tracing_invocation_reorder`（早期为 `VK_NV_ray_tracing_invocation_reorder`），shader 侧用 `SPV_EXT_shader_invocation_reorder` 的 `HitObject` + `ReorderThread`。
- DXR：`SetShaderExecutionReordering` / `ReorderThread`。

**参考工程怎么用**：`raytracer_interface.h.slang:207-234` 的 `USE_SER` 分支：`HitObject::TraceRay()` → `ReorderThread(hitObj)` → 再取属性着色。Ray Query compute 路径没有接 SER。

**实测收益**（`docs/benchmarking.md:233`）：Scatter（体积/强发散）**2.3–2.8×**；Surfaces（室内/透射表面）**20–25%**。

**本机**：RTX 3070 Ti Laptop 上 `VK_EXT_ray_tracing_invocation_reorder` 扩展存在，但 `rayTracingInvocationReorderReorderingHint = NONE_EXT`，即**没有 SER 硬件**。参考工程里 RT pipeline 在 scatter 场景的 2× 优势主要来自 SER，本机拿不到。

### 10.2 OMM — Opacity Micromap（不透明度微图）

**是什么**：一种加速 alpha test / 镂空的硬件结构。它把每个三角形细分成微三角形，并为每个微三角形预标记 opaque / transparent / unknown，挂在 BLAS 的三角形几何上。遍历时硬件直接查表，只有 unknown 的微三角形才回退到 any-hit 采样 alpha。

**解决什么问题**：alpha-tested 几何（树叶、栅栏、铁丝网、头发片）传统上靠 any-hit：每条射线每次打到三角形都要跑 shader、采样 alpha、决定 `IgnoreHit`。即使大部分命中最终被裁掉，也要付出 shader/纹理/发散成本，而且硬件不能把命中当作 opaque 快速提交。OMM 把大部分判定前移到遍历硬件。

| 微三角形状态 | 硬件行为 |
|---|---|
| Opaque | 直接当作不透明命中提交，不调用 any-hit |
| Transparent | 直接剔除 |
| Unknown | 回退到 any-hit，采样 alpha 贴图判定 |

**API**：`VK_KHR_opacity_micromap`（早期 `VK_EXT_opacity_micromap`）；micromap 本身是一种 acceleration structure，通过 `VkAccelerationStructureTrianglesOpacityMicromapKHR` 挂到 BLAS。RT pipeline 创建时加 `VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_KHR`；Ray Query 入口要在 SPIR-V 里声明 `OpacityMicromapIdKHR` execution mode（参考工程用 `USE_OMM` 宏控制）。

**参考工程**：`docs/RENDERING_ARCHITECTURE.md:66-99`（OMM 构建与挂接）、`docs/user-guide.md:155-161`（用法与可视化：绿色=OMM opaque，黄色=unknown，transparent=直接穿过）、`src/gltf_scene_omm.cpp:21`（从 glTF `EXT_mesh_opacity_micromap` 构建）。

**代价/限制**：

- 额外显存（micromap + 每 primitive 的 index buffer）、构建时间、资产管线要烘焙；
- **只解决 alpha cutout**，不解决玻璃、alpha blend、透射、体积吸收；
- 需要 RTX GPU + 较新驱动；KHR 与 EXT 的 feature struct **不是别名**，不能混用；
- 当前工程 SDK **1.4.313.2 的头文件里只有 EXT 版本**（`VK_EXT_opacity_micromap`），没有 KHR OMM / `VK_KHR_device_address_commands` / `vkCreateAccelerationStructure2KHR`；直接用 KHR 需先升级 Vulkan-Headers，并 probe Unity loader 是否能返回新命令；
- 本机支持：`vulkaninfo` 实测 `VK_KHR_opacity_micromap` 存在、`micromap = true`，与 SER 不同。

### 10.3 两者对比与本项目关系

| 维度 | SER | OMM |
|---|---|---|
| 解决的问题 | 着色发散、warp 利用率 | alpha-test / 镂空的遍历与 any-hit 开销 |
| 机制 | 求交后重排 invocation | 预分类微三角形，硬件遍历时直接判定 |
| Vulkan 扩展 | `VK_EXT_ray_tracing_invocation_reorder` | `VK_KHR_opacity_micromap` |
| 适用管线 | 参考工程只接在 RT pipeline 的 `HitObject` 路径 | RT pipeline（pipeline flag）与 Ray Query（execution mode）都支持 |
| 硬件门槛 | Ada / RTX 40 及以后 | Turing / RTX 20 及以后（看驱动） |
| 本机 3070 Ti | 扩展在，但 hint=NONE，**不可用** | `micromap=true`，**可用** |
| 对本项目 | 整体切 RT pipeline 的主要潜在收益；当前内容用不上 | 已决定暂不接入（见 10.4） |

**与刚完成的 alpha cutout 的关系**：当前实现走的是「主光线 candidate 循环 + 阴影 candidate 循环」的软件 alpha test（见 `NativePlugin/shaders/pathtrace.glsl`），语义正确、不需要额外资产；OMM 是后续可选的性能优化，需要烘焙 `EXT_mesh_opacity_micromap`，但**不改变可见性语义**。SER 则只有在迁到 RT pipeline 且使用 Ada+ GPU 时才可能成为收益项。

### 10.4 本项目的决策：暂不接入 OMM（2026-11-16）

**结论：保留刚实现的软件 alpha cutout，OMM 不作为近期目标。** 原因按优先级：

1. **SDK / loader 门槛**：当前 SDK 1.4.313.2 只有 EXT OMM 头文件；KHR OMM 依赖 `VK_KHR_device_address_commands` 和 `vkCreateAccelerationStructure2KHR`，Unity loader 能否返回这些新命令需要先 probe。
2. **没有烘焙管线**：OMM 必须离线生成微三角形 opaque/transparent/unknown 数据；Unity 资产侧没有 baker，需要处理 alpha 贴图、UV、`_Cutoff`、`_BaseColor.a`、4-state 保守分类。
3. **Ray Query 需要双变体**：OMM 版入口必须声明 `OpacityMicromapIdKHR` execution mode；当前是单份离线 SPIR-V/单 pipeline，需新增变体并切换；当前 glslang 15.2.0 也没有现成支持。
4. **BLAS / 材质绑定冲突**：OMM 挂在 BLAS 上，而当前 BLAS 按 Mesh 共享、材质按实例；同一个 Mesh 被多个 alpha 材质/cutoff 使用时一份 OMM 不能同时正确。需要拆 BLAS、限制单材质 Mesh，或用实例 `DISABLE_OPACITY_MICROMAPS` 回退。
5. **收益未证实**：当前教堂场景没有镂空，OMM 零收益；少量镂空用软件路径更简单，OMM 的构建/内存成本可能反而更高。

触发重新评估的条件：出现大量镂空几何（植被/栅栏/头发片）、profile 证明 alpha test 的 candidate/纹理采样是 PT 热点，并且 KHR OMM + Unity loader probe 通过。详细实现步骤与限制见 [PT 镂空支持说明](PT镂空材质支持说明.md)。

---

## 11. 一句话总结

**可行，且参考工程已经证明「同一份路径追踪可以同时跑两种后端」；但整体切换在当前 URPPT（表面型场景 + RTX 3070 Ti 无 SER）上大概率是十几到几十个百分点的 PT 回退（无 SER 时更差），而不是提升。除非目标是 SER/Slang/OMM 的长期架构，否则不建议整体切换；要做就做双后端，让 Ray Query 继续当默认，RT pipeline 作为按场景/按 GPU 的可选加速路径。**
