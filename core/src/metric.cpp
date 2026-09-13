#include "ehe/core/metric.h"

#include <algorithm>
#include <cmath>

#include "ehe/core/units.h"

namespace ehe::core {
namespace {

/// (t,x,y,z) → 分量下标
inline double at(const Vec4& v, int index) {
    switch (index) {
        case 0: return v.x;
        case 1: return v.y;
        case 2: return v.z;
        default: return v.w;
    }
}

inline void set_at(Vec4& v, int index, double value) {
    switch (index) {
        case 0: v.x = value; break;
        case 1: v.y = value; break;
        case 2: v.z = value; break;
        default: v.w = value; break;
    }
}

/// 与 T1.1 一致的钳制（避免极端输入导致 NaN）
inline double clamp_spin(double spin) { return clamp_d(spin, 0.0, units::kMaxSpin); }

}  // namespace

double ks_radius(const Vec3& position, double spin) {
    const double a = clamp_spin(spin);
    const double r2_euclid = position.x * position.x + position.y * position.y + position.z * position.z;
    if (a == 0.0) {
        return std::sqrt(r2_euclid);  // 退化为欧氏距离（§4.1，L2 用例 ks_radius_a0）
    }
    const double s = r2_euclid - a * a;
    const double disc = std::sqrt(s * s + 4.0 * a * a * position.z * position.z);
    return std::sqrt(0.5 * (s + disc));  // 显式正根，见 §4.1
}

KsPoint ks_evaluate(const Vec3& position, double spin) {
    KsPoint point;
    point.position = position;
    point.spin = clamp_spin(spin);

    const double a = point.spin;
    const double a2 = a * a;
    const double x = position.x;
    const double y = position.y;
    const double z = position.z;

    point.r = ks_radius(position, a);
    const double r = point.r;
    const double r2 = r * r;

    // H = r³/(r⁴ + a²z²)
    const double r3 = r2 * r;
    const double denom_h = r2 * r2 + a2 * z * z;
    point.H = r3 / denom_h;

    // k_μ = (1, (rx+ay)/(r²+a²), (ry−ax)/(r²+a²), z/r)
    const double denom_q = r2 + a2;
    point.k = Vec4{1.0, (r * x + a * y) / denom_q, (r * y - a * x) / denom_q, z / r};

    // k^μ = η^μν k_ν = (−k_t, k_x, k_y, k_z)
    point.k_up = Vec4{-point.k.x, point.k.y, point.k.z, point.k.w};

    // ∂_i r = 分子 / 分母（§4.1.1 隐函数求导）
    const double denom_dr = r * (x * x + y * y) / (denom_q * denom_q) + z * z / (r3);
    point.dr_dx = Vec3{x / denom_q / denom_dr, y / denom_q / denom_dr, z / r2 / denom_dr};

    // ∂_i H = H·(3∂_i r/r − (4r³∂_i r + 2a²z δ_iz)/(r⁴+a²z²))
    const double dh_prefix = 4.0 * r3 / denom_h;
    point.dH_dx = Vec3{
        point.H * (3.0 * point.dr_dx.x / r - dh_prefix * point.dr_dx.x),
        point.H * (3.0 * point.dr_dx.y / r - dh_prefix * point.dr_dx.y),
        point.H * (3.0 * point.dr_dx.z / r - dh_prefix * point.dr_dx.z - 2.0 * a2 * z / denom_h),
    };

    // ∂_i k_μ（逐分量，含 ∂_i r 项）
    const double kx_num = r * x + a * y;
    const double ky_num = r * y - a * x;
    const double inv_q2 = 1.0 / (denom_q * denom_q);

    for (int i = 0; i < 3; ++i) {
        const double dr_i = at(Vec4{point.dr_dx.x, point.dr_dx.y, point.dr_dx.z, 0.0}, i);
        const double delta_ix = (i == 0) ? 1.0 : 0.0;
        const double delta_iy = (i == 1) ? 1.0 : 0.0;
        const double delta_iz = (i == 2) ? 1.0 : 0.0;

        const double dkx = ((dr_i * x + r * delta_ix + a * delta_iy) * denom_q - kx_num * 2.0 * r * dr_i) * inv_q2;
        const double dky = ((dr_i * y + r * delta_iy - a * delta_ix) * denom_q - ky_num * 2.0 * r * dr_i) * inv_q2;
        const double dkz = delta_iz / r - z * dr_i / r2;

        point.dk_dx[i] = Vec4{0.0, dkx, dky, dkz};  // ∂_i k_t = 0
    }

    return point;
}

double metric_dot(const KsPoint& point, const Vec4& u, const Vec4& v) {
    return eta_dot(u, v) + 2.0 * point.H * plain_dot(point.k, u) * plain_dot(point.k, v);
}

Vec4 lower_index(const KsPoint& point, const Vec4& v_up) {
    // v_μ = g_μν v^ν = η_μν v^ν + 2H k_μ (k_ν v^ν)
    const double kv = plain_dot(point.k, v_up);
    return Vec4{
        -v_up.x + 2.0 * point.H * point.k.x * kv,
        v_up.y + 2.0 * point.H * point.k.y * kv,
        v_up.z + 2.0 * point.H * point.k.z * kv,
        v_up.w + 2.0 * point.H * point.k.w * kv,
    };
}

void christoffel(const KsPoint& point, double out[4][4][4]) {
    // ∂_α g_σβ 仅在 α ∈ {1,2,3}（空间）非零（稳态时空）
    double dg[4][4][4] = {};  // dg[alpha][sigma][beta]
    for (int alpha = 1; alpha < 4; ++alpha) {
        const int i = alpha - 1;  // 空间导数下标
        const double dH = at(Vec4{point.dH_dx.x, point.dH_dx.y, point.dH_dx.z, 0.0}, i);
        const Vec4& dk = point.dk_dx[i];
        for (int sigma = 0; sigma < 4; ++sigma) {
            for (int beta = 0; beta < 4; ++beta) {
                const double k_sigma = at(point.k, sigma);
                const double k_beta = at(point.k, beta);
                dg[alpha][sigma][beta] = 2.0 * dH * k_sigma * k_beta +
                                         2.0 * point.H * (at(dk, sigma) * k_beta + k_sigma * at(dk, beta));
            }
        }
    }

    // Γ^μ_αβ = ½ g^μσ (∂_α g_σβ + ∂_β g_σα − ∂_σ g_αβ)，g^μσ = η^μσ − 2H k^μ k^σ
    for (int mu = 0; mu < 4; ++mu) {
        for (int alpha = 0; alpha < 4; ++alpha) {
            for (int beta = 0; beta < 4; ++beta) {
                double sum = 0.0;
                for (int sigma = 0; sigma < 4; ++sigma) {
                    // η^μσ：对角，t 方向带负号
                    const double eta_up = (mu == sigma) ? ((mu == 0) ? -1.0 : 1.0) : 0.0;
                    const double g_up = eta_up - 2.0 * point.H * at(point.k_up, mu) * at(point.k_up, sigma);
                    const double bracket = dg[alpha][sigma][beta] + dg[beta][sigma][alpha] - dg[sigma][alpha][beta];
                    sum += g_up * bracket;
                }
                out[mu][alpha][beta] = 0.5 * sum;
            }
        }
    }
}

Vec4 geodesic_accel(const KsPoint& point, const Vec4& u) {
    // 直接缩并：a^μ = −g^μσ (∂_α g_σβ) u^α u^β + ½ g^μσ (∂_σ g_αβ) u^α u^β
    // 即 a^μ = −Γ^μ_αβ u^α u^β（对 k 的对称性做了显式展开，避免构造 64 个 Γ）
    double dg[4][4][4] = {};
    for (int alpha = 1; alpha < 4; ++alpha) {
        const int i = alpha - 1;
        const double dH = at(Vec4{point.dH_dx.x, point.dH_dx.y, point.dH_dx.z, 0.0}, i);
        const Vec4& dk = point.dk_dx[i];
        for (int sigma = 0; sigma < 4; ++sigma) {
            for (int beta = 0; beta < 4; ++beta) {
                const double k_sigma = at(point.k, sigma);
                const double k_beta = at(point.k, beta);
                dg[alpha][sigma][beta] = 2.0 * dH * k_sigma * k_beta +
                                         2.0 * point.H * (at(dk, sigma) * k_beta + k_sigma * at(dk, beta));
            }
        }
    }

    // 预计算两个缩并项
    double term1[4] = {};  // (∂_α g_σβ) u^α u^β
    double term3[4] = {};  // (∂_σ g_αβ) u^α u^β
    for (int sigma = 0; sigma < 4; ++sigma) {
        double acc1 = 0.0;
        double acc3 = 0.0;
        for (int alpha = 0; alpha < 4; ++alpha) {
            const double ua = at(u, alpha);
            for (int beta = 0; beta < 4; ++beta) {
                const double ub = at(u, beta);
                acc1 += dg[alpha][sigma][beta] * ua * ub;
                acc3 += dg[sigma][alpha][beta] * ua * ub;
            }
        }
        term1[sigma] = acc1;
        term3[sigma] = acc3;
    }

    Vec4 accel{};
    for (int mu = 0; mu < 4; ++mu) {
        double sum = 0.0;
        for (int sigma = 0; sigma < 4; ++sigma) {
            const double eta_up = (mu == sigma) ? ((mu == 0) ? -1.0 : 1.0) : 0.0;
            const double g_up = eta_up - 2.0 * point.H * at(point.k_up, mu) * at(point.k_up, sigma);
            sum += g_up * (term1[sigma] - 0.5 * term3[sigma]);
        }
        set_at(accel, mu, -sum);
    }
    return accel;
}

ObserverFrame static_observer_frame(const KsPoint& point) {
    ObserverFrame frame;

    // 静态观测者：u^μ = (u^t, 0,0,0)，u^t = 1/√(−g_tt)
    const double g_tt = metric_dot(point, Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0});
    const double u_t = 1.0 / std::sqrt(-g_tt);
    frame.u = Vec4{u_t, 0.0, 0.0, 0.0};

