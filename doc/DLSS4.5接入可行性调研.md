# DLSS 4.5 接入可行性调研

> 调研对象：URPPT（Unity 2022.3 + URP 14.0.12 + Vulkan Ray Query 路径追踪 + 原生 NGX DLSS RR）
> 调研时间：2026-11-14
> 调研范围：只评估「DLSS 4.5 对本项目当前技术路线（1spp 路径追踪 + RR 降噪超分）能带来什么、要改什么、有什么坑」。不评估 DLSS SR 回归、不评估帧生成。

---

## 1. 结论速览

**结论：可行，推荐做。** 属于「NGX SDK 依赖升级 + Preset 引脚 + 质量回归」，不涉及渲染架构改动，预估 **1.5 ~ 2.5 人天**。

| 维度 | 判定 | 依据 |
|---|---|---|
| 技术路线匹配 | ✅ 高 | DLSS 4.5 对本项目唯一有意义的增量是 **RR 的第二代 Transformer 模型（Preset F）**，正好落在现用的 `NVSDK_NGX_Feature_RayReconstruction` 路径上 |
| 代码改动量 | ✅ 小 | 新 SDK 的 Vulkan RR 接口与现用 310.7.0 **接口零变更**（源码级逐文件比对）：`nvsdk_ngx_defs.h` 零差异、`nvsdk_ngx_defs_dlssd.h` 仅注释差异、`nvsdk_ngx_helpers_dlssd_vk.h` 的 70 个 eval 参数字段逐一比对无增删 |
| 构建链路 | ✅ 不变 | CMake 的 `find_library(nvsdk_ngx_d)` + `VPT_DLSS_ENABLED` 机制不变，只换 `third_party/ngx/{include,lib}` 与运行时 DLL |
| Unity / URP 版本 | ✅ 无关 | DLSS 完全在原生 Vulkan 层（`IUnityGraphicsVulkan` 取 `VkDevice`/`VkCommandBuffer`），不受 Unity 2022.3 / URP 14 限制；也不依赖 NVIDIA 的 Unity 插件（那个只做 SR，不做 RR） |
| 硬件 / 驱动 | ✅ 已满足 | Preset F 要求 GRD **580.88+**，全 RTX 20/30/40/50 支持；本机 RTX 3070 Ti Laptop + **616.92** |
| 显存 | ⚠️ 需实测 | RR 模型 4K 下显存分配 +110.87 MB（467.97 → 578.84 MB）；本机 8 GB，路径追踪加速结构 + 7 张输入分辨率 RT 同时占用 |
| 性能 | ⚠️ 需实测 | 新模型 +20% 参数 / +35% 算力；RTX 5090 4K 处理时间 1.83 → 2.12 ms；RTX 20/30 公开测试「有好有坏」 |
| 分发授权 | ⚠️ 仅影响发布 | 现在用的是 SDK dev 版 DLL（含 debug overlay），只能自用；对外发布需换 rel 版并绑定自己的 NVIDIA app id |

**建议的落地顺序（按风险递增，每步都可独立回滚）**

1. **零构建 A/B（约 30 分钟）**：只把 `Assets/Plugins/x86_64/nvngx_dlssd.dll` 换成 310.9.16，重启 Unity 观察。因为 DLL 是运行时加载、接口 ABI 未变，头文件不用动就能先验证「能不能加载、默认是否已经变成 RR2」。
2. **正式升级 + Preset 引脚（约半天）**：换 `third_party/ngx` 的 include/lib，在 `VPT_DLSS_Init_Internal` 里按画质档写死 Preset F，重新编译。
3. **质量/性能回归（0.5 ~ 1 天）**：重点回到之前踩过的坑（HDR flag / jitter 符号 / 线性深度 / 固定曝光 / 边缘抖动），确认新模型不会让老问题复发。

---

## 2. DLSS 4.5 里与本项目相关的部分

DLSS 4.5 于 CES 2026 发布，由三块组成，**只有第二块和本项目有关**：

