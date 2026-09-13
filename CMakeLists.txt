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

# ---- 构建类型（必须显式，否则依赖与本项目配置不一致）---------------------
# 教训：glslang 在其 CMakeLists 中对未指定情形默认 Debug，会加 "d" 后辍
# （产出 libglslangd.a），而本项目空配置产出 libglslang.a，链接时找不到文件。
# 统一在此指定默认构建类型，CI 与本地保持一致。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "构建类型：Debug/Release/RelWithDebInfo/MinSizeRel" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()
message(STATUS "EHE 构建类型: ${CMAKE_BUILD_TYPE}")

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
# FetchContent 依赖统一在 cmake/Deps.cmake 声明并 pin 版本（DESIGN §9）
include(cmake/Deps.cmake)

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
