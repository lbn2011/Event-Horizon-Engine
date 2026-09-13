#pragma once

// EHE core —— Kerr-Schild 度规与其导数（DESIGN §4.1 / §4.1.1）
//
// 约定（与 DESIGN 完全一致）：
//   - 几何化单位 M = 1，号差 (−,+,+,+)，坐标 (t, x, y, z)（Kerr-Schild 笛卡尔形式）
//   - g_μν = η_μν + 2H k_μ k_ν，H = r³/(r⁴ + a²z²)，k 为关于 η 的零余矢量
//   - **KS 的 r 不是欧氏距离**，由 (x²+y²)/(r²+a²) + z²/r² = 1 隐式定义
//   - 逆度规有精确封闭形式：g^μν = η^μν − 2H k^μ k^ν（k 对 η 零 ⇒ 求逆无分母）
//
// 本模块是 L1 CPU 参考实现的物理内核，全程 fp64；shader 侧 metric.glsl 须与之逐行对齐（§5.8）。

#include "ehe/core/math.h"

namespace ehe::core {

/// 某一时空点上的 Kerr-Schild 度规数据（含一阶导数，避免重复求值）
struct KsPoint {
    Vec3 position{};      ///< 空间坐标 (x, y, z)
    double spin = 0.0;    ///< 自旋 a
    double r = 0.0;       ///< Kerr-Schild 径向坐标（隐式定义解）
    double H = 0.0;       ///< 度规修正函数
    Vec4 k{};             ///< k_μ（协变，分量顺序 t,x,y,z）
    Vec4 k_up{};          ///< k^μ = η^μν k_ν（逆变）
    Vec3 dr_dx{};         ///< ∂_i r（i = x,y,z；∂_t = 0）
    Vec3 dH_dx{};         ///< ∂_i H
    Vec4 dk_dx[3]{};      ///< ∂_i k_μ，下标 0/1/2 对应 ∂_x/∂_y/∂_z
};

/// η 缩并：η_μν u^μ v^ν = −u_t v_t + u_x v_x + u_y v_y + u_z v_z
inline double eta_dot(const Vec4& u, const Vec4& v) {
    return -u.x * v.x + u.y * v.y + u.z * v.z + u.w * v.w;
}

/// 分量直乘（无度规）：k_μ u^μ 这类协变-逆变缩并
inline double plain_dot(const Vec4& u, const Vec4& v) {
    return u.x * v.x + u.y * v.y + u.z * v.z + u.w * v.w;
}

/// 由空间坐标解 Kerr-Schild 径向坐标 r（取正根）；a = 0 时退化为欧氏距离
double ks_radius(const Vec3& position, double spin);

/// 在给定点求度规、零余矢量与全部一阶导数
KsPoint ks_evaluate(const Vec3& position, double spin);

/// 度规缩并 g_μν u^μ v^ν = η(u,v) + 2H (k·u)(k·v)
///
/// ⚠ 指标约定：u、v 必须是**逆变**分量（u^μ），函数内部按 k_μ u^μ 做直乘缩并；
///   传入协变分量（如 point.k 本身）会得到无意义结果。需要缩并协变量时先用 k_up 或 lower_index。
double metric_dot(const KsPoint& point, const Vec4& u, const Vec4& v);

/// 提升/降低指标（用于诊断）：v_μ = g_μν v^ν
Vec4 lower_index(const KsPoint& point, const Vec4& v_up);

/// Christoffel 符号 Γ^μ_αβ（索引顺序 [μ][α][β]）；
/// ∂_α g_σβ = 2(∂_α H) k_σ k_β + 2H[(∂_α k_σ) k_β + k_σ (∂_α k_β)]，时间导数恒为 0
void christoffel(const KsPoint& point, double out[4][4][4]);

/// 光子测地线加速度 a^μ = −Γ^μ_αβ u^α u^β（不显式构造 Γ，直接做缩并）
Vec4 geodesic_accel(const KsPoint& point, const Vec4& u);

/// 静态观测者的局部正交归一四元基（tetrad），按 §4.2 用 g 做 Gram–Schmidt
struct ObserverFrame {
    Vec4 u{};    ///< 观测者四速度 u_obs（g(u,u) = −1）
    Vec4 e_x{};  ///< 空间基（局部坐标轴 x 方向同向）
    Vec4 e_y{};
    Vec4 e_z{};
};

ObserverFrame static_observer_frame(const KsPoint& point);

/// 由观测者 tetrad 与局部单位空间方向 n̂（分量对应 e_x/e_y/e_z）构造光子波矢：
///   k^μ = u_obs^μ + n̂^μ，且 g(k,k) = 0（§4.2 的 null 构造）
Vec4 photon_from_frame(const ObserverFrame& frame, const Vec3& n_hat);

/// 局部正交归一性检查：返回 |g(e_a,e_b) − η_ab| 的最大偏差（诊断/测试用）
double frame_orthonormality_error(const KsPoint& point, const ObserverFrame& frame);

}  // namespace ehe::core
