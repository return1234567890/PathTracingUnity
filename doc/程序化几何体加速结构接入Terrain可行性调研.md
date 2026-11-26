# 程序化几何体加速结构接入 Unity 地形的可行性调研

> 调研对象：URPPT（Unity 2022.3.62f3c1 + URP 14.0.12 + Vulkan Ray Query 路径追踪 + DLSS RR）
> 调研时间：2026-11-16
> 调研范围：只回答「把 Unity Terrain 以**程序化几何体**的形式接进当前 PT 的加速结构」是否可行、代价多大、推荐哪种形态。不评估地形渲染管线改造、不评估地形编辑工具、不评估树木/草/Detail 的接入。
> 调研方法：**静态代码审计 + 本机 Vulkan 原型实验 + 公开资料交叉验证**。核心 API 机制已在本机 GPU 上跑通（见 §6.3 与 `Tools/vk_terrain_probe/`）；性能数字与 AMD/Intel 平台行为属于**未实测**，文中逐条标注。

---

## 1. 结论速览

**一句话：技术可行，核心机制已在本机跑通；但「程序化」有两种读法，只有"每个 patch 一个 AABB + shader 内高度场求交"这一种在内存和改动量上划算——"全分辨率网格化"只在低分辨率地形（≤513²）上划算，"每个 cell 一个 AABB"与网格化同样贵。**

| 维度 | 判定 | 依据 |
|---|---|---|
| Vulkan 侧能否用程序化 AABB | ✅ 可行 | `VK_GEOMETRY_TYPE_AABBS_KHR` + `rayQueryGenerateIntersectionEXT`；本机 RTX 3070 Ti / GRD 616.92 实测 8/8 通过 |
| 与现有三角形 TLAS 混挂 | ✅ 可行 | 探针同一 TLAS 内混挂 1 个三角形 BLAS + 2 个 AABB 实例，互不干扰 |
| Unity 侧数据可得性 | ✅ 齐全 | `heightmapTexture`、`GetHeights`、`GetPatchMinMaxHeights`、`alphamapTextures`、`holesTexture` 在 2022.3 均存在（本机 Editor 程序集 XML 已核实） |
| 加速结构内存 | ✅ 有数量级优势 | 2049² 地形：全分辨率网格 BLAS **559.6 MB** vs 32²-cell patch 的 AABB BLAS **151.5 KB**（本机实测） |
| 求交正确性 | ⚠️ 需自己保证 | ray query 里**没有 intersection shader**、generated hit **不再走 any-hit**、AABB 边界**允许被实现放大**、candidate **无序** → 水密性/acne/误命中全靠 shader |
| 性能 | ⚠️ 未实测 | 主要风险是掠射射线的 candidate 数量与每 candidate 的 DDA 成本；三角形网格是硬件最擅长的形态 |
| 改动面 | ⚠️ 中等 | native 新增 AABB BLAS 分支 + 地形 SSBO；shader 命中/阴影两条路径都要分支；材质系统要支持 splat |
| 平台 | ⚠️ 只验证 NVIDIA | 本机驱动**不支持** `VK_EXT_ray_flags_primitive_culling`（无 `gl_RayFlagsSkipAABBEXT`），阴影遍历必须显式处理 AABB candidate |
| 工作量（粗估） | **6 ~ 10 人天** | 含网格化基线、AABB 实现、孔洞/多地形/变换、调参与文档；不含 LOD/流式/运行时编辑 |
| 与 RT pipeline 迁移的关系 | ⚠️ 见 §13 | 若计划迁 RT pipeline，地形建议放到迁移之后；地形数据与求交函数两种后端共用 |

**按地形规模的三条推荐线**

| 地形高度图分辨率 | 三角形数（2·(R−1)²） | 推荐方案 | 理由 |
|---|---|---|---|
| ≤ 513 | ≤ 52.4 万 | **直接网格化**（方案 A） | BLAS 35 MB + 顶点 23 MB，一次性构建 ~2 ms；不引入新 API、不引入求交正确性风险 |
| 1025 | 209.7 万 | 网格化勉强 / AABB 更稳 | 网格 BLAS 139.9 MB + 顶点索引 92.2 MB，8 GB 显存下与 DLSS RR（4K 约 580 MB）叠加偏紧 |
| ≥ 2049 | ≥ 838.9 万 | **AABB patch + DDA**（方案 B） | 网格 BLAS 559.6 MB 起、4097² 达 2.24 GB，不可接受；AABB patch 仅 151.5 KB |

> 本项目的显存基线：本机 8 GB（RTX 3070 Ti Laptop），PT 加速结构 + 7 张输入分辨率 RT + DLSS RR 已占相当一部分，见 `doc/DLSS4.5接入可行性调研.md` §1 的显存表。

---

## 2. 先澄清「程序化几何体加速结构」的两种含义

这个说法在 Vulkan/DXR 语境里指向两个完全不同的东西，代价差 3 个数量级，必须先分开：

| | 含义 A：程序化**生成**三角网格 | 含义 B：**procedural primitive**（AABB 几何 + 自定义求交） |
|---|---|---|
| API 形态 | `VK_GEOMETRY_TYPE_TRIANGLES_KHR`，BLAS 里存三角形 | `VK_GEOMETRY_TYPE_AABBS_KHR`，BLAS 里只存 AABB；命中的「表面」由 shader 算 |
| 谁做求交 | 硬件 | 硬件只做盒子遍历，shader 做盒内求交（ray query 无 intersection shader） |
| 内存 | 与三角形数成正比（本机 ~66.7 B/三角形） | 与**盒子数**成正比，与三角形数无关 |
| 典型用法 | 引擎把高度图展开成三角形塞进 BLAS（Unity HDRP 的 `Terrain.enableHeightmapRayTracing`、UE5 Lumen 的 heightfield 回落表示） | 曲线/毛发/粒子/高度场等隐式几何 |
| 本文结论 | 低分辨率地形的最优解，也是最好的**基线** | 大地形的唯一可行解，但正确性要自己兜 |

本文主评 **B**（这才是「程序化几何体加速结构」的字面含义），并把 **A** 作为基线与回退方案一起量化。

---

## 3. 项目现状：地形为什么现在进不了 PT

### 3.1 几何来源只有 `MeshFilter` / `SkinnedMeshRenderer`

