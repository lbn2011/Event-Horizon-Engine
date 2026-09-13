#pragma once

// EHE core —— 光子测地线积分（DESIGN §4.3）
//
// 规格要点：
//   - 方程 d²x^μ/dλ² = −Γ^μ_αβ (dx^α/dλ)(dx^β/dλ)，状态 (x^μ, k^μ) 共 8 维
//   - 经典 RK4
//   - 步长策略 h = clamp(h₀ · f(r), h_min, h_max)，f(r) = (r − r₊)/r₊
//   - 终止条件（按优先级）：① r < r₊(1+ε) 命中视界 → 黑；② r > r_escape 逃逸 → 背景；
//     ③ 步数 ≥ N_max → 按逃逸处理
//
// 本模块是 L1 参考实现，全程 fp64（DESIGN §4.3 混合精度的「高精度基准」侧）。

#include <functional>

#include "ehe/core/config.h"
#include "ehe/core/metric.h"

namespace ehe::core {

/// 光线追踪结局（对应 §4.3 的三个终止条件）
enum class TraceOutcome { Captured, Escaped, Exhausted };

/// 单条光线的追踪结果（含诊断量，供调试视图与测试使用）
struct TraceResult {
    TraceOutcome outcome = TraceOutcome::Exhausted;
    int steps = 0;
    double lambda = 0.0;          ///< 总仿射参数推进量
    Vec3 final_position{};
    Vec4 final_k{};               ///< 终点波矢（逆变分量）
    double final_r = 0.0;
    double min_r = 0.0;           ///< 轨迹最小 KS 半径
    double null_norm_drift = 0.0; ///< 全程 max |g(k,k)|（类光性保持度，§4.4 验收 < 1e-3）
};

/// 积分参数（与 Config.integrator 对应；spin 由 BlackHoleConfig 传入）
struct IntegratorParams {
    int n_max = 300;
    double h0 = 0.05;
    double h_min = 1e-3;
    double h_max = 0.5;
    double spin = 0.0;
    double horizon_epsilon = 0.01;  ///< §4.3 ε：r < r₊(1+ε) 判为命中视界
    double escape_radius = 100.0;   ///< §4.3 r_escape
};

/// 从 Config 组装积分参数
IntegratorParams make_integrator_params(const Config& config);

/// 自适应步长（§4.3 建议默认）
double adaptive_step(double r, double spin, const IntegratorParams& params);

/// RK4 单步（原地更新 x 与 k；k 为逆变分量）
void rk4_step(Vec4& x, Vec4& k, double h, double spin);

/// 通用标量 RK4 单步，仅用于四阶收敛性验证（§6.5 rk4_convergence）
double rk4_step_scalar(double y, double h, const std::function<double(double)>& derivative);

/// 完整光线追踪
TraceResult trace_photon(const Vec4& x0, const Vec4& k0, const IntegratorParams& params);

/// 便利接口：从观测点按局部单位方向发射（内部构造 tetrad 与类光波矢，§4.2）
TraceResult trace_from_observer(const Vec3& position, const Vec3& n_hat,
                                const IntegratorParams& params);

// ---------------------------------------------------------------- 守恒量与解析基准

/// 稳态轴对称时空的守恒量（E、L_z 与冲击参数 b = L/E）
struct Conserved {
    double energy = 0.0;            ///< E = −k_t
    double angular_momentum = 0.0;  ///< L_z = x k_y − y k_x（绕 z 轴）
    double impact_parameter = 0.0;  ///< b = L/E
};

Conserved conserved_quantities(const Vec3& position, const Vec4& k, double spin);

/// a=0 临界冲击参数：b_crit = 3√3 M（DESIGN §4.4）
double critical_impact_parameter_a0();

/// 静态观测者看到的阴影角半径（a=0）：sin α = b_crit √(1 − r_s/r) / r
double shadow_angular_radius_a0(double observer_r);

/// 单次捕获判定（给定冲击参数；用于二分求临界值）
bool is_captured(double impact_parameter, const IntegratorParams& params, double start_distance = 1000.0);

/// 二分搜索临界冲击参数（返回捕获阈值 b_crit 的数值估计）
double find_critical_impact_parameter(const IntegratorParams& params, double lower = 4.0,
                                      double upper = 7.0, int iterations = 60);

}  // namespace ehe::core
