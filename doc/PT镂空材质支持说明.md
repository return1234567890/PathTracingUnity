# PT 镂空（Alpha Cutout）支持说明

> ⚠ **后端已切换（2026-11-21）**：PT 现在跑在 `VK_KHR_ray_tracing_pipeline` 上，本文描述的
> "主光线/阴影以 ray query candidate 循环采样 alpha 并丢弃微面"已改为 **any-hit + `IgnoreHit()`**
> （`pt.slang` 的 `rahitMain` / `rahitShadow`）。语义等价，映射见
> [RT_pipeline整体切换实现方案](RT_pipeline整体切换实现方案.md) §2.2 / §10.2。
> 材质 flags、`alphaCutoff` 语义、URP 侧配置步骤**全部不变**。

> 实现日期：2026-11-16
> 范围：URP Lit 的 **Alpha Clipping**（`_ALPHATEST_ON` + `_Cutoff`）在 Vulkan Ray Query 路径追踪里的支持。  
> 不涉及 alpha blend；OMM 经调研后暂不接入（原因见 §7）。玻璃透射仍走原来的 `MAT_IS_TRANSPARENT` 分支。

---

## 1. 怎么用

1. 材质使用 **URP Lit**（或同样声明 `_ALPHATEST_ON` / `_Cutoff` 的 Shader）。
2. **Surface Type 保持 Opaque**，勾选 **Alpha Clipping**，设置 **Alpha Cutoff**（默认 0.5）。
3. Base Map 的 alpha 通道提供镂空形状；建议 Texture Import 勾选 Alpha Is Transparency（只影响 Unity 侧，PT 直接读 alpha）。
4. 进入 Play 模式重新构建场景（`ModelManager` 只在构建时收集材质）。

> 不需要把 Surface Type 改成 Transparent。Transparent 是玻璃透射（带 IOR/Beer），不是镂空；两者可以同时勾选，但通常只对 Opaque + Alpha Clipping 使用。

---

## 2. 改动点

| 层 | 文件 | 改动 |
|---|---|---|
| C# 材质 | `Assets/Scripts/ModelManager.cs` | `PathMaterial` 末尾的 `padVolume` 复用为 `alphaCutoff`（[ModelManager.cs:80-86](../Assets/Scripts/ModelManager.cs)）；新增 `MAT_ALPHA_TEST = 128u`；`GetMaterial()` 用 `_ALPHATEST_ON` / `_AlphaClip` 识别并读 `_Cutoff`（[ModelManager.cs:827-839](../Assets/Scripts/ModelManager.cs)） |
| Native 数据 | `NativePlugin/src/vpt_types.h` | `VPT_Material.alphaCutoff`（仍 128 B）；`VPT_MAT_FLAG_ALPHA_TEST = 128u`（[vpt_types.h:94-97](../NativePlugin/src/vpt_types.h)） |
| TLAS | `NativePlugin/src/vpt_scene.cpp` | `WriteInstanceData()` 的 `nonOpaqueMat` 同时看 `TRANSPARENT` 与 `ALPHA_TEST`；镂空实例不标 `FORCE_OPAQUE`，硬件才会产生 candidate（[vpt_scene.cpp:722-740](../NativePlugin/src/vpt_scene.cpp)） |
| 主光线 | `NativePlugin/shaders/pathtrace.glsl` | 不再使用 `gl_RayFlagsOpaqueEXT`；candidate 循环里对 `MAT_ALPHA_TEST` 采样 alpha，`alpha < cutoff` 不 confirm、继续遍历（[pathtrace.glsl:951-986](../NativePlugin/shaders/pathtrace.glsl)） |
| 阴影 | 同上 | `traceShadowTransmission()` 先做 alpha test：洞 `continue`（不遮挡、不算介质边界），实心 `return 0`（[pathtrace.glsl:630-635](../NativePlugin/shaders/pathtrace.glsl)） |
| SPIR-V | `NativePlugin/shaders/pathtrace.spv` / `src/pathtrace_spv.h` | 已重新编译并生成嵌入头（90,460 B） |

