# render/gl —— OpenGL 4.5 后端（静态库）
# 规划（DESIGN §5.4.1 对照表）：
#   FP16 FBO（GL_RGBA16F）、UBO 走 glBindBufferBase(...,0)、黑体 LUT 纹理单元 1、
#   全屏三角形（gl_VertexID）、GL_TIME_ELAPSED 计时查询、glad 4.5 加载器
# 依赖接入见 T0.3（GLFW + glad），本文件当前不引入任何外部头。

add_library(ehe_render_gl STATIC
  src/gl_backend.cpp
)

target_include_directories(ehe_render_gl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 说明：ImGui 的“后端初始化/绘制”必须在拥有图形上下文的库里完成
# （ImGui_ImplOpenGL3_Init 需要 GL context；ImGui_ImplVulkan_Init 需要 VkDevice），
# 因此两个后端都链接 ehe_imgui；参数面板与应用层合成仍在 app/（DESIGN §5.1 / §5.3）。
target_link_libraries(ehe_render_gl
  PUBLIC ehe_render
  PRIVATE glfw                    # 窗口（GL context 在此创建）
  PRIVATE ehe_imgui               # 面板后端：imgui_impl_glfw + imgui_impl_opengl3
  PRIVATE ehe_glad                # GL 4.5 核心加载器（third_party/glad，生成入库）
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
