# DLSS Ray Reconstruction 集成踩坑记录

## 问题现象

DLSS 阶段 4（Ray Reconstruction）集成后，Play 模式下 RR 模式持续告警 `[PathTrace] DLSS NGX init failed, falling back to Bilinear`，而同一管线 **SR（超分）模式完全正常**。capability 查询显示 RR available=1，dlssd snippet 加载与初始化全部成功，唯独 `CreateFeature` 返回 `0xBAD00005`（FAIL_InvalidParameter）。

## 问题一：CreateFeature 失败 0xBAD00005

### 症状

```
[VPT-DLSS] RR create (flags=0x4a): W=1129 H=635 TW=1920 TH=1080 PQ=1
[VPT-DLSS]   -> 0xbad00005, handle=0000000000000000
```

### 排查弯路（均已排除，勿重走）

| 假设 | 验证实验 | 结论 |
|------|---------|------|
| 渲染尺寸需精确匹配 | 1129x635（1.7x 整数倍率）与 1114x626（OptimalSettings 推荐值） | 均失败。snippet 日志确认 `Custom dimensions overriding Optimal dimensions` 只是 info，尺寸只需在动态范围内 |
| Create 参数组合问题 | 12 组矩阵：flags(0x4a/0x42/0x40/0x8/0x0) × depth(Linear/HW) × roughness(Packed/Unpacked) × PQ 4 档 | 全部失败（因为没有一个组合带 IsHDR） |
| Vulkan 设备扩展缺失 | `NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements` 查询：RR/SR 需求完全相同（NVX_binary_import、NVX_image_view_handle、KHR_buffer_device_address、KHR_push_descriptor），设备已全部启用 | 排除（SR 成功即可佐证） |
| dev DLL 调试限制 | 换 SDK rel 版 nvngx_dlssd.dll 对照 | 同样失败，排除 |
| GIPA/GDPA 函数指针未透传 | Unity 拦截链 vs 真实 vulkan-1.dll ICD 链 A/B 探测 6 个 NVX/KHR 函数（vkCreateCuModuleNVX 等） | 两条链完全相同、全部可得，排除 |

### 突破口：snippet 级日志

NGX 有三层日志，**排查 CreateFeature/Evaluate 失败必须看 snippet 日志**（与插件 DLL 同目录）：

| 日志 | 内容 | 诊断价值 |
|------|------|---------|
| `nvngx.log`（Core） | validate 级信息 | 低。`CreateFeature_Validate: app id is 0` 刷屏是正常噪音，SR 成功时也打印 |
| `nvngx_dlssd_310_7_0.log` | **dlssd snippet 内部日志** | 高。真正的失败原因在这里 |
| `nvngx_dlss_310_7_0.log` | dlss (SR) snippet 内部日志 | 高（SR 侧对照用） |

dlssd snippet 日志中的决定性证据：

```
[NGXDLSSD::CreateDlssInstance:1812] HDR 0
[NgxSwinDenoiser::CreateDldnInstance:1645] Error: HDR Color required
[NgxSwinDenoiser::CreateDldnInstance:1646] NVSDK_NGX_Result_FAIL_InvalidParameter
```

### 根因

**RR 的 Preset_D transformer 模型（SwinDenoiser）强制要求 `NVSDK_NGX_DLSS_Feature_Flags_IsHDR`**（`nvsdk_ngx_defs.h:291`，`1 << 0`）。而 SR 的 CNN 模型（`nvngx_dlss_310_7_0.log` 中无任何 HDR 检查）不要求——这就是「SR 成功 / RR 全参数组合失败」的完整解释。

语义上完全合理：RR 期望输入为 tone-map 前的线性 HDR radiance（路径追踪输出 RGBA16F 线性值天然满足）；SR 超分的是普通颜色，无此约束。

### 修复

**文件**：`NativePlugin/src/vpt_dlss.cpp`

```cpp
// RR 分支 createFlags：IsHDR 是 RR 必需，SR 不需要
const int rrFlagCombos[] = {
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR        // ← 关键新增
  | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
  | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
  | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted,   // = 0x4b
    // 备选：去掉 DepthInverted = 0x43
};
```

其余创建参数（`NVSDK_NGX_DLSSD_Create_Params`）：`InDenoiseMode=DLUnified`、`InRoughnessMode=Packed`、`InUseHWDepth=Linear`，均已验证有效。

## 问题二：EvaluateFeature 失败 0xBAD0000A（黑屏）

### 症状

IsHDR 修复后创建成功、告警消失，但 Play 模式下 RR **黑屏**，每帧打印：

```
[VPT-DLSS] RR EvaluateFeature failed: 0xbad0000a
```

`0xBAD0000A = NVSDK_NGX_Result_FAIL_MissingInput`（`nvsdk_ngx_defs.h:146`）——evaluate 缺少必需输入。dlssd snippet 日志中无 evaluate 记录，说明被 **NGX Core 参数验证层**拒绝（未进 snippet）。

### 根因

RR Integration Guide 3.4 节的输入清单中，**Diffuse Albedo 与 Specular Albedo 是 RR 的必需 G-buffer 输入**（SR 不需要）。当前 evaluate 只绑定了 Color/Output/Depth/MotionVectors/Normals/Roughness，缺 albedo。

