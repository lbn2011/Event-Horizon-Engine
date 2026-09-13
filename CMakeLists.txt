cmake_minimum_required(VERSION 3.20)

# ============================================================================
# EHE —— Event Horizon Engine
# 顶层构建配置：C++20、警告集、构建选项、子目录挂接
# 规格依据：DESIGN §5.1（模块分解）、§5.4.1（target 依赖）、§11（编码规范）
# 工具链：见 cmake/llvm-mingw-toolchain.cmake（无 MSVC 环境）
# ============================================================================

project(EHE
  VERSION 0.1.0
  DESCRIPTION "实时 Kerr 黑洞渲染模拟器"
  LANGUAGES CXX C)

# ---- 构建选项 --------------------------------------------------------------
option(EHE_FP64_METRIC "度规/Christoffel 求值使用 fp64；关闭则整体回退纯 fp32（DESIGN §4.3）" ON)
option(EHE_BUILD_TESTS "构建单元测试目标 ehe_tests（DESIGN §6 L2）" ON)

# ---- 全局语言标准（DESIGN §2.2：C++20，不使用 modules）--------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 输出目录：产物集中，便于目标机拷贝调试 --------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---- 全局接口 target：警告集与编译期开关 -----------------------------------
# 开发期不启用 -Werror（DESIGN §11）
add_library(ehe_warnings INTERFACE)
target_compile_options(ehe_warnings INTERFACE -Wall -Wextra -Wpedantic)

add_library(ehe_options INTERFACE)
if(EHE_FP64_METRIC)
  target_compile_definitions(ehe_options INTERFACE EHE_FP64_METRIC=1)
endif()

# ---- 依赖（第三方）---------------------------------------------------------
# FetchContent 依赖统一在 cmake/Deps.cmake 声明并 pin 版本（DESIGN §9），
# 于 T0.3 接入；当前骨架不依赖任何第三方库即可编译。

# ---- 子目录（依赖方向：app → render_{vk,gl} → render → core）-------------
add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(render/vk)
add_subdirectory(render/gl)
add_subdirectory(app)

if(EHE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- 构建摘要 --------------------------------------------------------------
message(STATUS "EHE ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | "
               "fp64_metric=${EHE_FP64_METRIC} | tests=${EHE_BUILD_TESTS}")
message(STATUS "EHE 编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
               "(${CMAKE_CXX_COMPILER_TARGET})")
