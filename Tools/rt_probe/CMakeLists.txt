# rt_probe — RT pipeline 离线自检工具（不属于 Unity 插件，不进 Assets）
#
# 构建：
#   cmake -S Tools/rt_probe -B Tools/rt_probe/build
#   cmake --build Tools/rt_probe/build --config Release
#
# 运行：
#   Tools/rt_probe/build/Release/rt_probe.exe NativePlugin/shaders/pt.spv --trace

cmake_minimum_required(VERSION 3.18)
project(rt_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_executable(rt_probe rt_probe.cpp)
target_link_libraries(rt_probe PRIVATE Vulkan::Vulkan)

if(MSVC)
    target_compile_options(rt_probe PRIVATE /utf-8)
endif()

set_target_properties(rt_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