### 临时修复（诊断用，已被下方正式实现替换）

将 normalRough 纹理（RGBA16F、输入分辨率，格式合规）临时顶替两个 albedo 传入 `pInDiffuseAlbedo` / `pInSpecularAlbedo`：

```cpp
// vpt_dlss.cpp RR evaluate 分支（临时诊断代码，待替换为真实 albedo）
rrEval.pInDiffuseAlbedo  = &normalRoughRes;
rrEval.pInSpecularAlbedo = &normalRoughRes;
```

evaluate 立即返回 0x1，画面出现。

### 副作用（历史现象）

**模型边缘出现红、绿、蓝自发光**——法线 xyz 被当作 albedo 颜色使用，模型边缘法线突变区域被 transformer 识别为彩色反照率，产生彩色光边。这既确认了 albedo 通路接通，也确认必须提供真正的 albedo。

### 正式实现（已完成）

完整的 albedo G-buffer 通路已落地，替换临时顶替代码。**写入点选在主路径 kernel 首命中**（MotionVector kernel 只有位置/深度数据，无材质信息）：

- **Diffuse Albedo** = `albedo * (1 - metallic)`
- **Specular Albedo** = `EnvBRDFApprox(F0, roughness², NdotV)`（UE4 Karis 移动近似，Ray Tracing Gems Ch.32，a = roughness²）
- **sky 像素填 (0.5, 0.5, 0.5)**（Guide 建议；常见集成错误是 sky 未清理）

实现链路（8 处修改，自底向上）：

| 层 | 文件 | 修改 |
|----|------|------|
| native GLSL | `NativePlugin/shaders/pathtrace.glsl` | binding 14/15（rgba16f writeonly image2D）+ EnvBRDFApprox 函数 + miss/首命中（`depth==0 && s==0`）写入 |
| SPIR-V | `pathtrace.spv` / `pathtrace_spv.h` | 重新编译（`glslangValidator -V -S comp --target-env vulkan1.2`）+ 重新生成 header |
| Unity HLSL | `PathTrace.compute` | 全局 UAV `DiffuseAlbedoOut`/`SpecularAlbedoOut` + `SKY_ALBEDO` 宏 + PathTrace / ReSTIR_PrimaryRay 双 kernel 写入 |
| Vulkan 管线 | `vpt_dispatch.h/.cpp` | 输出 RT 3→5 张：bindings 14→16、`s_OutputViews[5]`、`imgInfos[5]`、`imgBarriers[5]`、cleanup 循环 5 |
| 导出层 | `vpt_api.h/.cpp` | `VPT_DispatchPathTrace` / `VPT_PrepareDispatch` / `VPT_DLSS_PrepareRRDispatch` 三函数签名加 albedo 参数 |
| NGX 层 | `vpt_dlss.h/.cpp` | `s_DiffuseAlbedoView`/`s_SpecularAlbedoView` 视图缓存 + EnsureDLSSImageView + **RenderCallback 绑定真实 albedo（替换顶替代码）** + cleanupView + stub 参数 |
| C# 桥接 | `NativeBridge.cs` | 3 个 P/Invoke + 3 个便捷方法（`DispatchPathTrace`/`PrepareDispatch`/`PrepareDLSSRRDispatch`）签名更新 |
| C# 渲染器 | `PathTraceRendererFeature.cs` | `_diffuseAlbedoRT`/`_specularAlbedoRT` 字段 + EnsureRTs/ReleaseRTs + K1/PathTrace kernel UAV 绑定 + 3 处调用传参 |

关键设计决策：

1. **albedo RT 无条件创建**（ARGBHalf，输入分辨率）：native pipeline binding 14/15 加入 descriptor set 后必须始终有效——若传 null，`PrepareDispatch` 的 null 检查会静默跳过整个 dispatch，路径追踪完全不渲染。即使 DLSS 关闭也创建这两张 RT。
2. **D3D11 pitfall**：`PathTrace.compute` 中全局 `RWTexture2D` 必须绑定到每个引用它的 kernel——只有 PathTrace 和 ReSTIR_PrimaryRay 写 albedo，其余 kernel 不引用不绑定。
3. **HLSL 函数先定义后使用**：`EnvBRDFApprox` 必须放在函数定义区（specularF0 之后），不能插在 kernel 之间。
4. **SPIR-V 重编译后必须重启 Unity 编辑器**（DLL 仅编辑器启动时加载），Play 模式内重载无效。

DLL 已重新编译并自动拷贝至 `Assets/Plugins/x86_64/`（CMake POST_BUILD）。

## 问题三：模型边缘轻微抖动（2026-09-07，待验证）

### 现象

RR 模式功能落地后，静止场景下**模型边缘持续轻微抖动**（参照 SR 模式静止完全不抖的标准）。参照 `doc/DLSS_抖动修复踩坑记录.md` 排查。

### 排查过程

**先排除 SR 旧根因回归**：MotionVector kernel 仍是对称投影实现（curUV/prevUV 都投影 worldPos），且 SR/RR 共享同一 MV、SR 实测不抖 → MV 计算无回归。