    // 空间三基：把坐标方向投影到 u 的正交补，再对 g 做 Gram–Schmidt 正交归一
    Vec4 basis[3] = {Vec4{0, 1, 0, 0}, Vec4{0, 0, 1, 0}, Vec4{0, 0, 0, 1}};
    for (int i = 0; i < 3; ++i) {
        Vec4 v = basis[i];

        // 去掉与 u 的分量（g(u,u) = −1）
        const double gu = metric_dot(point, frame.u, v);
        v = v + Vec4{gu * frame.u.x, gu * frame.u.y, gu * frame.u.z, gu * frame.u.w};

        // 对已接受基做 Gram–Schmidt
        for (int j = 0; j < i; ++j) {
            Vec4 accepted{};
            switch (j) {
                case 0: accepted = frame.e_x; break;
                case 1: accepted = frame.e_y; break;
                default: accepted = frame.e_z; break;
            }
            const double gv = metric_dot(point, accepted, v);
            v = v - Vec4{gv * accepted.x, gv * accepted.y, gv * accepted.z, gv * accepted.w};
        }

        const double norm2 = metric_dot(point, v, v);
        const double scale = (std::abs(norm2) > 1e-300) ? 1.0 / std::sqrt(norm2) : 0.0;
        v = Vec4{v.x * scale, v.y * scale, v.z * scale, v.w * scale};

        switch (i) {
            case 0: frame.e_x = v; break;
            case 1: frame.e_y = v; break;
            default: frame.e_z = v; break;
        }
    }
    return frame;
}

