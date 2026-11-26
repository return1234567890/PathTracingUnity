# Bug 追踪

记录**已知但暂不修 / 待观察 / 已确认属既有问题**的缺陷，避免它们在后续改动里被反复"重新发现"。
修复后把状态改成「已修」并保留条目，方便回溯当时为什么这么改。

## 编号与字段约定

| 字段 | 取值 |
|---|---|
| 级别 | **P0** 崩溃 / 静默错误结果；**P1** 资源泄漏、性能退化、可观测的功能异常；**P2** 潜在隐患（当前不可达或影响可忽略） |
| 状态 | 待修 / 修复中 / 已修 / 不修（必须写原因） |
| 引入版本 | 该问题第一次出现的提交或改动主题，用来判断"是不是这次改坏的" |

---

## BUG-001 `s_DispatchPending` 等标志跨线程读写未加原子性

| 字段 | 内容 |
|---|---|
| 级别 | P2 |
| 状态 | **待修（暂缓）** |
| 引入版本 | 既有问题：Plan B 两阶段 dispatch 引入时就是裸 `bool`，非本次改动引入 |
| 位置 | `NativePlugin/src/vpt_dispatch.cpp:350`（定义）、`:623-625`（`PrepareDispatch`，主线程写）、`:664`/`:673`（`RenderCallback`，渲染线程读写）、`:359`（`RequestPause`，主线程写）、`:880`（`IsDispatchPending`，任意线程读） |
| 首次记录 | 2026-11-15 代码审查 |

### 现象

`static bool s_DispatchPending` 是 C# 主线程（`VPT_PrepareDispatch`）与 Unity 渲染线程（`VPT_RenderCallback`，在 Unity 命令缓冲执行期被调用）之间传递「本帧有 dispatch 待录制」的标志，但它是**非原子的裸 `bool`**，两侧都在写，构成 C++ 数据竞争（未定义行为）。

同一段落里 `s_PendingWidth` / `s_PendingHeight`（`:348-349`）也是同一模式：主线程在 `:623-624` 写，渲染线程在 `:855-856` 读。

### 根因

Plan B 两阶段 dispatch 引入该标志时，周边状态全是裸变量、也没上锁。本次（2026-11-15）修复卡死问题时，把相邻的 `s_PauseRequested` / `s_Paused` 换成了 `std::atomic<bool>`，但漏掉了这一个 —— 属于"改造时漏网"，不是新引入的错误。

### 影响（为什么是 P2 而不是 P1）

- 在 x86-64 上 `bool` 读写本身不会撕裂，最坏情况是：
  - 渲染线程读到旧值 `false` → 丢一帧 dispatch（表现为偶发一帧不刷新）；
  - 或主线程写 `true` 的同时渲染线程写 `false` → 该帧的 dispatch 被吞掉。
- 以上都是"少画一帧"，肉眼几乎不可见，不会崩溃、不会累积错误状态。
- **但它仍是 UB**：编译器的可见性重排、循环内提升（把读提到循环外）、或未来加分支合并优化，都可能让它变成真正的间歇性 bug，届时极难复现定位。
- `s_PendingWidth/Height` 与之同理：若读到半更新状态（宽度是新的、高度是旧的）会让 dispatch 尺寸错一帧，`.x`/`.y` 分派数错位 → 部分像素不更新；概率极小但同样属于 UB。

### 建议修法

```cpp
static std::atomic<bool>    s_DispatchPending{ false };
static std::atomic<int32_t> s_PendingWidth{ 0 };
static std::atomic<int32_t> s_PendingHeight{ 0 };

// 写：s_DispatchPending.store(true, std::memory_order_relaxed);
// 读：if (!s_DispatchPending.load(std::memory_order_relaxed)) return;
```

用 `relaxed` 即可：这些只是单标志位/标量快照，不承担内存序同步职责（真正的 GPU 资源可见性由命令缓冲里的 barrier 保证，与此无关）。

若想更严格，可以把「待处理状态」收进一把 `std::mutex`（与 `vpt_scene` / `vpt_env` / `vpt_dlss` 现在的做法一致），但 `RenderCallback` 里持锁时间较长，`atomic` 更合适。

### 为什么暂缓

1. **非本次改动引入**：混进「卡死修复」的提交，会让 diff 里出现与本议题无关的并发语义改动，审查成本上升，且一旦后续出问题难以二分；
2. **需要重建 DLL**：本次批次刻意保持 native 二进制不动（`Assets/Plugins/x86_64/VulkanPathTracePlugin.dll` 与已核对的源码/SPV 一致），改它就得重新构建、重新验证二进制；
3. **收益低**：当前影响等价于「偶发少画一帧」。

