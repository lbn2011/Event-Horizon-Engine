# core —— 纯 C++ 静态库
# 编译期约束（DESIGN §5.2）：本 target 不得链接任何图形/窗口库
# （glfw / glad / volk / imgui 一律禁止），以保证物理与参数逻辑可独立测试。
#
# 模块规划（DESIGN §5.1，真实实现见 T1.1）：
#   Config / Camera / BlackHoleParams / ReferenceIntegrator / BlackbodyLut / units-math
# 当前为脚手架占位，仅提供版本查询以验证构建链路。

add_library(ehe_core STATIC
  src/version.cpp
  src/config.cpp
  src/camera.cpp
  src/metric.cpp
  src/integrator.cpp
  src/blackbody.cpp
  src/golden.cpp
  src/sha256.cpp
  src/image_io.cpp
  src/noise.cpp
  src/particles.cpp
  src/console.cpp
  src/tonemap.cpp
)

target_include_directories(ehe_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(ehe_core PRIVATE "EHE_VERSION_STRING=\"${PROJECT_VERSION}\"")

target_link_libraries(ehe_core
  PUBLIC glm::glm                  # 数学（header-only）
  PUBLIC nlohmann_json::nlohmann_json  # Config 序列化（DESIGN §8.1）
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
