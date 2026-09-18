# tests —— 单元测试与验证工具
# 规划（DESIGN §6）：
#   L1 CPU 参考实现（fp64 Kerr-Schild 积分器 / golden 渲染 / 黑体 LUT 生成）—— 实现于 core/，见 T1.2
#   L2 doctest 用例（§6.5）随各模块同步建立
#   shader 静态校验：用 glslang 对展开 include 后的源码做 parse（**无需 GPU**，可进 CI）
#   golden 工具 ehe_reftool：烘焙黑体 LUT、渲染 golden 基线、PFM 差分与 PNG 转换
#
# 说明：允许依赖 stb（PNG 写出）与 glslang（shader 校验）；core 保持零图形 API / 零文件格式依赖。

add_executable(ehe_tests
  src/test_main.cpp
  src/test_config.cpp
  src/test_camera.cpp
  src/test_metric.cpp
  src/test_integrator.cpp
  src/test_blackbody.cpp
  src/test_golden.cpp
  src/test_shaders.cpp
  src/test_image_io.cpp
  src/test_repo_hygiene.cpp
  src/test_noise.cpp
)

target_link_libraries(ehe_tests
  PRIVATE ehe_core
  PRIVATE ehe_render          # ShaderSource / SimParams（无图形 API 依赖）
  PRIVATE glslang::glslang    # shader 静态校验（parse，不生成 SPIR-V）
  PRIVATE glslang::glslang-default-resource-limits  # GetDefaultResources()（独立目标，不在 glslang 内）
  PRIVATE doctest::doctest
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)

add_executable(ehe_reftool
  src/reftool.cpp
  "${CMAKE_SOURCE_DIR}/third_party/stb_impl.c"
)

target_link_libraries(ehe_reftool
  PRIVATE ehe_core
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE ehe_stb
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
