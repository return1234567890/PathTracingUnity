# DLSS SR/RR 抖动修复踩坑记录

## 问题现象

DLSS 阶段 0-3 实现完成后，静止场景画面持续轻微抖动，且 **DLSS Quality Mode 拉得越高抖动越厉害**。

## 根因定位过程

### 阶段一：MVJittered flag + InMVScale（已修复，历史踩坑）

**症状**：静止时严重抖动。

**根因**：
1. `MVJittered` flag 被 SET，但运动矢量用未 jitter 的投影矩阵计算（不含 jitter 偏移）。DLSS 认为 MV 已含 jitter 且不做内部校正 → 时域对齐失败。
2. `InMVScaleX/Y = 0.0f`，将 MV 缩放为 0，DLSS 认为所有像素静止。
3. MV kernel 输出 UV 空间 MV（`prevUV - curUV`），DLSS 期望像素空间。

**修复**：
- 移除 `MVJittered` flag
- 设置 `InMVScaleX = renderWidth`、`InMVScaleY = renderHeight`（UV → 像素空间转换）

**文件**：`NativePlugin/src/vpt_dlss.cpp`

### 阶段二：curUV 含 jitter 导致双重校正（首次尝试，方向对但不完全）

**症状**：阶段一修复后仍有轻微抖动。

**分析**：MV kernel 的 `curUV` 包含了 jitter 偏移：

```hlsl
// 原代码（有问题）
float2 curUV = (id.xy + 0.5 + JitterOffset.xy) / float2(Width, Height);
```

`prevUV` 用未 jitter 的 `PrevViewProj` 计算（不含 jitter），而 `curUV` 含 jitter。DLSS feature 未设 `MVJittered` flag，DLSS 会用 `InJitterOffsetX/Y` 内部校正 MV → **current jitter 被计两次** → 静止时每帧误差 = `-cur_jitter`。

**首次修复**：去掉 `curUV` 中的 jitter：

```hlsl
float2 curUV = (id.xy + 0.5) / float2(Width, Height);
```

**结果**：抖动减轻但未消除。

### 阶段三：curUV 用像素中心而非投影位置（真正的根因）

**症状**：阶段二修复后仍有抖动。

**关键对比**：参照项目 `MyEngineVulkanV2/vk_gltf_renderer` 的 `calculateMotionVector`（`shaders/dlss_util.h`）：

```c
// 参考项目：curUV 和 prevUV 都通过投影 worldPos 得到
float2 currentScreenPos = currentClipPos.xy * 0.5 + 0.5;  // 投影 worldPos → currentMVP
float2 prevScreenPos    = prevClipPos.xy * 0.5 + 0.5;      // 投影 worldPos → prevMVP
float2 motionVector     = prevScreenPos - currentScreenPos; // 静态时 = 0
```

URPPT 的 MV kernel 用了**像素中心**作为 `curUV`：

```hlsl
// URPPT 阶段二（仍有问题）
float2 curUV = (id.xy + 0.5) / float2(Width, Height);  // 像素中心
```

**为什么像素中心不对**：G-buffer 中的 `worldPos` 是 **jittered 光线**命中的几何点。用未 jitter 的 `CurViewProj` 投影它，得到的是 `(pixel + 0.5 + jitter) / resolution`（近似），而非像素中心。所以：

\[
\text{MV}_{\text{static}} \approx \frac{\text{pixel} + 0.5 + \text{jitter}}{\text{res}} - \frac{\text{pixel} + 0.5}{\text{res}} = \frac{\text{jitter}}{\text{res}} \neq 0
\]

这个误差每帧随 Halton 相位变化 → 持续抖动。放大倍数越大，DLSS 在输出空间放大的误差越大 → **Quality Mode 越高抖得越厉害**。

**修复**：`curUV` 改为投影 worldPos 用 `CurViewProj`（复用已计算的 `curClip`）：

```hlsl
// 最终修复
float2 curNdc = curClip.xy / curClip.w;
float2 curUV  = curNdc * 0.5 + 0.5;
```

静态相机时 `CurViewProj == PrevViewProj`，投影同一个 `worldPos` 结果完全一致 → **MV = 0**。

