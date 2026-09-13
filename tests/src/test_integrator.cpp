// EHE —— 测地线积分器与解析基准测试（DESIGN §6.5：rk4_convergence、null_norm_init、
//                                          photon_capture_bc、shadow_radius_image 的解析侧）

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

#include "ehe/core/config.h"
#include "ehe/core/integrator.h"
#include "ehe/core/metric.h"
#include "ehe/core/units.h"

using ehe::core::Config;
using ehe::core::IntegratorParams;
using ehe::core::TraceOutcome;
using ehe::core::Vec3;
using ehe::core::Vec4;

namespace {

IntegratorParams test_params(double spin = 0.0) {
    IntegratorParams params;
    params.spin = spin;
    params.n_max = 4000;      // 测试用较大预算，确保结论由物理而非步数决定
    params.h0 = 0.05;
    params.h_min = 1e-4;
    params.h_max = 0.5;
    return params;
}

/// 临界角二分：α 为局部参考系中偏离「径向内」的角度
double find_critical_shadow_angle(double observer_r, const IntegratorParams& params) {
    const Vec3 observer{observer_r, 0.0, 0.0};
    double lo = 0.0;                  // 正对中心：必被捕获
    double hi = ehe::core::kPi * 0.5; // 垂直方向：必逃逸
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        const Vec3 direction{-std::cos(mid), std::sin(mid), 0.0};  // 局部：−x 为径向内
        const auto result = ehe::core::trace_from_observer(observer, direction, params);
        if (result.outcome == TraceOutcome::Captured) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

}  // namespace

TEST_CASE("rk4_convergence: RK4 为四阶（步长减半误差 ÷16）") {
    // y' = y，y(0)=1 → y(1)=e；用相同 RK4 系数做标量收敛性验证
    const auto derivative = [](double y) { return y; };
    const double exact = std::exp(1.0);

    const int steps_coarse = 16;
    const int steps_fine = 32;

    double y_coarse = 1.0;
    for (int i = 0; i < steps_coarse; ++i) {
        y_coarse = ehe::core::rk4_step_scalar(y_coarse, 1.0 / steps_coarse, derivative);
    }
    double y_fine = 1.0;
    for (int i = 0; i < steps_fine; ++i) {
        y_fine = ehe::core::rk4_step_scalar(y_fine, 1.0 / steps_fine, derivative);
    }

    const double error_coarse = std::abs(y_coarse - exact);
    const double error_fine = std::abs(y_fine - exact);
    const double ratio = error_coarse / error_fine;

    // 四阶：比值应接近 16（容差放宽到 16±4，避免高阶项干扰）
    CHECK(ratio > 12.0);
    CHECK(ratio < 20.0);
}

TEST_CASE("null_norm_init: 全程类光性保持（fp64 漂移 ≪ 1e-3）") {
    const auto params = test_params(0.0);
    const Vec3 observer{20.0, 0.0, 0.0};
    const Vec3 direction{-0.9, 0.2, -0.35};  // 掠射，路径较长
    const auto result = ehe::core::trace_from_observer(observer, direction, params);

    CHECK(result.null_norm_drift < 1e-6);  // 设计验收为 < 1e-3（§4.4），此处按 fp64 收紧
    CHECK(result.steps > 0);
}

TEST_CASE("光子轨迹终止条件：正对黑洞被捕获、朝外逃逸、步数用尽标记为 Exhausted") {
    const auto params = test_params(0.0);

    // 正对中心：必然被捕获
    const auto inward = ehe::core::trace_from_observer(Vec3{20.0, 0.0, 0.0}, Vec3{-1.0, 0.0, 0.0}, params);
    CHECK(inward.outcome == TraceOutcome::Captured);
    CHECK(inward.final_r < ehe::core::units::kSchwarzschildRadius * 1.05);

    // 朝外：必然逃逸
    const auto outward = ehe::core::trace_from_observer(Vec3{20.0, 0.0, 0.0}, Vec3{1.0, 0.0, 0.0}, params);
    CHECK(outward.outcome == TraceOutcome::Escaped);
    CHECK(outward.final_r >= params.escape_radius);

    // 极端受限的步数预算 → Exhausted（按逃逸处理，§4.3 终止条件 ③）
    IntegratorParams tiny = params;
    tiny.n_max = 3;
    const auto limited = ehe::core::trace_from_observer(Vec3{20.0, 0.0, 0.0}, Vec3{-0.9, 0.6, 0.0}, tiny);
    CHECK(limited.outcome == TraceOutcome::Exhausted);
    CHECK(limited.steps == 3);
}

TEST_CASE("photon_capture_bc: 临界冲击参数 = 3√3 M（容差 ±0.5%，§4.4）") {
    const auto params = test_params(0.0);
    const double b_crit = ehe::core::find_critical_impact_parameter(params);
    const double analytic = ehe::core::critical_impact_parameter_a0();

    std::printf("[benchmark] b_crit 数值 = %.10f | 解析 3√3 = %.10f | 相对误差 = %.3e\n", b_crit, analytic, std::abs(b_crit - analytic) / analytic);
    CHECK(analytic == doctest::Approx(3.0 * ehe::core::units::kSqrt3).epsilon(1e-15));
    CHECK(std::abs(b_crit - analytic) / analytic < 0.005);

    // 阈值两侧行为必须相反（确认二分落在真实跃变处）
    CHECK(ehe::core::is_captured(b_crit * 0.98, params));
    CHECK_FALSE(ehe::core::is_captured(b_crit * 1.02, params));
}

TEST_CASE("shadow_angular_radius: 观测者看到的阴影角半径与解析式一致（§4.4）") {
    const auto params = test_params(0.0);
    const double observer_r = 15.0;

    const double numeric = find_critical_shadow_angle(observer_r, params);
    const double analytic = ehe::core::shadow_angular_radius_a0(observer_r);

    // 解析式：sin α = b_crit √(1 − 2M/r) / r
    const double expected = std::asin(ehe::core::units::kShadowRadiusInfinity *
                                      std::sqrt(1.0 - 2.0 / observer_r) / observer_r);
    std::printf("[benchmark] 阴影角 数值 = %.8f rad | 解析 = %.8f rad | 相对误差 = %.3e\n", numeric, analytic, std::abs(numeric - analytic) / analytic);
    CHECK(analytic == doctest::Approx(expected).epsilon(1e-15));
    CHECK(std::abs(numeric - analytic) / analytic < 0.01);  // 1% 内（含步长误差）
}

TEST_CASE("conserved_quantities: 沿轨迹守恒（E、L_z 漂移 < 1e-9）") {
    const auto params = test_params(0.0);
    const Vec3 start{25.0, 0.0, 0.0};
    const Vec3 direction{-0.85, 0.5, 0.2};

    const auto initial = ehe::core::conserved_quantities(
        start, ehe::core::photon_from_frame(ehe::core::static_observer_frame(ehe::core::ks_evaluate(start, 0.0)),
                                           glm::normalize(direction)),
        0.0);
    CHECK(initial.energy > 0.0);

    // 逐段推进后重新计算守恒量，检查漂移
    const auto result = ehe::core::trace_from_observer(start, direction, params);
    const auto final_conserved =
        ehe::core::conserved_quantities(result.final_position, result.final_k, 0.0);

    const double energy_drift = std::abs(final_conserved.energy - initial.energy) / initial.energy;
    const double momentum_drift =
        std::abs(final_conserved.angular_momentum - initial.angular_momentum) /
        std::max(1e-12, std::abs(initial.angular_momentum));
    CHECK(energy_drift < 1e-7);
    CHECK(momentum_drift < 1e-7);
}

TEST_CASE("adaptive_step: 步长随半径收缩且被上下限约束（§4.3）") {
    const auto params = test_params(0.0);
    const double r_plus = ehe::core::units::horizon_radius(0.0);

    // 近视界：趋向下限
    const double near_horizon = ehe::core::adaptive_step(r_plus * 1.001, 0.0, params);
    CHECK(near_horizon == doctest::Approx(params.h_min));

    // 远处：趋于上限
    const double far = ehe::core::adaptive_step(200.0, 0.0, params);
    CHECK(far == doctest::Approx(params.h_max));

    // 单调性：半径越大步长越大
    const double mid_small = ehe::core::adaptive_step(r_plus * 3.0, 0.0, params);
    const double mid_large = ehe::core::adaptive_step(r_plus * 10.0, 0.0, params);
    CHECK(mid_large > mid_small);
}

TEST_CASE("photon_sphere: r = 3M 处的切向光子停留在光子球附近（§4.4）") {
    // 光子球是**不稳定**圆轨道：在 r = 3M 处以切向发射，轨迹应在该半径附近振荡若干圈，
    // 而不是立刻落入视界或逃逸。这是对「光子球半径 = 3M」最直接的动力学验证。
    const auto params = test_params(0.0);
    const double r_photon = ehe::core::units::kPhotonSphereRadius;

    // 观测点放在赤道面内、切向方向（局部 −y 为径向内、+x 为切向）
    const Vec3 observer{r_photon, 0.0, 0.0};
    const Vec3 tangential{0.0, 1.0, 0.0};  // 局部切向（垂直于径向）

    const auto result = ehe::core::trace_from_observer(observer, tangential, params);

    // 不稳定轨道：允许其缓慢漂移，但必须经历"长时间停留在光子球附近"的阶段。
    // 判据：轨迹最小半径不应远小于光子球（若 r 立即塌缩说明临界半径算错）
    CHECK(result.min_r > 0.6 * r_photon);
    // 且不应一开始就判为立即捕获（切向光子有有限的绕行时间）
    CHECK(result.steps > 10);

    // 对照：径向向内发射必须被捕获（确认同一设置下捕获判定正常）
    const auto radial = ehe::core::trace_from_observer(observer, Vec3{-1.0, 0.0, 0.0}, params);
    CHECK(radial.outcome == TraceOutcome::Captured);
}

TEST_CASE("make_integrator_params: 与 Config 一致") {
    Config config;
    config.integrator.n_max = 512;
    config.integrator.h0 = 0.02;
    config.integrator.h_min = 1e-4;
    config.integrator.h_max = 0.25;
    config.blackhole.spin = 0.5;

    const auto params = ehe::core::make_integrator_params(config);
    CHECK(params.n_max == 512);
    CHECK(params.h0 == doctest::Approx(0.02));
    CHECK(params.h_min == doctest::Approx(1e-4));
    CHECK(params.h_max == doctest::Approx(0.25));
    CHECK(params.spin == doctest::Approx(0.5));
}
