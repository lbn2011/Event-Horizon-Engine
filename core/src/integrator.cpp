#include "ehe/core/integrator.h"

#include <algorithm>
#include <cmath>

#include "ehe/core/units.h"

namespace ehe::core {
namespace {

/// 状态导数：dx/dλ = k，dk/dλ = −Γ^μ_αβ k^α k^β
void derivative(const Vec4& x, const Vec4& k, double spin, Vec4& dx, Vec4& dk) {
    dx = k;
    const KsPoint point = ks_evaluate(Vec3{x.y, x.z, x.w}, spin);
    dk = geodesic_accel(point, k);
}

}  // namespace

IntegratorParams make_integrator_params(const Config& config) {
    IntegratorParams params;
    params.n_max = config.integrator.n_max;
    params.h0 = config.integrator.h0;
    params.h_min = config.integrator.h_min;
    params.h_max = config.integrator.h_max;
    params.spin = config.blackhole.spin;
    params.horizon_epsilon = units::kHorizonEpsilon;
    params.escape_radius = units::kEscapeRadius;
    return params;
}

double adaptive_step(double r, double spin, const IntegratorParams& params) {
    // f(r) = (r − r₊)/r₊：近视界线性收缩；远处按 h_max 封顶（DESIGN §4.3 建议默认）
    const double r_plus = units::horizon_radius(spin);
    const double f = (r - r_plus) / r_plus;
    return clamp_d(params.h0 * f, params.h_min, params.h_max);
}

void rk4_step(Vec4& x, Vec4& k, double h, double spin) {
    Vec4 dx1;
    Vec4 dk1;
    derivative(x, k, spin, dx1, dk1);

    const Vec4 x2 = x + dx1 * (0.5 * h);
    const Vec4 k2 = k + dk1 * (0.5 * h);
    Vec4 dx2;
    Vec4 dk2;
    derivative(x2, k2, spin, dx2, dk2);

    const Vec4 x3 = x + dx2 * (0.5 * h);
    const Vec4 k3 = k + dk2 * (0.5 * h);
    Vec4 dx3;
    Vec4 dk3;
    derivative(x3, k3, spin, dx3, dk3);

    const Vec4 x4 = x + dx3 * h;
    const Vec4 k4 = k + dk3 * h;
    Vec4 dx4;
    Vec4 dk4;
    derivative(x4, k4, spin, dx4, dk4);

    x = x + (dx1 + dx2 * 2.0 + dx3 * 2.0 + dx4) * (h / 6.0);
    k = k + (dk1 + dk2 * 2.0 + dk3 * 2.0 + dk4) * (h / 6.0);
}

double rk4_step_scalar(double y, double h, const std::function<double(double)>& derivative_fn) {
    const double k1 = derivative_fn(y);
    const double k2 = derivative_fn(y + 0.5 * h * k1);
    const double k3 = derivative_fn(y + 0.5 * h * k2);
    const double k4 = derivative_fn(y + h * k3);
    return y + (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
}

TraceResult trace_photon(const Vec4& x0, const Vec4& k0, const IntegratorParams& params) {
    TraceResult result;
    Vec4 x = x0;
    Vec4 k = k0;
    const double r_plus = units::horizon_radius(params.spin);
    const double capture_radius = r_plus * (1.0 + params.horizon_epsilon);

    result.lambda = 0.0;
    result.min_r = ks_radius(Vec3{x.y, x.z, x.w}, params.spin);

    for (int step = 0; step <= params.n_max; ++step) {
        const Vec3 position{x.y, x.z, x.w};
        const double r = ks_radius(position, params.spin);
        result.min_r = std::min(result.min_r, r);

        // 类光性漂移诊断（§4.4：fp32 路径 < 1e-3；此处 fp64 应远小于该值）
        const KsPoint point = ks_evaluate(position, params.spin);
        result.null_norm_drift = std::max(result.null_norm_drift, std::abs(metric_dot(point, k, k)));

        // 终止条件 ①：命中视界（最先判定）
        if (r < capture_radius) {
            result.outcome = TraceOutcome::Captured;
            result.steps = step;
            result.final_position = position;
            result.final_k = k;
            result.final_r = r;
            return result;
        }
        // 终止条件 ②：逃逸
        if (r > params.escape_radius) {
            result.outcome = TraceOutcome::Escaped;
            result.steps = step;
            result.final_position = position;
            result.final_k = k;
            result.final_r = r;
            return result;
        }
        // 终止条件 ③：步数用尽（按逃逸处理）
        if (step == params.n_max) {
            result.outcome = TraceOutcome::Exhausted;
            result.steps = step;
            result.final_position = position;
            result.final_k = k;
            result.final_r = r;
            return result;
        }

        const double h = adaptive_step(r, params.spin, params);
        rk4_step(x, k, h, params.spin);
        result.lambda += h;
    }

    return result;
}

TraceResult trace_from_observer(const Vec3& position, const Vec3& n_hat,
                                const IntegratorParams& params) {
    const KsPoint point = ks_evaluate(position, params.spin);
    const ObserverFrame frame = static_observer_frame(point);
    const Vec3 unit = glm::length(n_hat) > 1e-12 ? glm::normalize(n_hat) : Vec3{0.0, 0.0, -1.0};
    const Vec4 k = photon_from_frame(frame, unit);
    return trace_photon(Vec4{0.0, position.x, position.y, position.z}, k, params);
}

Conserved conserved_quantities(const Vec3& position, const Vec4& k, double spin) {
    const KsPoint point = ks_evaluate(position, spin);
    const Vec4 k_down = lower_index(point, k);  // k_μ = g_μν k^ν

    Conserved conserved;
    // ξ = ∂_t 为 Killing 矢量：E = −k_μ ξ^μ = −k_t
    conserved.energy = -k_down.x;
    // ξ = ∂_φ = (−y, x, 0)：L_z = k_μ ξ^μ = −y k_x + x k_y
    conserved.angular_momentum = -position.y * k_down.y + position.x * k_down.z;
    conserved.impact_parameter =
        (std::abs(conserved.energy) > 1e-300) ? conserved.angular_momentum / conserved.energy : 0.0;
    return conserved;
}

double critical_impact_parameter_a0() { return units::kShadowRadiusInfinity; }

double shadow_angular_radius_a0(double observer_r) {
    // 静态观测者：sin α = b_crit √(1 − r_s/r) / r（Schwarzschild，M = 1）
    const double r_s = units::kSchwarzschildRadius;
    const double factor = 1.0 - r_s / observer_r;
    if (factor <= 0.0) {
        return 0.0;  // 观测者在视界内（不应出现）
    }
    const double value = units::kShadowRadiusInfinity * std::sqrt(factor) / observer_r;
    return std::asin(clamp_d(value, -1.0, 1.0));
}

bool is_captured(double impact_parameter, const IntegratorParams& params, double start_distance) {
    // 起点置于 −x 方向远处，沿 +x 前进，y 方向偏移给出冲击参数（D ≫ b 时 b ≈ y0；
    // 精确 b 由守恒量计算，二分时按 y0 搜索、判定用精确 b）
    //
    // ⚠ 关键：起始半径必须小于逃逸半径，否则第一步就会按「逃逸」终止（§4.3 终止条件 ②）。
    //   因此这里显式放大逃逸半径，保证追踪从起点开始向内推进。
    IntegratorParams local = params;
    local.escape_radius = std::max(params.escape_radius, start_distance * 1.5);

    const Vec4 x0{0.0, -start_distance, impact_parameter, 0.0};
    const Vec4 k0{1.0, 1.0, 0.0, 0.0};  // 无穷远近似：k^μ ≈ (1, n̂)
    const TraceResult result = trace_photon(x0, k0, local);
    return result.outcome == TraceOutcome::Captured;
}

double find_critical_impact_parameter(const IntegratorParams& params, double lower, double upper,
                                      int iterations) {
    // 用 y0 二分（判定用精确冲击参数 b = L/E 表示），返回临界 b
    double lo = lower;
    double hi = upper;
    for (int i = 0; i < iterations; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (is_captured(mid, params)) {
            lo = mid;  // 仍被捕获 → 临界值更大
        } else {
            hi = mid;
        }
    }
    // 取阈值处的精确冲击参数；b 的符号表示绕轴旋转方向，临界值取模长。
    // 起点距离须与 is_captured 的默认起点一致（1000，见头文件默认参数）。
    const double y0 = 0.5 * (lo + hi);
    const Vec3 position{-1000.0, y0, 0.0};
    const Vec4 k{1.0, 1.0, 0.0, 0.0};
    return std::abs(conserved_quantities(position, k, params.spin).impact_parameter);
}

}  // namespace ehe::core
