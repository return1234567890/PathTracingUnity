# URPPT — Unity URP 路径追踪实验项目

URPPT 是一个基于 **Unity URP（Universal Render Pipeline）** 的实时光线追踪/路径追踪实验项目。项目通过自定义 Renderer Feature 将路径追踪结果接入 URP 渲染管线，核心的求交与着色逻辑由 C++ 原生插件（Vulkan Ray Query）完成，Unity 侧通过 P/Invoke 桥接调用。

> **项目状态：纯软件侧方案已停止维护。**

## 当前技术路线

项目早期探索过多种降噪与重建方案（ReSTIR GI、NRD 算法移植、自研的时域累积 + 空域滤波管线等），经过对比评估后，**最终确定采用 NVIDIA DLSS Ray Reconstruction（DLSS RR）作为唯一的降噪/重建方案**：

- ❌ 不再使用 ReSTIR 系列算法
- ❌ 不再维护自研的时域累积与空域滤波降噪管线
- ✅ **DLSS RR 作为最终方案**，负责路径追踪原始信号的重建与降噪

DLSS RR 的集成踩坑记录可参考 `doc/` 目录下的相关文档。

## 项目结构

```
URPPT/
├── Assets/
│   ├── Scripts/                  # Unity C# 侧
│   │   ├── PathTraceRendererFeature.cs   # URP Renderer Feature，路径追踪 pass 接入点
│   │   ├── NativeBridge.cs               # C# ↔ 原生插件桥接
│   │   ├── RenderBufferManager.cs        # 渲染缓冲管理
│   │   ├── TextureArrayManager.cs        # 全局纹理数组（材质贴图上传）
│   │   ├── TinyBVH.cs / TinyBVHNative.cs # tinybvh C# 封装与 P/Invoke 绑定
│   │   ├── LightManager.cs / ModelManager.cs / CameraController.cs
│   │   └── TinyScene.cs
│   ├── Shaders/                  # Shader 相关
│   └── Scenes/                   # 示例场景
├── NativePlugin/                 # C++ 原生插件（路径追踪核心）
│   ├── src/                      # 插件源码（Vulkan Ray Query、DLSS 集成等）
│   ├── shaders/                  # Vulkan compute/GLSL 着色器
│   ├── third_party/ngx/          # NVIDIA NGX SDK（DLSS）
│   └── CMakeLists.txt
├── dep/tinybvh/                  # 第三方 BVH 库（本地副本）
└── doc/                          # DLSS 集成与问题排查文档
```

## 技术要点

- **渲染管线**：Unity 2022.3 LTS + URP，通过 `PathTraceRendererFeature` 注入自定义渲染 Pass
- **路径追踪**：Vulkan Ray Query 实现，多弹射路径追踪与光源采样在原生侧 compute shader 中完成
- **加速结构**：基于 [tinybvh](https://github.com/jbikker/tinybvh) 构建 BVH
- **降噪重建**：NVIDIA DLSS Ray Reconstruction（NGX SDK）
- **原生桥接**：CMake 构建 Windows 原生 DLL，C# 通过 P/Invoke 调用

## 环境要求

- Unity 2022.3.62f1 及以上（URP）
- Visual Studio 2022（MSVC v143）+ CMake 3.15+
- 支持 Vulkan Ray Query 与 DLSS RR 的 NVIDIA 显卡（RTX 系列）及最新驱动

## 相关文档

- `TinyBVH_Integration_Guide.md` — tinybvh 原生插件集成指南
- `路径追踪相交测试实现方案.md` / `光源收集与多弹射路径追踪` 相关方案文档（`.qoder/plans/`）
- `doc/DLSS_RR集成踩坑记录.md` — DLSS RR 集成记录
- `doc/DLSS_RR亮区抖动分析.md`、`doc/DLSS_抖动修复踩坑记录.md` — DLSS 抖动问题排查
- `dep/tinybvh/部署教程.md` — tinybvh 部署说明

## 声明

本项目为个人学习与实验性质，用于探索实时路径追踪在 Unity URP 中的可行性。纯软件侧（无硬件降噪加速）的方案已归档不再维护，后续所有开发均围绕 DLSS RR 路线进行。