**对照 NVIDIA nvpro 官方参考实现**（`MyEngineVulkanV2/vk_gltf_renderer`，DLSS-D 同族）：逐项比对 evaluate/create 参数，发现两处 RR 特有错误：

#### 根因 1：InJitterOffset 符号相反

- 渲染侧 jitter 应用方向两侧**完全一致**：`uv = (pixel + 0.5 + jitter) / res`（官方参考 gltf_pathtrace.slang L786 `subpixelJitter = jitter + float2(0.5, 0.5)`）
- 但官方参考传 NGX 时**取负**（dlss_wrapper.cpp L390）：`InJitterOffsetX = -info.jitter.x`
- URPPT RR 沿用了 SR 的 `+jitter` 传法 → transformer 模型帧间亚像素对齐每帧偏差 `2×jitter`，随 Halton 8 相位逐帧变化 → 边缘（高频细节）来回摆动
- 为什么 SR 传 +jitter 不抖：SR 输入是空域滤波后的干净图像，CNN 模型时域校正弱，亚像素误差不可见；RR 是 1spp 噪声 + transformer 精确时域累积，误差被直接放大

#### 根因 2：DepthInverted flag 与线性深度语义矛盾

- `DepthInverted` 语义：深度缓冲为**反向 z 排序（near 大 far 小）**，仅适用 SR 的 reversed-Z NDC 深度
- RR 用 Linear 深度：`LinearDepthOut = curClip.w`（near≈0.3 小，far≈1000 大，**非反向**）
- NVIDIA Streamline 集成清单原话：「如果所提供的深度缓冲区采用反向 z 排序，则设置 Inverted Depth 位」——线性视距离不满足
- 官方参考 Linear 模式下 `InFeatureCreateFlags = IsHDR | MVLowRes`（无 DepthInverted）
- 后果：RR 深度一致性/disocclusion 检测方向颠倒，边缘深度突变处时域历史被错误取舍

### 已排除项（勿重查）

| 疑点 | 验证结论 |
|------|---------|
| MV kernel 对称性 | 与 SR 共享同一 kernel，对称投影实现未回归（SR 不抖佐证） |
| 矩阵主序 | URPPT `MatrixToFloat16` 行主序直拷 ≡ 官方 `glm::transpose(M)` 后 `value_ptr`（数学矩阵行主序存储），一致。原「待确认项」关闭 |
| MVScale | URPPT 传 UV 空间 MV + `InMVScale=renderW/H` ≡ 官方像素空间 MV + `InMVScale=1`，数学等价 |
| Normals 空间 | 官方参考同样传世界空间法线（`pbrMat.N`），URPPT GBuffer1 世界空间法线一致 |
| EnvBRDFApprox | 同源实现（Ray Tracing Gems Ch.32） |

### 修复（`NativePlugin/src/vpt_dlss.cpp`）

1. RR create flags 首选组合改为 `IsHDR|MVLowRes|AutoExposure`（0x43，无 DepthInverted），0x4b 降为备选
2. RR evaluate jitter 取负：`InJitterOffsetX/Y = -s_PendingJitterX/Y`；**SR 分支保持 +jitter 不动**（实测不抖，避免回归）

### 残余风险（若修复后仍有轻微抖动，按序排查）

1. **深度量纲**：URPPT 传米制正视距离 `curClip.w`；官方参考传 `abs(NDC.z)`∈[0,1]。若 RR 期望归一化或带符号 viewZ，需调整 LinearDepthOut 写法
2. **矩阵 clip 约定**：Unity `Camera.projectionMatrix` 是 GL 风格（z∈[-1,1]，Y-up）；若 NGX 按 Vulkan clip 约定（z∈[0,1]，Y-down）解释，reprojection 有偏差。可尝试用 `GL.GetGPUProjectionMatrix` 转换后传入

## 下一步计划

1. ~~**正式实现 albedo 通路**（消除边缘彩光）~~ ✅ 已完成（见上方「正式实现」小节）
2. ~~**边缘轻微抖动排查**~~ ✅ 已定位并修复（见上方「问题三」，待重启验证）
3. **验证**（需完全重启 Unity 编辑器后 Play）：
   - 模型边缘红/绿/蓝彩光消失（Diffuse/Specular albedo 正确分解）
   - 静止场景模型边缘无抖动（问题三修复验证）
   - RR 降噪质量正常（对比 SR：金属高光稳定、非金属表面无彩边）
   - `nvngx_dlssd_310_7_0.log` 无新的 validate/evaluate 错误
4. **可选**：dev 版 nvngx_dlssd.dll 的 Debug Overlay（CTRL+ALT+F11 切换全屏）逐通道检查 albedo 输入正确性（Diffuse 应为无高光的纯色、Specular 应只有镜面反射率）
5. ~~**待确认项**：矩阵主序~~ ✅ 已确认（见「问题三·已排除项」：行主序直拷与官方参考等价，无需转置）；矩阵的 clip 约定（GL vs Vulkan）仍属残余风险，见「问题三·残余风险」