建议单独立一个 commit 处理（可与同文件其它线程安全清理合并），提交信息写明「纯并发语义修正，无行为变化」。

---

## BUG-002 重建 RT 时立即销毁旧 ImageView（延迟销毁未做）

| 字段 | 内容 |
|---|---|
| 级别 | P2（残余：只在「RT 重建的那一帧」可能触发；稳定运行时不会） |
| 状态 | **部分已修**：descriptor 改写部分已用「描述符集环形缓冲」修掉（验证层 `VUID-vkUpdateDescriptorSets-None-03047` 已消失）；**ImageView 延迟销毁仍待做** |
| 引入版本 | 既有问题：Plan B 两阶段 dispatch 引入 `EnsureImageView` + 单 descriptor set 时就是这个形态，**不是 RT pipeline 切换引入的** |
| 位置 | `NativePlugin/src/vpt_dispatch.cpp`：`EnsureImageView`（句柄变化时 `vkDestroyImageView` 立即销毁）、`EnsureTextureArrayView`（同模式） |
| 首次记录 | 2026-11-22 查 G3 通关日志时发现（当时对现象的判断有误，见文末"历史误判"） |

### 现在的现象（2026-11-22 修正后）

修掉 BUG-004（RT 每帧重建）之后，`view recreate /120 frames` 长期是 `out=0,0,0,0,0`：
**稳定运行时根本不重建 view**。只有三个时刻各重建一次：

- 进 Play 首帧（RT 首次创建）
- 切分辨率（RT 重建）
- 切 DLSS 开关（RT 重建）

### 残余风险

在建的那一帧里，旧 ImageView 被**立即**销毁，而前 1~2 帧的命令缓冲理论上仍在飞
（Unity 最多 ~3 帧在飞行）。规范上属于「销毁仍可能被 pending 命令缓冲引用的对象」。

- NVIDIA 驱动实践上容忍（销毁 view 不释放底层 image）；
- 验证层这三轮（VVL 全开）**没有**报 `vkDestroyImageView` 相关 VUID；
- 但换驱动/换硬件时，这是潜在的随机崩溃/花屏点，所以保留 P2 不关。

判断是否处于健康状态：看 `view recreate /120 frames` 的 `out=` 是否长期 `0,0,0,0,0`。

### 历史误判（留作教训，勿再复用）

本条最初写的现象是「Unity 对同一张 RenderTexture 每帧返回不同的 VkImage → 每帧重建 view」，
还统计出「16,890 次 ≈ 100% 命中」。**这个因果判断是错的**：

> 真相是 C# 侧 `EnsureRTs`/`ReleaseRTs` 的顺序 bug 让 RT **每帧重建**（BUG-004），
> 新建的 RT 自然带新的 VkImage。修掉 BUG-004 后 `out=0` 就是反证。

教训：看到「输入每帧都变」时，先查**上游是不是在每帧新建对象**，别急着归因到引擎内部。

### 建议修法

两条都要做，缺一不可（只做其中一条仍会残留另一条）：

1. ~~**descriptor ring**~~ ✅ **已做（2026-11-22）**：`s_PathDescSets[4]` 环形缓冲，
   `PrepareDispatch` 只用「本帧这一套」改写、`RenderCallback` 绑同一套；上一/上上帧那几套
   保持不动。验证层实测 `VUID-vkUpdateDescriptorSets-None-03047` 已消失。
2. **ImageView 延迟销毁**：`EnsureImageView` 不再立即 `vkDestroyImageView`，把旧 view 推进一个 retired 列表并打上帧号，
   在 `PrepareDispatch` 开头销毁「帧号 < 当前帧 - N」的条目。纹理数组的 view（`EnsureTextureArrayView`）同理。
   （`[RT]` 诊断确认 RT 现在只在进 Play / 切分辨率 / 切 DLSS 时重建，触发频率已从「每帧」降到「几次」，故优先级下降。）

不要用「每帧 `vkDeviceWaitIdle`」这种偷懒解法：那是把已经消灭的 `vkQueueWaitIdle` 又请回来了（见 Plan B 的初衷）。

### 为什么只修了一半

- **descriptor ring 已修**：验证层把它判为真实规范违背（每次 Play 都报），属于「不修就是明确的错」，
  所以当批就做了，并复跑验证层确认消失。
- **ImageView 延迟销毁暂缓**：它需要引入 retired 列表 + 帧号，改动落在资源生命周期上，
  而 BUG-004 修掉后触发频率已经从「每帧」降到「进 Play / 切分辨率 / 切 DLSS 各一次」，
  验证层也没报 `vkDestroyImageView`，收益/风险比暂时不划算。等出现随机崩溃/花屏时优先查这里。