| 组成 | 内容 | 与本项目的关系 |
|---|---|---|
| DLSS 4.5 超分辨率 | 第二代 Transformer 模型，全部 RTX 可用 | ❌ 无关。项目已删除 SR 链路（README「DLSS 侧只保留 RR」） |
| **DLSS 4.5 光线重建（RR）** | 第二代 Transformer 降噪/重建模型，即 **Preset F**，全部 RTX 20 系列起可用 | ✅ **就是它**。替换现用的 Preset D |
| 动态多帧生成 / 6X MFG | 自动调整帧倍率，RTX 50 独占 | ❌ 无关。需要帧生成链路（`nvngx_dlssg`），本项目没有 |

关键时间线（决定「现在能不能接」）：

- 2026-01 CES 2026 发布 DLSS 4.5（SR + 动态 MFG）
- 2026-08-25 Gamescom：RR 新模型以 **Preset F** 形式早期访问，要求 GRD 580.88+，NVIDIA App 里手动选
- 2026-09-08：**DLSS SDK 310.9.1 正式把「Ray Reconstruction Transformer Mode (Preset F)」放进公开 SDK**，开发者可以正式集成，不必再靠 NVIDIA App 覆盖或从游戏里扒文件
- 2026-10-16：SDK 310.9.16（仅 bug 修复），为当前最新版

公开数据（NVIDIA 文档口径，供预算参考）：

- 参数量 +20%，计算量 +35%，目标是性能开销与上一代接近
- 4K 显存分配：467.97 MB → 578.84 MB（+110.87 MB / +23.7%）
- RTX 5090 4K 处理时间：1.83 ms → 2.12 ms（+0.29 ms / +15.8%）
- RTX 20/30 系列：部分场景性能表现参差，存在抖动可能

