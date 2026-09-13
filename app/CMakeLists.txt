# app —— 可执行文件 ehe
# 规划（DESIGN §5.1）：main（启动/Config 加载/后端创建）、Window（GLFW 封装，切后端销毁重建
# 窗口句柄）、Ui（ImGui 面板 + 左上角监控 overlay，二者分离）、Audio（miniaudio）、
# Smoke（--smoke 离屏渲染 N 帧截图，供目标机验证）
# 当前为脚手架：仅打印版本与后端编译信息，退出码 0。窗口与面板见 T0.4。

add_executable(ehe_app
  src/main.cpp
)

set_target_properties(ehe_app PROPERTIES OUTPUT_NAME "ehe")

target_link_libraries(ehe_app
  PRIVATE ehe_core
  PRIVATE ehe_render
  PRIVATE ehe_render_vk
  PRIVATE ehe_render_gl
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
