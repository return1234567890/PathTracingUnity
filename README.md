# URPPT — Unity URP 路径追踪实验项目

URPPT 是一个基于 **Unity URP（Universal Render Pipeline）** 的实时光线追踪/路径追踪实验项目。项目通过自定义 Renderer Feature 将路径追踪结果接入 URP 渲染管线，**路径追踪的求交与着色逻辑全部由 C++ 原生插件（Vulkan Ray Tracing Pipeline）完成**，Unity 侧通过 P/Invoke 桥接调用。

> **项目状态：只维护 Vulkan 侧路径追踪。**
> 早期的 Unity 计算着色器（HLSL 求交）软件路径追踪链路、自研时域累积/空域滤波降噪、ReSTIR GI，以及其依赖的 tinybvh 加速结构库，均已从工程中移除。
> DLSS 侧只保留 Ray Reconstruction（RR）；DLSS SR 与 debug 深度图验证分支同样已删除。
> **2026-11-21：PT 后端已从 `VK_KHR_ray_query` + compute 整体切换到 `VK_KHR_ray_tracing_pipeline`（raygen 迭代弹射 + 延迟着色 + SBT）。** 设计见 [doc/RT_pipeline整体切换实现方案.md](doc/RT_pipeline整体切换实现方案.md)。

## 当前技术路线

- ✅ **路径追踪**：Vulkan **Ray Tracing Pipeline**（raygen 迭代弹射 + closest-hit 只记录命中 + any-hit 做阴影透射），BLAS/TLAS 由原生插件侧自建与重建
- ✅ **着色器**：Slang 单模块（`NativePlugin/shaders/pt.slang`），7 个 RT 入口共用一份 SPIR-V
- ✅ **降噪/重建**：NVIDIA DLSS Ray Reconstruction（DLSS RR）作为唯一方案
- ❌ 不再包含软件路径追踪（HLSL 求交 + tinybvh BVH）
- ❌ 不再支持 DLSS SR（超分）模式
- ❌ 不再使用 ReSTIR 系列算法
- ❌ 不再维护自研的时域累积与空域滤波降噪管线

DLSS RR 的集成踩坑记录可参考 `doc/` 目录下的相关文档。

## 项目结构

```
URPPT/
├── Assets/
│   ├── Scripts/                  # Unity C# 侧
│   │   ├── PathTraceRendererFeature.cs   # URP Renderer Feature，PT + DLSS 调度
│   │   ├── NativeBridge.cs               # C# ↔ 原生插件桥接
│   │   ├── TextureArrayManager.cs        # 全局纹理数组（材质贴图上传）
│   │   ├── LightManager.cs / ModelManager.cs / CameraController.cs
│   │   └── ...
│   ├── Shaders/
│   │   └── DlssAux.compute       # 仅 MotionVector kernel（DLSS 辅助图，无路径追踪）
│   └── Scenes/                   # 示例场景
├── NativePlugin/                 # C++ 原生插件（路径追踪核心）
│   ├── src/                      # 插件源码（RT pipeline、SW 加速结构、DLSS 集成等）
│   │   ├── vpt_intercept.cpp     # vkCreateDevice 拦截：注入 RT 扩展 + feature
│   │   ├── vpt_rt.cpp            # RT 扩展函数加载 + SBT 生成
│   │   ├── vpt_dispatch.cpp      # RT pipeline / 描述符 / traceRays / barrier
│   │   ├── vpt_scene.cpp         # BLAS/TLAS 构建与 refit
│   │   └── pathtrace_spv.h       # 内嵌 SPIR-V（由 build_shaders.ps1 生成）
│   ├── shaders/
│   │   ├── pt.slang              # 7 个 RT 入口（rgen/rmiss×2/rchit×2/rahit×2）
│   │   ├── pt_common.slang       # 材质/BSDF/BTDF/体积/环境采样等公共代码
│   │   ├── build_shaders.ps1     # slangc → pt.spv → pathtrace_spv.h
│   │   └── skinning.comp.glsl    # 骨骼蒙皮 compute（独立 GLSL，与本模块无关）
│   ├── third_party/ngx/          # NVIDIA NGX SDK（DLSS）
│   └── CMakeLists.txt
├── Tools/
│   └── rt_probe/                 # RT pipeline 离线自检（不需要 Unity）
└── doc/                          # 集成记录与调研文档
```

## 技术要点