**结果**：完全不抖动。

## 核心教训

### 1. MV 的 curUV 和 prevUV 必须对称

运动矢量的 `curUV` 和 `prevUV` 必须用**相同的方法**计算。参考项目的做法是两个都投影 `worldPos`（分别用 `currentMVP` 和 `prevMVP`），保证静态相机时 MV = 0。

**绝对不能用像素中心作为 `curUV`**，因为 `worldPos` 来自 jittered 光线，其投影位置天然包含 jitter 偏移，与像素中心之间存在固定差值 `jitter`。

### 2. Jitter 在 DLSS 管线中的正确流向

```
渲染侧（pathtrace.glsl）         MV 侧（PathTrace.compute）         DLSS 侧（vpt_dlss.cpp）
┌─────────────────┐              ┌─────────────────────┐           ┌────────────────────┐
│ uv = pixel+0.5  │              │ curClip = CurViewProj│           │ InJitterOffsetX/Y  │
│     + jitter    │              │          * worldPos  │           │   = jitter         │
│     / (w,h)     │              │ curUV = curNdc*0.5+0.5│          │ MVJittered = 0     │
│                 │              │ prevClip= PrevViewProj│          │ InMVScaleX/Y       │
│ 光线含 jitter   │              │          * worldPos  │           │   = renderW/H      │
│ → worldPos 含   │              │ prevUV = prevNdc*..  │           │                    │
│   jitter 偏移   │              │ MV = prevUV - curUV  │           │ DLSS 内部用        │
│                 │              │   （静态时 = 0）      │           │ InJitterOffset     │
└─────────────────┘              └─────────────────────┘           │ 做唯一 jitter 校正 │
                                                                    └────────────────────┘
```

- 渲染侧：jitter 加到光线方向上 → `worldPos` 天然含 jitter
- MV 侧：**不加 jitter**，用投影 `worldPos` 得到 `curUV`/`prevUV`，两者对称 → 静态时 MV = 0
- DLSS 侧：`MVJittered` flag 不设，DLSS 用 `InJitterOffsetX/Y` 做唯一 jitter 校正

### 3. 为什么不能用像素中心 + 去 jitter 的方案

阶段二的修复思路是"去掉 `curUV` 的 jitter，让 DLSS 自己校正"。但这有个隐含假设：**`prevUV`（投影 `worldPos`）≈ 像素中心**。实际上 `worldPos` 来自 jittered 光线，投影后 ≈ `(pixel + 0.5 + jitter) / resolution`，与像素中心差一个 jitter。所以 `prevUV - curUV ≈ jitter ≠ 0`。

只有把 `curUV` 也改成投影 `worldPos`，才能让两者完全对称。

## 最终代码状态

### MV kernel（`Assets/Shaders/PathTrace.compute` L1680-1731）

```hlsl
[numthreads(8, 8, 1)]
void MotionVector(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    float4 gpd = GBufferPosDepth[id.xy];
    float  depth = gpd.a;

    if (depth <= 0.0)  // 天空像素
    {
        DepthOut[id.xy] = 0.0;
        MotionVectorOut[id.xy] = float2(0.0, 0.0);
        return;
    }

    float3 worldPos = gpd.rgb;

    // 深度：投影 worldPos → CurViewProj → NDC 深度
    float4 curClip = mul(CurViewProj, float4(worldPos, 1.0));
    DepthOut[id.xy] = curClip.z / curClip.w;

    // prevUV：投影 worldPos → PrevViewProj
    float4 prevClip = mul(PrevViewProj, float4(worldPos, 1.0));
    if (prevClip.w <= 0.0)
    {
        MotionVectorOut[id.xy] = float2(0.0, 0.0);
        return;
    }
    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUV = prevNdc * 0.5 + 0.5;

    if (any(prevUV < 0.0) || any(prevUV > 1.0))
    {
        MotionVectorOut[id.xy] = float2(0.0, 0.0);
        return;
    }

    // curUV：投影 worldPos → CurViewProj（与 prevUV 对称）
    float2 curNdc = curClip.xy / curClip.w;
    float2 curUV  = curNdc * 0.5 + 0.5;

    // MV = prev - cur（静态时 = 0）
    MotionVectorOut[id.xy] = prevUV - curUV;
}
```