**布局**：`alphaCutoff` 复用原来的 4 B 对齐填充，`sizeof(VPT_Material) == 128`、`sizeof(PathMaterial) == 128` 不变；C# / C++ / GLSL 三处字段顺序一致。

---

## 3. 语义

- **alpha 来源**：`_BaseColor.rgba * _BaseMap.rgba`（与 URP Lit 一致），再乘材质 `uvScaleOffset`；`sampleBaseColor()` 已包含这些。
- **主光线**：命中 candidate 后按重心坐标插值 UV，采样 alpha；`alpha < cutoff` 视为洞，射线继续；否则 confirm 为表面。
- **阴影**：洞不遮挡、不改变介质状态；实心部分完全遮挡（返回 0）。如果材质同时是 Transparent，实心部分继续按透射率累积。
- **反射/二次弹射**：与主光线共用同一段 candidate 循环，洞在反射里同样可见。
- **G-buffer / DLSS RR**：只写最终 committed 命中；洞不会写入 G-buffer，因此 RR 看到的是穿透后的表面。

---

## 4. 已知限制

| 限制 | 说明 |
|---|---|
| 仅三角形 | 当前 TLAS 没有 AABB；procedural 几何的镂空不在本次范围 |
| 二值 cutout | 没有 alpha-to-coverage / 半透明边缘；`alpha < cutoff` 全丢，`>= cutoff` 全留 |
| `MAX_SHADOW_LAYERS = 16` | 阴影 candidate 上限与玻璃共用；非常密的植被可能超过，超过后按已累积透射率返回（偏亮）。需要时调大常量 |
| 无 OMM | 当前是软件 alpha test（每个 candidate 采样一次纹理）。经调研**暂不接入 KHR OMM**：SDK/loader、离线烘焙、Ray Query 变体、BLAS/材质绑定四道门槛，详见 §7 与 [RT pipeline 调研 §10](RT_pipeline整体切换可行性调研.md) |
| 主光线成本 | 不再使用 `gl_RayFlagsOpaqueEXT`；透明/镂空实例的 primary 命中会走 candidate + confirm。不透明实例仍 `FORCE_OPAQUE` 直接 commit，成本不变 |
| 材质构建时机 | 与现有材质参数一样，只在 `ModelManager` 构建场景时读取；Play 中改材质不会自动重建 |
| DLSS RR 边缘 | cutout 边缘是高频率二值边缘，1spp + RR 下可能有轻微闪烁/拖影；属于 RR 的常规边缘行为，不是 cutout 特有 |

---

## 5. 构建 / 部署

改了 `pathtrace.glsl` 后必须重新生成 SPIR-V 和嵌入头，再重编 DLL：

```powershell
# 1) 编译 shader（与现有构建命令一致）
& "C:\VulkanSDK\1.4.313.2\Bin\glslangValidator.exe" -V --target-env vulkan1.2 -S comp `
  "NativePlugin\shaders\pathtrace.glsl" -o "NativePlugin\shaders\pathtrace.spv"

# 2) 生成 src/pathtrace_spv.h
powershell -NoProfile -ExecutionPolicy Bypass -File "NativePlugin\shaders\spv_to_header.ps1" `
  -spvPath "NativePlugin\shaders\pathtrace.spv" `
  -headerPath "NativePlugin\src\pathtrace_spv.h" -varName "g_PathTraceSpv"

# 3) 重编 DLL
cmake --build "NativePlugin\build" --config Release --target VulkanPathTracePlugin
```

> **若构建时 Unity 正在运行**，`Assets/Plugins/x86_64/VulkanPathTracePlugin.dll` 会被占用，CMake 的 POST_BUILD 复制会失败；DLL 仍会生成在 `NativePlugin/build/bin/Release/VulkanPathTracePlugin.dll`。关闭 Unity 后复制，或直接重新构建：
>
> ```powershell
> Copy-Item -Force "NativePlugin\build\bin\Release\VulkanPathTracePlugin.dll" `
>   "Assets\Plugins\x86_64\VulkanPathTracePlugin.dll"
> ```
>
> 重新打开 Unity 后才会加载新 DLL。

