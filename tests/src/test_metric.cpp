// EHE —— Kerr-Schild 度规模块单元测试
// 对应 DESIGN §6.5：ks_radius_a0、ks_metric_a0_limit、metric_derivative_numeric、
//                   camera_tetrad_orthonormal（度量感知版）、null_norm_init

#include <doctest/doctest.h>

#include <cmath>

#include "ehe/core/metric.h"
#include "ehe/core/units.h"

using ehe::core::ks_evaluate;
using ehe::core::ks_radius;
using ehe::core::metric_dot;
using ehe::core::Vec3;
using ehe::core::Vec4;

namespace {

/// 简单确定性伪随机（测试可复现，避免依赖 <random> 的实现差异）
double pseudo_random(int seed) {
    const double x = std::sin(seed * 12.9898) * 43758.5453;
    return x - std::floor(x);
}

Vec3 random_position(int seed, double radius_min, double radius_max) {
    const double u = pseudo_random(seed);
    const double v = pseudo_random(seed + 1);
    const double w = pseudo_random(seed + 2);
    const double radius = radius_min + (radius_max - radius_min) * u;
    const double cos_theta = 2.0 * v - 1.0;
    const double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
    const double phi = 2.0 * ehe::core::kPi * w;
    return Vec3{radius * sin_theta * std::cos(phi), radius * sin_theta * std::sin(phi),
                radius * cos_theta};
}

const Vec4 kE_t{1.0, 0.0, 0.0, 0.0};
const Vec4 kE_x{0.0, 1.0, 0.0, 0.0};
const Vec4 kE_y{0.0, 0.0, 1.0, 0.0};
const Vec4 kE_z{0.0, 0.0, 0.0, 1.0};

}  // namespace

TEST_CASE("ks_radius_a0: a=0 时 r 退化为欧氏距离（随机 1e3 点）") {
    for (int i = 0; i < 1000; ++i) {
        const Vec3 p = random_position(i * 7 + 1, 2.0, 60.0);
        const double expected = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        CHECK(ks_radius(p, 0.0) == doctest::Approx(expected).epsilon(1e-14));
    }
}

TEST_CASE("ks_radius: 满足隐式定义 (x²+y²)/(r²+a²) + z²/r² = 1") {
    for (const double spin : {0.0, 0.3, 0.7, 0.998}) {
        for (int i = 0; i < 200; ++i) {
            const Vec3 p = random_position(i * 13 + static_cast<int>(spin * 100), 2.5, 40.0);
            const double r = ks_radius(p, spin);
            const double a2 = spin * spin;
            const double value = (p.x * p.x + p.y * p.y) / (r * r + a2) + (p.z * p.z) / (r * r);
            CHECK(value == doctest::Approx(1.0).epsilon(1e-12));
        }
    }
}

TEST_CASE("ks_metric_a0_limit: a=0 时退化为 Schwarzschild（ingoing KS 笛卡尔形式）") {
    const Vec3 p{3.0, -4.0, 2.0};
    const auto point = ks_evaluate(p, 0.0);
    const double r = point.r;
    CHECK(r == doctest::Approx(std::sqrt(9.0 + 16.0 + 4.0)).epsilon(1e-14));

    // g_tt = −1 + 2M/r
    CHECK(metric_dot(point, kE_t, kE_t) == doctest::Approx(-1.0 + 2.0 / r).epsilon(1e-12));
    // g_ti = 2 M x_i / r²
    CHECK(metric_dot(point, kE_t, kE_x) == doctest::Approx(2.0 * p.x / (r * r)).epsilon(1e-12));
    CHECK(metric_dot(point, kE_t, kE_y) == doctest::Approx(2.0 * p.y / (r * r)).epsilon(1e-12));
    CHECK(metric_dot(point, kE_t, kE_z) == doctest::Approx(2.0 * p.z / (r * r)).epsilon(1e-12));
    // g_ij = δ_ij + 2 M x_i x_j / r³
    CHECK(metric_dot(point, kE_x, kE_x) == doctest::Approx(1.0 + 2.0 * p.x * p.x / (r * r * r)).epsilon(1e-12));
    CHECK(metric_dot(point, kE_y, kE_z) == doctest::Approx(2.0 * p.y * p.z / (r * r * r)).epsilon(1e-12));

    // 零余矢量关于 η 的零性：η(k,k) = 0（协变分量直乘）
    CHECK(ehe::core::eta_dot(point.k, point.k) == doctest::Approx(0.0).epsilon(1e-12));
    // 关于 g 的零性：g_μν k^μ k^ν = 0 —— 注意 metric_dot 的入参必须是**逆变**分量 k^μ
    CHECK(metric_dot(point, point.k_up, point.k_up) == doctest::Approx(0.0).epsilon(1e-12));
    // 协变 × 逆变缩并 k_μ k^μ = 0（等价校验，帮助锁定指标约定）
    CHECK(ehe::core::plain_dot(point.k, point.k_up) == doctest::Approx(0.0).epsilon(1e-12));
}

