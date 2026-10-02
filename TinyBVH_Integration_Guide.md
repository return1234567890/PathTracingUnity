# TinyBVH Unity 原生插件跨平台集成方案

## 概述

本方案将 [tinybvh](https://github.com/jbikker/tinybvh) C++ 射线求交库封装为 Unity 可调用的原生插件，使用 CMake 统一管理多平台编译，支持 Windows、Android、iOS、Linux、macOS。

### 架构分层

```
tiny_bvh.h             ← C++ header-only 库（第三方，无需修改）
       ↓
tiny_bvh_wrapper.cpp   ← C 语言导出层（薄封装）
       ↓  CMake 编译
TinyBVHNative.dll/.so  ← 各平台原生二进制
       ↓  P/Invoke
TinyBVHNative.cs       ← C# 底层绑定声明
       ↓
TinyBVH.cs             ← C# 高层友好 API
       ↓
业务脚本（CameraController 等）
```

---

## 〇、前提动作：拷贝源码到工程内

所有后续操作均基于工程内的副本执行，**不要直接修改原始仓库**，避免污染上游代码。

```powershell
# 将 tinybvh 整个目录拷贝到当前 Unity 工程根目录下的 dep/ 中
Copy-Item -Recurse -Force `
    "D:\personalProject\bvh2file\pytinybvh\deps\tinybvh" `
    "D:\unityHub\project\URPPT\dep\tinybvh"
```

拷贝完成后，工程根目录结构如下：

```
URPPT/
├── Assets/
├── dep/
│   └── tinybvh/              ← 后续所有操作的基准目录
│       ├── tiny_bvh.h        # 已有，不修改
│       ├── tiny_bvh_wrapper.cpp        # 在此新建
│       └── CMakeLists-Wrapper.txt      # 在此新建
├── ProjectSettings/
└── TinyBVH_Integration_Guide.md
```

> ⚠️ 此后文档中所有涉及 tinybvh 的路径，均以 `dep/tinybvh/` 为基准，不再引用原始仓库路径。

---

## 一、目录结构

### 1.1 C++ 源码目录

在 `dep/tinybvh/` 下新建以下文件：

```
dep/tinybvh/
├── tiny_bvh.h                  # 已有，不修改
├── tiny_bvh_wrapper.cpp        # 新建：C 导出层
└── CMakeLists-Wrapper.txt      # 新建：CMake 构建配置
```

### 1.2 Unity 工程目录

```
Assets/
├── Plugins/
│   ├── x86_64/                 # Windows x64 / Linux x64
│   │   └── TinyBVHNative.dll   (或 .so)
│   ├── Android/
│   │   ├── arm64-v8a/
│   │   │   └── libTinyBVHNative.so
│   │   └── armeabi-v7a/
│   │       └── libTinyBVHNative.so
│   ├── iOS/
│   │   └── libTinyBVHNative.a
│   └── macOS/
│       └── TinyBVHNative.bundle
└── Scripts/
    ├── TinyBVHNative.cs        # P/Invoke 声明
    └── TinyBVH.cs              # 高层封装
```

---

## 二、C++ 导出层

### 2.1 `tiny_bvh_wrapper.cpp`

```cpp
// tiny_bvh_wrapper.cpp
// tinybvh 的 C 语言导出层，供 Unity P/Invoke 调用

#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"

#include <cstring>

// 跨平台导出宏
#if defined(_WIN32)
    #define TBVH_EXPORT extern "C" __declspec(dllexport)
#else
    #define TBVH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// 不透明句柄，C# 侧用 IntPtr 持有
struct BVHHandle {
    tinybvh::BVH bvh;
};

// 射线命中结果（平铺结构，跨语言安全传递）
struct HitResult {
    int   hit;      // 0 = 未命中, 1 = 命中
    int   primIdx;  // 命中的三角形索引
    float t;        // 射线参数 t（距离）
    float u, v;     // 重心坐标
};

// ── 生命周期 ──────────────────────────────────────────

TBVH_EXPORT BVHHandle* tinybvh_create() {
    return new BVHHandle();
}

TBVH_EXPORT void tinybvh_destroy(BVHHandle* handle) {
    delete handle;
}

// ── 构建 BVH ──────────────────────────────────────────
// vertices: float 数组，每顶点 4 float (x,y,z,pad)，共 numTriangles*3 个顶点
TBVH_EXPORT void tinybvh_build(BVHHandle* handle, const float* vertices, int numTriangles) {
    handle->bvh.Build(reinterpret_cast<const tinybvh::bvhvec4*>(vertices), numTriangles);
}

// ── 单条射线求交 ────────────────────────────────────────
TBVH_EXPORT HitResult tinybvh_intersect(
    BVHHandle* handle,
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float tMax)
{
    tinybvh::Ray ray(
        tinybvh::bvhvec3(ox, oy, oz),
        tinybvh::bvhvec3(dx, dy, dz),
        tMax
    );
    handle->bvh.Intersect(ray);

    HitResult result;
    result.hit     = (ray.hit.t < tMax) ? 1 : 0;
    result.primIdx = ray.hit.prim;
    result.t       = ray.hit.t;
    result.u       = ray.hit.u;
    result.v       = ray.hit.v;
    return result;
}

// ── 批量射线求交（减少 P/Invoke 跨边界开销）────────────────
TBVH_EXPORT void tinybvh_intersect_batch(
    BVHHandle*  handle,
    const float* origins,    // float[n*3]
    const float* directions, // float[n*3]
    HitResult*  results,     // HitResult[n]
    int         count,
    float       tMax)
{
    for (int i = 0; i < count; i++) {
        int oi = i * 3, di = i * 3;
        tinybvh::Ray ray(
            tinybvh::bvhvec3(origins[oi], origins[oi+1], origins[oi+2]),
            tinybvh::bvhvec3(directions[di], directions[di+1], directions[di+2]),
            tMax
        );
        handle->bvh.Intersect(ray);
        results[i].hit     = (ray.hit.t < tMax) ? 1 : 0;
        results[i].primIdx = ray.hit.prim;
        results[i].t       = ray.hit.t;
        results[i].u       = ray.hit.u;
        results[i].v       = ray.hit.v;
    }
}
```

---

## 三、CMake 构建配置

### 3.1 `CMakeLists-Wrapper.txt`

```cmake
cmake_minimum_required(VERSION 3.15)
project(TinyBVHNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 源文件 ──────────────────────────────────────────────
add_library(TinyBVHNative SHARED tiny_bvh_wrapper.cpp)

# ── 编译优化（Release 默认开启）─────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(TinyBVHNative PRIVATE /O2 /EHsc)
    else()
        target_compile_options(TinyBVHNative PRIVATE -O3 -ffast-math)
    endif()
endif()

# ── 符号可见性（非 Windows 平台隐藏内部符号）──────────
if(NOT WIN32)
    set_target_properties(TinyBVHNative PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

# ── 输出目录按平台分离 ─────────────────────────────────
if(ANDROID)
    set_target_properties(TinyBVHNative PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/Android/${ANDROID_ABI}"
    )
elseif(APPLE AND IOS)
    set_target_properties(TinyBVHNative PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/iOS"
    )
    # iOS 使用静态库
    set_target_properties(TinyBVHNative PROPERTIES TYPE STATIC_LIBRARY)
elseif(APPLE)
    set_target_properties(TinyBVHNative PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/macOS"
        SUFFIX ".bundle"
    )
elseif(UNIX)
    set_target_properties(TinyBVHNative PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/Linux/x86_64"
    )
elseif(WIN32)
    set_target_properties(TinyBVHNative PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output/Windows/x86_64"
    )
endif()
```

---

## 四、各平台编译命令

> **前提**：CMake 3.15+，各平台对应工具链已安装。  
> 所有命令均在 Unity 工程根目录下的 `dep/tinybvh/` 中执行，即先 `cd dep/tinybvh` 再运行。

### 4.1 Windows x64

**工具链**：Visual Studio 2022（MSVC）+ CMake

```powershell
# 在 x64 Native Tools Command Prompt 中执行，或使用 cmake -G 指定生成器
cmake -B build-win64 `
      -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_BUILD_TYPE=Release `
      -f CMakeLists-Wrapper.txt

cmake --build build-win64 --config Release
```

**产物**：`build-win64/output/Windows/x86_64/TinyBVHNative.dll`

---

### 4.2 Android ARM64

**工具链**：Android NDK（r25+），需设置 `ANDROID_NDK` 环境变量

```powershell
cmake -B build-android-arm64 `
      -DCMAKE_TOOLCHAIN_FILE="$env:ANDROID_NDK/build/cmake/android.toolchain.cmake" `
      -DANDROID_ABI=arm64-v8a `
      -DANDROID_PLATFORM=android-24 `
      -DCMAKE_BUILD_TYPE=Release `
      -f CMakeLists-Wrapper.txt

cmake --build build-android-arm64
```

**产物**：`build-android-arm64/output/Android/arm64-v8a/libTinyBVHNative.so`

---

### 4.3 Android ARMv7

```powershell
cmake -B build-android-armv7 `
      -DCMAKE_TOOLCHAIN_FILE="$env:ANDROID_NDK/build/cmake/android.toolchain.cmake" `
      -DANDROID_ABI=armeabi-v7a `
      -DANDROID_PLATFORM=android-24 `
      -DCMAKE_BUILD_TYPE=Release `
      -f CMakeLists-Wrapper.txt

cmake --build build-android-armv7
```

**产物**：`build-android-armv7/output/Android/armeabi-v7a/libTinyBVHNative.so`

---

### 4.4 iOS ARM64

**工具链**：Xcode（macOS 主机）

```bash
cmake -B build-ios \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=Release \
      -f CMakeLists-Wrapper.txt

cmake --build build-ios
```

**产物**：`build-ios/output/iOS/libTinyBVHNative.a`

---

### 4.5 Linux x64

**工具链**：gcc 或 clang

```bash
cmake -B build-linux64 \
      -DCMAKE_BUILD_TYPE=Release \
      -f CMakeLists-Wrapper.txt

cmake --build build-linux64
```

**产物**：`build-linux64/output/Linux/x86_64/libTinyBVHNative.so`

---

### 4.6 macOS（Apple Silicon + Intel 通用二进制）

**工具链**：Xcode

```bash
cmake -B build-macos \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_BUILD_TYPE=Release \
      -f CMakeLists-Wrapper.txt

cmake --build build-macos
```

**产物**：`build-macos/output/macOS/TinyBVHNative.bundle`

---

## 五、编译产物复制到 Unity 工程

> 以下脚本在 `dep/tinybvh/` 目录下执行（与编译命令相同目录）。

```powershell
# 先确保目标目录存在
$unityRoot = "D:\unityHub\project\URPPT\Assets\Plugins"
New-Item -ItemType Directory -Force -Path "$unityRoot\x86_64",
    "$unityRoot\Android\arm64-v8a",
    "$unityRoot\Android\armeabi-v7a",
    "$unityRoot\iOS",
    "$unityRoot\macOS" | Out-Null

# Windows
Copy-Item build-win64\output\Windows\x86_64\TinyBVHNative.dll       "$unityRoot\x86_64\" -Force

# Android
Copy-Item build-android-arm64\output\Android\arm64-v8a\libTinyBVHNative.so    "$unityRoot\Android\arm64-v8a\" -Force
Copy-Item build-android-armv7\output\Android\armeabi-v7a\libTinyBVHNative.so  "$unityRoot\Android\armeabi-v7a\" -Force

# iOS（macOS 上执行）
# cp build-ios/output/iOS/libTinyBVHNative.a   $unityRoot/iOS/

# Linux
Copy-Item build-linux64\output\Linux\x86_64\libTinyBVHNative.so     "$unityRoot\x86_64\" -Force

# macOS（macOS 上执行）
# cp -R build-macos/output/macOS/TinyBVHNative.bundle  $unityRoot/
```

---

## 六、Unity 插件平台配置

将各二进制文件拖入 Unity 后，在 Inspector 中逐一设置 **Platform Settings**：

| 文件 | 勾选平台 |
|------|---------|
| `Plugins/x86_64/TinyBVHNative.dll` | ✅ Windows x86_64 |
| `Plugins/x86_64/libTinyBVHNative.so` | ✅ Linux x86_64 |
| `Plugins/Android/arm64-v8a/libTinyBVHNative.so` | ✅ Android ARM64 |
| `Plugins/Android/armeabi-v7a/libTinyBVHNative.so` | ✅ Android ARMv7 |
| `Plugins/iOS/libTinyBVHNative.a` | ✅ iOS |
| `Plugins/macOS/TinyBVHNative.bundle` | ✅ macOS |

> ⚠️ 同一文件名在不同平台目录下可以共存，Unity 会按平台自动选择正确的二进制。

---

## 七、C# 绑定代码

### 7.1 `Assets/Scripts/TinyBVHNative.cs`

```csharp
using System;
using System.Runtime.InteropServices;

public static class TinyBVHNative
{
    private const string DllName = "TinyBVHNative";

    [StructLayout(LayoutKind.Sequential)]
    public struct HitResult
    {
        public int   hit;
        public int   primIdx;
        public float t;
        public float u, v;
    }

    [DllImport(DllName)]
    public static extern IntPtr tinybvh_create();

    [DllImport(DllName)]
    public static extern void tinybvh_destroy(IntPtr handle);

    [DllImport(DllName)]
    public static extern void tinybvh_build(IntPtr handle, float[] vertices, int numTriangles);

    [DllImport(DllName)]
    public static extern HitResult tinybvh_intersect(
        IntPtr handle,
        float ox, float oy, float oz,
        float dx, float dy, float dz,
        float tMax);

    [DllImport(DllName)]
    public static extern void tinybvh_intersect_batch(
        IntPtr handle,
        float[] origins,
        float[] directions,
        HitResult[] results,
        int count,
        float tMax);
}
```

> `DllImport("TinyBVHNative")` 不含扩展名，Unity 运行时会自动按当前平台匹配 `.dll` / `.so` / `.dylib` / `.bundle`。

### 7.2 `Assets/Scripts/TinyBVH.cs`

```csharp
using System;
using UnityEngine;

/// <summary>
/// tinybvh C# 封装，提供 BVH 构建与射线求交能力。
/// 使用示例：
///   var bvh = new TinyBVH();
///   bvh.Build(meshFilter.sharedMesh);
///   var hit = bvh.Raycast(origin, direction, 1000f);
///   bvh.Dispose();
/// </summary>
public class TinyBVH : IDisposable
{
    private IntPtr _handle;
    private bool   _disposed;

    public struct RayHit
    {
        public bool  hit;
        public int   triangleIndex;
        public float distance;
        public float u, v;
    }

    public TinyBVH()
    {
        _handle = TinyBVHNative.tinybvh_create();
    }

    /// <summary>从 Unity Mesh 构建 BVH</summary>
    public void Build(Mesh mesh)
    {
        Build(mesh.vertices, mesh.triangles);
    }

    /// <summary>从顶点 + 三角形索引构建 BVH</summary>
    public void Build(Vector3[] vertices, int[] triangles)
    {
        int triCount = triangles.Length / 3;
        // 每顶点 4 float (x,y,z,pad)，共 triCount*3 个顶点
        float[] data = new float[triCount * 3 * 4];
        for (int i = 0; i < triCount; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                Vector3 v = vertices[triangles[i * 3 + j]];
                int baseIdx = (i * 3 + j) * 4;
                data[baseIdx + 0] = v.x;
                data[baseIdx + 1] = v.y;
                data[baseIdx + 2] = v.z;
                data[baseIdx + 3] = 0f; // pad
            }
        }
        TinyBVHNative.tinybvh_build(_handle, data, triCount);
    }

    /// <summary>单条射线求交</summary>
    public RayHit Raycast(Vector3 origin, Vector3 direction, float maxDistance = 1e30f)
    {
        var r = TinyBVHNative.tinybvh_intersect(
            _handle,
            origin.x, origin.y, origin.z,
            direction.x, direction.y, direction.z,
            maxDistance);

        return new RayHit
        {
            hit           = r.hit != 0,
            triangleIndex = r.primIdx,
            distance      = r.t,
            u             = r.u,
            v             = r.v
        };
    }

    /// <summary>批量射线求交（减少跨边界调用开销）</summary>
    public RayHit[] RaycastBatch(Vector3[] origins, Vector3[] directions, float maxDistance = 1e30f)
    {
        int count = origins.Length;
        float[] oriFlat = new float[count * 3];
        float[] dirFlat = new float[count * 3];
        for (int i = 0; i < count; i++)
        {
            oriFlat[i * 3 + 0] = origins[i].x;
            oriFlat[i * 3 + 1] = origins[i].y;
            oriFlat[i * 3 + 2] = origins[i].z;
            dirFlat[i * 3 + 0] = directions[i].x;
            dirFlat[i * 3 + 1] = directions[i].y;
            dirFlat[i * 3 + 2] = directions[i].z;
        }

        var nativeResults = new TinyBVHNative.HitResult[count];
        TinyBVHNative.tinybvh_intersect_batch(
            _handle, oriFlat, dirFlat, nativeResults, count, maxDistance);

        var results = new RayHit[count];
        for (int i = 0; i < count; i++)
        {
            results[i] = new RayHit
            {
                hit           = nativeResults[i].hit != 0,
                triangleIndex = nativeResults[i].primIdx,
                distance      = nativeResults[i].t,
                u             = nativeResults[i].u,
                v             = nativeResults[i].v
            };
        }
        return results;
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            TinyBVHNative.tinybvh_destroy(_handle);
            _handle   = IntPtr.Zero;
            _disposed = true;
        }
    }

    ~TinyBVH() { Dispose(); }
}
```

---

## 八、使用示例

```csharp
using UnityEngine;

public class BVHTest : MonoBehaviour
{
    public MeshFilter targetMeshFilter;
    private TinyBVH _bvh;

    void Start()
    {
        _bvh = new TinyBVH();
        _bvh.Build(targetMeshFilter.sharedMesh);
        Debug.Log("BVH 构建完成");
    }

    void Update()
    {
        if (Input.GetMouseButtonDown(0))
        {
            Ray ray = Camera.main.ScreenPointToRay(Input.mousePosition);
            var hit = _bvh.Raycast(ray.origin, ray.direction, 1000f);
            if (hit.hit)
                Debug.Log($"命中三角形 #{hit.triangleIndex}，距离: {hit.distance:F3}");
            else
                Debug.Log("未命中");
        }
    }

    void OnDestroy() { _bvh?.Dispose(); }
}
```

---

## 九、常见问题

### Q: 打包时报 `DllNotFoundException`
检查对应平台的 `.so` / `.a` / `.bundle` 是否已放入正确的 `Plugins/` 子目录，并在 Inspector 中勾选了对应平台。

### Q: Android 上找不到符号 `tinybvh_create`
确认编译时使用了 `extern "C"` 且未开启符号 stripping（Release 模式下 CMake 默认不 strip）。

### Q: iOS 打包失败，提示链接错误
iOS 静态库需要在 Unity 的 `Assets/Plugins/iOS/` 下同时放一个 `TinyBVHNative.mm`（可为空文件）触发 Xcode 链接该 `.a`。

### Q: tinybvh 版本升级后需要重新编译吗？
需要。每次 `tiny_bvh.h` 更新后，对所有目标平台重新执行 `cmake --build` 即可。

### Q: 如何在 CI/CD 中自动编译？
将上述各平台编译命令写入一个 `build_all.sh` / `build_all.ps1` 脚本，在对应平台的 runner 上执行，产物自动复制到 Unity 工程的 `Assets/Plugins/` 目录。
