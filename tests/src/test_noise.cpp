// EHE —— 盘湍流噪声与差速旋转单元测试（DESIGN §4.5）
//
// 为什么这些用例重要：噪声**不进 golden 差分**（GPU 与 CPU 的 hash 不可能逐位一致），
// 所以它的正确性只能靠这里保证——确定性、值域、平滑性、以及**差速旋转的物理方向与速度**。

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "ehe/core/noise.h"

using ehe::core::derotate_for_disk;
using ehe::core::fbm;
using ehe::core::hash13;
using ehe::core::kepler_omega;
using ehe::core::value_noise;
using ehe::core::Vec3;

TEST_CASE("noise: hash 值域 [0,1) 且确定性（同输入同输出）") {
    for (int i = 0; i < 512; ++i) {
        const double x = 0.37 * i - 17.25;
        const Vec3 p{x, x * 0.5 + 3.0, std::sin(x) * 4.0};
        const double h = hash13(p);
        CHECK(h >= 0.0);
        CHECK(h < 1.0);
        CHECK(hash13(p) == h);  // 确定性
    }
}

TEST_CASE("noise: value noise 值域 [0,1]、连续（相邻采样不跳变）、非常数") {
    double minimum = 1.0;
    double maximum = 0.0;
    for (int i = 0; i < 200; ++i) {
        for (int j = 0; j < 7; ++j) {
            const Vec3 p{0.1 * i, 0.13 * j, 0.07 * i * j};
            const double v = value_noise(p);
            CHECK(v >= 0.0);
            CHECK(v <= 1.0);
            minimum = std::min(minimum, v);
            maximum = std::max(maximum, v);
        }
    }
    CHECK(maximum - minimum > 0.2);  // 不能退化成常数

    // 连续性：步长 1e-3 时变化量应很小（value noise 是连续函数）
    const Vec3 a{1.234, 2.345, 3.456};
    const Vec3 b{1.234 + 1e-3, 2.345, 3.456};
    CHECK(std::abs(value_noise(a) - value_noise(b)) < 0.05);
}

TEST_CASE("noise: fbm 4 倍频、值域归一、随频率自相似") {
    double minimum = 1.0;
    double maximum = 0.0;
    for (int i = 0; i < 400; ++i) {
        const Vec3 p{0.31 * i, 0.17 * i + 0.5, 0.09 * i - 2.0};
        const double v = fbm(p);
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
        minimum = std::min(minimum, v);
        maximum = std::max(maximum, v);
    }
    CHECK(maximum - minimum > 0.3);

    // 4 层（DESIGN §4.5 定值）：与显式逐层结果一致
    const Vec3 p{1.7, -0.4, 2.9};
    double sum = 0.0;
    double amplitude = 0.5;
    double normalization = 0.0;
    Vec3 q = p;
    for (int i = 0; i < 4; ++i) {
        sum += amplitude * value_noise(q);
        normalization += amplitude;
        q = Vec3{q.x * 2.02, q.y * 2.02, q.z * 2.02};
        amplitude *= 0.5;
    }
    CHECK(fbm(p, 4) == doctest::Approx(sum / normalization).epsilon(1e-12));
}

TEST_CASE("noise: 开普勒角速度 Ω(r) = r^-3/2，且 r ≤ 3 时为 0") {
    CHECK(kepler_omega(6.0) == doctest::Approx(1.0 / (6.0 * std::sqrt(6.0))).epsilon(1e-12));
    CHECK(kepler_omega(20.0) == doctest::Approx(1.0 / (20.0 * std::sqrt(20.0))).epsilon(1e-12));
    CHECK(kepler_omega(6.0) > kepler_omega(20.0));  // 内快外慢
    CHECK(kepler_omega(3.0) == 0.0);                // 光子球内无时序圆轨道
    CHECK(kepler_omega(2.0) == 0.0);
}

TEST_CASE("noise: 差速旋转——图案按 Ω(r) 顺行转动，内圈转得更多") {
    // 采样坐标反向旋转 −Ω·t，因此把**同一个点**在 t>0 的采样坐标等价于把图案正向转 Ω·t。
    const Vec3 point{8.0, 0.0, 0.0};
    const double r = 8.0;
    const double time = 4.0;

    const Vec3 rotated = derotate_for_disk(point, r, time);
    CHECK(rotated.z == doctest::Approx(point.z));  // 只绕 z 轴

    // 旋转角 = −Ω(r)·t，方向为负（顺时针，即图案顺行）
    const double omega = kepler_omega(r);
    const double expected_angle = -omega * time;
    const double actual_angle = std::atan2(rotated.y, rotated.x);
    CHECK(actual_angle == doctest::Approx(expected_angle).epsilon(1e-9));

    // 半径不变（刚体旋转）
    CHECK(std::sqrt(rotated.x * rotated.x + rotated.y * rotated.y) ==
          doctest::Approx(std::sqrt(point.x * point.x + point.y * point.y)).epsilon(1e-12));

    // 内圈比外圈转得多：同一时间内转过的角度之比 = (r_in/r_out)^{-3/2}
    const double inner_angle = std::abs(kepler_omega(6.0) * time);
    const double outer_angle = std::abs(kepler_omega(12.0) * time);
    CHECK(inner_angle == doctest::Approx(outer_angle * std::pow(2.0, 1.5)).epsilon(1e-9));

    // t = 0 或 r ≤ 3 时不做旋转（短路，保证 golden 可复现）
    CHECK(derotate_for_disk(point, r, 0.0).x == doctest::Approx(point.x));
    CHECK(derotate_for_disk(point, 2.0, 5.0).x == doctest::Approx(point.x));
}
