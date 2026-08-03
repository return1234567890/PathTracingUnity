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