### DLSS Evaluate 参数（`NativePlugin/src/vpt_dlss.cpp`）

```cpp
// Feature flags（创建时）
int createFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
// 不设 MVJittered

// Evaluate 参数（每帧）
evalParams.InJitterOffsetX = jitterX;   // 渲染像素空间 [-0.5, 0.5]
evalParams.InJitterOffsetY = jitterY;
evalParams.InMVScaleX      = (float)renderW;  // UV → 像素空间
evalParams.InMVScaleY      = (float)renderH;
```

> 以上为 **SR 分支**参数（本次未改动）。**RR 分支**的两处改动（create flags 首选 0x43、jitter 取负）见文末「RR 模式边缘抖动排查」。

## Jitter 生成（`Assets/Scripts/PathTraceRendererFeature.cs`）

```csharp
// Halton(2,3) 8 相位循环，渲染像素空间 [-0.5, 0.5]
private static float Halton(int index, int haltonBase)
{
    float f = 1f, r = 0f;
    for (int i = index; i > 0; i /= haltonBase)
    {
        f /= haltonBase;
        r += f * (i % haltonBase);
    }
    return r - 0.5f;
}

public static Vector2 GetJitter(int frameCount)
{
    int phase = (frameCount % 8) + 1;
    return new Vector2(Halton(phase, 2), Halton(phase, 3));
}
```

## 参考项目对比

| 参数 | 参考项目 (DLSS-D) | URPPT (DLSS-SR) |
|---|---|---|
| MV `curUV` | 投影 worldPos → `currentMVP` | 投影 worldPos → `CurViewProj` |
| MV `prevUV` | 投影 worldPos → `prevMVP` | 投影 worldPos → `PrevViewProj` |
| MV 空间 | 像素空间 (`* resolution`) | UV 空间（通过 `InMVScale` 转换）|
| `InMVScaleX/Y` | `1.0f` | `renderW / renderH` |
| `MVJittered` flag | 不设 | 不设 |
| Jitter 传给 DLSS | `-jitter`（取负） | `+jitter`（未取负） |

> **备注**：参考项目使用 DLSS-D (Ray Reconstruction)，URPPT 使用 DLSS-SR (Super Sampling)。两者共享 `Jitter_Offset` / `MVScale` / `MVJittered` 参数约定。参考项目传 jitter 时取负（`-info.jitter`），URPPT SR 传正数（实测不抖，保持现状）。**此差异在 RR 模式命中**——见下方「RR 模式边缘抖动排查」根因 1。

---

## RR 模式边缘抖动排查（2026-09-07，DLSS 阶段 4 后续，待重启验证）

> 本节记录 **RR（Ray Reconstruction）模式**的改动，**SR 分支代码未动**。完整诊断过程见 `doc/DLSS_RR集成踩坑记录.md`「问题三」。

### 现象

DLSS 阶段 4（RR）完成后，RR 模式下静止场景**模型边缘持续轻微抖动**；SR 模式仍完全不抖。

### 排除项（先确认上文 SR 三阶段无回归，再对照官方参考逐项核对）

| 疑点 | 结论 |
|---|---|
| MV kernel 对称性（SR 根因回归） | 排除：SR/RR 共享同一 MotionVector kernel，SR 实测不抖 |
| 矩阵主序 | 排除：`MatrixToFloat16` 行主序直拷 ≡ 官方 `glm::transpose(M)` + `value_ptr`，无需转置（RR 文档原「待确认项」关闭） |
| MVScale / Roughness Packed / Normals 世界空间 / EnvBRDFApprox | 排除：与官方参考数学等价 |

### 根因 1：RR 的 InJitterOffset 必须取负（上表 jitter 差异命中）