---

## BUG-003 NGX 把日志写进 `Assets/`，触发 AssetDatabase 刷新（卫生问题）

| 字段 | 内容 |
|---|---|
| 级别 | **P2**（会让编辑器反复做资源刷新/重载；**不是黑屏主因**，主因已定位为 BUG-004） |
| 状态 | **已修（2026-11-22）**：NGX 文件日志默认关闭，`nvngx*.log` 不再增长 |
| 引入版本 | 既有问题：`NVSDK_NGX_VULKAN_Init` 时把 `dataPath` 指向 `Assets/Plugins/x86_64`，并且一直开着 `NVSDK_NGX_LOGGING_LEVEL_VERBOSE` |
| 位置 | `NativePlugin/src/vpt_dlss.cpp`（`NgxLogCallback` + `NVSDK_NGX_VULKAN_Init` 调用处的 `NVSDK_NGX_LoggingInfo`） |
| 首次记录 | 2026-11-22 排查挂机黑屏时发现（当时误判为黑屏主因） |

### 现象（真实存在的部分）

NGX 初始化/评估时会往 `dataPath`（也就是 Unity 工程里的 `Assets/Plugins/x86_64/`）写：

```
nvngx.log / nvngx_dlssd_310_7_0.log / nvngx_dlss_310_7_0.log / nvsdk_ngx.log
```

Unity 监视 `Assets/`，于是 `Editor.log` 里出现：

```
Start importing Assets/Plugins/x86_64/nvngx_dlssd_310_7_0.log using Guid(...)
Refreshing native plugins compatible for Editor ...
```

即**每次 NGX 写日志都会触发一次 AssetDatabase 刷新 + 原生插件重载检查**。这本身是"脏"的：
刷新会重新反序列化 URP renderer 资产、短暂让 `dlssEnabled` 等字段回落默认值
（同一时刻的 `Feature.Create: pass created, dlssEnabled=False` 就是证据）。

### 更正：它不是黑屏主因

当时把上面这条链与本条目的标题（资源刷新 → renderer 重载 → RT 每帧重建 → ID 耗尽）连起来，
判定为黑屏主因。**这个因果链是错的**：

- 关掉 NGX 文件日志后（`nvngx*.log` mtime 不再变化，已复核），**黑屏照旧复现**；
- 编辑器里 `Feature.Create` 每 ~5 秒出现一次是**正常现象**（GameView 失焦时重建 renderer），
  在一次完全健康的会话里同样存在；
- 真正的原因是 C# 侧顺序 bug 每帧重建 RT（**BUG-004**），与 NGX 日志无关。

### 修复

`vpt_dlss.cpp`：初始化 NGX 时**默认关闭它的文件日志**。

```cpp
static void NVSDK_CONV NgxLogCallback(...) { /* 默认丢弃，VPT_NGX_LOG=1 时转发 stderr */ }

featureInfo.LoggingInfo.LoggingCallback            = NgxLogCallback;   // 关 sink 时回调必须非空
featureInfo.LoggingInfo.DisableOtherLoggingSinks   = true;             // 唯一能真正关掉文件写的开关
featureInfo.LoggingInfo.MinimumLoggingLevel        = NVSDK_NGX_LOGGING_LEVEL_OFF;
```

- `MinimumLoggingLevel = OFF` 单独用**不够**：SDK 文档说明它只能"抬高"阈值，不能压掉已配置的级别；
  真正生效的是 `DisableOtherLoggingSinks`（前提是 `LoggingCallback` 非空）。
- 能力查询用的 `featureInfoDiag` 同样关掉，避免每次 `FeatureDiscovery` 都写一次文件。
- 排障需要看 NGX 内部细节时：`set VPT_NGX_LOG=1` 再启动 Editor，日志会转发到 stderr（Editor.log）。

### 为什么这样修而不是「把 dataPath 挪出 Assets」

NGX Core 要靠 `PathListInfo`（当前就是 `Assets/Plugins/x86_64`）去加载 `nvngx_dlssd.dll`，
而 CMake 的 POST_BUILD 就是把这些 DLL 拷到 Unity 的 `Assets/Plugins/x86_64`（Unity 侧也要能看到
`nvngx*.dll`）。挪目录要同时改 CMake、`.meta`、NGX 路径与打包规则，收益只有"日志文件不落在 Assets"，
代价与风险都更大。关掉日志足以消除触发源。

### 回归判断依据

