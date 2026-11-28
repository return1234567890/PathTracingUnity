# PT 之后的自定义 Pass 管线设计（前置段不动 + 轻量 Pass 图）

> **需求变更（2026-11-28）**：初版曾计划把「PT → MotionVector → DLSS RR → 后处理」整条链都纳入自研图。现范围收窄为：**前置段（PT + DLSS RR + 最终上屏）的代码与资源管理保持现状不动**，自研的轻量 Pass 图**只服务 DLSS 输出之后的自定义 pass**（前向、后处理、调试可视化）。本文已按此重写；文件名沿用初版，旧链接仍然有效。
>
> 调研对象：URPPT —— Unity 2022.3.62f3c1 + URP 14.0.12 + Vulkan Ray Tracing Pipeline + 原生 NGX DLSS RR。
> 日期：2026-11-28。
> 范围：**定义接口、契约与工程边界**，并给出工作量与落地顺序。**不含具体效果实现**（tonemap / bloom / FXAA 的算法与调参不在本文）。
> 方法：前置段源码审计 + URP 14.0.12 包源码 + `Library/Bee` 实际编译参数。原生 RenderGraph 的可行性证据见附录 B。

---

## 0. 范围与已定决策（2026-11-28）

**v1 已拍板：只做 tonemap。**

| 决策 | 结论 |
|---|---|
| **D1** tonemap | **做**。v1 唯一的效果；提供 ACES / Reinhard / Clip 三选一，exposure 手动参数，默认 ACES + 1.0 |
| **D2** 前向 pass | **不做**，推迟。v1 没有任何前向/overlay 代码 |
| **D3** 几何排除 | **不做**，推迟（没有前向 pass 就不需要） |
| **D4** 深度 | **不做**，推迟（`FrameInputs.Depth` 保留字段，恒为 null） |
| **D5** 后处理清单 | **只做 tonemap**；bloom / FXAA / 调色全部推迟 |
| **D6** 参数存放 | **不序列化**。参数集中在 `PostGraphConfig` 的静态字段里，`URP-PathTrace-Renderer.asset` **不新增任何字段** |
| **D7** shader 引用 | 放在 `Assets/Shaders/PostTonemap.shader`，用 `Shader.Find` 取；已加入 Graphics Settings 的 **Always Included Shaders**，不占用 Renderer 资产的序列化引用 |

v1 实际链路：

```
Import(Color) → [Tonemap] → Present(cameraColor)
```

- `PostGraphConfig.Enabled = false`：走旧的直接 Blit（回退路径）；
- `PostGraphConfig.TonemapEnabled = false`：链退化为一次拷贝，用于 A/B；
- 前向、深度、bloom/FXAA 的设计保留在 §7 / §8 / §5.4，等需要时再实现。

**v1 实现落点**（已完成）：

```
Assets/Scripts/Rendering/PostGraph/
├── RgTypes.cs              RgTexture / RgSize / RgFormat / RgTextureDesc
├── FrameInputs.cs          交接契约
├── PostPass.cs             PostPass + PostPassContext（含 Flush）
├── PostGraphConfig.cs      tonemap 开关/曝光/算子（静态，不序列化）
├── PostGraphBuilder.cs     Import/Create/Read/ReadWrite/Add/Present + 校验
├── PostGraphPool.cs        持久池（按 desc 复用 RenderTexture）
├── PostGraph.cs            执行器（Rebuild/Execute/Present）
├── PostGraphPipeline.cs    v1 链：Import → [Tonemap] → Present
└── Passes/TonemapPass.cs   tonemap pass
Assets/Shaders/PostTonemap.shader       ACES / Reinhard / Clip
```

前置段唯一改动：`PathTraceRendererFeature.cs` 的 `Execute` 末尾把 `cmd.Blit` 换成 `Present(...)`；**feature 上不新增任何序列化字段**（开关/参数在 `PostGraphConfig`，shader 用 `Shader.Find` + Always Included）；`Dispose` 释放图与材质。

---

## 1. 结论速览

| 项 | 结论 |
|---|---|
| **前置段是否改** | **不改**。PT / MotionVector / DLSS 的代码、RT、DLSS 状态机全部保留；唯一改动是把末尾的 `cmd.Blit` 换成 `_postGraph.Execute(...)`（图内部以 `Present` 收尾），回退开关在 `PostGraphConfig.Enabled` |
| **后置段形态** | **单分辨率的轻量 Pass 图**；v1 只有 `Import → [Tonemap] → Present`，前向/深度/bloom/FXAA 全部推迟 |
| **输入契约** | `FrameInputs`：`Color`（必填；DLSS 关闭时是 1spp 原图）、`Depth`（可选，v1 恒 null）、`GBuffer0/1`（可选）+ 相机矩阵与帧号 |
| **资源规则** | 导入即只读；图内资源按版本分配、持久池化；**不做"两个同时存活的版本共享同一张 RT"**（§4.6） |
| **顺序规则** | **注册顺序即执行顺序**（显式、可读）；依赖只用于校验与生命周期，不做自动重排 |
| **校验** | 8 条构建期校验，失败直接报错并打印 pass/资源/版本（§6.1） |
| **工作量** | v1（图核心 + tonemap）：核心约 **400~500 行** + 一个 shader，**2~3 人日**；前向/bloom 另计 |
| **关键风险** | v1 只有全屏 blit，没有 `DrawRenderers`/几何/深度问题；风险集中在 **tonemap 观感（exposure 手调）** 与 RT 池的稳定性 |
| **不做** | 资源 aliasing、pass culling、async compute、自动 barrier、多相机/子图嵌套、把 native pass 建模进图 |
| **一句话** | 「前置段是一台 native 状态机，包进图只会把 native 的复杂度搬进图；真正需要图的是它后面那串可插拔的 pass。」 |

---

## 2. 边界定义（本文最重要的一节）

### 2.1 帧结构：前置段 / 交接点 / 后置段