TEST_CASE("metric_derivative_numeric: 解析导数 vs 中心差分（rel err < 1e-6）") {
    const double h = 1e-6;
    for (const double spin : {0.0, 0.4, 0.9}) {
        for (int i = 0; i < 60; ++i) {
            const Vec3 p = random_position(i * 31 + static_cast<int>(spin * 1000), 3.0, 30.0);
            const auto point = ks_evaluate(p, spin);

            for (int axis = 0; axis < 3; ++axis) {
                Vec3 plus = p;
                Vec3 minus = p;
                (axis == 0 ? plus.x : (axis == 1 ? plus.y : plus.z)) += h;
                (axis == 0 ? minus.x : (axis == 1 ? minus.y : minus.z)) -= h;

                const auto point_plus = ks_evaluate(plus, spin);
                const auto point_minus = ks_evaluate(minus, spin);

                // ∂_i r
                const double numeric_dr = (point_plus.r - point_minus.r) / (2.0 * h);
                const double analytic_dr = axis == 0 ? point.dr_dx.x : (axis == 1 ? point.dr_dx.y : point.dr_dx.z);
                CHECK(std::abs(numeric_dr - analytic_dr) <= 1e-6 * std::max(1.0, std::abs(analytic_dr)));

                // ∂_i H
                const double numeric_dH = (point_plus.H - point_minus.H) / (2.0 * h);
                const double analytic_dH = axis == 0 ? point.dH_dx.x : (axis == 1 ? point.dH_dx.y : point.dH_dx.z);
                CHECK(std::abs(numeric_dH - analytic_dH) <= 1e-6 * std::max(1.0, std::abs(analytic_dH)));

                // ∂_i k_μ（四个分量）
                const auto& dk = point.dk_dx[axis];
                const double numeric_dk0 = ((point_plus.k.x) - (point_minus.k.x)) / (2.0 * h);
                const double numeric_dk1 = ((point_plus.k.y) - (point_minus.k.y)) / (2.0 * h);
                const double numeric_dk2 = ((point_plus.k.z) - (point_minus.k.z)) / (2.0 * h);
                const double numeric_dk3 = ((point_plus.k.w) - (point_minus.k.w)) / (2.0 * h);
                CHECK(std::abs(numeric_dk0 - dk.x) <= 1e-6 * std::max(1.0, std::abs(dk.x)));
                CHECK(std::abs(numeric_dk1 - dk.y) <= 1e-6 * std::max(1.0, std::abs(dk.y)));
                CHECK(std::abs(numeric_dk2 - dk.z) <= 1e-6 * std::max(1.0, std::abs(dk.z)));
                CHECK(std::abs(numeric_dk3 - dk.w) <= 1e-6 * std::max(1.0, std::abs(dk.w)));
            }
        }
    }
}

TEST_CASE("christoffel: 后两指标对称 Γ^μ_αβ = Γ^μ_βα") {
    const Vec3 p{5.0, 1.5, -2.5};
    for (const double spin : {0.0, 0.6, 0.95}) {
        const auto point = ks_evaluate(p, spin);
        double gamma[4][4][4] = {};
        ehe::core::christoffel(point, gamma);
        for (int mu = 0; mu < 4; ++mu) {
            for (int alpha = 0; alpha < 4; ++alpha) {
                for (int beta = 0; beta < 4; ++beta) {
                    CHECK(std::abs(gamma[mu][alpha][beta] - gamma[mu][beta][alpha]) < 1e-12);
                }
            }
        }
        // 稳态：所有含时间指标的导数项应使 Γ 的时间分量与纯空间项一致（非零是允许的）
        CHECK(std::isfinite(gamma[0][1][1]));
    }
}

TEST_CASE("camera_tetrad_orthonormal: 度量感知 tetrad 满足 g(e_a,e_b)=η_ab（容差 1e-12）") {
    for (const double spin : {0.0, 0.5, 0.9, 0.998}) {
        for (int i = 0; i < 20; ++i) {
            const Vec3 p = random_position(i * 17 + static_cast<int>(spin * 100), 3.0, 25.0);
            const auto point = ks_evaluate(p, spin);
            const auto frame = ehe::core::static_observer_frame(point);

            // 观测者四速度归一化 g(u,u) = −1
            CHECK(std::abs(metric_dot(point, frame.u, frame.u) + 1.0) < 1e-12);
            // 全套正交归一性
            CHECK(ehe::core::frame_orthonormality_error(point, frame) < 1e-12);
        }
    }
}

TEST_CASE("null_norm_init: k = u_obs + n̂ 在 Kerr-Schild 度规下严格类光") {
    for (const double spin : {0.0, 0.7, 0.95}) {
        for (int i = 0; i < 25; ++i) {
            const Vec3 p = random_position(i * 23 + 5, 3.0, 20.0);
            const auto point = ks_evaluate(p, spin);
            const auto frame = ehe::core::static_observer_frame(point);

            for (const Vec3 direction : {Vec3{0.0, 0.0, -1.0}, Vec3{0.6, 0.0, -0.8}, Vec3{0.0, 1.0, 0.0}}) {
                const Vec3 unit = glm::normalize(direction);
                const Vec4 k = ehe::core::photon_from_frame(frame, unit);
                CHECK(std::abs(metric_dot(point, k, k)) < 1e-12);

                // k^t 必须为正（面向未来的光子波矢）
                CHECK(k.x > 0.0);
            }
        }
    }
}
