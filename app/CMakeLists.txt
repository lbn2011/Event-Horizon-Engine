# app —— 可执行文件 ehe
# 规划（DESIGN §5.1）：main（启动/Config 加载/后端创建）、Window（GLFW 封装，切后端销毁重建
# 窗口句柄）、Ui（ImGui 面板 + 左上角监控 overlay，二者分离）、Audio（miniaudio）、
# Smoke（--smoke 离屏渲染 N 帧截图，供目标机验证）
# 当前为脚手架：仅打印版本与后端编译信息，退出码 0。窗口与面板见 T0.4。

add_executable(ehe_app
  src/main.cpp
  src/backend_factory.cpp
)

set_target_properties(ehe_app PROPERTIES OUTPUT_NAME "ehe")

target_link_libraries(ehe_app
  PRIVATE ehe_core
  PRIVATE ehe_render
  PRIVATE ehe_render_vk
  PRIVATE ehe_render_gl
  PRIVATE glfw                    # 窗口/输入
  PRIVATE ehe_imgui               # 面板 + 左上角监控 overlay（§5.1 Ui）
  PRIVATE ehe_miniaudio           # 程序化音效（§5.7）
  PRIVATE ehe_stb                 # PNG 写出（smoke 预览，§6.3）
  PRIVATE ehe_fsr1                # FSR1 EASU/RCAS（§4.6）
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
