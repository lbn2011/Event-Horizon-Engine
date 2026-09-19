#include "ehe/core/golden.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ehe/core/tonemap.h"  // 预览用 ACES（与 shaders/post_final.frag 同式，V5.9）
#include "ehe/core/units.h"

namespace ehe::core {
namespace {

/// 盘中面密度归一化：通量剖面 F(r) ∝ (1 − √(r_in/r)) / r³（Shakura–Sunyaev 无扭矩内边界）
inline double raw_flux(double r, double r_in) { return (1.0 - std::sqrt(r_in / r)) / (r * r * r); }

/// 用于着色的伪彩（g 因子调试视图）
Vec3 g_factor_pseudocolor(double g) {
    const double t = clamp_d((g - 0.5) / 1.0, 0.0, 1.0);  // g ∈ [0.5, 1.5] → [0,1]
    return Vec3{t, 0.3 * (1.0 - std::abs(t - 0.5) * 2.0), 1.0 - t};
}

}  // namespace

double disk_flux_profile(double r, const DiskParams& disk) {
    if (r <= disk.r_in) {
        return 0.0;
    }
    // 归一化到内边界的剖面峰值，保证与温度标定解耦
    const double reference = raw_flux(disk.r_in * 1.5, disk.r_in);
    return raw_flux(r, disk.r_in) / reference;
}

double disk_temperature(double r, const DiskParams& disk) {
    const double flux = disk_flux_profile(r, disk);
    if (flux <= 0.0) {
        return 0.0;
    }
    return disk.t_scale * std::pow(flux, 0.25);
}

double disk_density(double r, double z, const DiskParams& disk) {
    if (!disk.enabled || r < disk.r_in || r > disk.r_out) {
        return 0.0;
    }
    const double sigma = disk.thickness_scale * r;
    if (sigma <= 0.0) {
        return 0.0;
    }
    const double vertical = std::exp(-(z * z) / (2.0 * sigma * sigma));
    // 采样加速（§4.5 建议默认）：|z| > 3σ 视为无盘介质
    if (z * z > 9.0 * sigma * sigma) {
        return 0.0;
    }
    return disk.density * vertical * disk_flux_profile(r, disk);
}

Vec4 disk_four_velocity(const Vec3& position, double r) {
    // a = 0：Ω = r^{−3/2}（M = 1），u^t = 1/√(1 − 3/r)
    const double denom = 1.0 - 3.0 / r;
    if (denom <= 0.0) {
        return Vec4{0.0, 0.0, 0.0, 0.0};  // 光子球内不存在时序圆轨道（§4.5）
    }
    const double omega = 1.0 / (r * std::sqrt(r));
    const double u_t = 1.0 / std::sqrt(denom);
    const double u_phi = omega * u_t;

    // ∂_φ = (−y, x, 0)（a=0 时 KS 笛卡尔与 Boyer–Lindquist 的方位角一致，§13.1）
    return Vec4{u_t, -position.y * u_phi, position.x * u_phi, 0.0};
}

double redshift_factor(double k_dot_u_obs, const Vec4& k, const Vec4& u_em, const KsPoint& point) {
    // g = (k·u)_obs / (k·u)_em，k 为逆变分量，u 同为逆变 → 用 metric_dot
    const double k_dot_u_em = metric_dot(point, k, u_em);
    if (std::abs(k_dot_u_em) < 1e-12) {
        return 1.0;
    }
    return k_dot_u_obs / k_dot_u_em;
}

HdrImage render_golden(const GoldenParams& params, RenderStats* stats) {
    HdrImage image;
    image.width = params.width;
    image.height = params.height;
    image.pixels.assign(static_cast<std::size_t>(3) * params.width * params.height, 0.0F);

    RenderStats local_stats;
    local_stats.pixels = static_cast<std::size_t>(params.width) * params.height;
    local_stats.min_g = 1e30;
    local_stats.max_g = -1e30;

    const Camera camera(params.camera);
    const double aspect = static_cast<double>(params.width) / static_cast<double>(params.height);
    const Vec3 observer_position = camera.position();
    const KsPoint observer_point = ks_evaluate(observer_position, params.integrator.spin);
    const ObserverFrame observer_frame = static_observer_frame(observer_point);

    // 观测端的 (k·u)_obs：同一 k 在观测点与 u_obs 的缩并（g 因子为比值，与 k 的整体缩放无关）
    auto observer_k_dot_u = [&](const Vec4& k) { return metric_dot(observer_point, k, observer_frame.u); };

    std::size_t total_steps = 0;

    for (int py = 0; py < params.height; ++py) {
        for (int px = 0; px < params.width; ++px) {
            // 像素 → NDC（中心在像素中心）
            const double ndc_x = (2.0 * (px + 0.5) / params.width) - 1.0;
            const double ndc_y = 1.0 - (2.0 * (py + 0.5) / params.height);

            const Vec3 direction = camera.ray_direction(ndc_x, ndc_y, aspect);
            const Vec4 k0 = photon_from_frame(observer_frame, direction);
            const double k_dot_u_obs = observer_k_dot_u(k0);

            Vec4 x{0.0, observer_position.x, observer_position.y, observer_position.z};
            Vec4 k = k0;

            Vec3 color{0.0, 0.0, 0.0};
            double alpha = 0.0;
            double g_at_max_density = 1.0;
            double max_density_seen = 0.0;
            double null_drift = 0.0;

            int step = 0;
            TraceOutcome outcome = TraceOutcome::Exhausted;
            const double r_plus = units::horizon_radius(params.integrator.spin);
            const double capture_radius = r_plus * (1.0 + params.integrator.horizon_epsilon);

            for (; step <= params.integrator.n_max; ++step) {
                const Vec3 position{x.y, x.z, x.w};
                const double r = ks_radius(position, params.integrator.spin);
                const KsPoint point = ks_evaluate(position, params.integrator.spin);

                null_drift = std::max(null_drift, std::abs(metric_dot(point, k, k)));

                // ---- 盘体渲染（发射-吸收前向合成，§4.5）----
                // 采样点取步长中点：单点采样会在厚薄交界处产生"层状"伪影，中点采样显著改善
                if (params.disk.enabled && params.debug_view != DebugView::Classify) {
                    const double h = adaptive_step(r, params.integrator.spin, params.integrator);
                    const Vec3 mid{x.y + 0.5 * h * k.x, x.z + 0.5 * h * k.y, x.w + 0.5 * h * k.z};
                    const double r_mid = ks_radius(mid, params.integrator.spin);
                    const double density = disk_density(r_mid, mid.z, params.disk);
                    if (density > 1e-6) {
                        const KsPoint mid_point = ks_evaluate(mid, params.integrator.spin);
                        const Vec4 u_em = disk_four_velocity(mid, r_mid);
                        const double g = redshift_factor(k_dot_u_obs, k, u_em, mid_point);
                        if (params.compute_g_factor) {
                            local_stats.min_g = std::min(local_stats.min_g, g);
                            local_stats.max_g = std::max(local_stats.max_g, g);
                        }
                        if (density > max_density_seen) {
                            max_density_seen = density;
                            g_at_max_density = g;
                        }

                        const double temperature = disk_temperature(r_mid, params.disk) * g;  // T_eff = g·T
                        Vec3 emitted;
                        if (!params.lut.empty()) {
                            emitted = sample_blackbody_lut(params.lut, temperature, params.lut_size);
                        } else {
                            emitted = planck_linear_srgb(temperature);
                        }
                        // 发射 ∝ emission_scale·ρ·g³·LUT(T_eff)；吸收 ∝ κ·ρ（§4.5 + 规格补充）
                        const double g3 = g * g * g;
                        const double emission = params.disk.emission_scale * density * g3;
                        const double absorb = 1.0 - std::exp(-params.disk.kappa * density * h);

                        color += (1.0 - alpha) * emission * emitted * h;
                        alpha += (1.0 - alpha) * absorb;
                        if (alpha >= 0.99) {
                            outcome = TraceOutcome::Escaped;  // 不透明饱和：提前终止（§4.5）
                            total_steps += static_cast<std::size_t>(step);
                            break;
                        }
                    }
                }

                // ---- 终止条件（§4.3，按优先级）----
                if (r < capture_radius) {
                    outcome = TraceOutcome::Captured;
                    break;
                }
                if (r > params.integrator.escape_radius) {
                    outcome = TraceOutcome::Escaped;
                    break;
                }
                if (step == params.integrator.n_max) {
                    outcome = TraceOutcome::Exhausted;
                    break;
                }

                rk4_step(x, k, adaptive_step(r, params.integrator.spin, params.integrator),
                         params.integrator.spin);
            }

            total_steps += static_cast<std::size_t>(std::max(0, step));
            local_stats.max_null_drift = std::max(local_stats.max_null_drift, null_drift);

            // ---- 结局着色 ----
            Vec3 output;
            switch (params.debug_view) {
                case DebugView::Classify:
                    if (outcome == TraceOutcome::Captured) {
                        output = Vec3{0.0, 0.0, 0.0};
                        ++local_stats.captured;
                    } else if (outcome == TraceOutcome::Escaped) {
                        output = Vec3{1.0, 1.0, 1.0};
                        ++local_stats.escaped;
                    } else {
                        output = Vec3{1.0, 0.0, 0.0};
                        ++local_stats.exhausted;
                    }
                    break;
                case DebugView::GFactor:
                    output = g_factor_pseudocolor(g_at_max_density);
                    break;
                case DebugView::Shaded:
                default:
                    if (outcome == TraceOutcome::Captured) {
                        ++local_stats.captured;  // 视界黑体：不加背景
                    } else {
                        // 逃逸与步数用尽均按逃逸处理（§4.3 终止条件 ③）
                        if (outcome == TraceOutcome::Escaped) {
                            ++local_stats.escaped;
                        } else {
                            ++local_stats.exhausted;
                        }
                        color += (1.0 - alpha) * params.background;
                    }
                    output = color;
                    break;
            }

            float* pixel = image.at(px, py);
            pixel[0] = static_cast<float>(output.x);
            pixel[1] = static_cast<float>(output.y);
            pixel[2] = static_cast<float>(output.z);
        }
    }

    if (params.width > 0 && params.height > 0) {
        local_stats.mean_steps =
            static_cast<double>(total_steps) / static_cast<double>(params.width * params.height);
    }
    if (local_stats.max_g < local_stats.min_g) {
        local_stats.min_g = local_stats.max_g = 0.0;
    }
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return image;
}

std::vector<std::uint8_t> tonemap_to_srgb8(const HdrImage& image, double exposure, bool aces) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(image.width) * image.height * 3, 0);
    // 预览用色调映射：曝光 → ACES（Hill 拟合，与 shaders/post_final.frag 同式）→ sRGB 传输函数。
    // ACES 是**三通道联合**运算（输入/输出矩阵），故按像素（RGB 三元组）处理，不能逐标量走。
    // aces=false 时退化为 Reinhard（仅作对照）；PFM 基线始终是线性 HDR 原值（§6.3）。
    const std::size_t pixel_count = image.pixels.size() / 3;
    for (std::size_t p = 0; p < pixel_count; ++p) {
        const Vec3 hdr{static_cast<double>(image.pixels[p * 3 + 0]) * exposure,
                       static_cast<double>(image.pixels[p * 3 + 1]) * exposure,
                       static_cast<double>(image.pixels[p * 3 + 2]) * exposure};
        Vec3 mapped;
        if (aces) {
            mapped = aces_fitted(hdr);
        } else {
            mapped = Vec3{hdr.x / (1.0 + hdr.x), hdr.y / (1.0 + hdr.y), hdr.z / (1.0 + hdr.z)};
        }
        bytes[p * 3 + 0] = static_cast<std::uint8_t>(
            std::lround(clamp_d(linear_to_srgb(mapped.x), 0.0, 1.0) * 255.0));
        bytes[p * 3 + 1] = static_cast<std::uint8_t>(
            std::lround(clamp_d(linear_to_srgb(mapped.y), 0.0, 1.0) * 255.0));
        bytes[p * 3 + 2] = static_cast<std::uint8_t>(
            std::lround(clamp_d(linear_to_srgb(mapped.z), 0.0, 1.0) * 255.0));
    }
    return bytes;
}

}  // namespace ehe::core