- `Assets/Plugins/x86_64/` 下 `nvngx*.log` / `nvsdk_ngx*.log` 的 mtime 不应再随运行时间变化；
- `Editor.log` 里不应再出现成片的 `Start importing Assets/Plugins/x86_64/nvngx*.log`。


---

## BUG-004 每帧重建全部 RenderTexture → resource ID 耗尽 → `GetResource` 越界刷屏 → 黑屏

| 字段 | 内容 |
|---|---|
| 级别 | **P0（黑屏 + 编辑器卡死）** |
| 状态 | **已修（2026-11-22，根因是 C# 侧一行顺序问题）** |
| 引入版本 | 既有问题（`git show HEAD` 里 `EnsureRTs` 就已经是"先记状态再 ReleaseRTs"的顺序），**与 RT pipeline 切换无关** |
| 位置 | `Assets/Scripts/PathTraceRendererFeature.cs`（`EnsureRTs` / `ReleaseRTs`） |
| 首次记录 | 2026-11-22 |

### 现象

Play 若干分钟（实测 5~6 分钟后开始加剧）后：

```
Resource ID out of range in GetResource: 1048601 (max is 1048575)
```

这条错误每帧刷几十~几百次，`Editor.log` 涨到 **250 MB**，帧率从 200 fps 一路掉到个位数，
最终渲染线程不再推进（`WATCHDOG … 最后阶段 = idle`，帧号停住）→ 黑屏、编辑器必须强杀。

### 已确认的事实（两次复现都有）

1. `1048575 = 2^20 - 1`：Unity 的 resource ID 是 20 bit 的环形表，**用完不会回收**（Web 上大量
   Unity 官方论坛案例，典型诱因都是 RenderTexture 释放方式不对 / 每帧新建资源）。
2. 插件侧每次查询都"命中换图"分支：`view recreate /120 frames: out=120,120,120,120,120`
   ——即**每帧** 5 张输出图各拿到一个新的 `VkImage`。
3. `texArray=0,0,0,0`：4 张纹理数组的句柄一直是稳定的。
4. ~~C# 侧没有每帧重建 RenderTexture~~ —— **这条推断当时是错的**：当时用
   "`Execute: DLSS reinit begin` 整场只有 1 次"来排除 RT 重建，但 RT 重建并不会触发
   DLSS 重初始化（`_dlssInitialized` 保持 true、尺寸没变），所以这个判据无效。
   真相见下面"真正的根因"。
5. NGX 日志已确认不再写入 `Assets/`（BUG-003 已修），黑屏依旧 → 不是它。

⇒ 也就是说：**只要插件每帧去查这 5 张图，Unity 就会为每次查询产出一个新的 backing image
记录，这些记录在长时间运行后把 20 bit 的 ID 表耗尽。**

### 真正的根因（2026-11-22 用 `[RT]` 诊断日志抓到）

`PathTraceRendererFeature.cs` 的 `EnsureRTs()`：

```csharp
if (_cachedW == w && _cachedH == h && _outputRT != null && !dlssChanged && !outResChanged)
    return;

_prevDlssEnabled = _feature.dlssEnabled;   // ← 先把状态记成 true
ReleaseRTs();                              // ← 它结尾又 _prevDlssEnabled = false;（BUG）
...新建 7 张 RT...
```

`ReleaseRTs()` 末尾有一句 `_prevDlssEnabled = false;`，于是**每次重建之后**标志都被复位成
`false`，而 `_feature.dlssEnabled` 是 `true` ⇒ 下一帧 `dlssChanged` 又是 `true` ⇒ 再重建 ⇒
**只要 DLSS 开着就每帧重建全部 7 张 RenderTexture，永远停不下来**。

抓到的日志（`[RT]` 是本次临时加的诊断）：

```
[RT] recreate #1920: w=1129x635 out=1920x1080 cached=1129x635/1920x1080
                   dlssChanged=True outResChanged=False rtNull=False
[RT] ReleaseRTs #1920
[PTR] ptrCache: newObject=23009 age=24 null=0
```

- `cached=` 与当前尺寸一致、`outResChanged=False`、`rtNull=False` ⇒ 唯一触发条件就是
  `dlssChanged=True`（且每帧都是 True）。
- `[PTR] newObject` 每 120 帧涨 1440 = 12 张 RT × 120 帧 ⇒ 所有 RT 对象每帧都是新的。

**这条链把前面所有现象都解释了**：RT 每帧重建 → 插件每帧拿到新贴图对象/新 VkImage →
每帧重建 ImageView（`out=120`）→ 7 张 RT/帧持续消耗 Unity resource ID → ~6.6 万帧耗尽 →
`GetResource` 越界刷屏 → 主循环被拖死。也解释了为什么"挂久了才黑"。