> 参考：[NVIDIA 官方 DLSS 4.5 发布文](https://www.nvidia.cn/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/)、[GitHub DLSS v310.9.1 Release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1)、[heise：DLSS 4.5 RR 上线（Preset F + 580.88 驱动）](https://www.heise.de/en/news/DLSS-4-5-Nvidia-releases-new-Ray-Reconstruction-model-11425666.html)、[显存与耗时数据转述](https://www.eet-china.com/mp/a523969.html)

---

## 3. 项目现状（判定基线）

### 3.1 依赖与调用链

| 项 | 现状 | 位置 |
|---|---|---|
| NGX SDK | **310.7.0**（dev 版，`nvngx_dlssd.dll` FileVersion `310,7,0,0`，39.0 MB） | `NativePlugin/third_party/ngx/`、`Assets/Plugins/x86_64/` |
| App ID | `241534723`（= `0x0E658703`），必须与 snippet 内嵌元数据一致 | `NativePlugin/src/vpt_dlss.cpp:39` |
| 初始化 | `NVSDK_NGX_VULKAN_Init` → `AllocateParameters` → `NGX_VULKAN_CREATE_DLSSD_EXT1` | `vpt_dlss.cpp:235 / 387 / 501` |
| 求值 | `NGX_VULKAN_EVALUATE_DLSSD_EXT`（Phase 2，复用 Unity 命令缓冲） | `vpt_dlss.cpp:772` |
| 功能集 | 只有 RR；`nvngx_dlss.dll`（66.5 MB）已无引用，属历史遗留 | `Assets/Plugins/x86_64/` |
| Preset | **未显式设置**，走默认 → 日志显示 `Using Default Overriden Preset Preset_D` | `nvngx_dlssd_310_7_0_01.log` |

### 3.2 运行时实测（本机，2026-11-14）

来自 `Assets/Plugins/x86_64/nvngx.log` 的 telemetry：

```
ngxSDKVersion     = 1.5          ngxCoreVersion   = 1.4.0.0
ngxFeatureVersion = 310.7.0      displayDriverVersion = 616.92
cmsId             = 241534723    GpuAPI = 3 (Vulkan)
VRAMAllocated     = 137 (MB)     MVExtent = 960x540
```

说明：RR 创建/求值本来是通的，基线干净，升级后有明确的对比对象。

### 3.3 输入齐备度（RR 必需项）

RR 要求 HDR 颜色 + 线性深度 + 运动矢量 + 法线/粗糙度 + Diffuse/Specular Albedo，项目已全部提供（albedo 通路见 `doc/DLSS_RR集成踩坑记录.md`「正式实现」一节），矩阵与 jitter 也已按官方参考对齐（jitter 取负、`IsHDR|MVLowRes`、无 `DepthInverted`、`PreExposure=1`）。

**结论：不需要为 4.5 新增任何必需输入。** 4.5 只是换模型，不改输入契约。

### 3.4 本机驱动侧已就绪

`C:\ProgramData\NVIDIA\NGX\models\nvngx_config.txt`：

```
[dlss_override]
app_E658700 = 310.9.16        ← 驱动 NGX 的 OTA 模型分发已到 310.9.16
[sl_dlss_d_0]
app_E658703 = 2.14.16         ← 与项目 app id(0x0E658703) 同族
```

即：驱动侧（616.92）已经认识 310.9.16 与 RR2 模型，**不需要为了 Preset F 升级驱动**，本地就能验证。

---

## 4. 可行性判定：逐项证据

### 4.1 接口兼容性 ✅（已通过源码级比对验证）

把 `third_party/ngx/include/*` 与上游 v310.9.16 / main 逐文件对比：

| 头文件 | 差异 | 影响 |
|---|---|---|
| `nvsdk_ngx_defs.h` | 0 行差异 | 无 |
| `nvsdk_ngx_defs_dlssd.h` | 仅 2 行注释：`Preset_D` 改称「Transformer model」、**`Preset_F` 由「Do not use」改为「Default model RR2」** | 枚举值不变（F = 6），但语义变了 → 正是本次要用的东西 |
| `nvsdk_ngx_helpers_dlssd_vk.h` | 70 个 eval 参数字段**完全一致**（无增删） | 现有 create/eval 代码不用改 |
| `nvsdk_ngx_helpers_dlssd.h` | 新版退化为 deprecated shim，只 `#include "nvsdk_ngx_helpers_dlssd_d3d.h"` | 项目已直接 include `_vk.h`，**不受影响** |
| `nvsdk_ngx_vk.h` | 0 行差异 | 无 |

另外 `NVSDK_NGX_VERSION_API_MACRO` 仍是 `0x0000015`（1.5.0），即 NGX API 版本未跳变，`NVSDK_NGX_Version_API` 传参不用改。

> 结论：**这不是一次 API 迁移，只是换 snippet + 换模型。** 编译期最多遇到「新 SDK 去掉了 `nvsdk_ngx_helpers_dlssd.h` 里的 D3D 无关内容」这类问题，本项目不涉及。

### 4.2 Preset 控制机制 ✅（需一条日志验证）

310.7.0 的头文件里**已经有**按画质档覆盖 preset 的参数名（所以可以先写代码、后换 DLL）：

```c
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality  "RayReconstruction.Hint.Render.Preset.Quality"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced "RayReconstruction.Hint.Render.Preset.Balanced"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance "RayReconstruction.Hint.Render.Preset.Performance"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance "RayReconstruction.Hint.Render.Preset.UltraPerformance"
```

且 snippet 日志已证明它真的在读这套参数（每档都打印了一次 preset 决策）：

```
[NgxRayReconstruction::FillCreationParams] Info: (Balanced) Using Default Overriden Preset Preset_D
```

**验证断言**：换 310.9.16 并写入 `Preset_F` 后，同一行应变成 `Preset_F`；若仍显示 `Preset_D`，说明该版本这条参数只接受 NVIDIA App 覆盖，需要退回「用 NVIDIA App 强制 Preset F」做对照（此时也说明 SDK 路径不可控，属于要重新评估的信号）。

### 4.3 硬件与驱动 ✅

- Preset F 要求 GRD 580.88+，本机 616.92 ✅
- 支持范围是 RTX 20 系列起，项目 README 本来就限定「RTX 系列 + 最新驱动」✅
- 本机是 **RTX 3070 Ti Laptop / 8 GB**，属于公开评测里「性能表现参差」的 30 系区间 → 收益要实测，不能只看评测 ⚠️

### 4.4 显存 ⚠️（本机最需要关注的一项）

- 模型本身：4K 量级 +110.87 MB；1080p 按像素数折算约 +25~30 MB（估算值，以 telemetry 实测为准）
- 项目侧固定占用：7 张输入分辨率 RT（radiance、worldPos+depth、normal+rough、motionVector、linearDepth、diffuse/specular albedo）+ 1 张全分辨率输出 + BLAS/TLAS
- 8 GB 上叠加 1spp 路径追踪的加速结构，**高分辨率下可能顶到上限**；现有 telemetry 已能直接读到 `VRAMAllocated`，升级前后对比即可，不需要额外工具

### 4.5 分发授权 ⚠️（不影响自用，影响发布）

- 当前 DLL 是 SDK 的 **dev 版**（带 debug overlay、体积 39 MB），只适合开发自用
- 对外发布要换 `rel` 版 DLL，并且 app id 必须换成 NVIDIA 分配的真实 id（现在借用的是 snippet 内嵌的 `0x0E658703`）
- 结论：**学习/实验用途不受影响**，与项目「个人学习与实验性质」的定位一致

---

## 5. 最小接入方案

### 5.1 步骤

| # | 动作 | 文件 | 说明 |
|---|---|---|---|
| 1 | 下载 DLSS SDK 310.9.16（或 310.9.1） | — | `github.com/NVIDIA/DLSS/releases` |
| 2 | 替换头文件 | `NativePlugin/third_party/ngx/include/*` | 16 → 23 个头文件（新增 `_d3d` 拆分版、`nvsdk_ngx_loader.h`、`nvsdk_ngx_standalone_*.h` 等）；本项目只需保证 `nvsdk_ngx_vk.h` / `_helpers_vk.h` / `_helpers_dlssd_vk.h` / `_defs*.h` 就位 |
| 3 | 替换导入库 | `NativePlugin/third_party/ngx/lib/nvsdk_ngx_d*.lib` | CMake 的 `find_library` 不用改 |
| 4 | 替换运行时 DLL | `Assets/Plugins/x86_64/nvngx_dlssd.dll` | 建议同时删掉无用的 `nvngx_dlss.dll`（-66.5 MB） |
| 5 | 引脚 Preset F | `NativePlugin/src/vpt_dlss.cpp`（`AllocateParameters` 之后、create 循环之前） | 见 5.2 |
| 6 | 复核 App ID | `vpt_dlss.cpp:39` | 跑一次看 `nvngx.log` 的 `cmsId` 是否仍为 241534723；变了就改常量 |
| 7 | 重新编译 + 重启 Unity | — | DLL 只在编辑器启动时加载，Play 模式内热替换无效（老坑，见踩坑记录） |

### 5.2 Preset 引脚代码（建议按档位区分）

```cpp
// 4.5 RR2 模型 = Preset F。旧版（310.7）该枚举语义是「Do not use」，
// 所以这段代码必须在换 DLL 之后才真正生效。
// 与 C# 侧 PathTraceRendererFeature.dlssQualityMode 约定一致：0=Quality 1=Balanced 2=Performance 3=UltraPerf
static void ApplyRrPresetHints(NVSDK_NGX_Parameter* p, int qualityMode)
{
    const int preset = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F; // = 6
    const char* key = nullptr;
    switch (qualityMode)
    {
        case 0:  key = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;          break;
        case 1:  key = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;         break;
        case 2:  key = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;      break;
        default: key = NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance; break;
    }
    NVSDK_NGX_Parameter_SetI(p, key, preset);
}
```

- 想保守一点，可以只对 0/1/2 档用 F，UltraPerf 档保留 E（4.5 在低分辨率输入下的性价比尚未验证）
- 引脚的好处：NVIDIA 后续 OTA 改默认模型时，画面不会「悄悄变了」

### 5.3 建议保留的 A/B 开关

在 `PathTraceRendererFeature` 上加一个 `rrPreset` 枚举（Default / D / E / F），方便同场景逐档对比截图。成本很低，回归阶段非常有用。

---

## 6. 验证清单

**日志断言**（`Assets/Plugins/x86_64/nvngx_dlssd_310_9_16.log`）

- [ ] `CreateDlssInstance` 成功，`CreateFeature` 返回 `0x1`
- [ ] `Using ... Preset_F`（不再是 `Preset_D`）
- [ ] 每帧 `EvaluateFeature` 返回 `0x1`，无 `0xbad0000a`（MissingInput）
- [ ] 无 `Error: HDR Color required`（`IsHDR` flag 不能丢）

**画面回归**（重点复检历史坑）

- [ ] 模型边缘无红/绿/蓝彩边（albedo 通路仍在）
- [ ] 静止镜头边缘无抖动（jitter 取负 + 线性深度语义仍成立）
- [ ] 亮区无逐帧闪烁（固定曝光仍正确；新模型对 `PreExposure` 的敏感度可能不同）
- [ ] 移动镜头无拖影/鬼影

**性能与显存**

- [ ] 现有 GPU timer 的 T5→T6（DLSS 段）耗时变化
- [ ] `VRAMAllocated` 变化（1080p 预计 +25~30 MB，4K 预计 +110 MB）
- [ ] 整体帧时间不劣化（RR 段 +0.3 ms 量级，相对 1spp 路径追踪通常可忽略）

---

## 7. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|---|---|---|---|
| App ID 与新版 dev DLL 内嵌元数据不匹配 | 中 | `CreateFeature` 直接失败（`0xbad00005` / `Denied`） | 换 DLL 后第一件事就是看 `nvngx.log` 的 `cmsId`；不匹配则改常量重编。**这是零构建 A/B 的唯一失败点，30 分钟即可探明** |
| 用户驱动 < 580.88 | 中 | `Fail_OutOfDate`，回退 Bilinear | 现有代码已查询 `MinDriverVersion` 并打印；建议在 UI/日志里显式提示 |
| Preset 参数不生效（只认 NVIDIA App 覆盖） | 低 | 拿不到 4.5 质量 | 用日志行验证；失败则用 NVIDIA App 强制 Preset F 做对照，确认是「SDK 路径问题」还是「模型本身问题」 |
| RTX 20/30 性能抖动 / 收益不明显 | 中 | 低端卡体验反而变差 | 保留 D/E/F 三档开关，按档位或按 GPU 架构回退 |
| 8 GB 显存吃紧 | 中 | 高分辨率下掉驱动/降级 | 升级前后对比 `VRAMAllocated`；必要时对 UltraPerf 档保留 E 模型 |
| OTA 默认模型漂移 | 中 | 画面/性能无预警变化 | 显式引脚 Preset F，不依赖 `Preset_Default` |
| dev DLL 不能随包分发 | 高（仅发布场景） | 合规问题 | 发布前换 rel 版 + 申请自有 app id；当前学习用途不受影响 |

---

## 8. 可选增强（不在本次范围）

如果 4.5 RR 的收益值得继续挖，新模型对以下**可选输入**更敏感（当前项目一个都没传）：

- `pInDiffuseHitDistance` / `pInSpecularHitDistance`（路径追踪里 `tMin` 附近已有命中距离，导出成本低）
- `pInDiffuseRayDirection` / `pInSpecularRayDirection`
- `pInReflectedAlbedo`、`pInResponsivityMask`

这些属于「锦上添花」，建议先完成 4.5 基线接入、拿到画质/性能数据后再决定是否投入。

---

## 9. 最终结论

1. **技术上没有拦路虎**：DLSS 4.5 对本项目 = 换一个 RR snippet + 一个 preset 引脚，Vulkan 接口零变更，Unity/URP 完全不参与。
2. **收益预期**：降噪质量（时域稳定性、阴影/光照准确性）有可见提升，代价是 RR 段约 +15% 耗时、显存 +20% 量级——对 1spp 路径追踪这种「RR 只占小头」的管线，代价可接受。
3. **最大不确定性不是技术，而是硬件档位**：本机 8 GB / 30 系正好落在「收益与开销都不确定」的区间，必须实测，不能照搬 5090 的评测结论。
4. **建议动作**：先做 30 分钟的「只换 DLL」A/B，确认 app id 与默认模型是否已经变成 RR2；再决定是否投入正式升级。

---

## 附：参考链接

- [NVIDIA DLSS 4.5 官方发布文（SR + 动态 MFG）](https://www.nvidia.cn/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/)
- [GitHub：DLSS SDK v310.9.1（新增 RR Transformer Mode / Preset F）](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1)
- [GitHub：DLSS SDK v310.9.16（当前最新）](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.16)
- [heise：DLSS 4.5 Ray Reconstruction 上线，Preset F，驱动 580.88+](https://www.heise.de/en/news/DLSS-4-5-Nvidia-releases-new-Ray-Reconstruction-model-11425666.html)
- [显存/耗时数据（NVIDIA 文档转述）](https://www.eet-china.com/mp/a523969.html)
- [Unity Asset Store：DLSS 4.5 - Upscaling for Unity（仅 SR，不含 RR，本项目不适用）](https://assetstore.unity.com/packages/tools/utilities/dlss-4-5-upscaling-for-unity-332398)
