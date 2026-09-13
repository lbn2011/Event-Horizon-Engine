# core —— 纯 C++ 静态库
# 编译期约束（DESIGN §5.2）：本 target 不得链接任何图形/窗口库
# （glfw / glad / volk / imgui 一律禁止），以保证物理与参数逻辑可独立测试。
#
# 模块规划（DESIGN §5.1，真实实现见 T1.1）：
#   Config / Camera / BlackHoleParams / ReferenceIntegrator / BlackbodyLut / units-math
# 当前为脚手架占位，仅提供版本查询以验证构建链路。

add_library(ehe_core STATIC
  src/version.cpp
)

target_include_directories(ehe_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(ehe_core PRIVATE "EHE_VERSION_STRING=\"${PROJECT_VERSION}\"")

target_link_libraries(ehe_core
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
