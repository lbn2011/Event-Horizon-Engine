#pragma once

// EHE core —— 盘湍流噪声与差速旋转（DESIGN §4.5）
//
// 用途：调制吸积盘的密度 ρ（进而影响发射 ε ∝ ρ），并按开普勒角速度 Ω(r) = r^{−3/2}（M=1）
//   让噪声图案**差速旋转**（内快外慢），时间由 UBO `time` 驱动。
//
// ⚠ 与 shader 的关系：本文件是 `shaders/common/noise.glsl` 的 C++ 对照实现——
//   **同一算法，但不是逐位一致**（GPU 与 CPU 的 float 运算次序/融合不同，而 hash 是
//   非连续函数，极小的输入差异会给出完全不同的输出）。
//   因此：**golden 基线一律取噪声振幅 0**（§6.1「无噪声动画」），噪声不进 NMSE 差分；
//   噪声自身的正确性由本文件的单元测试（确定性/值域/平滑性/旋转方向与速度）保证。
//
// 为什么 fbm 用 4 倍频：DESIGN §4.5 的规定值（早期注释里写过 5，以 §4.5 为准）。

#include <cstdint>

#include "ehe/core/math.h"

namespace ehe::core {

/// 3D→[0,1) hash（iq 风格，与 shader 同式）
double hash13(const Vec3& p);

/// 3D value noise（三线性插值 + 平滑步），值域 [0,1]
double value_noise(const Vec3& p);

/// 分形叠加：octaves 层，振幅 0.5 递减，频率 ×2.02，结果归一化到 [0,1]
double fbm(const Vec3& p, int octaves = 4);

/// 把盘采样点旋到「共转参考系」：图案按 Ω(r) 顺行旋转，等价于把采样坐标反向旋转 Ω(r)·t。
/// @param position 采样点（Kerr-Schild 笛卡尔）
/// @param r        该点的 KS 径向坐标
/// @param time     动画时间（秒）
/// @return 旋转后的采样坐标（z 不变）
Vec3 derotate_for_disk(const Vec3& position, double r, double time);

/// 开普勒角速度 Ω(r) = r^{−3/2}（M=1，a=0；光子球内无时序圆轨道 → r ≤ 3 时返回 0）
double kepler_omega(double r);

}  // namespace ehe::core