```
┌─────────────────────────── 前置段（现有代码，不动）───────────────────────────┐
│ PathTraceRenderPass.Execute                                                    │
│  ├─ EnsureRTs（9 张 RT）              Assets/Scripts/PathTraceRendererFeature.cs:827 │
│  ├─ DLSS 初始化/重建状态机            :429-501                                  │
│  ├─ 场景同步（实例/蒙皮）             :506-511                                  │
│  ├─ native PT（IssuePluginEvent）     :555-563                                  │
│  ├─ MotionVector compute              :575-590                                  │
│  └─ native DLSS RR（IssuePluginEvent）:601-607                                  │
└────────────────────────────────────────────────────────────────────────────────┘
                                   │
                     FrameInputs（§2.2，只读交接）
                                   ▼
┌─────────────────────────── 后置段（本文定义，v1 只有 tonemap）──────────────────┐
│ PostGraph                                                                       │
│  ├─ Import(Color)        只读导入 DLSS/PT 结果                                   │
│  ├─ [Tonemap]            线性 HDR → 显示参考（ACES / Reinhard / Clip）            │
│  └─ Present → cameraColor                                                        │
│                                                                                 │
│  （推迟，接口预留：DepthFromGBuffer / Copy / ForwardOverlay / Bloom / FXAA）       │
└────────────────────────────────────────────────────────────────────────────────┘
```

**关键点：交接点只有 `FrameInputs` 一个入口。** 后置段不读 `PathTraceRenderPass` 的私有字段，也不改前置段的状态；前置段不知道后置段内部结构（只是把末尾"上屏"这一步交给图）。

### 2.2 交接契约：`FrameInputs`

```csharp
public struct FrameInputs
{
    /// 输出分辨率，必填。
    /// DLSS 开启：_dlssOutputRT；DLSS 关闭：_outputRT（此时它本身就是全分辨率）。
    public RenderTexture Color;

    /// 输出分辨率，可选。现在恒为 null；
    /// 将来 native 直接输出深度时填这里，后置段不需要改任何 pass。
    public RenderTexture Depth;

    /// 渲染分辨率（DLSS 前），可选：worldPos.xyz + 命中距离。
    public RenderTexture GBuffer0;

    /// 渲染分辨率（DLSS 前），可选：normal.xyz + roughness。
    public RenderTexture GBuffer1;

    public Matrix4x4 View;
    public Matrix4x4 Proj;
    public Matrix4x4 PrevViewProj;
    public Vector2   Jitter;
    public int       FrameIndex;
}
```

规则：

1. **`Color` 稳态下是输出分辨率**。前置段负责把"低分辨率渲染 → 全分辨率"这件事做完（DLSS 或直接全分辨率 PT）。后置段不需要知道 DLSS 存在；唯一的例外是 DLSS 未就绪的 1~2 帧（见本节末尾）。
2. **`Color` 的选取必须复用前置段现有的降级判据**，不能只看 `dlssEnabled`：
   ```csharp
   bool useDlssOutput = _feature.dlssEnabled && _dlssInitialized && cs != null;  // 与 `:589` 的 blit 判据完全一致
   Color = useDlssOutput ? _dlssOutputRT : _outputRT;
   ```
   否则"开了 DLSS 但初始化失败 / motionVectorShader 没赋值"时，`_dlssOutputRT` 是一张**从没写过的图**，后置段会拿到黑屏。
3. **`GBuffer0/1` 是渲染分辨率**（DLSS 关闭时它就等于输出分辨率），只能通过显式的升采样/重建 pass 消费（如 `DepthFromGBuffer`），**不能直接当渲染目标**。
4. **所有输入只读**。导入的 RT 在图里被 `Write` 一律校验失败。
5. 输入为 `null` 时，依赖它的 pass 自动禁用（例如没有 `Depth` 且没有 `GBuffer0` 时，前向 pass 关闭并打一条 warning）。
6. **不要拿 `_linearDepthRT` 冒充 `Depth`**：它是渲染分辨率的视空间线性距离，给 RR 用的；前向 pass 需要的是输出分辨率的 NDC 深度。语义不同，宁可走 `DepthFromGBuffer`。

### 2.3 DLSS 关闭 / 回退时的行为（直接回答"噪点"问题）

这一条决定了后置段在两种模式下的输入差异，必须写清楚：

| 项 | DLSS 开启且成功 | DLSS 关闭 / 初始化失败 |
|---|---|---|
| `FrameInputs.Color` | `_dlssOutputRT`（RR 重建后的全分辨率） | `_outputRT`：DLSS 关闭时是**全分辨率 1spp**；初始化失败时是**渲染分辨率 1spp**（靠 Bilinear 拉伸上屏） |
| 渲染分辨率 `w×h` | 按质量模式缩放（如 1.7×） | **等于输出分辨率**（`PathTraceRendererFeature.cs:389-396`） |
| `GBuffer0/1` 尺寸 | 渲染分辨率（低分） | **输出分辨率**（全分辨率，和 Color 同尺寸） |
| jitter | Halton 8 相位 | **恒为 0**（`:407-409`） |
| 降噪 | RR 负责 | **没有任何降噪**（软件链路已移除） |

结论：

1. **是**。DLSS 关闭时，后置段直接拿 `_outputRT`——**原始 1spp 结果**。图把它当一张不透明输入纹理：不检测噪点、不降噪、也不因此改变链路（直接输出，不做额外处理）。
2. **DLSS 关闭时没有 jitter**，所以任何依赖时域累积的自定义 pass（TAA/时域降噪）需要自己产生 jitter，不能指望前置段。
3. **DLSS 专用 RT 在关闭时是 `null`**（`EnsureRTs` 里 `if (_feature.dlssEnabled)` 才创建，`:864-869`）。后置段通过 `FrameInputs` 拿不到它们，因此也不会误用；如果以后要暴露，必须带 null 判断。

> **一个必须写进代码的例外**：DLSS 开启但 `_dlssInitialized == false` 的前 1~2 帧（`CreateFeature` 是排队异步创建的），`useDlssOutput == false`，而此时的 `w/h` 仍是**渲染分辨率**——`_outputRT` 就是低分图，旧路径靠 `cmd.Blit` 的 Bilinear 拉伸上屏。
>
> 所以图**不能假设 `Color` 一定是输出分辨率**：按 `inputs.Color.width/height` 实际尺寸处理，并把 `Color` 的宽高纳入重建 key（§6.3）。这 1~2 帧里 FXAA/tonemap 在低分图上跑一遍，视觉上和旧的拉伸 blit 等价，不会出错。

### 2.4 前置段保留什么 / 改什么

**保留（零改动）**：`EnsureRTs`/`ReleaseRTs`、DLSS 初始化与重建状态机、native 三段事件、`MotionVector` compute、GPU 计时、设备丢失处理、`PathTraceCameraGate` 的抑制逻辑。

**改动（三处，很小）**：