**本次已执行（2026-11-16）**：

- `pathtrace.spv` 编译成功，90,460 B；`spirv-val --target-env vulkan1.2` 通过。
- C# 已由 Unity 编译（`Assembly-CSharp.dll` 生成成功，无 `error CS`）。
- Native DLL 构建成功，270,848 B；Unity 关闭后已复制到 `Assets/Plugins/x86_64/VulkanPathTracePlugin.dll`。
- 源/目标 DLL SHA256 一致：`ECBDEA4FEFE1DF24462826CDD58E7175A06B1485CCF82E6BF63CF6A09809C7DF`。
- 重新打开 Unity 后生效；运行时视觉验证尚未进行（见 §6）。

---

## 6. 验证清单

- [x] `pathtrace.spv` 编译通过，`spirv-val --target-env vulkan1.2` 通过。
- [x] C# 编译通过（Unity 关闭前 `Assembly-CSharp.dll` 已生成，无 `error CS`）。
- [x] Native DLL 构建通过，并已复制到 `Assets/Plugins/x86_64/`。
- [ ] 栅栏 / 树叶卡片：主视线能看到洞，洞后能看到背景/其他物体。
- [ ] 同一网格的阴影：洞透光，实心部分有影子；没有整片黑或整片亮。
- [ ] 镜面/金属反射里能看到洞。
- [ ] 镂空边缘没有自交黑边（偏移沿用现有 `surfaceOffset`）。
- [ ] 玻璃 + 镂空同时存在时，玻璃的透射/Beer 行为不回归。
- [ ] `VPT-trace-native.log` 无设备丢失/验证错误。

---

## 7. 为什么暂不接入 OMM

结论：**软件 alpha test 已满足当前需求，KHR OMM 不作为近期目标。** 主要门槛：

1. **SDK / loader 太旧**：当前 Vulkan SDK 1.4.313.2 的头文件里只有 `VK_EXT_opacity_micromap`，没有 `VK_KHR_opacity_micromap` / `VK_KHR_device_address_commands` / `vkCreateAccelerationStructure2KHR`；KHR OMM 还依赖新的 device-address-commands 创建路径。Unity 的 loader 能否返回新命令也需要 probe。
2. **必须离线烘焙**：OMM 数据不是驱动从 alpha 贴图自动生成的。需要按材质处理 alpha 贴图、UV、`_Cutoff`、`_BaseColor.a`，生成 2-state/4-state 微三角形数据、triangle records、per-primitive index buffer；当前 Unity 资产管线没有 baker。
3. **Ray Query 需要专门变体**：使用 OMM 的 Ray Query 入口必须在 SPIR-V 里声明 `OpacityMicromapIdKHR` execution mode；当前是单份离线 SPIR-V/单 pipeline，需要双变体；当前 `glslangValidator` 15.2.0 也没有现成 OMM 支持。
4. **BLAS / 材质绑定冲突**：OMM 挂在 BLAS 上，而当前 BLAS 按 `Mesh` 共享、材质按实例；同一个 Mesh 被多个 alpha 材质/cutoff 使用时，一份 OMM 不能同时正确。需要拆 BLAS、限制单材质 Mesh，或用 `DISABLE_OPACITY_MICROMAPS` 实例 flag 回退。
5. **收益未证实**：教堂等无镂空场景零收益；少量镂空用软件路径更简单。只有在植被/栅栏/头发片很多、profile 证明 alpha test 是热点时才值得。

详细分析见 [RT pipeline 调研 §10](RT_pipeline整体切换可行性调研.md)。如果未来要接，建议先做 probe（`apiVersion` / `deviceAddressCommands` / `micromap` / `vkCreateAccelerationStructure2KHR` 指针 + 单 Mesh 原型），不要直接上资产管线。