这是**既有 bug**：`git show HEAD` 里就是这个顺序，与 RT pipeline 切换无关。

### 修复

两处一起改（`PathTraceRendererFeature.cs`）：

1. `ReleaseRTs()` 删掉 `_prevDlssEnabled = false;` —— 释放 RT 不代表 DLSS 开关变了；
2. `EnsureRTs()` 把 `_prevDlssEnabled = _feature.dlssEnabled;` **挪到 `ReleaseRTs()` 之后**，
   避免以后再被任何复位动作踩到。

DLSS 开关的检测逻辑本身不变（`_prevDlssEnabled != _feature.dlssEnabled`）。

### 保留的缓解措施（仍然有用，不是根因修复）

排查过程中加的两层"少打扰 Unity"的机制保留下来，它们把稳态下的资源查询压到接近 0：

1. **native 侧 `QueryThrottle`**（`vpt_dispatch.cpp`）：`nativeTex` 不变 + 已有 view 时跳过
   `AccessTexture`，按 1→600 帧自适应校验；发现换图立刻退回逐帧。
2. **C# 侧 `CachedPtr`**（`NativeBridge.cs`）：按贴图对象缓存 `GetNativeTexturePtr()`，
   对象变化或 300 帧到期才重取；`VPT_NO_PTR_CACHE=1` 可关闭。
3. 诊断：`view recreate /120 frames … ptrChanged … queryInterval`（native）、
   `[RT] recreate/ReleaseRTs`、`[PTR] ptrCache`（C#）。

### 回归判断依据

正常状态下应该看到：

```
[RT] recreate #1 ...（只在进入 Play / 切分辨率 / 切 DLSS 时出现）
[PTR] ptrCache: newObject≈12 age 缓慢增长        ← 对象不再每帧新建
view recreate /120 frames: out=0,0,0,0,0 ... | ptrChanged out=0,...
```

- `view recreate` 的 `out=` 长期 `0`；
- `Editor.log` 不再出现 `Resource ID out of range`；
- 帧率应比修复前**明显更高**（少掉每帧 7 次 RenderTexture 创建）。

### 验证结果（2026-11-22 01:38–01:49，11 分钟 / 54,414 帧 / 1129×635 + DLSS RR）

| 观察项 | 结果 |
|---|---|
| `[RT] recreate` | **1 次**（Play 首帧，`cached=-1x-1` 全为新值，属正常首次创建） |
| `[RT] ReleaseRTs` | 3 次（1 次随重建 + 2 次随 Play 停止时的 `Pass.Dispose`） |
| `[PTR] ptrCache` | `newObject=0` 全程（仅 `age` 按 300 帧周期增长） |
| `view recreate /120 frames` | `out=0,0,0,0,0 \| ptrChanged out=0,0,0,0,0 \| queryInterval out=600 tex=600` |
| `Editor.log` | **422 KB**（修复前 240~250 MB），`Resource ID out of range` **0 条** |
| 稳定性 | 11 分钟无崩溃、无 device lost、无看门狗告警、退出干净（`VPT_Destroy: done`） |

> 顺带确认：分辨率稳定时 `GetNativeTexturePtr()` / `AccessTexture()` 返回的句柄是**稳定**的
> （纹理数组一直是 `ptrChanged tex=0`、`queryInterval tex=600`），所以"Unity 每帧换图"这个
> 早先的假设可以彻底否掉。

### 附：排查中走过的两条错路（留作教训）

| 错误假设 | 为什么当时像 | 为什么否掉 |
|---|---|---|
| NGX 日志写进 `Assets/` 触发刷新循环 | 出错时刻与 NGX 写日志时刻重合（BUG-003） | 关掉 NGX 日志后错误照旧 |
| 每帧 `AccessTexture` 查询本身在泄漏资源 ID | 关掉纹理数组查询后它们确实稳定了 | 根因是 RT 每帧重建；查询只是把"重建"暴露成日志 |

**教训**：先问"这个输入为什么每帧都变"，再猜 Unity 内部；`[RT]`/`[PTR]` 这类"对象身份"日志比
统计日志更能一针见血。

---

## BUG-005 验证层（VVL）一轮扫出的 4 个规范违背 —— 记录与改法