| 位置 | 改动 |
|---|---|
| `Execute` 末尾（原 `:577-598`，现 `Present(...)`） | 删除原来的两处 `cmd.Blit`，改为 `Present(cmd, context, camera, colorTarget, source, jitter)`；`PostGraphConfig.Enabled == false` 或图不可用时，`Present` 内部仍走旧的 `cmd.Blit` |
| 新字段 | **feature 上无新增序列化字段**；参数在 `PostGraphConfig`；pass 内 `_postGraph` / `_tonemapMaterial` |
| `Dispose` | `_postGraph?.Dispose()`（释放池里的 RT） |

> 这条"旧路径保留"很重要：**图出问题时能一个开关退回现在这套**，而不是只能 revert 代码。

### 2.5 后置段的职责 / 非职责

**职责**：管理自己创建的 RT 的生命周期；按注册顺序执行 pass；解析版本；校验读写；把最终颜色写到 `cameraColor`。

**非职责**：不管理前置段的 RT；不重建 DLSS；不碰 native 资源；不做场景剔除/光照设置；不做相机栈。

### 2.6 为什么前置段不进图（这是需求变更的核心理由）

1. **前置段是 native 状态机**：DLSS 的 `CreateFeature` 排队/轮询、分辨率变化时的销毁重建、`vkQueueWaitIdle`、设备丢失，这些是"有状态的初始化流程"，不是"每帧的渲染 pass"。图的价值在每帧的 pass 组合，包进图只会把这些状态塞进 `Declare`，更难懂。
2. **native 图的 layout 账本在插件手里**（`vpt_dispatch.cpp:441-466, 533-537`），图的资源模型（transient + 复用）与它冲突（附录 B.3）。
3. **收益为零**：前置段只有 3 个"pass"，没有可选组合、没有 aliasing 收益；图能提供的校验/复用在这里都不需要。
4. **风险隔离**：后置段的图出错，画面顶多缺个后处理；前置段包进图出错，整个 PT 不出图。**把风险关在后置段**是这次收窄范围最大的收益。

---

## 3. 可行性回顾：为什么不用原生 RenderGraph

（详细证据见附录 B；这里只留结论。）

| 路线 | 结论 | 关键理由 |
|---|---|---|
| **A. 2022.3 内置 URP RenderGraph** | **不可行** | 那套移植被 `RENDER_GRAPH_ENABLED` 宏整体关掉，该宏在工程的实际编译参数里不存在；`ScriptableRenderPass.RecordRenderGraph` 又是 `internal`，自定义 pass 无法参与 |
| **A′. 在一个 pass 里 `new` core 14 实验 `RenderGraph`** | **不推荐** | API 表面够用，但它是 HDRP 内部实现；资源池会在帧间轮换 `VkImage`，与插件的 ImageView 缓存冲突 |
| **B. 升级 Unity 6 + URP 17** | 可行，但**现在不值** | 要换引擎；native 插件（`vkCreateDevice` 拦截 / RT 扩展 / NGX）要在新后端重测，估算 10~27 人日 + 不确定硬阻塞 |
| **C. 自研后置段轻量图** | **推荐** | 纯 C#，核心 500~700 行；只覆盖后置段，风险与前置段隔离 |

**多出来的一个理由**：即使升到 Unity 6，原生 RG 也不会替我们处理"native 图 layout 与 Unity 账本不一致"这个真正的痛点——它只知道 Unity 资源。所以原生 RG 对这条管线的边际价值比一般项目更小。

---

## 4. 资源模型

### 4.1 单分辨率原则（对前置段最大的简化）

后置段**只在输出分辨率工作**：

- `Output`：相机输出分辨率（`camera.pixelWidth/Height`），绝大多数资源；
- `Fraction`：输出分辨率的 1/2、1/4，给 bloom mip 链；
- `Absolute`：显式宽高，给特殊 pass（如 LUT）。

**没有"渲染分辨率"这一类可分配资源。** 低分辨率数据只能以 `Import` 形式进来（`GBuffer0/1`），并且只能被"升采样/重建"类 pass 读取。DLSS 关闭时 `GBuffer0/1` 就是全分辨率，`DepthFromGBuffer` 照常工作（只是不需要升采样）。这条规则把初版设计里"两套分辨率混用"的整类 bug 直接从类型层面消掉了。

### 4.2 三种资源

| 类型 | 来源 | 谁释放 | 图内是否可写 |
|---|---|---|---|
| **Imported** | 前置段 / native 的 RT（`FrameInputs.*`） | 前置段 | ❌ 只读 |
| **Transient** | `Create(desc)`，图自己分配 | 图的池 | ✅ |
| **PresentTarget** | URP 的 `cameraColorTargetHandle` | URP | 只由 `Present` 写一次 |

### 4.3 资源、版本与链的传递

```csharp
public readonly struct RgTexture
{
    public readonly int Id;       // 逻辑资源槽
    public readonly int Version;  // 第几次写入；0 = 未写
}
```

- **每个 pass 的输出默认是一张新图**（`Create` → 新资源 v1）。链的先后关系由 `BuildChain` 里传递的句柄**显式**表达：`color = g.Add(pass { Input = color })`。
- `ReadWrite` 用于"原地写"（典型：前向物体混入已有背景）：不新建资源，**`Id` 不变、`Version + 1`**，返回新版本的句柄。
- 版本号的作用只有三个：校验"先写后读"、计算生命周期（最后一个读者）、在报错/dump 里定位。
- **可选 pass 关闭时，`BuildChain` 直接跳过它**，下游拿到的是上一个 pass 的句柄——不需要写"如果关闭就走另一条连线"的分支。

> 例：`Import(Color) → Tonemap 新图 → FXAA 新图 → Present`。关掉 FXAA，`Present` 读的是 Tonemap 的输出；关掉 Tonemap，读的是导入色。整条链的"回退"是句柄自然沿用的结果，不是图在背后做隐式解析。

### 4.4 尺寸与格式

```csharp
public enum RgSize { Output, Fraction, Absolute }

public enum RgFormat
{
    ColorRGBAHalf,   // 后处理中间结果（HDR）
    ColorRGBAFloat,  // 需要更高精度时（少用，带宽翻倍）
    ColorRFloat,     // 单通道（如 AO、深度重建的中间值）
    ColorRGFloat,    // 双通道
    Depth32,         // D32_SFLOAT，深度附件
}

public sealed class RgTextureDesc
{
    public string     Name;
    public RgSize     Size = RgSize.Output;
    public float      Fraction = 1f;          // Size == Fraction 时用
    public RgFormat   Format = RgFormat.ColorRGBAHalf;
    public bool       RandomWrite;            // compute 写必须 true
    public FilterMode Filter = FilterMode.Point;
}
```

