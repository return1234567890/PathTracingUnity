# PT 管线对 Unity 物理系统（碰撞体）的支持情况调研

> 调研对象：URPPT（Unity 2022.3 + URP 14.0.12 + Vulkan Ray Query 路径追踪 + DLSS RR）
> 调研时间：2026-11-16
> 调研范围：只回答「当前 PT 管线与 Unity 自带物理（PhysX / Collider / Rigidbody）是什么关系，给模型加碰撞体能否影响路径追踪」。不评估自研物理、不评估 GPU 粒子/布料等其它系统。
> 调研方法：静态代码审计（C# / C++ / GLSL 全链路）+ 已有日志与注释交叉验证。**未在编辑器里实际运行**，下文标注「需实测」的条目属于推断。

---

## 1. 结论速览

**一句话：PT 与 Unity 物理完全解耦。给模型加碰撞体对 PT 画面没有任何影响（不增加遮挡、不增加几何、也不影响性能）；PT 也不会反过来驱动物理或提供射线拾取。两者唯一的交汇点是 `Transform`。**

| 需求 / 用法 | 当前是否支持 | 说明 |
|---|---|---|
| 给已有渲染模型加 Box/Sphere/Capsule/MeshCollider，让物体参与物理 | ✅ 支持（但只是 Unity 侧） | 物理正常模拟；PT 画面不变。PT 只认 `MeshRenderer + Mesh`，从不读 Collider |
| 用碰撞体给 PT 当遮挡体 / 隐形墙（光被挡住、但看不见） | ❌ 不支持 | 只有 Collider、没有 `MeshFilter + MeshRenderer` 的物体，PT 里完全不存在，光线直接穿过 |
| 用简化 `MeshCollider` 代理几何降低 PT 求交成本 | ❌ 不支持 | PT 永远用渲染网格（`mesh.triangles`），不会读 `MeshCollider.sharedMesh` |
| 用 Layer / 物理层控制 PT 可见性 | ❌ 不支持 | PT 没有 LayerMask / 剔除掩码，只按 Tag（单一 Tag）+ 收集时 `renderer.enabled` 过滤 |
| Rigidbody 驱动的位置/旋转（掉落、堆叠、被推动） | ✅ 支持（有约束） | 每帧读 `Transform.localToWorldMatrix` → 重建 TLAS；但物体必须在 Play 前就存在 |
| 运行时 `Instantiate` 新物体进入 PT | ❌ 不支持 | `BuildScene()` 只在 `Start()` 跑一次；native 侧没有「新增 mesh / 新增实例」的 API |
| 运行时销毁物体 | ⚠️ 部分支持 | 销毁后从实例表跳过 → TLAS 少一个实例；但「所有实例都被跳过」时会保留上一帧 TLAS（残影） |
| 物理驱动的形变（Cloth、Softbody、BlendShape、脚本改顶点） | ❌ 不支持 | 只有骨骼蒙皮一条变形通路（关节矩阵 + BLAS refit），网格顶点只在构建时读一次 |
| Terrain / TerrainCollider | ❌ 不支持 | Terrain 没有 `MeshFilter`，收集器收不到；加 Collider 也不会让它进 PT |
| 物理相机（CharacterController / 碰撞检测相机） | ✅ 支持 | PT 每帧读相机位姿，物理驱动的相机移动能被正确渲染；但现有 `CameraController` 是自由飞行、无碰撞 |
| `Physics.Raycast` 做拾取/交互 | ✅ 可用（与 PT 无关） | 坐标系一致（世界空间），可以正常用；但命中结果是碰撞体，不是 PT 几何 |
| PT 结果参与物理（例如按 PT 深度做碰撞） | ❌ 不支持 | PT 只输出颜色/G-buffer 给 DLSS RR，不回读 CPU |

> 现状背景：`ProjectSettings` 与 `Packages/manifest.json` 里 `com.unity.modules.physics` 是启用的，PhysX 完全可用；但 `Assets/` 与 `NativePlugin/src/` 中**没有任何一处** `Collider` / `Physics.` / `Raycast` / `Rigidbody` 调用（唯一命中是 FBX 导入设置里的 `addColliders: 0`）。也就是说，本项目至今没有写过一行「PT ↔ 物理」的桥接代码。

---

## 2. PT 的几何到底从哪来（证据链）

### 2.1 场景收集只发生一次

