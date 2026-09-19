#pragma once

// EHE core —— 粒子初始分布生成器（DESIGN §5.6 粒子模式）
//
// 职责边界：只做 **CPU 侧初始化数据生成**（上传 GPU SSBO 前的一次性内容），
//   leapfrog 积分与回收在 compute shader 内完成（shaders/particle_update.comp）。
//   本模块零图形依赖（core 约束 §5.2），因此生成规则可进 L2 单测。
//
// SSBO 布局（每粒子 2 × vec4 = 32 字节，与 shader 逐字段对齐）：
//   [0] pos.xyz + seed（seed 用于 shader 内回收重生时的伪随机）
//   [1] vel.xyz + pad
//
// 分布规格（§5.6）：
//   - 半径：环带 [r_in, r_out] 均匀（面密度均匀，非面积均匀——盘的初始简化）
//   - 厚度：z ~ 高斯（σ = 0.5M【建议默认】）
//   - 初速：牛顿开普勒圆速 v = sqrt(M/r) 方位向 ± 5% 各向同性扰动【建议默认】
//   - 方位角：[0, 2π) 均匀

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ehe/core/math.h"

namespace ehe::core {

/// SSBO 中单个粒子的字节数（2 × vec4）
inline constexpr std::size_t kParticleFloatsPerParticle = 8;

/// 生成初始粒子缓冲（交错 SOA，float 数组直接上传 SSBO）。
/// @param count     粒子数
/// @param mass      黑洞质量（几何化单位，M=1）
/// @param r_in      盘内半径
/// @param r_out     盘外半径
/// @param thickness 高斯厚度 σ（建议 0.5M）
/// @param seed      生成种子（同种子逐位可复现——便于测试与确定性对照）
/// @return count × 8 个 float；pos.xyz+seed / vel.xyz+pad 交错
std::vector<float> generate_particle_buffer(std::size_t count, double mass, double r_in,
                                            double r_out, double thickness,
                                            std::uint64_t seed);

/// 开普勒圆速（牛顿近似，§5.6）：v = sqrt(M/r)
double particle_kepler_speed(double mass, double radius);

/// 伪随机（uint64 PCG 哈希一步；与 shader 侧回收重生的 hash 规则独立——CPU 只需可复现）
std::uint64_t particle_hash64(std::uint64_t value);

}  // namespace ehe::core