### 4.5 池化与生命周期

- **键**：`(w, h, format, randomWrite, filter)`；同键复用同一个 `RenderTexture` 对象。
- **跨帧保留**：池里的 RT 不主动销毁；只有连续 N 帧（如 120）没被任何图用到，才 `Release()`。
- **帧内释放**：一个版本在"最后一个读者执行完之后"归还池子，可被后续 pass 复用；**复用时该版本已不再存活**，不会出现两张同时存活的图共享同一张 RT（§4.6）。
- **绝不每帧 Create/Release**：`doc/bug追踪.md` BUG-004 就是每帧重建 RT 导致 Unity 20 bit resource ID 耗尽、黑屏。这条规则是硬约束，不是优化。

### 4.6 为什么不做"两个同时存活的版本共享同一张 RT"

那是真正的 aliasing，需要精确的 barrier 与生命周期证明。收益是省 1~2 张 1080p ARGBHalf（约 16~33 MB），代价是把"图算错生命周期"变成花屏。**池化已经能把稳态显存压到"最大并发版本数"张，够用了。**

---

## 5. API 定义

### 5.1 构建器

```csharp
public sealed class PostGraphBuilder
{
    /// 导入外部 RT（只读）。同一个 RT 一帧只能导入一次。
    public RgTexture Import(RenderTexture rt, string name);

    /// 创建/写入一个新版本。
    public RgTexture Create(RgTextureDesc desc);

    /// 声明只读。
    public void Read(RgTexture t);

    /// 原地写：不产生新版本，返回同一个句柄。
    /// 只能用于图内资源（Imported 会被校验拒绝）。
    public RgTexture ReadWrite(RgTexture t);

    /// 注册一个 pass，返回它的输出句柄。
    /// pass.Enabled == false 时直接返回 pass.Input（不注册、不 Declare）。
    /// 内部：pass.Declare(this) → 记录读写 → 计算生命周期。
    public RgTexture Add(PostPass pass);

    /// 指定最终上屏的颜色（必须恰好一次，且在所有 pass 之后）。
    public void Present(RgTexture color);
}
```

```csharp
public sealed class PostGraph : IDisposable
{
    /// 构造：传入链的组装函数（§5.5）；只在 Rebuild 时调用。
    public PostGraph(Func<PostGraphBuilder, FrameInputs, RgTexture> buildChain);

    /// 强制重建（一般不用；配置变化由 Execute 内部自动检测）。
    public void Rebuild(FrameInputs inputs);

    /// 每帧调用：检测配置变化 → 必要时重建 → 按顺序执行 → Present。
    /// 返回 false 表示图不可用，调用方退回直接 Blit。
    public bool Execute(CommandBuffer cmd, ScriptableRenderContext context, Camera camera,
                        FrameInputs inputs, RTHandle cameraColor);

    public string DumpChain();
    public void Dispose();     // 释放池
}
```

### 5.2 Pass 基类

```csharp
public abstract class PostPass
{
    public abstract string Name { get; }
    public virtual bool Enabled => true;

    /// 由 pipeline 赋值的颜色输入（无颜色输入的 pass 可为 default）。
    public RgTexture Input;
    /// 由 Declare 赋值的颜色输出。
    public RgTexture Output;

    /// 只声明资源，不录命令。可以在这里 g.Read(...) 额外输入。
    public abstract void Declare(PostGraphBuilder g);

    /// 录制命令。用 ctx.Get(handle) 解析成 RenderTexture。
    public abstract void Render(PostPassContext ctx);
}
```

### 5.3 执行上下文

```csharp
public struct PostPassContext
{
    public ScriptableRenderContext context;
    public CommandBuffer            cmd;
    public RTHandle                 cameraColor;
    public FrameInputs              inputs;
    public Camera                   camera;

    public RenderTexture Get(RgTexture t);

    /// 把已录的命令立刻交给 context 并清空 cmd。
    /// 任何要 context.DrawRenderers 的 pass 必须先调它（§6.2）。
    public void Flush();
}
```

### 5.4 三个参考实现（① 已实现；②③ 暂缓）

> v1 只实现 ①（`TonemapPass`）。②③ 是前向 pass 的参考实现，接口已经能覆盖，但**代码未写**（D2/D3/D4 推迟）。保留在这里是为了证明 API 不只能做全屏后处理。

**① Tonemap：典型后处理，一进一出**

```csharp
public sealed class TonemapPass : PostPass
{
    public override string Name => "Tonemap";
    readonly Material _mat;
    readonly TonemapSettings _s;

    public override void Declare(PostGraphBuilder g)
    {
        g.Read(Input);
        Output = g.Create(new RgTextureDesc
        {
            Name = "Tonemap.Out",
            Format = RgFormat.ColorRGBAHalf,   // 若输出 LDR 可换 ARGB32
            Filter = FilterMode.Bilinear,
        });
    }

    public override void Render(PostPassContext ctx)
    {
        var src = ctx.Get(Input);
        var dst = ctx.Get(Output);
        _mat.SetFloat("_Exposure", _s.exposure);
        ctx.cmd.Blit(src, dst, _mat, 0);       // 或 cmd.SetRenderTarget + DrawMesh
    }
}
```

**② ForwardOverlay：需要混入背景，所以链首要先 Copy 一次**

```csharp
public sealed class ForwardOverlayPass : PostPass
{
    public override string Name => "ForwardOverlay";
    readonly RgTexture _depth;          // 输出分辨率深度，构造时传入
    readonly ForwardSettings _s;

    public override void Declare(PostGraphBuilder g)
    {
        g.Read(_depth);                 // 深度只读
        Output = g.ReadWrite(Input);    // 原地混入：Id 不变、Version + 1
        // Input 必须已经由图内 CopyPass 拿到所有权；
        // 若直接传入导入的 DLSS 输出，ReadWrite 会被校验拒绝（§6.1 第 1 条）
    }

    public override void Render(in PostPassContext ctx)
    {
        ctx.cmd.SetRenderTarget(ctx.Get(Input), ctx.Get(_depth));
        // 不 Clear：背景色和 PT 深度都要保留
        ctx.Flush();                              // DrawRenderers 之前必须 Flush
        ctx.context.DrawRenderers(/* layerMask / 透明队列 / sorting */);
    }
}
```

