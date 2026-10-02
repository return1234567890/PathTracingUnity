cmake_minimum_required(VERSION 3.18)

project(VulkanPathTracePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 路径配置 ──────────────────────────────────────────────
set(VULKAN_SDK_DIR    "$ENV{VULKAN_SDK}")
set(UNITY_EDITOR_DIR  "D:/tjEditor/2022.3.62f3c1")
set(UNITY_PLUGIN_API  "${UNITY_EDITOR_DIR}/Editor/Data/PluginAPI")
# ── Vulkan SDK ─────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# ── 源文件 ─────────────────────────────────────────────────
file(GLOB_RECURSE VPT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# ── DLL 目标 ───────────────────────────────────────────────
add_library(${PROJECT_NAME} SHARED ${VPT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${UNITY_PLUGIN_API}"
    "${VULKAN_SDK_DIR}/Include"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
)

# ── NVIDIA NGX DLSS SDK（条件链接：SDK 不存在时跳过） ──────────
# SDK 结构：third_party/ngx/include/*.h + third_party/ngx/lib/nvsdk_ngx_d*.lib
# 库命名约定：_d = 动态链接(对应 nvngx_dlss.dll)，_dbg = Debug 断言版
set(NGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx")
set(UNITY_PLUGINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Plugins/x86_64")

if(EXISTS "${NGX_SDK_DIR}/include")
    target_include_directories(${PROJECT_NAME} PRIVATE "${NGX_SDK_DIR}/include")

    # 标准 Core 路径：_d.lib 提供 thunk 到驱动内的 NGX Core，
    # Core 从 PathListInfo 路径加载 snippet DLL（nvngx_dlssd.dll）。
    # ApplicationId 必须与 snippet 元数据匹配（dev DLL 绑定 241534723）
    find_library(NGX_LIBRARY_RELEASE  nvsdk_ngx_d     PATHS "${NGX_SDK_DIR}/lib" NO_DEFAULT_PATH)
    find_library(NGX_LIBRARY_DEBUG    nvsdk_ngx_d_dbg PATHS "${NGX_SDK_DIR}/lib" NO_DEFAULT_PATH)

    set(NGX_LINK_LIBS "")
    if(NGX_LIBRARY_RELEASE)
        list(APPEND NGX_LINK_LIBS optimized ${NGX_LIBRARY_RELEASE})
    endif()
    if(NGX_LIBRARY_DEBUG)
        list(APPEND NGX_LINK_LIBS debug ${NGX_LIBRARY_DEBUG})
    endif()

    if(NGX_LINK_LIBS)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${NGX_LINK_LIBS})
        target_compile_definitions(${PROJECT_NAME} PRIVATE VPT_DLSS_ENABLED=1)
        message(STATUS "NGX DLSS SDK found at ${NGX_SDK_DIR}, linked (NGX Core path with matching ApplicationId).")
    else()
        message(WARNING "NGX headers found but import library missing in ${NGX_SDK_DIR}/lib. DLSS disabled.")
    endif()

    # 复制 NGX 运行时 DLL 到 Unity Plugins 目录（若 third_party/ngx/bin 存在）
    set(NGX_BIN_DIR "${NGX_SDK_DIR}/bin")
    if(EXISTS "${NGX_BIN_DIR}")
        file(GLOB NGX_RUNTIME_DLLS "${NGX_BIN_DIR}/*.dll")
        foreach(dll ${NGX_RUNTIME_DLLS})
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${UNITY_PLUGINS_DIR}/"
            )
        endforeach()
    endif()
else()
    message(STATUS "NGX SDK not found at ${NGX_SDK_DIR}. DLSS integration skipped (download SDK first).")
endif()

# Windows 链接设置
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/lib"
        LINK_FLAGS                       "/SUBSYSTEM:CONSOLE"
    )
    # 复制 DLL 到 Unity Plugins 目录
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${UNITY_PLUGINS_DIR}/VulkanPathTracePlugin.dll"
    )
endif()

# Debug 后缀
set_target_properties(${PROJECT_NAME} PROPERTIES
    DEBUG_POSTFIX "_d"
)
