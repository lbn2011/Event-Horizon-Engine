# render/vk —— Vulkan 1.2 后端（静态库）
# 规划（DESIGN §5.4.1 对照表）：
#   HDR R16G16B16A16_SFLOAT attachment、descriptor set 0（UBO=0 / 黑体 LUT=1 / 粒子 SSBO=2）、
#   全屏三角形、VkQueryPool 计时、frames-in-flight=2、volk 运行时加载 vulkan-1.dll（免 SDK）
#   shader 经 glslang 运行时编译为 SPIR-V
# 依赖接入见 T0.3（Vulkan-Headers + volk + glslang），本文件当前不引入任何外部头。

add_library(ehe_render_vk STATIC
  src/vk_backend.cpp
)

target_include_directories(ehe_render_vk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ehe_render_vk
  PUBLIC ehe_render
  PRIVATE volk                    # 运行时加载 vulkan-1.dll（免 SDK，§5.4.1）
  PRIVATE Vulkan::Headers
  PRIVATE glslang::glslang        # GLSL → SPIR-V（启动时编译，§5.2）
  # 说明：不链接 glslang::SPIRV —— ENABLE_OPT=OFF 时该库仅为空壳（stub），
  #       glslang 主库自身已包含 ${SPIRV_SOURCES}，运行时编译只需 glslang 即可。
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