**③ DepthFromGBuffer：消费低分辨率输入，产出输出分辨率深度**

```csharp
public sealed class DepthFromGBufferPass : PostPass
{
    public override string Name => "DepthFromGBuffer";
    readonly RgTexture _gb0;        // 渲染分辨率，只读；构造时传入
    readonly ComputeShader _cs;
    readonly int _k;

    public override void Declare(PostGraphBuilder g)
    {
        g.Read(_gb0);
        Output = g.Create(new RgTextureDesc
        {
            Name = "Depth.Full", Format = RgFormat.Depth32,
        });
    }

    public override void Render(in PostPassContext ctx)
    {
        // 先 compute 把 NDC 深度写进一张 R32F 临时图，再用 SV_Depth 全屏 pass 写 D32；
        // 或者直接写 depth 附件（见 §7.2 的两种做法）
    }
}
```

> 三个例子覆盖了"一进一出 / 原地混合 / 低分输入 → 高分输出"，这三类能长出后面绝大多数 pass。如果某个新 pass 套不进这三类，再加接口，不要提前设计。

### 5.5 组装链：`BuildChain`（决定 pass 顺序的唯一地方）

实际实现是 `PostGraphPipeline.Build`，以委托形式传给 `PostGraph`，**只在 Rebuild 时调用**；每帧执行不会重新 Declare。

v1 的全部内容就这几行（`PostGraphPipeline.cs`）：

```csharp
public static RgTexture Build(PostGraphBuilder g, FrameInputs inputs, Material tonemapMaterial)
{
    RgTexture color = g.Import(inputs.Color, "Input.Color");

    if (PostGraphConfig.TonemapEnabled && tonemapMaterial != null)
        color = g.Add(new TonemapPass(tonemapMaterial) { Input = color });

    // 以后按顺序插入：Bloom / FXAA / ForwardOverlay / OverlayUI ...

    g.Present(color);
    return color;
}
```

要点：

- **顺序就是代码顺序**，没有隐式重排；想调顺序就改这几行。
- tonemap 关掉时链退化为 `Import → Present`（一次拷贝），用于和旧 Blit 做 A/B。
- 新增效果 = 新 pass 类 + 在这里插一行 + `PostGraphConfig` 加一组静态字段，**不动 `PostGraph` 本体**。

参数**不序列化**，全部在 `PostGraphConfig`（静态类）：

| 字段 | 作用 |
|---|---|
| `Enabled` | 后置段总开关；false 时走旧 Blit |
| `TonemapEnabled` | 关掉则链退化为一次拷贝 |
| `TonemapExposure` | 线性域曝光倍率（曲线之前乘） |
| `TonemapMode` | 0 = ACES，1 = Reinhard，2 = 直接截断（对照） |

> 为什么不做序列化：`URP-PathTrace-Renderer.asset` 是共享的 Renderer 资产，后置段还在快速迭代，参数塞进去会不断膨胀。以后如果确实需要 Inspector 调参，建议单独建一个 `ScriptableObject` 配置资产（甚至用 `Resources` 加载），而不是往 Renderer 资产里加字段。

### 5.6 接线：只动 `Execute` 末尾

实际改动（`PathTraceRendererFeature.cs`）：

```csharp
// PathTraceRenderPass.Execute 末尾
// 判据必须与原来的 blit 分支完全一致：开了 DLSS + 初始化成功 + 辅助 shader 可用
bool useDlssOutput = _feature.dlssEnabled && _dlssInitialized && cs != null;
if (useDlssOutput)
{
    ... PrepareDLSSRRDispatch + cmd.IssuePluginEvent(...) ...
}

Present(cmd, context, camera, colorTarget,
        useDlssOutput ? _dlssOutputRT : _outputRT, jitter);
```

`Present` 内部：

1. `PostGraphConfig.Enabled == false` → `cmd.Blit(source, colorTarget)`（旧路径）；
2. 否则构造 `FrameInputs` 并 `_postGraph.Execute(...)`；
3. 图不可用（shader 缺失 / 校验失败 / 构建异常）→ 同样退回 `cmd.Blit`，**不会黑屏**。

`FrameInputs` 里 `Color` 的选取沿用同一判据；`_dlssOutputRT` 在 DLSS 未启用时是 `null`（`EnsureRTs` 只在 `dlssEnabled` 时创建）。

---

## 6. 校验与执行

### 6.1 构建期校验（8 条）

| # | 规则 | 失败后果 |
|---|---|---|
| 1 | 导入资源只读：`Write`/`SetColor` 指向 Imported → 报错 | 防止后置段写坏 DLSS 输出 |
| 2 | `Present` 恰好一次，且在所有 pass 之后 | 保证"一定上屏、只上屏一次" |
| 3 | `Read` 的版本必须已写（或 Imported） | 抓"读到没写过的图" |
| 4 | 同一版本最多一个写者（`ReadWrite` 的原地 pass 除外，需显式声明） | 抓"两个 pass 抢写" |
| 5 | 尺寸：`Fraction` 资源按 `round(w*f)` 解析；低分辨率 Import 不能当渲染目标 | 抓"DLSS 前后混用" |
| 6 | 格式：`RandomWrite` 必须是支持 UAV 的格式；深度附件必须是 `Depth32` | 抓"compute 写了不可写格式" |
| 7 | 写了但没人读 → warning（通常是忘了接链或 pass 没启用） | 抓"白算一遍" |
| 8 | pass 名字唯一 | Frame Debugger 可读 |

失败一律 `Debug.LogError`，并打印 **pass 名 / 资源名 / 版本号**。**不要静默跳过**——静默跳过正是现在最难查的一类 bug。`Compile()` 返回 null 时，`PostGraph.Execute` 返回 false，调用方自动退回直接 Blit（不黑屏）。

> **v1 实际实现的是第 1、2、3、4（部分）、8 条**：导入只读（`ReadWrite` 拒绝 Imported）、`Present` 一次、先写后读、同名 pass、非法句柄。尺寸/格式/UAV 校验和"写了没人读"的 warning 等后面加 compute/raster pass 时再补——v1 只有一个全屏 blit，没有这些错误面。

### 6.2 执行顺序与 CommandBuffer / Flush

执行器按注册顺序跑：

