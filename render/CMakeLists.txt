# render —— 后端无关的渲染抽象层
# 内容（DESIGN §5.1）：
#   IRenderer.h   渲染器抽象接口（生命周期 / 参数注入 / HDR 回读）
#   ShaderSource  GLSL 450 文本 include 展开与加载（§5.8）
#   SimParams     UBO 的 C++ 镜像（§5.3，std140）
#   PostChain     FSR1 / FXAA / ACES / 色差 的声明与参数（T1.5）
#
# 依赖方向：ehe_render → ehe_core（单向）；具体后端（gl/vk）→ ehe_render。

add_library(ehe_render STATIC
  src/renderer_info.cpp
  src/shader_source.cpp
  src/sim_params.cpp
  src/spirv.cpp
)

target_include_directories(ehe_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ehe_render
  PUBLIC ehe_core
  # GLSL → SPIR-V（VK 侧运行时编译，§5.2）；头文件不暴露 glslang 类型，故 PRIVATE
  PRIVATE glslang::glslang
  # GetDefaultResources() 在独立目标里（不在 glslang 主库内）——tests 侧同样链接它
  PRIVATE glslang::glslang-default-resource-limits
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
