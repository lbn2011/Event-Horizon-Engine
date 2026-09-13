#pragma once

// EHE core —— 数学类型与工具（与 GLSL 语义对齐）
//
// 约定：
//   - core 侧数值计算一律 fp64（CPU 无性能顾虑，且是 L1 参考实现的精度基准，DESIGN §4.7/§6）
//   - 上传 GPU 时再转 fp32（见各后端的 UBO 组装）
//   - 本文件只做 GLM 的类型别名与少量几何工具，不引入任何图形依赖

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace ehe::core {

using Vec2 = glm::dvec2;
using Vec3 = glm::dvec3;
using Vec4 = glm::dvec4;
using Mat3 = glm::dmat3;
using Mat4 = glm::dmat4;

inline constexpr double kPi = glm::pi<double>();

inline double deg_to_rad(double degrees) { return degrees * kPi / 180.0; }
inline double rad_to_deg(double radians) { return radians * 180.0 / kPi; }

inline double clamp_d(double value, double low, double high) {
    return std::min(std::max(value, low), high);
}

/// 极角防万向锁（DESIGN §4.7：polar ∈ [0.05, π−0.05]）
inline constexpr double kPolarMin = 0.05;
inline constexpr double kPolarMax = kPi - 0.05;

/// 轨道距离范围（DESIGN §4.7：dist ∈ [8, 80]，滚轮对数缩放）
inline constexpr double kOrbitDistanceMin = 8.0;
inline constexpr double kOrbitDistanceMax = 80.0;

/// FOV 范围与默认（DESIGN §4.7：默认 40°，范围 [20°, 90°]）
inline constexpr double kFovMinDeg = 20.0;
inline constexpr double kFovMaxDeg = 90.0;
inline constexpr double kFovDefaultDeg = 40.0;

/// 安全的 acos：钳制入参防浮点越界导致的 NaN
inline double safe_acos(double value) { return std::acos(clamp_d(value, -1.0, 1.0)); }

/// 球坐标 → 笛卡尔（数学约定：polar 自 +Z 轴量起，azim 绕 Z 轴）
inline Vec3 spherical_to_cartesian(double radius, double azim_rad, double polar_rad) {
    const double sin_polar = std::sin(polar_rad);
    return Vec3{radius * sin_polar * std::cos(azim_rad),
                radius * sin_polar * std::sin(azim_rad),
                radius * std::cos(polar_rad)};
}

}  // namespace ehe::core