```
foreach pass:  pass.Render(ctx)      // 录命令；raster pass 内部自己 Flush
Present:       cmd.Blit(final, cameraColor)
```
v1 的 RT 只在 **Rebuild** 时向池子取/还；稳态每帧不分配、不释放（这是不重演 BUG-004 的关键）。

**两个必须记住的细节：**

1. `context.DrawRenderers` 是"立即写进 context"，不是写进 `cmd`。所以 raster pass 必须先 `Flush()`：
   ```csharp
   public void Flush()
   {
       context.ExecuteCommandBuffer(cmd);
       cmd.Clear();                    // ← 不清会重复执行！
   }
   ```
   URP 自己的 `ScriptableRenderer` 也是 `ExecuteCommandBuffer(cmd); cmd.Clear();` 这个写法。
2. `Flush()` 之后往 `cmd` 里补录的命令一定排在 `DrawRenderers` 之后。**任何"先录一半、Draw、再补录"的 pass 都要按 Flush 点重新核对顺序。**

### 6.3 编译一次、每帧执行

- 图的拓扑只在**配置变化**时重建：`(outW, outH, inputs.Color.width/height/format, PostGraphConfig.TonemapEnabled, inputs.Depth != null, inputs.GBuffer0 != null)`。
- 每帧只做"解析句柄 → 走 pass 列表 → 录命令"，C# 侧应在**几十微秒**量级。
- pass 数据用 `struct` + 缓存委托，避免每帧闭包 GC。
- 池按 desc 复用 RT，稳态下**没有任何 Create/Release**。

### 6.4 调试

- `DumpChain()`：资源（名字/版本/尺寸/格式/物理 RT 名）、pass（名字/开关/读写）、执行顺序，一条文本表；
- 每个 pass 包 `ProfilingSampler`，Frame Debugger / Profiler 可见；
- 校验失败时打印 pass 名 + 资源名 + 版本；
- 提供一个"只跑某个 pass 并直接上屏"的 debug 开关（排查"到底哪一步坏了"）。

---

## 7. 前向 pass 的落地约束（v1 暂缓，保留给后续）

> **本节暂不实现。** 保留是因为以后要加前向时，坑都在这里：几何排除、深度、光照一致性。v1 的图接口（`ReadWrite`、`FrameInputs.Depth`、`RgSize.Fraction`）已经为这些留好位置。

### 7.0 先说清楚"overlay"指什么

本文的 **overlay** 不是 UGUI Canvas、不是 Unity 的 Overlay Camera、也不是屏幕空间后处理。它指的是：

> **画在 PT 成片之上的、不参与 PT 光照 / 阴影 / GI / 反射的内容层。**

判断标准只有一句：**这个东西如果不跟随 PT 场景的光照，观众会不会觉得假？**

| | overlay（D2-A） | 场景内前向物体（D2-B） |
|---|---|---|
| 典型内容 | 自发光粒子/火花、光晕、刀光拖尾、选中描边、调试线框、准星/标记、体积光柱 | 需要和场景一样被照亮、被遮挡、投射/接收阴影的半透明物体、动态角色 |
| 光照来源 | 自发光 / 手调常量 / 环境贴图近似 | 必须与 PT 的直接光、间接光、阴影一致 |
| 阴影 | 不投不收 | 要收 PT 阴影、最好也投到 PT 场景 |
| 进入 PT 的 TLAS | 不进入 | 通常要进入（否则 PT 里没有它，反射/阴影对不上） |
| 主要难点 | 投影正确、排序、深度遮挡 | 光照一致性——PT 的阴影/间接光不在 raster 可用的数据里，只能近似 |
| 工作量 | 小（一个透明队列 pass + 一个 Unlit shader 族） | 大（近似光照方案 + 与 PT 的材质/曝光对齐 + 反复调） |

所以 D2 的两条路本质是：

- **A**：前向 pass 只负责"把世界空间的东西投影正确、排序正确、被 PT 几何正确遮挡"，**不负责光照一致**；
- **B**：前向 pass 还要负责"和路径追踪的结果看起来是同一个光照环境"——这是渲染质量工程，不是 pass 基础设施。

本项目 PT 已经支持薄壁/体积透明材质（见 `doc/PT半透明材质渲染流程.md`），所以"场景里的透明物体"优先走 PT，而不是走 overlay。overlay 的定位是 **PT 不擅长/不需要的东西**。

### 7.1 几何必须排除，否则双重渲染

`ModelManager` 里目前 grep 不到任何 layer/exclude 过滤逻辑，TLAS 收所有 Renderer。前向 pass 再画一遍同一批物体 = 双重渲染（变暗/闪烁）。

**必须先选一种排除机制**：

- 专用 layer（如 `ForwardOnly`）：`ModelManager` 收集时跳过该 layer；前向 pass 用同一个 layer 的 `layerMask`；
- 或 `PathTraceIgnore` 组件：收集时过滤；
- 方向反过来也行（PT 只收 `PTScene` layer），但要改现有场景里所有物体的 layer，成本更高。

### 7.2 深度从哪来

PT 是光线求交，**没有光栅深度**；`m_RequireDepthTexture: 0`，`_CameraDepthTexture` 不存在。所以前向 pass 的 `ZTest` 只能对"我们自己重建的深度"做：

- **现在**：`DepthFromGBuffer` 读 `gb0`（worldPos + 命中距离）→ `clip = mul(ViewProj, float4(worldPos,1))` → `clip.z / clip.w`（Unity reversed-Z：near=1、far=0；miss 写 0）；
- **升采样**：输出分辨率深度不能只 bilinear（前景会糊到背景）；用 **3×3 取更远者**（reversed-Z 下取 `min`），代价是前景边缘漏背景，对软粒子可接受；
- **写 D32 的两种做法**：
  a) compute 写一张 `R32F`，再用全屏 pass 以 `SV_Depth` 写进 D32（多一张图、多一个 pass）；
  b) 直接在 fragment 里算深度并 `SV_Depth` 输出到 D32（少一张图，但需要全屏 pass 自己算）。
  先做 (a)，简单、好调。
- **将来**：native 直接输出深度 → `FrameInputs.Depth` 填上，`DepthFromGBuffer` 自动禁用，后置链一行不改。

### 7.3 排序、混合与 MSAA

