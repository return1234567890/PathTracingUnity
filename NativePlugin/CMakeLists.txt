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
        "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Plugins/x86_64/VulkanPathTracePlugin.dll"
    )
endif()

# Debug 后缀
set_target_properties(${PROJECT_NAME} PROPERTIES
    DEBUG_POSTFIX "_d"
)