- 渲染侧 jitter 应用方向两侧**完全一致**：`uv = (pixel + 0.5 + jitter) / res`（官方 gltf_pathtrace.slang：`subpixelJitter = jitter + float2(0.5, 0.5)`）
- 但官方参考传 NGX 时**取负**：`InJitterOffsetX = -info.jitter.x`（dlss_wrapper.cpp `cmdDenoise`）
- RR 沿用了 SR 的 `+jitter` → transformer 模型帧间亚像素对齐每帧偏差 `2×jitter`，随 Halton 8 相位逐帧变化 → 边缘高频细节来回摆动
- **为什么 SR 传 +jitter 不抖而 RR 抖**：SR 输入是空域滤波后的干净图像、CNN 模型时域校正弱，亚像素误差不可见；RR 是 1spp 噪声 + transformer 精确时域累积，误差被直接放大

### 根因 2：DepthInverted flag 与线性深度语义矛盾

- `DepthInverted` 语义：深度为**反向 z 排序（near 大 far 小）**，仅适用 SR 的 reversed-Z NDC 深度
- RR 的线性深度 `LinearDepthOut = curClip.w` 是 near 小 far 大的**非反向**视距离
- NVIDIA Streamline 集成清单：「如果所提供的深度缓冲区采用反向 z 排序，则设置 Inverted Depth 位」——线性视距离不满足
- 官方参考 Linear 模式下 `InFeatureCreateFlags = IsHDR | MVLowRes`（无 DepthInverted）
- 后果：RR 深度一致性/disocclusion 检测方向颠倒 → 边缘深度突变处时域历史被错误取舍

### 修复（`NativePlugin/src/vpt_dlss.cpp`，仅 RR 分支）

```cpp
// 1. RR create flags：首选 0x43（移除 DepthInverted），0x4b 降为备选对照
const int rrFlagCombos[] = {
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR
  | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
  | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure,              // 0x43 首选
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR
  | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
  | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
  | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted,             // 0x4b 备选
};

// 2. RR evaluate：jitter 取负（SR 分支保持 +jitter 不动）
rrEval.InJitterOffsetX = -s_PendingJitterX;
rrEval.InJitterOffsetY = -s_PendingJitterY;
```

DLL 已重编译部署至 `Assets/Plugins/x86_64/`（MD5 校验一致）。验证点：Editor.log 中 `RR create (flags=0x43 noDepthInv)` 应首次尝试即成功。

### RR 残余排查方向（若修复后仍有轻微抖动，按序排查）

1. **深度量纲**：URPPT 传米制正视距离 `curClip.w`；官方参考实际写 `abs(NDC.z)` ∈ [0,1]（变量名 ViewZ，且其同样声明 Depth_Type_Linear——参考实现自身量纲存疑）。若 RR 对深度归一化/带符号 viewZ 有内部假设，需实验调整 MotionVector kernel 的 `LinearDepthOut` 写法
2. **矩阵 clip 约定**：Unity `Camera.projectionMatrix` 是 GL 风格（z∈[-1,1]，Y-up）；若 NGX 按 Vulkan clip 约定（z∈[0,1]，Y-down）解释，reprojection 有偏差。可尝试 `GL.GetGPUProjectionMatrix` 转换后传入
3. **A/B 归因**：两处根因如需区分贡献，可单独翻转一处（jitter 符号 / flags 顺序）观察抖动变化

---

## RR 亮处抖动排查（2026-09-07，边缘抖动修复验证后）

> 上节两处修复重启验证后边缘抖动消除，但出现新现象。本节接续上节「残余排查方向」——方向 3（A/B 归因）思路命中：flags 与官方参考还有第三处差异。

### 现象

静止场景**被光照到的像素持续抖动，暗处像素基本不抖**。抖动与像素亮度强相关，与几何位置无关。

### 排除项

| 疑点 | 结论 |
|---|---|
| MV/矩阵/深度量纲（上节残余方向 1/2） | 排除：几何对齐类问题的症状应集中在物体边缘与深度突变处，且与亮度无关，无法解释「亮抖暗稳」 |
| InColor alpha 恒 1.0（pathtrace.glsl `imageStore(..., vec4(radiance, 1.0))`） | 排除：官方 RR 指南 4.1.5 节仅要求 3 通道噪声颜色；specular hit distance 是独立可选 buffer（4.1.9，`kBufferTypeSpecularHitDistance`）；AlphaUpscaling 未启用时 alpha 被忽略 |