- `FilteringSettings`：`RenderQueueRange.transparent` + `SortingCriteria.CommonTransparent`；
- `ZTest LEqual`、`ZWrite Off`，混合用 `SrcAlpha OneMinusSrcAlpha`（或预乘，按素材定）；
- **不开 MSAA**：PT/DLSS 输出是 1x，前向开 MSAA 需要额外的 resolve，且深度附件格式/采样数要单独管；先用 FXAA/后处理抗锯齿；
- `context.SetupCameraProperties(camera)` 在 `DrawRenderers` 之前调一次，确保 view/proj 与相机一致（**待实测**：URP 14 在 600 注点是否已经设好）。

### 7.4 光照不可能和 PT 完全一致（要提前接受）

前向物体走 URP 的光照，而 PT 的光照/阴影是路径追踪算出来的：PT 的阴影不会出现在 shadow map 里，前向物体**拿不到 PT 的阴影**，直接光强度/环境光也可能对不上。

**结论：前向 pass 先只用于"不需要与 PT 光照严格一致"的东西**——自发光粒子、UI、调试几何、描边/选中效果。如果要做"与场景光照一致的半透明物体"，那是另一个量级的工作（要么进 PT，要么手动近似光照），不要指望图的引入能解决它。

### 7.5 位置：DLSS 之后、tonemap 之前

- 不能在 DLSS 之前：RR 的输入契约是"PT 的 gbuffer + 颜色"，光栅化的半透明会让网络看到没训练过的分布（同 `doc/DLSS_RR亮区抖动分析.md` 的"输入分布错配"）；
- 应在 tonemap 之前：前向物体是 HDR 线性光，和 PT 结果在同一个线性空间里混合。

---

## 8. 后处理 pass 的落地约束

| 项 | 规则 |
|---|---|
| 色彩空间 | 工程是 **Linear**（`m_ActiveColorSpace: 1`）；PT 输出线性 radiance，**当前链路没有 tonemap**，>1 高光直接截断。是否加 tonemap 见 §0 D1（默认关闭） |
| tonemap 位置 | 若启用，放在 FXAA 之前、`Present` 之前：`Bloom（HDR）→ Tonemap → FXAA（LDR）→ Present`；不启用时 FXAA 直接在线性 HDR 上做（阈值要按 HDR 调） |
| 中间格式 | `ARGBHalf`；tonemap 之后可以降到 `ARGB32`（LDR），但 `Present` 到 cameraColor 时由 URP 的格式决定 |
| 与 URP 后处理的关系 | `PathTraceCameraGate` 已经把 `renderPostProcessing = false`，URP 的 Bloom/Tonemap 不会和这里打架；不要重新打开 |
| Bloom 的 mip 链 | 用 `RgSize.Fraction`（1/2、1/4、1/8），每级一个 pass；这是 `Fraction` 存在的唯一理由 |
| sRGB 编码 | **tonemap 只输出线性值**，让 sRGB backbuffer 做编码；不要在 tonemap shader 里手动 `pow(1/2.2)`，否则和 URP 的颜色空间转换叠加，画面发灰/过亮 |
| 不要在 pass 里 `cmd.Blit` 到 cameraColor | 上屏只允许 `Present` 一次，否则顺序/格式失控 |

---

## 9. 新增一个 pass 的步骤（给后续实现者的 checklist）

1. 新建 `XxxPass : PostPass`，`Name` 唯一；
2. 构造函数缓存 `Material` / `ComputeShader` / kernel（**不要每帧 `new Material`**）；
3. `Declare`：`g.Read(Input)` + `g.Create(desc)` 得到 `Output`；需要额外输入就再 `g.Read(...)`；
4. `Render`：`ctx.Get(handle)` 取 RT；compute 用 `ctx.cmd.DispatchCompute`；raster 用 `SetRenderTarget` + `Flush` + `DrawRenderers`；全屏用 `ctx.cmd.Blit`；
5. 在 `BuildChain` 里按顺序插一行，指定 `Input`；
6. 加 Inspector 开关（`Enabled` + 序列化设置）；
7. 跑一次 `DumpChain()` + Frame Debugger，确认：尺寸对、格式对、名字在、顺序对；
8. 验收：关掉该 pass 时画面回到上一版（版本回退语义自动生效）。

**不要做的事**：不要在 pass 里自己 `new RenderTexture`；不要写导入的 RT；不要依赖"上一个 pass 的字段"（只依赖 `Input` 句柄）。

---

## 10. 实现进度与工作量

v1（图核心 + tonemap）已完成，实际约 **2~3 人日**（专注）。

| 项 | 状态 | 落点 |
|---|---|---|
| 图核心：Import/Create/Read/ReadWrite/Add/Present + 校验 + 池 + Rebuild/Execute | ✅ 已完成 | `PostGraph.cs` / `PostGraphBuilder.cs` / `PostGraphPool.cs` |
| Tonemap pass + shader（ACES / Reinhard / Clip） | ✅ 已完成 | `Passes/TonemapPass.cs` / `Assets/Shaders/PostTonemap.shader` |
| 接线：Present + 回退 + Dispose | ✅ 已完成 | `PathTraceRendererFeature.cs` |
| `DumpChain` 调试输出 | ✅ 基础版 | `PostGraph.DumpChain()` |
| 前向 overlay / 几何排除 / 深度 | ⏸ 推迟 | 设计见 §7；接口已预留（`FrameInputs.Depth` / `ReadWrite` / `RgSize.Fraction`） |
| Bloom / FXAA / 调色 | ⏸ 推迟 | 设计见 §8 |

实际代码规模（v1）：

```
Assets/Scripts/Rendering/PostGraph/
├── RgTypes.cs              ~60 行
├── FrameInputs.cs          ~35 行
├── PostPass.cs             ~55 行
├── PostGraphConfig.cs      ~25 行
├── PostGraphBuilder.cs     ~210 行
├── PostGraphPool.cs        ~95 行
├── PostGraph.cs            ~230 行
├── PostGraphPipeline.cs    ~30 行
└── Passes/TonemapPass.cs   ~50 行
Assets/Shaders/PostTonemap.shader   ~55 行
```

---

## 11. 验收标准