- **渲染管线**：Unity 2022.3 LTS + URP，通过 `PathTraceRendererFeature` 注入自定义渲染 Pass
- **路径追踪**：Vulkan **Ray Tracing Pipeline**（`vkCmdTraceRaysKHR`）实现；着色全部在 raygen，closest-hit 只回写 `{hitT, instance, prim, bary}`，材质/BSDF/纹理/光源采样都在 raygen（延迟着色）；阴影用 any-hit 逐层累积 Beer-Lambert 透射率
- **SBT**：5 个 group（raygen / miss×2 / hit group×2），`maxPipelineRayRecursionDepth = 1`；对齐参数全部从 `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` 取
- **加速结构**：BLAS/TLAS 由原生插件用 `vkCmdBuildAccelerationStructuresKHR` 构建，支持动态场景 TLAS 重建与骨骼动画 BLAS refit
- **降噪重建**：NVIDIA DLSS Ray Reconstruction（NGX SDK）；运动矢量/深度由 `DlssAux.compute` 从 native 写出的 G-buffer 生成
- **材质**：支持 URP Lit 的 **Alpha Clipping（镂空）**：镂空实例不标 `FORCE_OPAQUE`，主光线由 any-hit（`rahitMain`）采样 alpha 并 `IgnoreHit()` 丢弃 `alpha < _Cutoff` 的微面；阴影侧同一套逻辑在 `rahitShadow`；玻璃透射仍走原 BSDF
- **原生桥接**：CMake 构建 Windows 原生 DLL，C# 通过 P/Invoke 调用

## 环境要求

- Unity 2022.3.62f1 及以上（URP）
- Visual Studio 2022（MSVC v143）+ CMake 3.15+
- **NVIDIA RTX 20 系及以上**显卡（需要 `VK_KHR_ray_tracing_pipeline` + `VK_KHR_acceleration_structure`）
  + 支持 DLSS RR 的驱动（建议最新 Game Ready / Studio）
- 着色器构建需要 **Slang**：Vulkan SDK 1.4.313.2 自带的 `slangc`（`build_shaders.ps1` 会用到）

## 着色器与自检

```powershell
# 1) Slang → SPIR-V → src/pathtrace_spv.h（改完 pt.slang / pt_common.slang 必跑）
powershell -ExecutionPolicy Bypass -File NativePlugin\shaders\build_shaders.ps1

# 2) 重建原生插件（会顺带把 DLL 拷到 Assets/Plugins/x86_64）
cmake --build NativePlugin\build --config Release

# 3) 不开 Unity 的 RT 自检（闸门 G0/G1/G2 + traceRays 冒烟）
cmake -S Tools\rt_probe -B Tools\rt_probe\build
cmake --build Tools\rt_probe\build --config Release
Tools\rt_probe\build\bin\Release\rt_probe.exe NativePlugin\shaders\pt.spv --trace
```

`rt_probe` 会验证：RT 扩展/feature 能建设备、7 个入口能链接、SBT 句柄与对齐正确、`vkCmdTraceRaysKHR`
能跑出有限且非零的 radiance，并顺带用非对称 `viewInv` 检查 UBO 矩阵方向（防 `row_major` 被转置）。

## 相关文档

- `doc/DLSS_RR集成踩坑记录.md` — DLSS RR 集成记录
- `doc/DLSS_RR亮区抖动分析.md`、`doc/DLSS_抖动修复踩坑记录.md` — DLSS 抖动问题排查
- `doc/PT半透明材质渲染流程.md` — 半透明材质（薄片 / 体积）在当前 PT 中的完整渲染流程、公式与坑
- `doc/PT镂空材质支持说明.md` — URP Lit Alpha Clipping（镂空）的用法、实现、限制与验证清单
- `doc/RT_pipeline整体切换可行性调研.md` — 参考 vk_gltf_renderer 的 Ray Tracing Pipeline 迁移可行性、SER/OMM 名词解释与性能实测对比
- `doc/RT_pipeline整体切换实现方案.md` — 本次整体切换的设计、阶段计划、风险与验收清单（末尾附实施记录与偏差说明）
- `doc/PT管线与Unity物理系统调研.md` — PT 与 Unity 物理（碰撞体）的关系与改造方案
- `doc/PT体积雾接入可行性调研.md` — 体积雾接入 PT 的四种形态、解析高度雾/自由程采样的落地方案、成本与 RR 风险
- `doc/程序化几何体加速结构接入Terrain可行性调研.md` — 地形以 AABB 程序化几何接入 PT 的可行性、实测数据与落地路线（探针见 `Tools/vk_terrain_probe/`）

## 声明

本项目为个人学习与实验性质，用于探索实时路径追踪在 Unity URP 中的可行性。所有路径追踪相关开发均围绕 Vulkan Ray Tracing Pipeline + DLSS RR 路线进行。
