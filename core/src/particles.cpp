#include "ehe/core/particles.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ehe::core {
namespace {

/// [0,1) 均匀浮点（来自 53 位整数缩放）
double uniform01(std::uint64_t& state) {
    state = particle_hash64(state);
    // 53 位尾数：避免 32 位 hash 直接 % 的低位相关性
    return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
}

/// Box–Muller 标准正态（一对只取一个，简单优先——初始化不在热路径）
double gaussian01(std::uint64_t& state) {
    const double u1 = std::max(uniform01(state), 1e-12);
    const double u2 = uniform01(state);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.14159265358979323846 * u2);
}

}  // namespace

std::uint64_t particle_hash64(std::uint64_t value) {
    // PCG-XSH-RR 风格的线性输出混合：无小循环、无查表；仅用于初始化与 shader 重生
    std::uint64_t x = value + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

double particle_kepler_speed(const double mass, const double radius) {
    return std::sqrt(mass / std::max(radius, 1e-9));
}

std::vector<float> generate_particle_buffer(const std::size_t count, const double mass,
                                            const double r_in, const double r_out,
                                            const double thickness, const std::uint64_t seed) {
    std::vector<float> buffer(count * kParticleFloatsPerParticle, 0.0F);
    if (count == 0) {
        return buffer;
    }

    const double r_lo = std::min(r_in, r_out);
    const double r_hi = std::max(r_in, r_out);
    const double sigma = std::max(thickness, 1e-6);

    std::uint64_t state = particle_hash64(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL);
    for (std::size_t i = 0; i < count; ++i) {
        // 环带半径均匀 + 方位均匀
        const double r = r_lo + (r_hi - r_lo) * uniform01(state);
        const double phi = 2.0 * 3.14159265358979323846 * uniform01(state);

        // 厚度：高斯（σ = thickness，§5.6 建议 0.5M）
        const double z = sigma * gaussian01(state);

        // 初速：开普勒圆速方位向 ±5% 各向同性扰动（§5.6 建议默认）
        const double speed = particle_kepler_speed(mass, r) * (0.95 + 0.10 * uniform01(state));

        // 方位向单位切向：t̂ = (-sin φ, cos φ, 0)（绕 +z 逆时针——与盘差速旋转方向一致）
        float* particle = buffer.data() + i * kParticleFloatsPerParticle;
        particle[0] = static_cast<float>(r * std::cos(phi));      // pos.x
        particle[1] = static_cast<float>(r * std::sin(phi));      // pos.y
        particle[2] = static_cast<float>(z);                      // pos.z
        particle[3] = static_cast<float>(particle_hash64(state)); // seed（shader 重生用）
        particle[4] = static_cast<float>(-speed * std::sin(phi)); // vel.x
        particle[5] = static_cast<float>(speed * std::cos(phi));  // vel.y
        particle[6] = 0.0F;                                       // vel.z
        particle[7] = 0.0F;                                       // pad
    }
    return buffer;
}

}  // namespace ehe::core
