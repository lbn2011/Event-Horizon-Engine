// EHE —— 粒子初始分布生成器单测（core/particles.cpp，DESIGN §5.6）
//
// 覆盖（§5.6 分布规格）：
//   - 缓冲尺寸：count × 8 floats（2 × vec4：pos.xyz+seed / vel.xyz+pad）
//   - 半径：环带 [r_in, r_out] 均匀
//   - 厚度：z ~ 高斯 σ（统计近似检验）
//   - 初速：开普勒圆速 sqrt(M/r) ± 5% 扰动
//   - 种子可复现：同种子逐位一致，异种子必然不同
//   - 边界：count = 0 → 空缓冲；hash 函数确定性

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "ehe/core/particles.h"

namespace {

constexpr double kRIn = 6.0;
constexpr double kROut = 20.0;
constexpr double kMass = 1.0;
constexpr double kThickness = 0.5;

/// 从交错缓冲取第 i 个粒子（pos.xyz / pos_seed / vel.xyz / vel_pad）
struct ParticleView {
    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    double seed = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;
};

ParticleView particle_at(const std::vector<float>& buffer, std::size_t index) {
    const std::size_t base = index * ehe::core::kParticleFloatsPerParticle;
    ParticleView view{};
    view.px = static_cast<double>(buffer[base + 0]);
    view.py = static_cast<double>(buffer[base + 1]);
    view.pz = static_cast<double>(buffer[base + 2]);
    view.seed = static_cast<double>(buffer[base + 3]);
    view.vx = static_cast<double>(buffer[base + 4]);
    view.vy = static_cast<double>(buffer[base + 5]);
    view.vz = static_cast<double>(buffer[base + 6]);
    return view;
}

}  // namespace

TEST_CASE("particles: 缓冲尺寸与边界（§5.6）") {
    // count = 0 → 空缓冲
    CHECK(ehe::core::generate_particle_buffer(0, kMass, kRIn, kROut, kThickness, 1).empty());

    // 常规 count → count × 8 floats
    const std::size_t count = 1000;
    const std::vector<float> buffer =
        ehe::core::generate_particle_buffer(count, kMass, kRIn, kROut, kThickness, 20260920ULL);
    CHECK(buffer.size() == count * ehe::core::kParticleFloatsPerParticle);
}

TEST_CASE("particles: 半径 ∈ [r_in, r_out]、方位均匀覆盖、厚度为高斯（§5.6 分布）") {
    const std::size_t count = 5000;
    const std::vector<float> buffer =
        ehe::core::generate_particle_buffer(count, kMass, kRIn, kROut, kThickness, 42ULL);

    double z_sum = 0.0;
    double z_sq_sum = 0.0;
    std::size_t inner_half = 0;   // 半径落在环带内半区
    std::size_t in_annulus = 0;   // 半径落在环带内（全部粒子都应满足）
    for (std::size_t i = 0; i < count; ++i) {
        const ParticleView p = particle_at(buffer, i);
        const double r = std::sqrt(p.px * p.px + p.py * p.py);
        CHECK(r >= kRIn * 0.999);  // 容忍浮点舍入
        CHECK(r <= kROut * 1.001);
        if (r < (kRIn + kROut) * 0.5) {
            ++inner_half;
        }
        ++in_annulus;
        z_sum += p.pz;
        z_sq_sum += p.pz * p.pz;
    }
    CHECK(in_annulus == count);

    // 环带均匀（面密度）→ 内半区应约占一半（3σ 置信，n=5000 时 σ≈0.7%）
    const double inner_fraction = static_cast<double>(inner_half) / static_cast<double>(count);
    CHECK(inner_fraction > doctest::Approx(0.5 - 0.03).epsilon(0.001));
    CHECK(inner_fraction < doctest::Approx(0.5 + 0.03).epsilon(0.001));

    // 厚度：样本 std ≈ σ = 0.5（高斯抽样的 std 波动 ~ σ/sqrt(2n) ≈ 0.5%）
    const double mean = z_sum / static_cast<double>(count);
    CHECK(std::abs(mean) < 0.1);  // 均值应近 0
    const double variance = z_sq_sum / static_cast<double>(count) - mean * mean;
    const double sample_std = std::sqrt(variance);
    CHECK(sample_std > doctest::Approx(kThickness * 0.9).epsilon(0.01));
    CHECK(sample_std < doctest::Approx(kThickness * 1.1).epsilon(0.01));
}

TEST_CASE("particles: 初速为开普勒圆速 ±5%（牛顿近似，§5.6）") {
    const std::size_t count = 2000;
    const std::vector<float> buffer =
        ehe::core::generate_particle_buffer(count, kMass, kRIn, kROut, kThickness, 7ULL);

    for (std::size_t i = 0; i < count; ++i) {
        const ParticleView p = particle_at(buffer, i);
        const double r = std::sqrt(p.px * p.px + p.py * p.py);
        const double speed = std::sqrt(p.vx * p.vx + p.vy * p.vy + p.vz * p.vz);
        const double kepler = ehe::core::particle_kepler_speed(kMass, r);
        // 扰动 ±5% 内；给一点浮点余量
        CHECK(speed <= kepler * 1.051);
        CHECK(speed >= kepler * 0.949);
    }

    // 开普勒速度本身：v = sqrt(M/r)
    CHECK(ehe::core::particle_kepler_speed(1.0, 4.0) == doctest::Approx(0.5));
    CHECK(ehe::core::particle_kepler_speed(4.0, 4.0) == doctest::Approx(1.0));
}

TEST_CASE("particles: 种子可复现（同种子逐位一致，异种子不同）") {
    const std::vector<float> a = ehe::core::generate_particle_buffer(
        500, kMass, kRIn, kROut, kThickness, 12345678901234567890ULL);
    const std::vector<float> b = ehe::core::generate_particle_buffer(
        500, kMass, kRIn, kROut, kThickness, 12345678901234567890ULL);
    const std::vector<float> c = ehe::core::generate_particle_buffer(
        500, kMass, kRIn, kROut, kThickness, 12345678901234567891ULL);

    CHECK(a == b);  // 同种子逐位可复现（确定性对照的基础）
    CHECK(a != c);  // 异种子必然不同（否则回收重生的伪随机形同虚设）
}

TEST_CASE("particles: hash64 确定且非平凡") {
    const std::uint64_t x = 0x9E3779B97F4A7C15ULL;
    CHECK(ehe::core::particle_hash64(x) == ehe::core::particle_hash64(x));  // 确定性
    CHECK(ehe::core::particle_hash64(x) != x);                              // 非恒等
    CHECK(ehe::core::particle_hash64(x) != ehe::core::particle_hash64(x + 1));  // 单步变化
}
