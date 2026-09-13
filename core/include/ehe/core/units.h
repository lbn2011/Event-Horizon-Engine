#pragma once

// EHE core —— 几何化单位与解析基准常量（DESIGN §4）
//
// 单位约定：G = c = 1，长度单位取黑洞质量 M（即下文中 "M=1"）。
// 全部物理量以 M 为尺度无量纲化，面板"质量"仅改变场景尺度与盘温标定。
//
// 该文件是纯常量集，供 core（CPU 参考实现）、shader（同名常量需保持同步）与 UI 共用。
// 改动任何数值都必须同步检查 DESIGN §4.4 的解析基准表。

#include <cmath>

namespace ehe::core::units {

/// 黑洞质量（几何化单位下恒为 1；面板质量仅作展示/标定）
inline constexpr double kMass = 1.0;

/// 质量参数 M 的便捷别名（公式中常写作 M）
inline constexpr double M = kMass;

// ---------------------------------------------------------------- 解析基准（§4.4）
// a = 0（Schwarzschild）情形下的闭式结果，用作 L1 验证基准

/// 事件视界半径（a = 0）：r_s = 2M
inline constexpr double kSchwarzschildRadius = 2.0 * kMass;

/// 光子球半径：1.5 r_s = 3M
inline constexpr double kPhotonSphereRadius = 3.0 * kMass;

/// 阴影半径（a = 0）：3√3 M ≈ 5.196152422706632
/// 注：C++20 下 std::sqrt 非 constexpr（libc++），故直接写 √3 的字面量
inline constexpr double kSqrt3 = 1.7320508075688772935;
inline constexpr double kShadowRadiusInfinity = 3.0 * kSqrt3 * kMass;

/// 盘内半径（ISCO，a = 0）：3 r_s = 6M
inline constexpr double kIscoA0 = 6.0 * kMass;

// ---------------------------------------------------------------- 自旋与外视界（§4.1）

/// 自旋上限（DESIGN §4.1：钳制 0.998 防极端 Kerr 数值病态）
inline constexpr double kMaxSpin = 0.998;

/// 外视界半径（Kerr，M=1）：r₊ = M + √(M² − a²)
inline double horizon_radius(double spin) {
    const double a = (spin < 0.0) ? 0.0 : ((spin > kMaxSpin) ? kMaxSpin : spin);
    return kMass + std::sqrt(kMass * kMass - a * a);
}

// ---------------------------------------------------------------- 积分与场景默认（§4.3）
inline constexpr double kEscapeRadius = 100.0;   ///< 逃逸判定半径
inline constexpr double kHorizonEpsilon = 0.01;  ///< 视界命中判据：r < r₊(1+ε)

}  // namespace ehe::core::units
