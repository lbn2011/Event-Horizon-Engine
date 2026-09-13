// EHE 测试入口 —— doctest 主程序
//
// T0.3：doctest 经 FetchContent 接入，本文件改用 DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN。
// 真实用例按 DESIGN §6.5 清单随模块落地（T1.1 起）：config_roundtrip、
// camera_tetrad_orthonormal、ks_radius_a0、metric_derivative_numeric、rk4_convergence、
// photon_capture_bc、shadow_radius_image、null_norm_init、g_factor_monotonic、
// lut_monotonic_peak、lut_files_identical …

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "ehe/core/version.h"

TEST_CASE("core: 版本与构建开关可查询") {
    const std::string version = ehe::core::version_string();
    CHECK_FALSE(version.empty());

    const std::string flags = ehe::core::build_flags();
    CHECK_FALSE(flags.empty());
    // 编译期开关须与 CMake 选项一致（EHE_FP64_METRIC 默认 ON，DESIGN §4.3）
    CHECK(flags.rfind("fp64_metric=", 0) == 0);
}

TEST_CASE("core: 测试基础设施可用（doctest 接入自检）") {
    const int dummy = 0;
    CHECK(dummy == 0);
}