`ModelManager.Start()` → `BuildScene()`（[ModelManager.cs:213](../Assets/Scripts/ModelManager.cs#L213-L216)）。采集对象是：

| 收集器 | 代码 | 收进来的东西 |
|---|---|---|
| `FindObjectsOfType<MeshFilter>()` | [ModelManager.cs:324](../Assets/Scripts/ModelManager.cs#L324) | 静态网格：`sharedMesh.vertices / normals / tangents / uv / colors / triangles` |
| `FindObjectsOfType<SkinnedMeshRenderer>()` | [ModelManager.cs:406](../Assets/Scripts/ModelManager.cs#L406) | 骨骼网格：bind-pose 顶点 + `bones` + `bindposes` + 骨骼权重 |

过滤条件（任何一条不满足就跳过，且**与物理无关**）：

- Tag 匹配 `targetTag`（[L348-352](../Assets/Scripts/ModelManager.cs#L348-L352)，只能设一个 Tag）；
- `mesh.isReadable == true`（[L340-345](../Assets/Scripts/ModelManager.cs#L340-L345)）——Read/Write 关闭的模型连顶点都读不到，直接不进 PT；
- 物体 `activeInHierarchy`，且 `MeshRenderer.enabled`（[L355-357](../Assets/Scripts/ModelManager.cs#L355-L357)）；
- 退化 mesh（顶点 0 / 索引 < 3）跳过，避免 blasID 错位（[L360-373](../Assets/Scripts/ModelManager.cs#L360-L373)）。

### 2.2 几何 → BLAS → 实例 → TLAS（全部 native 自建）

```
Mesh 顶点/索引 ──追加到全局数组──> BLAS 区间登记（vertBase/triBase）
                                        │
MeshRenderer.transform + materialID ────┴──> 实例表 _instances
                                        │
每帧：Transform.localToWorldMatrix ──> VPT_UpdateInstances ──> 重建 TLAS
```

- BLAS 按 `Mesh` 去重（同一 mesh 的多个实例共享 BLAS），登记见 [RegisterBlas / AddInstance](../Assets/Scripts/ModelManager.cs#L602-L624)；
- 每帧同步在渲染前执行：`mm.SyncInstancesNative()` / `mm.SyncSkinnedMeshesNative()`（[PathTraceRendererFeature.cs:489-495](../Assets/Scripts/PathTraceRendererFeature.cs#L489-L495)）；
- 同步内容**只有** `materialID`、`blasID`、3×4 世界矩阵（[ModelManager.cs:1155-1177](../Assets/Scripts/ModelManager.cs#L1155-L1177)），遍历时只判断 `t != null && activeInHierarchy`（[L1160](../Assets/Scripts/ModelManager.cs#L1160)）；
- native 侧 `UpdateInstances` 写主机侧实例数组，`RecordTLASUpdateIfNeeded` 每帧全量重建 TLAS（[vpt_scene.cpp:1678](../NativePlugin/src/vpt_scene.cpp#L1678-L1756)，注释见 [L778](../NativePlugin/src/vpt_scene.cpp#L778)）。

**这条链上没有任何 PhysX 结构（PxScene / PxRigidActor / PxShape）参与**：Vulkan 的 BLAS/TLAS 是原生插件自己 `vkCmdBuildAccelerationStructuresKHR` 建的，与 Unity 物理的 PxScene 是两套互不相通的加速结构。

### 2.3 光栅几乎不画，所以「碰撞体看不见」是彻底的

`PathTraceCameraGate` 在 Play 时把 Game 相机 `cullingMask = 0`、关阴影/后处理/天空盒，并切到只含 PT feature 的 Renderer（[PathTraceCameraGate.cs:96-103](../Assets/Scripts/PathTraceCameraGate.cs#L96-L103)）。因此 Game 视图里的画面**100% 来自 PT**：

- 只有 Collider 的物体：PT 里没有几何 → 完全不可见，连阴影都没有；
- 有 MeshRenderer 的物体：PT 正常渲染，有没有 Collider 都一样；
- 物理调试可视化（Physics Debugger / Collider Gizmo）在 Game 视图看不到，只能在 Scene 视图看。

---

## 3. 为什么「加碰撞体」不会影响 PT（架构层面的三个断层）

1. **数据源不同**：PT 读的是 `Mesh`（渲染网格），物理读的是 `Collider`（PhysX shape）。两者在 Unity 里本来就是独立的两份数据，BoxCollider 甚至没有对应的三角形。
2. **加速结构不同**：PT 用自己的 Vulkan BLAS/TLAS；PhysX 用 PxScene 的 BVH。PT 的求交不可能命中 PhysX 的 shape。
3. **没有同步通道**：没有「Collider → PT 几何」的注册代码，也没有「PT 命中 → 物理事件」的回读代码。唯一的共享数据是 `Transform`（刚体位姿）。

---

## 4. 物理驱动动态物体的实际支持度与限制

PT 对「动态场景」的支持停在**刚体变换**这一层：

| 能力 | 状态 | 依据 |
|---|---|---|
| 平移 / 旋转 / 父子层级 / 均匀缩放 | ✅ | 每帧读 `localToWorldMatrix` 写 TLAS 实例（[ModelManager.cs:1163-1174](../Assets/Scripts/ModelManager.cs#L1163-L1174)） |
| 非均匀缩放 | ⚠️ 位置正确、光照可能错 | `transformDir()` 直接用同一 3×4 变换方向，没有逆转置（[pathtrace.glsl:173-181](../NativePlugin/shaders/pathtrace.glsl#L173-L181)），非均匀缩放下法线/切线会偏 |
| Play 前就存在的物体 | ✅ 必需 | `BuildScene()` 仅 `Start()` 调用一次（[L215](../Assets/Scripts/ModelManager.cs#L215)） |
| 运行时新增物体 | ❌ | native 无「新增 mesh/BLAS」API，`VPT_UpdateInstances` 只能复用已登记的 `blasID`（[NativeBridge.cs](../Assets/Scripts/NativeBridge.cs) 导出列表） |
| 运行时销毁物体 | ⚠️ | 实例被跳过 → TLAS 实例数变少；若全部被跳过则 `written == 0` 不调用更新，**上一帧 TLAS 会保留 → 残影**（[ModelManager.cs:1176-1177](../Assets/Scripts/ModelManager.cs#L1176-L1177)） |
| 运行时 `renderer.enabled = false` | ❌ 无效 | 只在构建时检查一次；每帧同步只查 `activeInHierarchy`（[L1160](../Assets/Scripts/ModelManager.cs#L1160)） |
| 运行时 `SetActive(false)` | ✅ 生效 | 同上，实例被跳过 |
| 运行时改材质 / 贴图 / 换 mesh | ❌ 无效 | 材质参数与纹理数组在 `BuildScene()` 一次性打包（见 [L218-228](../Assets/Scripts/ModelManager.cs#L218-L228) 关于运行中重建会 TDR 的说明） |
| 骨骼蒙皮形变 | ✅ | GPU 蒙皮 compute + BLAS refit（[skinning.comp.glsl](../NativePlugin/shaders/skinning.comp.glsl)） |
| Cloth / Softbody / BlendShape / 脚本改顶点 | ❌ | 没有顶点回读/重建路径；BlendShape 不被应用 |
| Terrain | ❌ | 没有 `MeshFilter`，收集器收不到 |

**另一个与「物理动态」直接相关的坑：运动矢量只含相机重投影。** `DlssAux.compute` 的 MotionVector kernel 用当前帧世界坐标 + 上一帧 viewProj 对称投影（[DlssAux.compute:73-100](../Assets/Shaders/DlssAux.compute#L73-L100)），**没有物体自身运动矢量**。刚体快速移动时，DLSS RR 拿到的 MV ≈ 0（相对相机），预期会出现拖影/时域滞后；静态场景与纯相机运动不受影响。（需实测确认 RR 的 disocclusion 逻辑能压住多少。）

---

## 5. 三种典型诉求的判断

### 5.1 「我要让物体有物理、能掉落/堆叠，PT 里也要正常显示」

**可以做，不用改代码。** 保持物体在 Play 前存在于场景、mesh 开 Read/Write、有 `MeshRenderer`，然后随便加 Collider + Rigidbody。PT 每帧跟随 Transform；注意：

- 别在运行时 Instantiate 新物体（不会出现在 PT）；
- 快速运动时 RR 可能拖影（见上）；
- 非均匀缩放的物体会光照异常。

### 5.2 「我要一堵隐形墙挡住光/视线，但玩家看不见」

**当前做不到。** 物理侧加 Collider 能挡住带 Rigidbody/CharacterController 的物体（注意：现有 `CameraController` 是自由飞行、没有碰撞体，相机本身不会被挡住），但 PT 光线直接穿过去。可选替代：

- 放一个真实的低模 mesh 当墙（可见，能挡光）——最省事；
- 或者按 §6 方案 A 给 PT 加「不可见但参与求交」的材质/代理几何。

### 5.3 「我想让 PT 用 MeshCollider 的简化网格来省性能」

**当前做不到，而且方向也不对。** PT 的成本主要来自 BLAS 三角形数与 TLAS 遍历，简化代理确实能省，但要新增「Collider → 代理 BLAS」通路（方案 A），并且要处理代理与渲染网格的重叠（双重遮挡、自阴影、阴影 any-hit 重复计数）。

---

## 6. 如果确实需要「碰撞体进入 PT」：可选改造方案

> 以下均为**方案评估**，当前代码未实现。工作量按本仓库现状粗估，仅供排期参考。

### 方案 A：Collider → 代理三角网格（推荐，改动最小）

- **做法**：在 `BuildScene()` 里额外遍历 `Collider`，按类型程序化生成三角形：
  - `BoxCollider` → 12 三角形；`SphereCollider` → 经纬球（如 32×16）；`CapsuleCollider` → 圆柱 + 两个半球；`MeshCollider.sharedMesh` → 直接取其三角形；
  - 生成结果走现有 `RegisterBlas + AddInstance` 通路，不需要动 native 代码（Vulkan 侧只认三角形 BLAS）；
  - 在 `PathMaterial` 的 flags 里加一个 `MAT_INVISIBLE`（或复用 `MAT_IS_TRANSPARENT` + alpha=0），在 `pathtrace.glsl` 的主路径命中分支里「命中即继续」，阴影 any-hit 分支仍按不透明遮挡处理，就能得到「挡光但不可见」；
- **改动点**：`ModelManager.cs`（收集 + 代理生成）、`PathMaterial` 结构（flags 位）、`pathtrace.glsl`（命中分支）。native C++ 基本不用动；
- **风险**：代理与渲染网格重复（同一物体既有 mesh 又有 collider 会自我遮挡/自阴影，需要在收集时去重或只处理「无 MeshRenderer 的 Collider」）；`MAT_INVISIBLE` 与透明材质/体积玻璃的交互要单独回归；
- **粗估**：1～2 人天（含场景验证）。

### 方案 B：运行时增删实例 / 新增 mesh

- 需要 native 侧新增 `VPT_AddMesh / VPT_RemoveMesh / VPT_AddInstance / VPT_RemoveInstance`，处理 BLAS 构建/释放、TLAS 环形资源扩容、实例索引重排；
- 现状 `VPT_UpdateInstances` 只接受「已登记的 blasID」，扩容还要在渲染线程 `vkDeviceWaitIdle`（[vpt_scene.cpp:1716-1748](../NativePlugin/src/vpt_scene.cpp#L1716-L1748)），运行时频繁增删有卡顿/TDR 风险；
- **粗估**：3～5 人天，风险中等。

### 方案 C：物理形变（Cloth / Softbody / 顶点动画）

- 需要每帧把变形后的顶点回读并上传 + BLAS rebuild/refit；当前只有骨骼关节矩阵这一条轻量通路；
- 顶点回读本身就要 CPU 同步，与「实时 PT」冲突较大；
- **不建议**在本项目当前阶段做。

### 方案 D：用 PhysX 的 PxScene 给 PT 做遮挡查询

- 不可行：PT 求交在 GPU（Vulkan Ray Query），PhysX 场景在 CPU，且 shape 与三角形加速结构不通用。

---

## 7. 建议的实测验证清单（约 10 分钟）

> 静态审计已能定性，以下步骤用于在编辑器里复核，并可用于回归。

1. **碰撞体不影响 PT**：在 SampleScene 放一个空 GameObject，只加 `BoxCollider`（不加 MeshRenderer），位置放在相机正前方。预期：PT 画面完全不变；Play 时扔一个 Rigidbody 小球会发现它被挡住（物理生效），但 PT 里看不到这堵墙。
2. **几何来源是 Mesh 而非 Collider**：给已有模型（如 church3 的某个子物体）加 `MeshCollider/BoxCollider`。预期：PT 画面与性能都无变化。
3. **运行时可见性**：对某个已收集物体，运行时分别执行 `renderer.enabled = false` 和 `SetActive(false)`。预期：前者 PT 仍显示，后者 PT 消失。
4. **刚体跟随**：场景里预置一个 `Rigidbody` + `MeshRenderer` 的球，从空中落下。预期：PT 中位置跟随；快速下落时观察 RR 是否拖影（对应 §4 的 MV 限制）。
5. **运行时新增**：Play 中 `Instantiate` 一个带 MeshRenderer 的物体。预期：PT 里不出现（光栅也被 Gate 关了，所以整个 Game 视图都看不到）。
6. **无地形**：放一个 Terrain（自带 TerrainCollider）。预期：PT 里没有地形，光直接穿到天空。

> 日志辅助：`ModelManager.logBuildStats` 会打印 mesh / material / instance 数量；`VptTrace` 的 `BuildScene` 阶段日志在 `<项目根>/Logs/VPT-trace-csharp.log`。

---

## 8. 最终结论

- **不支持。** 当前 PT 管线没有接入 Unity 物理系统：碰撞体既不参与 PT 的几何与遮挡，PT 也不读物理、不回馈物理。给模型加碰撞体对画面**零影响**，只影响 Unity 侧的物理行为。
- 二者唯一的联动是 `Transform`：物理驱动的刚体位移/旋转能被 PT 逐帧跟随（前提是物体在 Play 前已在场景中、mesh 可读、且只做刚性变换）。
- 如果需求是「物理玩法 + PT 画面」，现在就能用，不需要改代码；如果需求是「碰撞体当 PT 遮挡/隐形墙/简化代理」，属于**新功能**，建议按方案 A 落地。
- 待实测项：移动刚体在 DLSS RR 下的拖影程度、非均匀缩放的光照偏差、全实例被销毁时的残影边界。
