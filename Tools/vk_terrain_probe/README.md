# vk_terrain_probe — 地形作为「程序化几何体加速结构」的可行性探针

配套调研文档：[`doc/程序化几何体加速结构接入Terrain可行性调研.md`](../../doc/程序化几何体加速结构接入Terrain可行性调研.md)

## 这是什么

两个独立的 Vulkan 小程序，用来在**不改动 URPPT 插件**的前提下回答两个问题：

1. `probe`：`VK_KHR_ray_query` 能否用 **AABB(procedural) 几何** 代替三角形网格做地形求交？
   —— 构建 AABB BLAS、混挂三角形 BLAS、在 candidate 循环里跑高度场求交、用
   `rayQueryGenerateIntersectionEXT` 提交命中，覆盖 FORCE_OPAQUE / TerminateOnFirstHit / miss。
2. `sz`：在同一台机器/同一驱动上，三角网格地形与 AABB patch 的 **BLAS 尺寸**差多少？
   —— 只调用 `vkGetAccelerationStructureBuildSizesKHR`，不需要真实数据。

## 环境

- Windows + VS2022（MSVC v143）
- Vulkan SDK（默认 `C:\VulkanSDK\1.4.313.2`，含 `glslangValidator.exe` 与 `vulkan-1.lib`）
- 支持 `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` 的 GPU

## 构建与运行

```powershell
# 在 VS2022 x64 开发者命令行里
glslangValidator -V --target-env vulkan1.2 probe.comp -o probe.spv
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 probe.cpp /I "%VULKAN_SDK%\Include" /Fe:probe.exe /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib
cl /nologo /utf-8 /std:c++17 /EHsc /O2 sz.cpp    /I "%VULKAN_SDK%\Include" /Fe:sz.exe    /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib

.\probe.exe   # 8 条射线，打印 candidate / committed / t / 法线，最后 ALL CHECKS PASSED
.\sz.exe      # 打印两种表示的 BLAS 尺寸表
```

也可以直接 `powershell -ExecutionPolicy Bypass -File .\build.ps1`（内部走 `vcvars64.bat`；有 PowerShell 7 时 `pwsh -File` 亦可）。

> 源码是 UTF-8 无 BOM + 中文注释，MSVC 必须加 `/utf-8`（否则会按 GBK 解析注释、报出莫名其妙的语法错误）。

## 结果（本机：RTX 3070 Ti Laptop / GRD 616.92 / Vulkan 1.4.351）

`results_probe.txt`：8 条检查全部 PASS，包括

- 垂直/斜射命中地形（`committed = GENERATED`，t 与解析值一致）；
- AABB candidate 确实被投递（`gl_RayQueryCandidateIntersectionAABBEXT`）；
- 与三角形 BLAS 混挂在同一个 TLAS 中互不干扰；
- `FORCE_OPAQUE` 实例上的 AABB 命中依旧走 `generateIntersection`；
- `TerminateOnFirstHit` 的阴影式遍历正常；
- 空地/向上射线正确 miss。

`results_as_size.txt`：`FAST_TRACE`、全分辨率、无 LOD：

| 高度图 | 三角形数 | 网格 BLAS | 网格 scratch | 顶点+索引(80B/12B) |
|---|---|---|---|---|
| 513² | 52.4 万 | 35.0 MB | 10.6 MB | 23.1 MB |
| 1025² | 209.7 万 | 139.9 MB | 42.3 MB | 92.2 MB |
| 2049² | 838.9 万 | 559.6 MB | 169.3 MB | 368.4 MB |
| 4097² | 3355 万 | 2238.3 MB | 677.0 MB | 1472.7 MB |

| 高度图 | patch = 8² cell | 16² cell | 32² cell |
|---|---|---|---|
| 513² | 4096 prim → 151.5 KB | 1024 → 39.2 KB | 256 → 11.2 KB |
| 1025² | 16384 → 600.5 KB | 4096 → 151.5 KB | 1024 → 39.2 KB |
| 2049² | 65536 → 2396.4 KB | 16384 → 600.5 KB | 4096 → 151.5 KB |

结论：**AABB patch 的 BLAS 比全分辨率网格小 3 个数量级**，但前提是「一个 patch 一个 AABB」，
而不是「一个 cell 一个 AABB」（后者 BVH 开销与网格同量级）。

## 已知边界（探针不验证）

- 只在 NVIDIA + Windows 驱动上跑过；AMD/Intel 的 AABB candidate 行为需要另测。
- `probe.comp` 的地形求交用的是**固定步长 march**（便于隔离 API 问题），不是生产用的
  DDA + min/max 金字塔；生产实现要点见调研文档 §7。
- 没有测吞吐（rays/ms），只测了功能正确性与 BLAS 尺寸。

