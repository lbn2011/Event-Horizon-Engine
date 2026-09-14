# app —— 可执行文件 ehe
# 内容（DESIGN §5.1）：
#   main（启动 / Config 加载 / 后端创建 / smoke 模式）、backend_factory（两后端工厂）
#   Window / Ui（ImGui 面板 + 监控 overlay，T1.7 拆分）、Audio（miniaudio，T1.8）、
#   Smoke（--smoke 离屏渲染 N 帧 → PFM/PNG → NMSE 差分，§6.3）
#
# 依赖方向：ehe_app → ehe_render_vk / ehe_render_gl → ehe_render → ehe_core

add_executable(ehe_app
  src/main.cpp
  src/backend_factory.cpp
  "${CMAKE_SOURCE_DIR}/third_party/stb_impl.c"
)

set_target_properties(ehe_app PROPERTIES OUTPUT_NAME "ehe")

target_link_libraries(ehe_app
  PRIVATE ehe_core
  PRIVATE ehe_render
  PRIVATE ehe_render_vk
  PRIVATE ehe_render_gl
  PRIVATE glfw                    # 窗口/输入
  PRIVATE ehe_imgui               # 面板（§5.1 Ui）
  PRIVATE ehe_miniaudio           # 程序化音效（§5.7，T1.8）
  PRIVATE ehe_stb                 # PNG 写出（smoke 预览，§6.3）
  PRIVATE ehe_fsr1                # FSR1 EASU/RCAS（§4.6，T1.5）
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