- 场景收集在 `ModelManager.BuildScene()` 里一次性完成：`FindObjectsOfType<MeshFilter>()`（[ModelManager.cs:324](../Assets/Scripts/ModelManager.cs#L324)）与 `FindObjectsOfType<SkinnedMeshRenderer>()`（[ModelManager.cs:406](../Assets/Scripts/ModelManager.cs#L406)）。
- `Terrain` 组件不挂 `MeshFilter`，地形网格由引擎内部生成、**不对 C# 暴露**（`TerrainData` 只给高度图/贴图，不给渲染 Mesh），所以现有收集器连一个三角形都收不到。
- 结论与既有调研一致：`doc/PT管线与Unity物理系统调研.md` 的结论表里「Terrain / TerrainCollider ❌ 不支持」。

### 3.2 三处「三角形假设」是接入的硬边界

| 位置 | 现状 | 接 AABB 地形必须改 |
|---|---|---|
| `BuildBLAS()` | 只构造 `VK_GEOMETRY_TYPE_TRIANGLES_KHR`（[vpt_scene.cpp:475-490](../NativePlugin/src/vpt_scene.cpp#L475-L490)），`vertexFormat/vertexStride/indexType` 写死 | 新增 AABB 分支：`geometryType = AABBS`、`stride = 24`、`geometry.aabbs.data` |
| `getHitInfo()` | 无条件读 `rayQueryGetIntersectionBarycentricsEXT` 并查 `indices/vertices`（[pathtrace.glsl:717-763](../NativePlugin/shaders/pathtrace.glsl#L717-L763)） | 按 `committed type` 分支：AABB 命中没有 barycentrics，法线/UV 必须解析计算 |
| `traceShadowTransmission()` | candidate 循环**显式跳过非三角形**，注释写明「本项目 TLAS 里没有 AABB / procedural」（[pathtrace.glsl:563-566](../NativePlugin/shaders/pathtrace.glsl#L563-L566)） | AABB candidate 要么参与遮挡判定，要么被 `continue` 掉——后者会让地形**不投影**（阴影直接穿山） |

另外两处隐性约束：

- `InstanceInfo` 是 64 B（`transformRow0..2` + `vertexOffset` + `indexOffset` + `materialID` + `pad`，[vpt_scene.cpp:61-72](../NativePlugin/src/vpt_scene.cpp#L61-L72)），shader 侧同构（[pathtrace.glsl:91-99](../NativePlugin/shaders/pathtrace.glsl#L91-L99)）。地形实例需要「地形 ID / patch 基址」，最省事的做法是复用 `vertexOffset/indexOffset` 两个字段（地形不走统一顶点缓冲）。
- TLAS **每帧全量重建**（`kTLASRingSize = 4` 环形槽位，[vpt_scene.cpp:95-113](../NativePlugin/src/vpt_scene.cpp#L95-L113)）。地形实例是静态的，跟着每帧重建只是多写 64 B 实例数据，可忽略；但如果把地形切成**每 patch 一个 TLAS 实例**（用于 LOD/剔除），实例数会涨到几千，需要重新评估 CPU 侧的实例写入成本。

### 3.3 硬件/驱动基线（本机实测）

| 项 | 值 | 来源 |
|---|---|---|
| GPU | NVIDIA GeForce RTX 3070 Ti Laptop（deviceID 0x24a0） | `vulkaninfo --summary` |
| 驱动 | 616.92（`driverVersion 2585198592`），Vulkan 1.4.351 | 同上 |
| 扩展 | `VK_KHR_acceleration_structure` rev 13、`VK_KHR_ray_query` rev 1、`VK_KHR_ray_tracing_pipeline` rev 1 | 同上 |
| `VK_EXT_ray_flags_primitive_culling` | **不支持** | 同上（扩展列表无此项） |
| `maxPrimitiveCount` | 536,870,911 | `VkPhysicalDeviceAccelerationStructurePropertiesKHR` |
| `maxInstanceCount` | 16,777,215 | 同上 |
| scratch 对齐 | 128 B | 同上 |

`maxPrimitiveCount` 是 5.3 亿，AABB 图元数量**不构成限制**；真正的限制来自驱动对 `gl_RayFlagsSkipAABBEXT` 的不支持（见 §6.1）。

---

## 4. Unity 侧：地形数据到底能拿到什么（2022.3 事实）

> 下表的「核实方式」：`docs` = Unity 2022.3 官方 Scripting API 页面；`editor-xml` = 本机 `D:\tjEditor\2022.3.62f3c1\Editor\Data\Managed\UnityEngine\UnityEngine.TerrainModule.xml` 里的成员签名。

| 数据 | API | 核实 | 对 PT 的用途 |
|---|---|---|---|
| 高度图分辨率 | `TerrainData.heightmapResolution` | docs：取值被夹到 {33,65,129,257,513,1025,2049,4097} | 决定三角形数/高度图纹理大小 |
| 高度数据（CPU） | `GetHeights(0,0,w,h)` / `GetInterpolatedHeight(x,z)` | docs | 一次性烘焙成网格或 AABB patch |
| 高度数据（GPU） | `TerrainData.heightmapTexture`，类型是 `RenderTexture` | docs 签名 `public RenderTexture heightmapTexture;` | 可 `Graphics.Blit`/拷贝到 `GraphicsBuffer`，避免 CPU 读回 |
| 尺寸/缩放 | `TerrainData.size`、`heightmapScale`、`Terrain.transform` | docs | 世界空间映射（顶点 = 归一化坐标 × size，再乘 transform） |
| patch 级 min/max | `TerrainData.GetPatchMinMaxHeights()` / `OverrideMinMaxPatchHeights()`、`GetMaximumHeightError()` | editor-xml + docs | **可直接复用**成 AABB；但 patch 边长未公开（见下方陷阱） |
| 孔洞 | `TerrainData.holesTexture`、`holesResolution`（= `heightmapResolution − 1`）、`SetHoles()` | docs + 源码 | 求交后采样孔洞掩码，**0 = 洞**（URP 用 `< 0.0005` 判洞） |
| 混合层权重 | `alphamapTextures`、`GetAlphamapTexture(i)`、`alphamapTextureCount`、`GetAlphamaps()` | docs + 源码 | **每张 alphamap 4 个通道**，`textureCount = ceil(layers/4)` |
| 层定义 | `TerrainLayer`（diffuseTexture / normalMapTexture / maskMapTexture / tileSize / metallic / smoothness） | docs | 每层对应纹理数组的 slice + uv scale |
| 物理碰撞 | `TerrainCollider`（PhysX heightfield，16-bit 采样、全分辨率、含孔洞） | Unity 手册 | 与 PT 无关；也说明「物理侧本来就是高度场，不是三角网格」 |
| 渲染网格 | **无公开 API**：`Terrain` 是 `Behaviour` 不是 `Renderer`，patch mesh 在 native 侧 | 源码 + docs | 所以地形几何只能自己从高度图重建 |

几个直接可用的推论，以及三个**必须避开的数据陷阱**：

1. **三角形数量是「分辨率」的平方**：`2·(R−1)²`。513² → 52.4 万；1025² → 209.7 万；2049² → 838.9 万；4097² → 3355 万。这个量级决定了网格化方案的显存上限（§5.2 实测）。
2. **高度图本身很小**：4097² 用 R32F 是 67 MB、R16 是 33.6 MB——与 2.24 GB 的网格 BLAS 差 60~70 倍。这是「用程序化几何体省内存」这一直觉的**唯一正确来源**：省的是**三角形/BVH**，不是高度数据。
3. **`GetPatchMinMaxHeights` 能省一次全图扫描，但不能假设 patch 尺寸**：Unity 没有公开 patch 的边长（社区逆向的「17×17 顶点 = 16×16 quad」与流传的 32×32 互相矛盾，均未经官方确认）。落地时现场量：
   `patchesPerSide = round(sqrt(GetPatchMinMaxHeights().Length))`，`quadsPerPatch = (heightmapResolution-1)/patchesPerSide`；或者干脆自己按 16²/32² 重算 AABB（CPU 扫一遍 2049² 高度图，几十 ms 量级，只在 BuildScene 做一次）。
4. **陷阱一：`heightmapTexture` 与 `GetHeights` 值域不同**。纹理里存的是 **[0, 0.5]**（0.5 对应 `size.y`），`GetHeights` 返回 **[0, 1]**。两者混用会得到 2 倍高度误差。
5. **陷阱二：Vulkan 上高度图纹理不是 R32F，而是 RG8 打包的 16-bit 定点**。Unity 的 `PackHeightmap/UnpackHeightmap`（core `Common.hlsl`）在 `SHADER_API_VULKAN` 下把 16-bit 值拆成 R/G 两字节，解码为 `(r + g*256)/257`。所以**不要把它当单通道 float 直接采样**：要么按此公式解码，要么（更推荐）用 `GetHeights` 自己上传一份 R16/R32F——本项目只在 BuildScene 传一次。
6. **陷阱三：光栅化的地形在远处不是高度图**。terrain LOD 用 `skipScale`（1/2/4/8…）对高度图做**点采样抽稀**（`Terrain.heightmapMaximumLOD = 1` 的文档原话：高度图宽高减半、三角形数降到 1/4），超过 `basemapDistance` 后画的是预生成 basemap 贴图而不是几何。PT 追全分辨率高度图 → **远景山脊/轮廓会与光栅版有差异**；做对比测试时应先把 `heightmapMaximumLOD` 设为 0、`heightmapPixelError` 调小。
7. **`GetHeights` 是一次性托管分配**：2049² 每次 16.8 MB、4097² 每次 67 MB（LOH）。只在 BuildScene 调一次；运行时改地形用 `SetHeightsDelayLOD` + `SyncHeightmap`（`SetHeights` 每次都会重算 LOD 与植被）。

---

## 5. 方案对比与量化

### 5.1 五种表示

| 方案 | 表示 | BLAS 里有什么 | 求交在哪 | 主要问题 |
|---|---|---|---|---|
| **A** 全分辨率网格 | 每个 cell 两个三角形 | 全部三角形 | 硬件 | 显存与分辨率平方成正比 |
| **A′** 分块 + LOD 网格 | 近处细、远处粗的网格块 | 各 LOD 的三角形 | 硬件 | LOD 裂缝/法线跳变、需运行时切实例 |
| **B** 粗 AABB patch + DDA | 每 N×N cell 一个 AABB | patch 数（几十~几千） | shader 内 DDA | 正确性（acne/接缝/水密）+ 掠射成本 |
| **B′** 每 cell 一个 AABB | 每个 cell 一个 AABB | cell 数（百万级） | shader 内二三角形 | BVH 成本与网格同量级，**没有意义** |
| **C** 纯 DDA，不进 TLAS | 无地形 AS | — | shader 内 DDA | 每条射线都要 march；与 TLAS 命中做 min 合成 |

### 5.2 本机实测：加速结构内存（`Tools/vk_terrain_probe/sz.cpp`）

`FAST_TRACE`、全分辨率、无 LOD、无压缩（未调用 `vkCmdBuildAccelerationStructuresKHR` 的 compaction）：

| 高度图 | cell 数 | 三角形数 | 网格 BLAS | 构建 scratch | 顶点+索引（80 B/12 B 估算） |
|---|---|---|---|---|---|
| 129² | 1.6 万 | 3.3 万 | 2.2 MB | 0.7 MB | 1.5 MB |
| 257² | 6.6 万 | 13.1 万 | 8.7 MB | 2.6 MB | 5.8 MB |
| 513² | 26.2 万 | 52.4 万 | **35.0 MB** | 10.6 MB | 23.1 MB |
| 1025² | 104.9 万 | 209.7 万 | **139.9 MB** | 42.3 MB | 92.2 MB |
| 2049² | 419.4 万 | 838.9 万 | **559.6 MB** | 169.3 MB | 368.4 MB |
| 4097² | 1677.7 万 | 3355.4 万 | **2238.3 MB** | 677.0 MB | 1472.7 MB |

折算 ≈ **66.7 B/三角形**（139.9 MB ÷ 209.7 万），与公开资料里「动态 BVH ~60–80 B/三角形」一致（[HPG 2020 slides](https://www.highperformancegraphics.org/slides20/monday_gruen.pdf)，PDF 索引）。压缩（RTXMU 一类的 compaction）通常再省 44%~55%（[NVIDIA RTXMU](https://developer.nvidia.com/blog/reducing-acceleration-structure-memory-with-nvidia-rtxmu)），即 2049² 也要 ~280 MB，仍然不可接受。

同一套高度图的 AABB patch 表示：

| 高度图 | patch = 8² cell | patch = 16² cell | patch = 32² cell |
|---|---|---|---|
| 513² | 4096 prim → 151.5 KB | 1024 → 39.2 KB | 256 → **11.2 KB** |
| 1025² | 16384 → 600.5 KB | 4096 → 151.5 KB | 1024 → **39.2 KB** |
| 2049² | 65536 → 2396.4 KB | 16384 → 600.5 KB | 4096 → **151.5 KB** |

**读法**：
- B 相对 A 的 BLAS 缩小 **~3700 倍**（2049²：559.6 MB → 151.5 KB），且与高度图分辨率**弱相关**（patch 粒度固定时，prim 数只跟地形面积/分辨率成正比，不是平方级）。
- B′（每 cell 一个 AABB）不省：2049² 的 419 万 cell 会产生同量级 prim，BVH 开销与网格相当——所以**patch 粒度是 B 方案的关键参数**，不是实现细节。

### 5.3 方案对比总表

| 维度 | A 全网格 | A′ 分块 LOD | B 粗 AABB patch | C 纯 DDA |
|---|---|---|---|---|
| 2049² 显存 | 559.6 MB + 368 MB 顶点索引 | 视 LOD 比例 ~1/4 | **~151 KB + 高度图 17 MB** | 高度图 17 MB |
| 构建成本 | 一次性 ~13–17 ms（按 1.5–2 ms/百万三角形推） | 每 LOD 一次 | 一次性 <1 ms（几千个 AABB） | 无 |
| 每帧成本 | TLAS 实例 1 个 | 每块一个实例 | 实例 1 个 | 无 AS |
| 硬件剔除 | 最好 | 好 | 好（粗粒度） | **无** |
| 正确性风险 | 低（几何=高度图；但仍与光栅 LOD 有差异） | LOD 裂缝、法线跳变 | acne/接缝/水密/误命中 | 同 B + 与 TLAS 命中合成 |
| 改动面 | C# 网格生成 + 材质 | 再加 LOD 调度 | native BLAS + shader + 材质 | 只改 shader + 上传高度图 |
| 适用 | ≤513² | 大世界但要求几何一致 | ≥1025²，静态地形 | 小地形/原型验证 |
| 本文推荐 | ✅ 基线/小地形 | ⚠️ 备选 | ✅ **大地形目标方案** | ⚠️ 仅原型 |

---

## 6. 关键 API 语义：ray query 里的 AABB 与「三角形路径」不一样

这一节是可行性的核心。以下结论来自 Vulkan/GLSL 规范与 DXR 规范，并与本机实验互相印证。

### 6.1 规范层面的六条硬事实

| 事实 | 含义（对本项目） |
|---|---|
| **ray query 没有 intersection shader**，AABB 命中以 candidate 形式回到 `while (rayQueryProceedEXT(rq))` 循环 | 盒内求交必须在 `pathtrace.glsl` 里手写；这也正是我们要的（无 shader 表、无发散调用） |
| 对 AABB candidate 只能调 `rayQueryGenerateIntersectionEXT(q, tHit)`；对三角形 candidate 只能调 `rayQueryConfirmIntersectionEXT()` | 两条路径的 API 不同，shader 里必须按 `rayQueryGetIntersectionTypeEXT(rq,false)` 分支 |
| generated hit **不再经过「确认」阶段**（没有 any-hit） | 不透明/半透明判定、孔洞、alpha 都要在 `generateIntersection` 之前自己算完 |
| **candidate 之间没有顺序保证** | 阴影路径现有的 `abs(hitT - prevT)` 做法对地形不适用；地形求交必须自己保证返回**最近**的合法 t（`generateIntersection` 传错 t 会得到不可预测的最近命中） |
| AABB 的边界**允许被实现放大**（"implementation-defined bound which includes the specified box"），可能产生 false positive | 拿到 candidate 后必须自己验证射线真的与盒子相交、命中点真的在 `[tmin, tmax]` 内；不能假设「有 candidate 就一定命中」 |
| AABB 的 t 区间是**闭区间** `[tmin, tmax]`，且**不做背面剔除**；起点在盒内时**保证命中** | 贴地射线的 `tmin` 偏移、地表自阴影的处理方式与三角形不同 |

补充两条工程事实：

- **没有 barycentrics**：AABB 命中拿不到 `rayQueryGetIntersectionBarycentricsEXT`，UV/法线/切线必须由 `primitiveIndex + 命中点` 反算（地形恰好可以：UV = 世界 XZ，法线 = 高度场梯度）。
- **`gl_RayFlagsSkipAABBEXT` 不可用**（本机驱动没有 `VK_EXT_ray_flags_primitive_culling`）：不能像某些引擎那样「阴影线只查三角形、跳过 AABB」，阴影 candidate 循环必须同时处理两种几何。反过来，如果将来要「只查地形不查三角形」也不行，只能靠材质/实例掩码或第二个 TLAS。

### 6.2 与 DXR 的对应关系（决定未来可移植性）

DXR 1.1 inline 的语义与这里一一对应：`CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE`、`CommitProceduralPrimitiveHit(t)`（对应 `rayQueryGenerateIntersectionEXT`）、`RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES`、AABB 描述 `D3D12_RAYTRACING_GEOMETRY_AABBS_DESC`。也就是说：**今天在 Vulkan 侧实现的地形 AABB 方案，换 D3D12 后端时算法与数据布局可以原样复用**，只是 API 名字不同（[DXR spec](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)、[DXR 1.1 博客](https://devblogs.microsoft.com/directx/dxr-1-1/)）。

### 6.3 本机原型实验（`Tools/vk_terrain_probe/`）

为了不把 API 问题和工程问题混在一起，写了一个**独立于本插件**的 Vulkan 小程序：

- 64×64 cell 高度场（1 m 网格，中心有 6 m 圆锥凸起），按 16×16 cell 切成 **16 个 AABB patch**；
- 同一 TLAS 里再挂一个 2 三角形的 quad（世界 x∈[100,104]）；
- 3 个实例：地形（普通）、地形（`FORCE_OPAQUE`，平移 +200 m）、quad（`FORCE_OPAQUE`）；
- 8 条射线覆盖：垂直命中、斜射命中、向上 miss、打三角形、`TerminateOnFirstHit` 阴影式、凸起处斜法线、`FORCE_OPAQUE` 实例、空地 miss。

结果（`results_probe.txt`，全部 PASS）：

| 射线 | 场景 | cand | aabb | tri | committed | inst | prim | t | 法线 |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 垂直打平坦区 | 1 | 1 | 0 | **GENERATED** | 0 | 10 | 40.004 | nY=1.000 |
| 1 | 斜射 | 1 | 1 | 0 | **GENERATED** | 0 | 10 | 45.947 | nY=1.000 |
| 2 | 向上 | 0 | 0 | 0 | NONE | — | — | — | — |
| 3 | 打三角形 quad | 0 | 0 | 0 | TRIANGLE | 2 | 0 | 40.000 | — |
| 4 | `TerminateOnFirstHit` | 0 | 0 | 0 | TRIANGLE | 2 | 0 | 40.000 | — |
| 5 | 凸起（有坡度） | 1 | 1 | 0 | **GENERATED** | 0 | 15 | 24.643 | nY=0.686 |
| 6 | `FORCE_OPAQUE` 实例 | 1 | 1 | 0 | **GENERATED** | 1 | 10 | 40.004 | nY=1.000 |
| 7 | 空地 | 0 | 0 | 0 | NONE | — | — | — | — |

**三个重要观测**：

1. AABB 的 candidate 数量在垂直/斜射下都是 **1**（只有命中的那个 patch 的 AABB 被投递）——因为 patch 的 AABB 在 Y 方向是紧的（平坦区只有 ~0.02 m 厚），射线只有在接近地表时才进入盒子。这是 B 方案性能成立的关键。
2. `FORCE_OPAQUE` 实例依然会走 candidate + `generateIntersection`，且 committed 仍是 `GENERATED`——**实例级 opaque 标记不影响 AABB 的提交路径**，阴影/主路径可以继续用现有的 `FORCE_OPAQUE` 约定。
3. 三角形 BLAS 与 AABB BLAS 混挂时，`instanceId/primitiveIndex` 各归各的，不会串号——这意味着地形可以作为一个普通实例挂在现有 TLAS 里，不需要独立 TLAS。

### 6.4 必须改的代码点（清单）

| 文件 | 现状 | 需要新增 |
|---|---|---|
| `vpt_scene.cpp` `BuildBLAS()` | 只建三角形 | AABB 分支 + `s_TerrainAabbBuffers`/地形 BLAS 列表 |
| `vpt_scene.cpp` `WriteInstanceData()` | `instanceCustomIndex = materialID` | 地形实例用 `materialID = TERRAIN_MAT_ID`，`InstanceInfo.vertexOffset/indexOffset` 复用为 `terrainID/patchBase` |
| `vpt_scene.cpp`/`vpt_types.h` | `VPT_SceneUpdate` 只有 meshes/materials/lights/instances | 新增 `VPT_TerrainData*`（高度图指针、尺寸、patch 数、layer 数） |
| `pathtrace.glsl` `getHitInfo()` | 三角形插值 | `committed type` 分支：AABB → 解析法线/UV/层权重 |
| `pathtrace.glsl` `traceShadowTransmission()` | 跳过非三角形 | AABB candidate → 地形遮挡测试（或直接把地形视作不透明） |
| `ModelManager.cs` | 只收 `MeshFilter`/`SkinnedMeshRenderer` | 新增 `CollectTerrains()`（`Terrain.activeTerrains`） |
| `TextureArrayManager.cs` | 从材质收集贴图 | 收集 `TerrainLayer` 的 diffuse/normal/mask 进纹理数组 |
| `PathMaterial` | 单材质 per instance | 新增 `MAT_IS_TERRAIN` + 层索引表 |

---

## 7. 推荐架构：粗 AABB patch + shader 内 DDA

### 7.1 数据流

```
Unity Terrain
  ├─ TerrainData.GetHeights()（推荐；或 heightmapTexture，注意 Vulkan 上 RG8 打包）─┐
  ├─ TerrainData.size / Terrain.transform ──────────────────┤
  ├─ TerrainLayer[] → TextureArrayManager ─────────────────┤
  ├─ TerrainData.holesTexture ──────────────────────────────┤
  └─ patch AABB（自算 min/max 或复用 GetPatchMinMaxHeights）┤
                                                            ▼
             C#：new NativeBridge.VPT_AddTerrain(desc)  →  native
                                                            ▼
   native：VkBuffer（高度图 R16/R32F）+ VkBuffer（patch AABB）
           + BLAS(VK_GEOMETRY_TYPE_AABBS_KHR) + TLAS 实例（静态，不参与每帧更新）
                                                            ▼
   shader：binding 18+ 的 SSBO 读高度图/patch 表；
           命中 = rayQuery committed GENERATED → 解析法线/UV/splat
```

### 7.2 GPU 资源与 SSBO 布局（建议）

| binding | 内容 | 备注 |
|---|---|---|
| 18 | `TerrainInfo[]`（每地形：高度图 base/cell 尺寸/分辨率/patch 数/layer 数/worldToLocal） | 与 `instanceID` 对应 |
| 19 | `float heightmap[]`（或 `texture2D` + Sampler） | 用自己的 R16/R32F，**不要直接采样 `heightmapTexture`**（Vulkan 上是 RG8 打包、值域 [0,0.5]，见 §4 陷阱二）；R16 在 600 m 高度范围下量化 ≈9 mm |
| 20 | `vec4 patchAabbMin[]` / `vec4 patchAabbMax[]` | 24 B/prim 的 BLAS 输入可以与之共用同一 buffer |
| 21 | `TerrainLayerGpu[]`（slice 索引、uv scale、metallic/smoothness） | 与 `VPT_Material` 分开，避免撑爆 128 B |
| 22 | `float splat[]`（或 `texture2DArray` 的 alphamap） | 每地形 `alphamapTextureCount` 张 |

> 当前 shader 已用到 binding 0–17（`17` 是 HDR 环境 alias 表），18 之后是空闲的。

### 7.3 求交算法（生产版伪代码）

```glsl
// 一个 patch = PATCH x PATCH cells；patch 的 AABB 由 CPU 预计算（含 1e-3 量级膨胀）
float traceTerrain(vec3 ro, vec3 rd, float tMax, uint patchId, out vec3 n, out vec2 xz)
{
    vec3 lo = patchMin[patchId].xyz, hi = patchMax[patchId].xyz;

    // ① AABB slab：注意 AABB 的 t 是闭区间，tmin/tmax 都要处理
    float t0, t1;
    if (!slab(ro, rd, lo, hi, 0.0, tMax, t0, t1)) return -1.0;

    // ② 盒内 DDA（2D，xz 平面）
    //    ★ 两个已踩的坑：
    //    (a) 零方向分量：1/0 = inf 会把 slab/DDA 判成空区间 → 对 x/z 分量做 ±1e-6 扰动；
    //    (b) tMaxAxis 必须从「进入点 p = ro + dd*t0」算起，不能从 ro 算，
    //        否则 t0 > 0 时边界 t 会小于当前 t，DDA 原地打转。
    vec3 dd = rd;
    dd.x = (abs(dd.x) < 1e-6) ? 1e-6 : dd.x;
    dd.z = (abs(dd.z) < 1e-6) ? 1e-6 : dd.z;
    vec3 inv = 1.0 / dd;
    vec3 p = ro + dd * t0;
    ivec2 cell = clamp(ivec2(floor(p.xz / CELL)), patchOrigin, patchOrigin + PATCH - 1);
    vec2 tMaxAxis = (nextBoundary(cell) - p.xz) * inv.xz + vec2(t0);

    for (int i = 0; i < PATCH * PATCH; i++) {
        int axis = (tMaxAxis.x < tMaxAxis.y) ? 0 : 1;
        float tCellExit = min(t1, tMaxAxis[axis]);

        // ③ 层内加速：先查该 cell 的 min/max（可选：2x2/4x4 的 min-max 金字塔）
        // ④ cell 内两个三角形做精确求交（与 CPU/物理保持同一对角线约定）
        float tHit = intersectCellTriangles(ro, rd, cell, t0, tCellExit);
        if (tHit > 0.0) { n = normalFromGradient(...); xz = (ro + rd*tHit).xz; return tHit; }

        t0 = tCellExit;
        if (t0 >= t1) break;
        cell[axis] += stepSign[axis];
        tMaxAxis[axis] += tDelta[axis];
    }
    return -1.0;
}
```

要点：

- **每 cell 两个三角形**（不是双线性）与 CPU 侧/物理侧保持同一表面；Unity 地形自身的三角化约定要在 C# 侧确认后写死，否则阴影边界会出现亚像素级错位。
- 若追求更快的「跳过空气」，在 patch 内加 **min/max 金字塔**（Tevs 2008 的 maximum mipmap 思路），代价是多一份 `(PATCH/2)²` 的辅助数据（每 patch 几百字节），收益是掠射射线少走很多 cell。
- **`generateIntersection` 的 t 必须落在 `[t0, t1]`**；宁可在命中 t 上加 `1e-4 × 地形尺度` 的正偏移，也不要传 `t - eps`（会导致最近命中判定为「更远的盒子」）。

### 7.4 材质 / UV / 法线 / 孔洞

| 项 | 做法 |
|---|---|
| UV | `uv = (worldPos.xz - terrainOrigin) / terrainSize.xz`（不是三角形插值） |
| albedo | 按 alphamap 权重混合 N 层 `baseColorTex`；注意 **每张 alphamap 只有 4 个通道**（`GetAlphamapTexture(i)` 的 R/G/B/A 分别对应 layer 4i..4i+3），URP 是「每 pass 4 层、pass 数不限」 |
| roughness/metallic | `maskMap` 的 R/A 通道（URP Terrain 的 mask 约定），逐层加权 |
| 法线 | 宏观法线用高度场中心差分（或三角形几何法线）；层法线贴图叠加到切线空间需要构造 TBN（`T = normalize(cross(N, up))`，地形可用世界 XZ 基） |
| 孔洞 | 命中后采样 `holesTexture`：**0 = 洞**（URP 用 `hole < 0.0005` 判洞），UV 用 `(cellIndex + 0.5) / holesResolution`；洞分辨率是 `heightmapResolution − 1`，按 cell 采样否则边界会有半个纹素的接缝 |
| 半透明 | 地形不参与半透明；阴影路径按「不透明遮挡」处理即可 |

### 7.5 阴影 / GI / DLSS

- **NEE 阴影线**：`origin = P + Ngeo * surfaceOffset(mat)`，`tmin = 0`。地形命中点抬升 5 mm（现有 `EPS_OFFSET`）在坡度大的地方可能仍不够；建议沿 `Ngeo` 抬 `max(EPS_OFFSET, 0.02 * cellSize)`。
- **阴影 candidate 循环**：AABB candidate 需要跑一次地形求交判断是否真的挡住；如果只是「有 candidate 就算挡住」，平坦地形的薄 AABB 会把大量擦边候选误判为遮挡（阴影发黑）。
- **GI**：地形作为 bounce 命中面走同一套 BSDF 即可；地形面积大，`NEE` 采样效率不受影响。
- **DLSS RR**：G-buffer 的 `gbufferNormalRough` 用几何法线 + 层 roughness；`gbufferPosDepth` 用世界坐标 + `hitT`。地形是静态的，**运动矢量仍只含相机重投影**，与现状一致（`DlssAux.compute` 不写物体 MV，见既有调研）。
- **感受野**：地形的 `hitT` 可能很大（远景山体），RR 的深度/法线输入需要保证不溢出 `rgba32f`（现状没问题）。

### 7.6 地形变换

Unity 的 `Terrain` 支持平移/旋转/缩放。AABB 在**地形本地空间**，TLAS 实例矩阵负责变换：

- 平移 + 均匀缩放 + 旋转：直接可用（射线由硬件变换到本地空间）。
- **非均匀缩放**：AABB 本身仍正确（轴对齐盒在本地空间），但法线需要逆转置、`t` 参数化会被缩放影响（`t` 是世界空间的，硬件处理）；建议**先只支持平移 + 均匀缩放**，非均匀缩放时在 C# 侧报 warning 或烘焙到高度图。
- 现有 shader 的 `transformDir()` 没有逆转置（[pathtrace.glsl:174-181](../NativePlugin/shaders/pathtrace.glsl#L174-L181)），地形法线要单独处理（本地空间算完再乘 `transpose(inverse(M3x3))`）。

---

## 8. 风险清单

| # | 风险 | 影响 | 缓解 | 判定 |
|---|---|---|---|---|
| 1 | **自交（acne）**：命中点贴着地表，阴影/下一跳立刻打到自身 | 画面发黑/噪点 | `tmin` 沿几何法线偏移；`generateIntersection` 的 t 加正 eps；与现有 `EPS_OFFSET`/`surfaceOffset()` 对齐 | ⚠️ 必做 |
| 2 | **双命中/水密性**：两个三角形共享边、相邻 cell 共享顶点 | 亚像素漏光/重复命中 | 固定对角线约定；用「水密 ray-triangle」（共享边只归属一侧）；patch 边界 cell 只归属一个 patch | ⚠️ 必做 |
| 3 | **AABB 被实现放大 → false positive** | 误遮挡、阴影错误 | candidate 内先验证 slab 与命中 t 区间，再 `generateIntersection` | ⚠️ 必做 |
| 4 | **掠射射线 candidate 爆炸**：沿山谷/地平线飞行的射线穿过大量 patch | 帧率抖动 | patch 尺寸调优（16²/32²）；patch 内 min/max 金字塔；限制 bounce 的最大距离 | ⚠️ 需实测 |
| 5 | **阴影路径回归**：现有 candidate 循环跳过非三角形 | 地形不投影（光穿山） | 显式处理 AABB candidate；加「阴影 AABB 命中即遮挡」的最简实现先跑通 | ✅ 明确 |
| 6 | **非均匀缩放/旋转地形** | 法线/位置错 | 先只支持平移+均匀缩放 | ✅ 可控 |
| 7 | **运行时改地形（SetHeights）** | AABB 与高度图不一致 | `ALLOW_UPDATE` 的 BLAS refit 只更新 AABB（几千个，亚毫秒）；高度图整张重传（17 MB，~1 ms） | ✅ 可控 |
| 8 | **高度量化** | 台阶感/阴影错位 | R32F 最稳；R16 在 600 m 高度范围下量化 9 mm，可接受 | ✅ |
| 9 | **平台差异**（AMD/Intel 的 AABB candidate 行为、驱动 bug） | 不可预期 | 发布前在 RDNA/Arc 上回归探针 | ⚠️ 未实测 |
| 10 | **与光栅不一致**：PT 追全分辨率高度图，光栅在远处按 `skipScale` 抽稀甚至用 basemap 贴图 | 远景山脊/轮廓差异 | 参考渲染时把 `heightmapMaximumLOD = 0`、`heightmapPixelError` 调小；PT 侧保持全分辨率（本来也不需要 LOD） | ⚠️ 可接受 |
| 11 | **显存**：8 GB 卡上，4K 高度图 R32F 67 MB + RR 580 MB + 其他 RT | OOM/掉驱动 | 高度图用 R16；只上传一次 | ⚠️ 需实测 |

**性能预期（未实测，仅供排期参考）**：现有 PT 是 1 spp + RR，内部约 960×540 = 51.8 万主射线；若地形 patch 的 candidate 平均 ~1、每 candidate 走 ~10–40 个 cell，主路径增量在**亚毫秒到几毫秒**量级；阴影线数量与主射线同量级且 t 更短，增量应更小。真正的风险是「相机贴地朝远处看」的掠射情形，需要用 min/max 金字塔压。

---

## 9. 落地路线（可回滚，按风险递增）

| 阶段 | 内容 | 产出 | 粗估 |
|---|---|---|---|
| **P0** | 复跑探针（`Tools/vk_terrain_probe/build.ps1`）；在空场景放一个 Terrain，确认「PT 里地形不可见」的基线 | 环境/基线确认 | 0.5 d |
| **P1** | **网格化最小闭环**（≤513²）：C# 收集 Terrain → 生成网格 → 走现有 BLAS 通路；`MAT_IS_TERRAIN` + 4 层 splat；法线/UV 用解析值 | 视觉基线、G-buffer/RR 验证 | 2–3 d |
| **P2** | **AABB patch + DDA**：native AABB BLAS + 地形 SSBO + shader 求交 + 阴影路径；保留 P1 的网格路径做 A/B 开关 | B 方案可运行 | 3–4 d |
| **P3** | 孔洞、多地形、变换、acne/接缝打磨、性能采集（patch 尺寸/金字塔） | 可交付版本 | 1–2 d |
| **P4** | 文档 + 回归清单（含 AMD/Intel 待测项） | 文档 | 0.5 d |

**总计 6–10 人天**。如果只要「能看见地形」而不追求大地形，P1 单独就是可交付的（2–3 天）。

**建议的决策点**：P1 完成后先量一次「网格化地形在 513²/1025² 下的帧时间与显存」，再决定 P2 是否值得——如果目标场景的地形分辨率 ≤513²，P2 可以直接不做。

---

## 10. 需要实测的清单（本机）

1. **吞吐**：在真实 PT 场景里，地形 AABB 路径 vs 网格路径的 ms/帧（统一 1 spp、960×540、固定相机路径）。
2. **掠射**：相机贴地 1.7 m 朝地平线，统计 `candidate/ray` 与 `cells/ray`。
3. **acne**：`EPS_OFFSET = 5e-3` 是否够；斜坡（>45°）与陡崖处的阴影噪点。
4. **水密性**：地形边缘、patch 边界、孔洞边缘的漏光。
5. **splat 正确性**：4/8 层混合与 URP 光栅版的 A/B 对比。
6. **显存**：4K 高度图 R32F vs R16 下的总占用与是否 OOM。
7. **AMD/Intel**：探针在 RDNA/Arc 上复跑（AABB candidate、`generateIntersection` 行为）。
8. **DLSS RR**：地形大面积远景下的边缘抖动/拖影是否与现有场景一致。

---

## 11. 结论

- **可行，且已被本机实验证实**：Vulkan ray query 支持 AABB 程序化几何 + `generateIntersection`，与现有三角形 TLAS 可混挂；Unity 侧需要的全部地形数据（高度图、patch min/max、splat、孔洞）在 2022.3 都有公开 API。
- **但「程序化」必须落在「粗 patch」上**：每 patch 一个 AABB 时 BLAS 只有 **151.5 KB**（2049²），而全分辨率网格是 **559.6 MB**、每 cell 一个 AABB 则回到同量级。patch 粒度是这个方案的**核心参数**。
- **成本不在 API，在正确性**：ray query 没有 intersection shader/any-hit，水密性、acne、AABB 放大导致的误命中都要 shader 自己保证；阴影路径现有的「只处理三角形 candidate」假设必须改。
- **推荐策略**：≤513² 直接网格化（2–3 天，零风险）；≥1025² 走粗 AABB patch + DDA（6–10 天，含网格基线）。两条路共用同一套地形材质/UV/法线代码，先做网格化拿视觉基线、再决定是否投入 AABB，是风险最低的顺序。

---

## 12. 参考资料

**Vulkan / DXR 规范与最佳实践**

- Vulkan spec, Ray Traversal（AABB candidate → `OpRayQueryGenerateIntersectionKHR`、generated 不再确认、candidate 无序、AABB 边界可被放大、闭区间、起点在盒内必中）：<https://docs.vulkan.org/spec/latest/chapters/raytraversal.html>
- GLSL_EXT_ray_query（`rayQueryGenerateIntersectionEXT` / `rayQueryConfirmIntersectionEXT` 的适用条件）：<https://docs.vulkan.org/glslext/latest/glslext/ext/GLSL_EXT_ray_query.html>
- DXR 功能规范（`CANDIDATE_PROCEDURAL_PRIMITIVE`、`CommitProceduralPrimitiveHit`、`RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES`）：<https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html>
- DXR 1.1 发布博客（inline ray tracing 的定位与「procedural 通常更慢」）：<https://devblogs.microsoft.com/directx/dxr-1-1/>
- HPG 2020 "Ray tracing hardware is great!"（procedural/动态细分通常更差；动态 BVH ~60–80 B/三角形）：<https://www.highperformancegraphics.org/slides20/monday_gruen.pdf>
- NVIDIA RTXMU（BLAS compaction 44%–55%）：<https://developer.nvidia.com/blog/reducing-acceleration-structure-memory-with-nvidia-rtxmu>

**高度场求交算法**

- Tevs, Ihrke, Seidel, *Maximum Mipmaps for Fast, Accurate, and Scalable Dynamic Height Field Rendering*, I3D 2008：<https://dl.acm.org/doi/abs/10.1145/1342250.1342279>
- Dick et al., *GPU Ray-Casting for Scalable Terrain Rendering*, Eurographics 2009：<https://diglib.eg.org/items/2e7fb9c6-3bff-46e4-821e-80218187e456>
- Timokhin & Mikhaylyuk, 2023（Vulkan RT pipeline 里把高度场编码成 AABB、"transparent AABB"）：<https://link.springer.com/article/10.1134/S0361768823030064>
- Mitsuba 的解析高度场（min-max mipmap，无显式三角形）：<https://www.mitsuba-renderer.org/devblog/2013/11/>
- *Cool Patches*（ray/bilinear patch 精确求交，及与两三角形的对比）：<https://research.nvidia.com/sites/default/files/pubs/2019-03_Cool-Patches%3a-A//Chapter_08.pdf>

**引擎先例**

- Unity HDRP：`Terrain.enableHeightmapRayTracing`（2022.3 起公开 API，「Terrain heightmap geometries will be added in acceleration structures」）：<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Terrain-enableHeightmapRayTracing.html>
- Unity 特性矩阵（Unity 6.0 的 Raytracing 表里 Terrain 一行：HDRP = 「Heightmap only. No support for terrain details and terrain trees」；URP/Built-in = No）：<https://docs.unity3d.com/6000.0/Documentation/Manual/render-pipelines-feature-comparison.html>
- UE5 Lumen：Nanite landscape 与 heightfield 表示在 GI/RT 上的取舍：<https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine>

**Unity API（2022.3）**

- `TerrainData.heightmapResolution`（取值集合 33…4097）：<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TerrainData-heightmapResolution.html>
- `TerrainData.heightmapTexture`（`public RenderTexture`）：<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TerrainData-heightmapTexture.html>
- `TerrainData.GetPatchMinMaxHeights` / `OverrideMinMaxPatchHeights`、`alphamapTextures`、`holesTexture`：本机 `UnityEngine.TerrainModule.xml` 与 Scripting API 交叉核实
- `TerrainData` 源码（`holesResolution = heightmapResolution − 1`、`heightmapTexture` 为 get-only `RenderTexture`、`alphamapTextures` 是 `GetAlphamapTexture` 的包装）：<https://github.com/Unity-Technologies/UnityCsReference/blob/2022.3/Modules/Terrain/Public/TerrainData.bindings.cs>
- URP 14 Terrain 着色器（`UnpackHeightmap` 用法、孔洞 `clip(hole < 0.0005)`、splat 通道映射）：<https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.universal/Shaders/Terrain/TerrainLitInput.hlsl>
- core `Common.hlsl` 的 `PackHeightmap/UnpackHeightmap`（Vulkan/GLES 下 RG8 打包）：<https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.core/ShaderLibrary/Common.hlsl>
- Terrain colliders（「PhysX generates Terrain colliders from the heightmap data」，非 trimesh）：<https://docs.unity3d.com/2022.3/Documentation/Manual/terrain-colliders-introduction.html>
- `Terrain.heightmapMaximumLOD`（LOD1 高度图宽高减半、三角形数 1/4）：<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Terrain-heightmapMaximumLOD.html>

**本仓库内证据**

- 探针源码与实测输出：`Tools/vk_terrain_probe/`（`probe.cpp`、`probe.comp`、`sz.cpp`、`results_probe.txt`、`results_as_size.txt`）
- 现有几何管线与三角形假设：`Assets/Scripts/ModelManager.cs`、`NativePlugin/src/vpt_scene.cpp`、`NativePlugin/shaders/pathtrace.glsl`
- 相关既有调研：`doc/PT管线与Unity物理系统调研.md`（地形当前完全不进 PT）、`doc/DLSS4.5接入可行性调研.md`（显存基线）

---

## 13. 与「迁移到 ray tracing pipeline」的先后关系

> 追加于 2026-11-16（第二次讨论）。背景：计划把当前 ray query（compute + inline）管线改成 RT pipeline（raygen / closest-hit / any-hit / intersection / miss + SBT），并问「地形是不是放到迁移之后再适配」。

### 13.1 先纠正一个前提

「程序化几何体更适合 RT pipeline」在**写法**上成立：AABB + intersection shader（IS）本来就是 procedural 的原生形态。但在**必要性**上不成立——**ray query 已经支持 AABB 程序化几何**（§6.3 已在本机实测：AABB BLAS + candidate + `rayQueryGenerateIntersectionEXT` 全通）。所以「为了地形」并不是迁 RT pipeline 的充分理由。

两种后端对同一份地形数据的差别只有「谁做求交、属性怎么回来」：

| 项 | ray query（现状） | RT pipeline |
|---|---|---|
| 谁遍历 | `while (rayQueryProceedEXT)`，AABB 以 candidate 回到 shader | 硬件遍历，AABB 触发 **IS** 调用 |
| 谁求交 | shader 自己算 t，再 `rayQueryGenerateIntersectionEXT` | IS 里算 t，`ReportHit(t, hitKind, attr)` |
| 最近命中 | 自己保证 t 合法、自己挑最近 | 硬件按 t 取最近（同一 candidate 集合内） |
| 不透明/孔洞判定 | `generate` 之前自己做完（没有 any-hit） | **any-hit 里 `IgnoreHit()`**，天然支持 |
| 命中属性 | 无 barycentrics，自己从命中点反算 | `hitAttributeEXT` 传给 CH（本机上限 **32 B**） |
| 每次命中的额外开销 | 无 SBT，inline | SBT 间接 + IS/CH/AH 调用 |
| 代码形态 | 一个 compute shader 里 for 循环 | raygen + SBT 多阶段，状态经 payload |

### 13.2 对本项目：什么会变、什么不变

| 项 | 迁移后是否需要重做 |
|---|---|
| 地形 BLAS（`VK_GEOMETRY_TYPE_AABBS_KHR` + patch AABB buffer） | **不变**，两种后端完全一样 |
| 高度图 / patch 表 / splat / 孔洞数据上传 | **不变** |
| 高度场求交函数 `traceTerrain()`（slab + DDA + 两三角形） | 算法不变，**换入口**：candidate 循环里调用 → IS 里调用 |
| 材质混合 / UV / 法线（`getHitInfo` 的地形分支） | 逻辑不变，**换宿主**：compute 命中分支 → closest-hit |
| 透明阴影（现有 `traceShadowTransmission` 的 candidate 累积透射） | **必须重写**：改为 any-hit + `inout` payload + `IgnoreHit` |
| NEE / 体积 / 俄罗斯轮盘 / DLSS RR G-buffer 写入 | **必须重写**（搬进 CH 或 raygen + payload） |
| 多弹射循环 | 改写：compute for 循环 → 递归（本机 `maxRayRecursionDepth = 31`，够用）或 CH 内再 `traceRayEXT` |

### 13.3 结论：地形什么时候做

- **若 RT pipeline 迁移已经排期 → 地形放到迁移之后更划算。** 因为地形的求交、材质、阴影都要落进 hit shader；现在按 ray query 写一遍，迁移时等于重写一遍。
- **若迁移还停留在「想做」→ 不要等。** ray query 方案已验证，按 §9 的 P1（网格化，2–3 天）先拿到视觉基线；迁移时**网格地形路径 100% 保留**（三角形 + BLAS 在两个后端完全一致，只有着色入口要搬）。
- **若迁移的唯一动机就是地形 → 不值得。** 迁移本身（见 13.5）粗估 **2–4 周**，而地形两条路合计 6–10 人天；ray query 已经够用。

### 13.4 若确定迁移：推荐顺序

| 阶段 | 内容 | 地形状态 |
|---|---|---|
| **R0** | RT pipeline 骨架 + 同场景等价性回归（同 spp / 同相机 / 逐像素 diff） | 仍缺席（与现状一致） |
| **R1** | 地形**网格化**（三角形 BLAS + CH 着色）：后端无关，先拿视觉基线 | 可见 |
| **R2** | 地形 **AABB + IS**：`traceTerrain()` 搬进 IS，孔洞用 any-hit，属性走 `hitAttributeEXT` | 大地形可用 |
| **R3** | 孔洞 / 多地形 / 变换 / 性能调优 | 收尾 |

**现在就可以定的三件事**（迁移前后都不变，先定下来不会浪费）：

1. patch 布局（16² / 32² cell）与 patch AABB 的生成方式；
2. 地形数据 SSBO 布局（§7.2 的 binding 18–22）；
3. `traceTerrain(ro, rd, tMax, patchId) -> t / 命中信息` 的函数签名——两种后端共用同一份实现，只是外面包一层 candidate 循环或 IS 入口。

### 13.5 迁移本身的风险（与地形无关，但会拖累地形）

- 现有 `pathtrace.glsl` 约 1200 行、单个 compute shader（`local_size 8×8`），只有 2 处 `rayQueryInitializeEXT`；迁到 RT pipeline 要拆成 raygen/CH/AH/IS/MISS 五类、建 SBT、改描述符与管线创建，**粗估 2–4 周**（含等价性回归）。
- 本机 RT pipeline 能力：`shaderGroupHandleSize = 32`、`shaderGroupBaseAlignment = 64`、`maxRayRecursionDepth = 31`、`maxRayHitAttributeSize = 32`、`maxRayDispatchInvocationCount = 1,073,741,824`（`vulkaninfo` 实测）。
- **`hitAttributeEXT` 只有 32 B**：地形能塞下（`vec2 uv` + `uint patchId` + `float t`），但正常要留给更大属性的几何时要注意。
- **payload 上限是实现相关的**（本机 vulkaninfo 未列出该字段，需查 `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`）；稳妥做法是 CH 把命中信息写进 SSBO，payload 只传索引。
- **透明/孔洞的 any-hit 语义**与现在的 candidate 累积不同：`IgnoreHit()` 之后遍历继续，`RayFlagsTerminateOnFirstHit` 只在「未被忽略的命中」后终止——这正是想要的，但要逐条回归玻璃/体积材质。
- **性能方向不确定**：NVIDIA 官方口径是 inline 适合「shading 简单、shader 极少」的场景，RT pipeline 适合「多 shader、复杂 shading」；本项目 1 spp 路径追踪里每种材质最终只有一套着色，迁过去不一定更快，**必须实测**。
- 本机**没有** `VK_EXT_ray_flags_primitive_culling`，`gl_RayFlagsSkipAABBEXT` 不可用：迁移后阴影线同样无法「跳过地形」，只能靠两条 TLAS / 实例 mask / 在 AH 里快速判地形，这也是地形方案要一起定的设计点。

### 13.6 一句话

地形不是「更适合 RT pipeline」，而是「在 RT pipeline 里写法更自然」；**数据层与求交函数两种后端完全共用**。因此：迁移已定 → 先迁移、地形随后；迁移未定 → 先做地形网格化基线，AABB 部分等迁移方案定稿再写；迁移只为地形 → 不做。