> 2026-11-22 首次用 `VK_LAYER_KHRONOS_validation` 跑 Unity + 本插件，一轮扫出 4 个**我们自己的**
> 规范违背（另有 1 类属 Unity 自身，见 §5.5）。这些**全部是既有问题**：01387/03328/02601
> 在 ray-query 后端时期就存在，只是从没开过验证层；03047 由 BUG-002 的单 descriptor set 引起。
> 之所以单独成章：这类错误驱动层会"容忍"，不开验证层就永远看不到，但换驱动/换硬件就可能变成崩溃。

**验证环境与复现方法**

```powershell
# 1) 打开验证层，然后用同一个 shell 启动 Editor（VVL 的错误会被 Unity 写进它的 log）
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
& "D:\tjEditor\2022.3.62f3c1\Editor\Unity.exe" -projectPath <工程> -logFile <log路径>
# 2) 统计扫到的 VUID 种类（<log路径> 里每条错误都带 vulkan.lunarg.com 的 VUID 锚点）
Select-String -Path <log路径> -Pattern "#(VUID-[A-Za-z0-9\-]+)" |
  ForEach-Object { $_.Matches[0].Groups[1].Value } | Group-Object | Sort-Object Count -Descending
```

- 需要"进 Play 跑几十秒"才有意义（设备创建类错误在启动时出现，描述符类要等到出帧）。
  本次用了一个临时的 `[InitializeOnLoad]` 编辑器脚本（环境变量 `VPT_AUTO_PLAY=1` 时自动开 SampleScene →
  进 Play 40 s → 退 Play → 退编辑器），**验证完已删除**，没有留在工程里。
- 坑：用 `-logFile` 启动时，**原生插件自己的 stderr（`[VPT]` 日志）会丢失**（VVL 的错误仍然在，
  因为它走 Unity 的 debug messenger）。要同时看插件日志，需要用 `-RedirectStandardError` 起进程，
  或者按平时的方式从 Unity Hub 启动、改看 `Editor.log`。
- VVL 对同一 VUID 默认只重复报 10 次（`duplicated_message_limit`），所以"条数"不代表"次数"。

### 5.1 启用了 `VK_KHR_acceleration_structure`，却漏了它的规范依赖 `VK_EXT_descriptor_indexing`

```
VULKAN: VALIDATION ERROR: vkCreateDevice(): pCreateInfo->ppEnabledExtensionNames[45]
  Missing extension required by the device extension VK_KHR_acceleration_structure: VK_EXT_descriptor_indexing.
  VUID-vkCreateDevice-ppEnabledExtensionNames-01387
```

| | 内容 |
|---|---|
| 根因 | `vpt_intercept.cpp` 注入扩展时只考虑了"功能闭包"（deferred_host_operations / BDA / AS / RT pipeline），没考虑**规范声明的依赖闭包**：`VK_KHR_acceleration_structure` 在规范里明确依赖 `VK_EXT_descriptor_indexing` |
| 为什么以前没炸 | 驱动（RTX 3070 Ti，Vulkan 1.4）对"少启一个依赖扩展"是宽容的，功能照常；只有验证层会拦 |
| 改法 | 在注入 AS **之前**补 `AddIfMissingQuiet(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)`（用静默版：1.2+ 上它是核心，驱动可能不再单独广告扩展名） |
| 复验 | 该 VUID 消失 |

### 5.2 KHR 与 EXT 两个 BDA 别名同时进了启用列表

```
VULKAN: VALIDATION ERROR: vkCreateDevice(): pCreateInfo->ppEnabledExtensionNames
  must not contain both VK_KHR_buffer_device_address and VK_EXT_buffer_device_address.
  VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328
```

| | 内容 |
|---|---|
| 根因 | Unity 自己的扩展表里带着 `VK_EXT_buffer_device_address`（Unity 日志：`[Vulkan init] extensions: name=VK_EXT_buffer_device_address [enabled=0, external=0]`，即它并不使用），而我们又无条件追加了 `VK_KHR_buffer_device_address` → 两个等价别名同时出现 |
| 第一次改错了（值得记） | 我最初改成「Unity 已有 EXT 就不加 KHR」。结果 BDA **feature 谁都没启用**，验证层立刻换了一组错误：<br>`VUID-VkMemoryAllocateInfo-flags-03331`（用了 `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` 但 bufferDeviceAddress feature 未启用）×10<br>`VUID-VkMemoryAllocateFlagsInfo-flags-parameter` ×10<br>→ 说明 Unity 只是"列出"了 EXT，并没有链它的 `VkPhysicalDeviceBufferDeviceAddressFeaturesEXT` |
| 正确改法 | 从 Unity 给的扩展列表里**摘掉 EXT**，统一保留 KHR，并照常链 KHR 的 feature 结构：<br>`for (auto it = extensions.begin(); ...) if (strcmp(*it, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0) erase`<br>同时把 BDA 函数指针的加载做成 KHR → EXT → core 三个名字兜底（`vpt_scene.cpp` / `vpt_rt.cpp`），避免将来 Unity 换别名时取不到函数 |
| 复验 | 03328、03331、flags-parameter 全部消失 |

