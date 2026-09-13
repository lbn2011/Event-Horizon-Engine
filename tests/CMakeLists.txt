# tests —— 单元测试与验证工具
# 规划（DESIGN §6）：
#   L1 CPU 参考实现（fp64 Kerr-Schild 积分器 / golden 渲染 / 黑体 LUT 生成）—— 实现于 core/，见 T1.2
#   L2 doctest 用例（§6.5）随各模块同步建立
#   golden 工具 ehe_reftool：烘焙黑体 LUT、渲染 golden 基线（PFM 差分基准 + PNG 预览）
#
# 说明：本目标是唯一允许依赖 stb（PNG 写出）的构建单元；core 保持零图形 API / 零文件格式依赖。

add_executable(ehe_tests
  src/test_main.cpp
  src/test_config.cpp
  src/test_camera.cpp
  src/test_metric.cpp
  src/test_integrator.cpp
  src/test_blackbody.cpp
  src/test_golden.cpp
)

target_link_libraries(ehe_tests
  PRIVATE ehe_core
  PRIVATE doctest::doctest
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)

add_executable(ehe_reftool
  src/reftool.cpp
  src/stb_impl.c
)

target_link_libraries(ehe_reftool
  PRIVATE ehe_core
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE ehe_stb
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