1. **一键回退**：`PostGraphConfig.Enabled = false` 时与改动前逐像素一致（走旧 `cmd.Blit`）；
2. **tonemap 关闭等价**：`PostGraphConfig.Enabled = true` 且 `PostGraphConfig.TonemapEnabled = false` 时，链退化为 `Import → Present`，与旧路径逐像素一致；
3. **tonemap 开启有预期观感**：高光滚降、无黑屏、无闪烁；`tonemapExposure` 可调；
4. **校验 0 错误**：Console 里没有 `[PostGraph]` 开头的报错；`DumpChain()` 打印的资源/pass 与预期一致；
5. **RT 不 churn**：稳态下（不切分辨率/不切 DLSS）没有新的 RT 分配；
6. **C# 开销**：图部分 < 0.05 ms（v1 只有一次 blit + 三个 uniform）；
7. **切换稳定**：切 DLSS 开关、质量模式、tonemap 开关，不黑屏、不报错；
8. **30 分钟 soak**：无 `Resource ID out of range`、无验证层 layout 报错。

### 待你实机验证的清单

- [ ] Game 视图对比 `PostGraphConfig.Enabled` 开/关，确认回退路径正常；
- [ ] 固定场景，扫 `tonemapExposure`（1.0 起上下）与 `tonemapMode`，定下默认值；
- [ ] 切 DLSS 开/关与三种质量模式，确认画面不黑；
- [ ] 看 Console 是否有 `[PostGraph]` 报错；
- [ ] 出包前确认 `Assets/Shaders/PostTonemap.shader` 仍在 Graphics Settings 的 Always Included Shaders 里（已加）。

---

## 12. 风险清单（v1）

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| 1 | ACES 让现有观感变暗/对比变强，曝光不对 | **高** | 画面偏暗/发灰 | 用 `PostGraphConfig.Enabled` / `TonemapEnabled` 做 A/B；`tonemapExposure` 手调后再定默认 |
| 2 | shader 被移出 Always Included / 改名 | 低 | 后置段静默回退（画面无 tonemap） | 保持在 `Assets/Shaders/PostTonemap.shader`；Console 有 warning |
| 3 | 图校验失败 → 静默回退 | 低 | "没效果"但不黑 | Console 有 `[PostGraph]` 报错；`DumpChain()` 可查 |
| 4 | RT 每帧重建（重演 BUG-004） | 低 | 长跑黑屏 | 池只在 Rebuild 时取/还；稳态无分配 |
| 5 | tonemap 输出用 ARGBHalf，带宽略高 | 低 | 多一点带宽 | 需要时可改 `ARGB32`（值已在 [0,1]） |
| 6 | 以后加 pass 时版本/ReadWrite 语义用错 | 低 | 校验报错/花屏 | 校验先拦；文档 §4.3 / §6.1 已写 |

---

## 附录 A：前置段关键源码位置

> 行号会随改动漂移，优先按函数名定位。

| 内容 | 位置 |
|---|---|
| PT/DLSS pass 主体 | `Assets/Scripts/PathTraceRendererFeature.cs` 的 `PathTraceRenderPass`（`:291` 起） |
| 末尾 Present 调用（唯一改动点） | `PathTraceRendererFeature.cs:608`（`Execute` 末尾） |
| RT 创建/释放（不要动） | `PathTraceRendererFeature.cs` 的 `EnsureRTs` / `ReleaseRTs`（`:827` / `:892`） |
| 注入点选择的原因 | `PathTraceRendererFeature.cs:161-170` |
| 相机抑制（cullingMask=0 / 后处理关闭） | `Assets/Scripts/PathTraceCameraGate.cs:96-103` |
| 相机颜色目标 | `PathTraceRendererFeature.cs:512` |
| GBuffer 约定（worldPos + hit distance） | `Assets/Shaders/DlssAux.compute` 的 `MotionVector` kernel |
| 工程色彩空间 | `ProjectSettings/ProjectSettings.asset:50`（`m_ActiveColorSpace: 1`） |

## 附录 B：原生 RenderGraph 证据（供追溯）

### B.1 2022.3 里是一段没编进去的死代码

- URP 14.0.12 包内有 `UniversalRendererRenderGraph.cs`、`UniversalRenderPipelineRenderGraph.cs`、`ScriptableRenderPass.RecordRenderGraph`，但整条路径被 `#if RENDER_GRAPH_ENABLED` 包住（`Runtime/UniversalRenderPipeline.cs:304-310`）。
- 本工程 URP 运行时的实际编译参数 `Library/Bee/artifacts/1900b0aE.dag/Unity.RenderPipelines.Universal.Runtime.rsp` 共 **125 个 `-define:`，没有 `RENDER_GRAPH_ENABLED`**；包内无 `#define`，asmdef 的 `versionDefines` 也没有。
- `enableRenderGraph` 是 `internal`（`Runtime/Data/UniversalRenderPipelineAsset.cs:1443-1446`），Inspector 开关也藏在同一个宏后面。

### B.2 就算打开，自定义 pass 也进不去

- `ScriptableRenderPass.RecordRenderGraph` 是 `internal virtual`（`Runtime/Passes/ScriptableRenderPass.cs:681`），工程侧无法 override，基类只打一条 warning；
- 图路径只在 `BeforeRenderingPrePasses` 一个注点记录自定义 pass（`UniversalRendererRenderGraph.cs:292`），`AfterOpaque` 那行仍是注释（`:350`）；
- core RP 14 的 `RenderGraph` 的 `InternalsVisibleTo` 只开给 HDRP 与 Core 编辑器/测试，`RenderGraphResourceRegistry` 是 `internal`。

### B.3 资源模型与 native 插件相斥

- 插件用 `kUnityVulkanResourceAccess_ObserveOnly` 拿图，layout 由插件自己记账（`NativePlugin/src/vpt_dispatch.cpp:441-466, 533-537`），Unity 的 RTHandle 账本与它不是同一份；
- 原生 RG 的 transient/复用会让 `VkImage` 在帧间轮换，插件的 ImageView 缓存（按 `VkImage` 键）反复失效 —— 正是 BUG-002/004 的诱因；
- 所以即便升级 Unity 6，所有 native 图也只能 `ImportTexture` 成持久资源，RG 的 aliasing 收益归零。

## 附录 C：术语

| 术语 | 含义 |
|---|---|
| 前置段 | PT + MotionVector + DLSS RR + 现有资源管理，本文不改 |
| 后置段 | DLSS 输出之后的自定义 pass 链，本文定义的对象 |
| Imported | 外部传入的只读 RT（DLSS 输出、gbuffer、未来的 native 深度） |
| 版本（Version） | 资源被第几次写入；读写依赖与可选 pass 回退都靠它 |
| 持久池 | 同 desc 的 RT 跨帧复用同一个对象，避免 `VkImage` churn |
| Present | 把最终颜色写到 `cameraColor` 的唯一出口 |