### 5.3 对没有 `SHADER_DEVICE_ADDRESS_BIT` 的 buffer 查设备地址（5 处）

```
VULKAN: VALIDATION ERROR: vkGetBufferDeviceAddressKHR():
  pInfo->buffer (VkBuffer 0x215a000000215a) was created with VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT
  but requires VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
  VUID-VkBufferDeviceAddressInfo-buffer-02601
```

| | 内容 |
|---|---|
| 根因 | `vpt_scene.cpp` 的 `CreateBufferResource()` 对**所有** buffer 无条件调 `GetBufferAddress()`。staging / TRANSFER_SRC 这类 buffer 的 usage 里没有 `SHADER_DEVICE_ADDRESS_BIT`，按规范不允许查地址（我们其实也不需要它的地址） |
| 改法 | 只在 usage 含该位时才查，否则地址记 0：<br>`out.address = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? GetBufferAddress(...) : 0;` |
| 复验 | 该 VUID 消失（原来正好 5 条 = 5 个这类 buffer） |

### 5.4 每帧改写「仍在 pending 命令缓冲里使用」的 descriptor set

```
VULKAN: VALIDATION ERROR: vkUpdateDescriptorSets():
  pDescriptorWrites[0].dstBinding (0) was created with VkDescriptorBindingFlags(0),
  but VkDescriptorSet 0x20240000002024 is in use by VkCommandBuffer 0x1a58f6ae0d0.
  VUID-vkUpdateDescriptorSets-None-03047
```

| | 内容 |
|---|---|
| 根因 | 单套 descriptor set + 每帧主线程改写，而前 1~2 帧的命令缓冲仍在飞（Unity 最多 ~3 帧）。规范禁止改写"仍被 pending 命令缓冲使用"的 set（除非用 `UPDATE_AFTER_BIND` / `UPDATE_UNUSED_WHILE_PENDING` 绑定标志） |
| 为什么以前没炸 | NVIDIA 驱动实践上容忍；也解释了为什么这个隐患从 Plan B 起就一直"看起来没事" |
| 改法 | 描述符集**环形缓冲**：`kDescRingSize = 4` 套 set，`PrepareDispatch` 只改写「本帧这一套」，`RenderCallback` 绑同一套；上一/上上帧那几套保持不动。详见 **BUG-002 第 1 条** |
| 复验 | 该 VUID 消失（这条之前是每帧都报，是 4 条里最"热"的） |

### 5.5 剩下那 10 条不是我们的：Unity 自己的 swapchain 信号量复用

```
VULKAN: VALIDATION ERROR: vkQueueSubmit(): pSubmits[0].pSignalSemaphores[0] (VkSemaphore 0xc2…)
  is being signaled by VkQueue 0x…, but it may still be in use by VkSwapchainKHR 0xd4…
  Swapchain image 0 was presented but was not re-acquired, so VkSemaphore 0xc2… may still be in use.
  VUID-vkQueueSubmit-pSignalSemaphores-00067
```

| | 内容 |
|---|---|
| 判定依据 | ①消息里出现 `VkSwapchainKHR`；②本插件**从不提交队列、从不接触信号量**（所有命令都录进 Unity 的命令缓冲）；③它在开验证层的第一轮就存在，与插件改动无关；④VVL 报满 10 条后自己停了 |
| 处理 | 不属于本工程可修范围，记录在案。若将来要追，方向是 Unity 的 swapchain 实现（给每个 swapchain image 各配一个 present 信号量） |

**修复前后对比（同一进程内 Play 40 s / 3.4k 帧）**

| VUID | 修复前 | 修复后 |
|---|---|---|
| `vkCreateDevice-…-01387`（descriptor_indexing） | 1 | 0 |
| `VkDeviceCreateInfo-…-03328`（BDA 双别名） | 1 | 0 |
| `VkMemoryAllocateInfo-flags-03331`（改错时的中间态） | 0 → 10（我改错那次） | 0 |
| `VkBufferDeviceAddressInfo-buffer-02601` | 5 | 0 |
| `vkUpdateDescriptorSets-None-03047` | 9（+ 触及重复上限） | 0 |
| `vkCmdPipelineBarrier-dstAccessMask-06257`（清理 rayQuery 后才出现，§5.6） | — | 0（清理当轮出现 10 条，修 barrier 后归零） |
| `vkQueueSubmit-pSignalSemaphores-00067`（Unity） | 10 | 10（不变，非本工程） |