### 根因 3：RR create flags 中的 AutoExposure（从 SR 沿袭，官方参考没有）

- 官方参考 Linear 深度模式 `InFeatureCreateFlags = IsHDR | MVLowRes`（**0x03**），URPPT RR 首选 **0x43**（多出 AutoExposure）。该位是从 SR 分支（`MVLowRes|AutoExposure|DepthInverted`）沿袭的——上节修复只对齐了「移除 DepthInverted」
- Streamline 官方文档（`docs/ProgrammingGuideDLSS_RR.md` 第 5 节 NOTE）明确注明：**"DLSS-RR will ignore DLSS options sharpness and useAutoExposure"**——官方集成路径刻意屏蔽 AutoExposure；URPPT 直连 NGX API 绕过了这层屏蔽
- 日志实锤（`nvngx_dlssd_310_7_0.log`）：RR 实例创建了 `AutoExposure`、`PrevAutoExposureTexture_0/1` 内部纹理——自动曝光通路真实启用
- 机制：RR 输入是 1spp 逐帧独立噪声（NEE 随机选 1 灯 × lightCount 补偿 + Halton jitter），全图亮度统计逐帧波动 → AutoExposure 每帧估计的曝光逐帧波动（PrevAutoExposure 纹理仅有限时域平滑）→ 曝光是全图乘性作用（`输出 ≈ network(color/exposure)×exposure`），亮区绝对波动远大于暗区 → **亮抖暗稳**
- 场景放大因素：两盏点光源 intensity=8/100 差 12.5 倍，2m 处 NEE 单帧值在 ~18 与 ~1.4 双峰跳变，帧间差一个数量级；且 rgba16f 长尾 + RR 模式跳过 AntiFirefly，偶发极亮样本加剧统计波动
- 为什么 SR 带 AutoExposure 不抖：SR 输入是空域滤波后的干净图像，帧间稳定，曝光估计不波动（与「SR 传 +jitter 不抖而 RR 抖」同一类机制：RR 对输入扰动远比 SR 敏感）
- NRD 官方 README 同源佐证：时域降噪管线「难以响应瞬时的曝光变化」（a significant momentary change in exposure is hard to react to）

### 修复（`NativePlugin/src/vpt_dlss.cpp`，仅 RR 分支）

```cpp
const int rrFlagCombos[] = {
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR
  | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,          // 0x03 首选（与官方参考完全一致）
    // 0x43（+AutoExposure，旧首选）与 0x4b（+DepthInverted）降为备选对照
};
// evaluate 侧 InPreExposure / InExposureScale = 1.0 即固定曝光（pInExposureTexture 可选，官方参考同此约定）
```

DLL 已重编译部署至 `Assets/Plugins/x86_64/`（MD5 校验一致，2026-09-07 23:42）。SR 分支不动。

### 验证点（需完全重启 Unity 编辑器后 Play）

1. Editor.log 中 `RR create (flags=0x3 refFlags(noAutoExposure))` 应首次尝试即成功（若回退到 0x43 说明 snippet 拒绝无 AutoExposure 的组合，需另查）
2. dlssd snippet 日志中不再分配 AutoExposure / PrevAutoExposureTexture 内部纹理
3. 静止场景亮区（灯照处）抖动显著减轻或消除，暗区不受影响

### 若仍有残余亮区抖动（次因：输入噪声分布外）

- NEE 改按光强加权选灯（power-based sampling）或每帧采样全部光源，消除双峰跳变
- 对 NEE/BSDF 贡献加 clamp 压 firefly（长尾偶发极亮样本）
- 或调低 intensity=100 的点光源

> **2026-09-08 更新**：单平行光场景验证后仍有亮区抖动，且调强度无效——上述「双峰跳变」假设被排除（双峰需要 ≥2 盏灯）。新根因分析（RR 自适应累积对低方差直接光弱累积 + 训练分布 OOD）与样例代码逐层对比、验证实验计划见 `doc/DLSS_RR亮区抖动分析.md`。
