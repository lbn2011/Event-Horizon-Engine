# tests —— 单元测试与验证工具
# 规划（DESIGN §6）：
#   L1 CPU 参考实现（fp64 Kerr-Schild 积分器 / golden 渲染 / 黑体 LUT 生成）见 T1.2
#   L2 doctest 用例（§6.5 共 12 条）随各模块同步建立
# 当前为脚手架：目标可用，测试框架在 T0.3（FetchContent 接入 doctest）后启用。

add_executable(ehe_tests
  src/test_main.cpp
  src/test_config.cpp
  src/test_camera.cpp
  src/test_metric.cpp
  src/test_integrator.cpp
)

target_link_libraries(ehe_tests
  PRIVATE ehe_core
  PRIVATE doctest::doctest
  PRIVATE ehe_warnings
  PRIVATE ehe_options
)