### 5.6 清理 rayQuery 后**才浮出来**的一条：AS barrier 把 COMPUTE 阶段留在 dstStageMask

```
VULKAN: VALIDATION ERROR: (vkCmdPipelineBarrier)
  If the rayQuery feature is not enabled and a memory barrier dstAccessMask includes
  VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, dstStageMask must not include any of the
  VK_PIPELINE_STAGE_*_SHADER_BIT stages except VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
  VUID-vkCmdPipelineBarrier-dstAccessMask-06257
```

| | 内容 |
|---|---|
| 触发时机 | 只在**删除 `VK_KHR_ray_query` 注入之后**才出现。规范这条规则的字面前提是"rayQuery feature 未启用"——之前 rayQuery 开着，这条检查对我们不生效，所以 §5.1~§5.4 修完的第一轮验证是干净的 |
| 根因 | 5 处「AS 写 → 着色器读」的 barrier 还留着旧 compute 后端的消费阶段：<br>`dstStageMask = AS_BUILD \| COMPUTE_SHADER \| RAY_TRACING_SHADER`、`dstAccessMask = AS_READ \| SHADER_READ`。<br>PT 早已只在 RT 阶段读 AS，`COMPUTE_SHADER` 是迁移遗留 |
| 改法 | `vpt_scene.cpp` 5 处把 `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` 从 **dstStageMask** 去掉，只留 `AS_BUILD \| RAY_TRACING_SHADER`（srcStageMask 里的 COMPUTE 是产出者，保留，例如蒙皮 dispatch 写顶点缓冲） |
| 复验 | 该 VUID 消失；PT 仍正常出帧（`out=0`、`queryInterval=600`） |
| 教训 | **"删掉一个扩展"也会改变验证层的判定面**。清理后端不能只做 grep + 删除，必须再跑一轮验证层——这条就是靠"清理后复跑"才抓到的 |

### 5.7 这次踩出来的三条经验

1. **一次只改一条 VUID，改完立刻复跑**。我一次改了"BDA 策略"（Unity 有 EXT 就不加 KHR），
   结果把 feature 关了、换来两组新 VUID（03331 / flags-parameter），定位成本翻倍。
   验证层跑一轮只要 ~2 分钟，比读代码猜快得多。
2. **先判断"这条错误归谁"**。`VkSwapchainKHR` / `vkQueueSubmit` / `pSignalSemaphores` 这类
   一定不是插件的（插件只往 Unity 的命令缓冲里录命令）；反过来，带 `vkCreateDevice` /
   `vkAllocateMemory` / `vkUpdateDescriptorSets` 的基本都是我们。
3. **扩展注入要按规范的"依赖闭包"写**，不是"跑得起来就行"。AS 依赖 descriptor_indexing 这种事，
   只有验证层会告诉你；驱动宽容恰恰是最危险的（它掩盖了换驱动就会炸的问题）。

---

## 本批次已处理（不再跟踪，仅供回溯）

| 问题 | 结论 |
|---|---|
| 验证层 4 个规范违背（01387 / 03328 / 02601 / 03047） | 见 **BUG-005**（含现象、根因、改法、复验与教训） |
| 退化 mesh 被 native 跳过 → BLAS 索引错位（P0） | C# 侧在登记 BLAS 之前挡掉「不可读 / 0 顶点 / 0 三角形」的 mesh，保持 `blasID == 登记顺序 == mesh 索引` 不变量，native 的 `SKIP degenerate` 分支不再可能命中 |
| `VptTrace` 看门狗线程从未回收（P1） | `PathTraceRendererFeature.Dispose` 末尾补 `VptTrace.Shutdown()`（实测退出 Play 会走到 Dispose，日志可见 `Feature.Dispose (disposing=True)`） |
| C# 诊断日志每帧 2 行、无轮转（P1） | 去掉 `Execute enter` / `Execute exit` 两行 per-frame `Line()`，改 60 帧一条心跳；卡死定位仍靠 `Stage()` + 看门狗 |
| `VPT_EnvFlag` 注释称"读取结果缓存"与实现不符 | 修正注释，说明每次调用都真读环境变量、以及各开关的实际调用频率 |
| `VPTTrace::Path()` 出锁后返回 `c_str()` 的悬垂指针风险（P2） | 不改实现，改为在 `vpt_trace.h` / `vpt_api.h` 写明生命周期与「调用方必须立即拷贝」的约定；彻底修复方案（固定静态缓冲）写在那两处注释里 |
