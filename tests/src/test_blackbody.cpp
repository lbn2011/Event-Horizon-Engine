// EHE —— 黑体色温与 LUT 测试（DESIGN §6.5：lut_monotonic_peak、lut_files_identical）
//
// 另含 SHA-256 的标准 KAT（空串摘要），确保一致性校验工具本身可信。

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ehe/core/blackbody.h"
#include "ehe/core/sha256.h"

using ehe::core::build_blackbody_lut;
using ehe::core::kBlackbodyLutSize;
using ehe::core::lut_index_for_temperature;
using ehe::core::lut_temperature_for_index;
using ehe::core::planck_linear_srgb;
using ehe::core::planck_spectral_radiance;
using ehe::core::planck_xyz;
using ehe::core::sample_blackbody_lut;
using ehe::core::Vec3;
using ehe::core::wien_peak_wavelength_nm;

TEST_CASE("sha256: 标准 KAT（空串摘要）") {
    // FIPS 180-4 已知答案，用于确认校验工具本身可信
    const auto digest = ehe::core::sha256(nullptr, 0);
    CHECK(ehe::core::sha256_hex(digest) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const std::string text = "abc";
    const auto digest_abc =
        ehe::core::sha256(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    CHECK(ehe::core::sha256_hex(digest_abc) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("planck_spectral_radiance: 峰值波长符合 Wien 位移定律") {
    for (const double temperature : {1000.0, 3000.0, 6500.0, 20000.0, 40000.0}) {
        const double predicted = wien_peak_wavelength_nm(temperature);
        if (predicted < ehe::core::kSpectrumMinNm || predicted > ehe::core::kSpectrumMaxNm) {
            continue;  // 峰值落在积分区间之外（如 1000 K 的 2898 nm）
        }
        // 在预测峰值附近 ±5 nm 扫描，数值峰值应与 Wien 定律一致
        double best_wavelength = predicted;
        double best_value = -1.0;
        for (double wavelength = predicted - 5.0; wavelength <= predicted + 5.0; wavelength += 0.1) {
            const double value = planck_spectral_radiance(wavelength, temperature);
            if (value > best_value) {
                best_value = value;
                best_wavelength = wavelength;
            }
        }
        CHECK(std::abs(best_wavelength - predicted) < 0.5);
    }
    // Wien 位移单调性：温度越高峰值波长越短
    CHECK(wien_peak_wavelength_nm(2000.0) > wien_peak_wavelength_nm(10000.0));
}

TEST_CASE("planck_xyz: 三刺激值随温度变化的定性正确性") {
    const auto cool = planck_xyz(2000.0);
    const auto hot = planck_xyz(20000.0);
    // 归一化后 Y = 1；低温相对偏红（X/Y 大），高温相对偏蓝（Z/Y 大）
    CHECK(cool.y == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(cool.x > hot.x);
    CHECK(hot.z > cool.z);
}

TEST_CASE("planck_linear_srgb: 低温暖红、高温暖蓝，且按峰值归一") {
    const Vec3 warm = planck_linear_srgb(1500.0);
    CHECK(warm.x == doctest::Approx(1.0).epsilon(1e-9));  // 峰值通道为红
    CHECK(warm.x > warm.y);
    CHECK(warm.y >= warm.z);

    const Vec3 cool = planck_linear_srgb(30000.0);
    CHECK(cool.z == doctest::Approx(1.0).epsilon(1e-9));  // 峰值通道为蓝
    CHECK(cool.z > cool.y);
    CHECK(cool.y > cool.x);

    // 所有温度下分量都在 [0, 1] 且至少一个通道为 1
    for (const double temperature : {1000.0, 3000.0, 6500.0, 10000.0, 40000.0}) {
        const Vec3 rgb = planck_linear_srgb(temperature);
        CHECK(rgb.x >= 0.0);
        CHECK(rgb.y >= 0.0);
        CHECK(rgb.z >= 0.0);
        const double peak = std::max({rgb.x, rgb.y, rgb.z});
        CHECK(peak == doctest::Approx(1.0).epsilon(1e-9));
    }
}

TEST_CASE("lut: 对数映射可逆且覆盖全温度域") {
    const double t_min = ehe::core::kBlackbodyTemperatureMin;
    const double t_max = ehe::core::kBlackbodyTemperatureMax;

    CHECK(lut_index_for_temperature(t_min) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(lut_index_for_temperature(t_max) == doctest::Approx(kBlackbodyLutSize - 1.0).epsilon(1e-9));

    // 索引 → 温度 → 索引 往返
    for (std::size_t i = 0; i < kBlackbodyLutSize; i += 17) {
        const double temperature = lut_temperature_for_index(i);
        CHECK(lut_index_for_temperature(temperature) == doctest::Approx(static_cast<double>(i)).epsilon(1e-9));
    }

    // 越界温度被钳制
    CHECK(lut_index_for_temperature(10.0) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(lut_index_for_temperature(1e9) == doctest::Approx(kBlackbodyLutSize - 1.0).epsilon(1e-9));
}

TEST_CASE("lut: 建构正确（长度、值域、随色温单调变化）") {
    const auto lut = build_blackbody_lut();
    REQUIRE(lut.size() == 3 * kBlackbodyLutSize);

    // 每个条目峰值通道为 1、无负值
    for (std::size_t i = 0; i < kBlackbodyLutSize; ++i) {
        const double r = lut[3 * i + 0];
        const double g = lut[3 * i + 1];
        const double b = lut[3 * i + 2];
        CHECK(r >= 0.0);
        CHECK(g >= 0.0);
        CHECK(b >= 0.0);
        CHECK(std::max({r, g, b}) == doctest::Approx(1.0).epsilon(1e-6));
    }

    // 红蓝比随温度单调递减（越热越蓝）
    double previous_ratio = 1e30;
    for (std::size_t i = 0; i < kBlackbodyLutSize; i += 8) {
        const double ratio = lut[3 * i + 0] / std::max(1.0e-6, static_cast<double>(lut[3 * i + 2]));
        CHECK(ratio <= previous_ratio + 1e-6);
        previous_ratio = ratio;
    }

    // 采样插值落在相邻条目之间；越界温度钳制
    const Vec3 low = sample_blackbody_lut(lut, 500.0);
    CHECK(low.x == doctest::Approx(lut[0]).epsilon(1e-6));
    const Vec3 high = sample_blackbody_lut(lut, 1e6);
    CHECK(high.z == doctest::Approx(lut[3 * (kBlackbodyLutSize - 1) + 2]).epsilon(1e-6));
}

TEST_CASE("lut: 二进制写入/读取往返一致") {
    const std::string path = "ehe_test_lut.f32";
    const auto lut = build_blackbody_lut(64);

    REQUIRE(ehe::core::write_blackbody_lut(path, lut, 64));

    std::vector<float> loaded;
    std::size_t count = 0;
    REQUIRE(ehe::core::read_blackbody_lut(path, loaded, count));
    CHECK(count == 64);
    REQUIRE(loaded.size() == lut.size());
    for (std::size_t i = 0; i < lut.size(); ++i) {
        CHECK(loaded[i] == doctest::Approx(lut[i]).epsilon(1e-7));
    }

    std::remove(path.c_str());

    // 损坏文件应被拒绝
    CHECK_FALSE(ehe::core::read_blackbody_lut("ehe_not_exist.f32", loaded, count));
}

TEST_CASE("lut_files_identical: tests/ 与 shaders/ 两份 LUT 的 SHA-256 一致（DESIGN §4.5）") {
    const std::string a = "tests/golden/blackbody_lut.f32";
    const std::string b = "shaders/blackbody_lut.f32";
    const std::string hash_a = ehe::core::sha256_file_hex(a);
    const std::string hash_b = ehe::core::sha256_file_hex(b);

    // LUT 由 ehe_reftool lut 生成；缺失即视为验收失败（DESIGN §6.5 要求两份拷贝一致）
    REQUIRE_FALSE(hash_a.empty());
    REQUIRE_FALSE(hash_b.empty());
    CHECK(hash_a == hash_b);
    CHECK(hash_a.size() == 64);
}