Vec4 photon_from_frame(const ObserverFrame& frame, const Vec3& n_hat) {
    // k^μ = u_obs^μ + n̂^μ（§4.2）；n̂ 为局部正交归一基下的单位空间矢量
    return Vec4{
        frame.u.x + n_hat.x * frame.e_x.x + n_hat.y * frame.e_y.x + n_hat.z * frame.e_z.x,
        frame.u.y + n_hat.x * frame.e_x.y + n_hat.y * frame.e_y.y + n_hat.z * frame.e_z.y,
        frame.u.z + n_hat.x * frame.e_x.z + n_hat.y * frame.e_y.z + n_hat.z * frame.e_z.z,
        frame.u.w + n_hat.x * frame.e_x.w + n_hat.y * frame.e_y.w + n_hat.z * frame.e_z.w,
    };
}

double frame_orthonormality_error(const KsPoint& point, const ObserverFrame& frame) {
    const Vec4 basis[4] = {frame.u, frame.e_x, frame.e_y, frame.e_z};
    double max_error = 0.0;
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            const double expected = (a == b) ? ((a == 0) ? -1.0 : 1.0) : 0.0;
            const double actual = metric_dot(point, basis[a], basis[b]);
            max_error = std::max(max_error, std::abs(actual - expected));
        }
    }
    return max_error;
}

}  // namespace ehe::core
