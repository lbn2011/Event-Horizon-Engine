// EHE —— golden 渲染器测试（DESIGN §6.5：shadow_radius_image、g_factor_monotonic）
//
// 思路：用 classify 调试视图（命中=黑、逃逸=白）渲染小图，在像素域测量阴影角半径，
//       与解析式 sin α = b_crit √(1 − 2M/r) / r 对比；这是「图像侧」对物理内核的独立校验。

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

#include "ehe/core/blackbody.h"
#include "ehe/core/golden.h"
#include "ehe/core/integrator.h"
#include "ehe/core/units.h"

using ehe::core::CameraConfig;
using ehe::core::DebugView;
using ehe::core::GoldenParams;
using ehe::core::IntegratorParams;

namespace {

GoldenParams make_params(int size, DebugView view) {
    GoldenParams params;
    params.width = size;
    params.height = size;
    params.debug_view = view;

    params.camera.mode = ehe::core::CameraMode::Orbit;
    params.camera.dist = 15.0;
    params.camera.azim_deg = 0.0;
    params.camera.polar_deg = 90.0;  // 赤道面内观察：阴影为圆形
    params.camera.fov_deg = 60.0;  // 阴影角半径约 18.8°，60° 视场下留足边距

    params.integrator.spin = 0.0;
    params.integrator.n_max = 2000;
    params.integrator.h0 = 0.05;
    params.integrator.h_min = 1e-3;
    params.integrator.h_max = 0.5;

    params.disk.enabled = false;  // 纯阴影测量
    params.background = ehe::core::Vec3{0.0, 0.0, 0.0};
    params.lut = ehe::core::build_blackbody_lut();
    return params;
}

/// 在图像中心行上测出「黑→白」跃变的半宽（像素），换算为角度
double measure_shadow_angle(const ehe::core::HdrImage& image, double fov_deg) {
    const int center_y = image.height / 2;
    const int center_x = image.width / 2;
    int boundary = -1;
    for (int x = center_x; x < image.width; ++x) {
        if (image.at(x, center_y)[0] > 0.5F) {  // classify 视图：逃逸=白
            boundary = x;
            break;
        }
    }
    REQUIRE(boundary >= 0);

    // 跃变发生在 boundary-1（黑）与 boundary（白）之间，取中点
    const double x_boundary = static_cast<double>(boundary) - 0.5;
    const double ndc = 2.0 * x_boundary / static_cast<double>(image.width) - 1.0;
    const double tan_half_fov = std::tan(fov_deg * ehe::core::kPi / 360.0);
    // 方形视口下，局部参考系中的偏离角 α 满足 tan α = ndc · tan(fov/2)
    return std::atan(ndc * tan_half_fov);
}

}  // namespace

TEST_CASE("shadow_radius_image: 图像测得的阴影角半径与解析式一致（§4.4）") {
    const int size = 192;
    const GoldenParams params = make_params(size, DebugView::Classify);
    const auto image = ehe::core::render_golden(params);

    const double measured = measure_shadow_angle(image, params.camera.fov_deg);
    const double analytic = ehe::core::shadow_angular_radius_a0(params.camera.dist);

    std::printf("[benchmark] 阴影角（图像测量）= %.6f rad | 解析 = %.6f rad | 相对误差 = %.3e\n",
                measured, analytic, std::abs(measured - analytic) / analytic);
    // 图像侧含像素离散误差与有限步长误差，容差 3%
    CHECK(std::abs(measured - analytic) / analytic < 0.03);
}

TEST_CASE("classify 视图：捕获/逃逸像素统计合理") {
    const GoldenParams params = make_params(128, DebugView::Classify);
    ehe::core::RenderStats stats;
    const auto image = ehe::core::render_golden(params, &stats);
    (void)image;

    CHECK(stats.captured > 0);
    CHECK(stats.escaped > 0);
    CHECK(stats.captured + stats.escaped + stats.exhausted == stats.pixels);
    // 阴影面积占比应远小于全图（r=15、FOV=40° 时约 18.8° 半径 → 面积占比 ~ (sin α/sin(fov/2))² 量级）
    const double captured_fraction = static_cast<double>(stats.captured) / stats.pixels;
    CHECK(captured_fraction > 0.01);
    CHECK(captured_fraction < 0.6);
}

TEST_CASE("g_factor_monotonic: 多普勒聚束使 g 跨越 1（近侧 g>1、远侧 g<1）") {
    GoldenParams params = make_params(96, DebugView::GFactor);
    params.disk.enabled = true;
    params.disk.r_in = 6.0;
    params.disk.r_out = 20.0;
    params.disk.density = 1.0;
    params.camera.polar_deg = 75.0;  // 倾斜视角，两侧多普勒效应不对称

    ehe::core::RenderStats stats;
    ehe::core::render_golden(params, &stats);

    std::printf("[benchmark] g 因子范围 = [%.4f, %.4f]（mean steps = %.1f）\n", stats.min_g,
                stats.max_g, stats.mean_steps);
    CHECK(stats.min_g > 0.2);
    CHECK(stats.max_g < 3.0);
    CHECK(stats.min_g < 1.0);   // 退行侧红移
    CHECK(stats.max_g > 1.0);   // 接近侧蓝移 + 聚束
}

TEST_CASE("shaded 视图：盘体渲染产生非零亮度，且无 NaN") {
    GoldenParams params = make_params(96, DebugView::Shaded);
    params.disk.enabled = true;
    params.background = ehe::core::Vec3{0.02, 0.02, 0.02};

    ehe::core::RenderStats stats;
    const auto image = ehe::core::render_golden(params, &stats);

    double max_value = 0.0;
    double sum = 0.0;
    for (const float value : image.pixels) {
        CHECK(std::isfinite(value));
        max_value = std::max(max_value, static_cast<double>(value));
        sum += static_cast<double>(value);
    }
    CHECK(max_value > 1e-3);
    CHECK(sum > 0.0);
    std::printf("[benchmark] 类光性漂移 = %.3e（设计验收 < 1e-3，§4.4）\n", stats.max_null_drift);
    CHECK(stats.max_null_drift < 1e-6);  // fp64 参考实现（长路径 + 自适应小步长）
}
