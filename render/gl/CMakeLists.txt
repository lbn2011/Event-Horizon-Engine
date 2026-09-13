# render/gl —— OpenGL 4.5 后端（静态库）
# 规划（DESIGN §5.4.1 对照表）：
#   FP16 FBO（GL_RGBA16F）、UBO 走 glBindBufferBase(...,0)、黑体 LUT 纹理单元 1、
#   全屏三角形（gl_VertexID）、GL_TIME_ELAPSED 计时查询、glad 4.5 加载器
# 依赖接入见 T0.3（GLFW + glad），本文件当前不引入任何外部头。

add_library(ehe_render_gl STATIC
  src/gl_backend.cpp
)

target_include_directories(ehe_render_gl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ehe_render_gl
  PUBLIC ehe_render
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
