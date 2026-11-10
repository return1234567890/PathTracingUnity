# URPPT — Unity URP 路径追踪实验项目

URPPT 是一个基于 **Unity URP（Universal Render Pipeline）** 的实时光线追踪/路径追踪实验项目。项目通过自定义 Renderer Feature 将路径追踪结果接入 URP 渲染管线，**路径追踪的求交与着色逻辑全部由 C++ 原生插件（Vulkan Ray Query）完成**，Unity 侧通过 P/Invoke 桥接调用。

> **项目状态：只维护 Vulkan 侧路径追踪。**
> 早期的 Unity 计算着色器（HLSL 求交）软件路径追踪链路、自研时域累积/空域滤波降噪、ReSTIR GI，以及其依赖的 tinybvh 加速结构库，均已从工程中移除。
> DLSS 侧只保留 Ray Reconstruction（RR）；DLSS SR 与 debug 深度图验证分支同样已删除。

## 当前技术路线

- ✅ **路径追踪**：Vulkan Ray Query 硬件加速，BLAS/TLAS 由原生插件侧自建与重建
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
│   ├── src/                      # 插件源码（Vulkan Ray Query、BLAS/TLAS、DLSS 集成等）
│   ├── shaders/                  # Vulkan compute/GLSL 着色器
│   ├── third_party/ngx/          # NVIDIA NGX SDK（DLSS）
│   └── CMakeLists.txt
└── doc/                          # DLSS 集成与问题排查文档
```

## 技术要点

- **渲染管线**：Unity 2022.3 LTS + URP，通过 `PathTraceRendererFeature` 注入自定义渲染 Pass
- **路径追踪**：Vulkan Ray Query 实现，多弹射路径追踪与光源采样在原生侧 compute shader 中完成
- **加速结构**：BLAS/TLAS 由原生插件用 `vkCmdBuildAccelerationStructuresKHR` 构建，支持动态场景 TLAS 重建与骨骼动画 BLAS refit
- **降噪重建**：NVIDIA DLSS Ray Reconstruction（NGX SDK）；运动矢量/深度由 `DlssAux.compute` 从 native 写出的 G-buffer 生成
- **原生桥接**：CMake 构建 Windows 原生 DLL，C# 通过 P/Invoke 调用

## 环境要求

- Unity 2022.3.62f1 及以上（URP）
- Visual Studio 2022（MSVC v143）+ CMake 3.15+
- 支持 Vulkan Ray Query 与 DLSS RR 的 NVIDIA 显卡（RTX 系列）及最新驱动

## 相关文档

- `doc/DLSS_RR集成踩坑记录.md` — DLSS RR 集成记录
- `doc/DLSS_RR亮区抖动分析.md`、`doc/DLSS_抖动修复踩坑记录.md` — DLSS 抖动问题排查
- `半透明BSDF实现方案.md` — 透射/半透明 BSDF 设计参考（现实现位于 `NativePlugin/shaders/pathtrace.glsl`）

## 声明

本项目为个人学习与实验性质，用于探索实时路径追踪在 Unity URP 中的可行性。所有路径追踪相关开发均围绕 Vulkan Ray Query + DLSS RR 路线进行。
