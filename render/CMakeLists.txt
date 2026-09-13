# render —— 后端无关的渲染抽象层
# 内容规划（DESIGN §5.1）：
#   IRenderer.h   渲染器抽象接口（生命周期/resize/render/参数注入）
#   ShaderSource  共享 GLSL 450 源码与 C++ 侧 include 展开（§5.8）
#   PostChain     FSR1 / FXAA / ACES / 色差 的声明与参数（实现由各后端自带）
# 当前为脚手架占位；IRenderer 接口与后端实现见 T0.4 / T1.3 / T1.6。

add_library(ehe_render STATIC
  src/renderer_info.cpp
)

target_include_directories(ehe_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ehe_render
  PUBLIC ehe_core
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
