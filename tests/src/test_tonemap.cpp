// EHE —— 后处理链数学单元测试（DESIGN §4.6）
//
// 为什么必须测：后处理作用在 golden **之前**（golden 是 post 前的 HDR 输出，§6.1），
// 所以它不进 NMSE 差分——它的正确性只能由这里保证：单调性、值域、锚点、恒等性质、可逆性。

#include <doctest/doctest.h>

#include <cmath>

#include "ehe/core/tonemap.h"

using ehe::core::aces_fitted;
using ehe::core::chroma_sample_uv;
using ehe::core::downsample_box;
using ehe::core::HdrImageF;
using ehe::core::linear_to_srgb;
using ehe::core::srgb_to_linear;
using ehe::core::upscale_catmull_rom;
using ehe::core::Vec3;

namespace {

HdrImageF make_gradient(int width, int height, double base) {
    HdrImageF image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(3) * width * height, 0.0F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float* pixel = image.at(x, y);
            pixel[0] = static_cast<float>(base + 0.001 * x);
            pixel[1] = static_cast<float>(base + 0.002 * y);
            pixel[2] = static_cast<float>(base);
        }
    }
    return image;
}

HdrImageF make_constant(int width, int height, float value) {
    HdrImageF image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(3) * width * height, value);
    return image;
}

}  // namespace

TEST_CASE("tonemap: sRGB 传输函数与逆函数互逆（§4.6 末尾编码）") {
    // 锚点：线性 0.0031308 ↔ 编码 0.04045（sRGB 曲线的分段点）
    CHECK(linear_to_srgb(0.0) == doctest::Approx(0.0));
    CHECK(linear_to_srgb(1.0) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(linear_to_srgb(0.0031308) == doctest::Approx(0.04045).epsilon(1e-4));

    for (int i = 0; i <= 100; ++i) {
        const double linear = 0.01 * i;
        const double round_trip = srgb_to_linear(linear_to_srgb(linear));
        CHECK(round_trip == doctest::Approx(linear).epsilon(1e-9));
    }
    // 单调递增
    CHECK(linear_to_srgb(0.25) < linear_to_srgb(0.5));
    CHECK(linear_to_srgb(0.5) < linear_to_srgb(0.75));
}

TEST_CASE("tonemap: ACES（Hill 拟合）单调、值域 [0,1]、零点与高光行为") {
    CHECK(aces_fitted(Vec3{0.0, 0.0, 0.0}).x == doctest::Approx(0.0).epsilon(1e-9));

    // 单调递增（HDR 值域内抽样）
    double previous = -1.0;
    for (int i = 0; i <= 200; ++i) {
        const double value = 0.005 * i;
        const double mapped = aces_fitted(Vec3{value, value, value}).x;
        CHECK(mapped >= previous - 1e-12);
        CHECK(mapped >= 0.0);
        CHECK(mapped <= 1.0);
        previous = mapped;
    }

    // 精确锚点：场景线性 1.0 → 线性 0.6191（手算：矩阵行和 = 1 → RRT/ODT 拟合
    //   numerator = 1.0245786 − 0.000090537 = 1.0244881；
    //   denominator = 0.983729 + 0.432951 + 0.238081 = 1.6547610；比值 = 0.61912）
    // 注意：常说"ACES 把 1.0 映射到 ~0.8"指的是 **sRGB 编码后**的值（≈0.81），不是线性值。
    const double mid = aces_fitted(Vec3{1.0, 1.0, 1.0}).x;
    CHECK(mid == doctest::Approx(0.61912).epsilon(1e-4));
    CHECK(linear_to_srgb(mid) == doctest::Approx(0.8100).epsilon(2e-3));

    // 高光挤压：10 与 100 的差远小于 100 倍
    const double high = aces_fitted(Vec3{10.0, 10.0, 10.0}).x;
    const double higher = aces_fitted(Vec3{100.0, 100.0, 100.0}).x;
    CHECK(high > mid);
    CHECK(higher >= high);
    CHECK(higher - high < 0.05);  // 已接近饱和

    // 通道独立性：单通道激励不应显著串扰到另外两个通道之外的通道（矩阵是正的但会混合）
    const Vec3 red = aces_fitted(Vec3{1.0, 0.0, 0.0});
    CHECK(red.x > red.y);
    CHECK(red.x > red.z);
}

TEST_CASE("tonemap: SSAA 面积加权降采样——整数倍率退化为等权、恒等倍率保持原值、能量守恒") {
    const HdrImageF src = make_gradient(8, 8, 0.5);

    // 1) 1:1 必须逐位保持
    const HdrImageF same = downsample_box(src, 8, 8);
    REQUIRE(same.width == 8);
    for (std::size_t i = 0; i < src.pixels.size(); ++i) {
        CHECK(same.pixels[i] == src.pixels[i]);
    }

    // 2) 2:1 整数倍率 = 2×2 等权平均（与手算一致）
    const HdrImageF half = downsample_box(src, 4, 4);
    REQUIRE(half.width == 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                double expected = 0.0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        expected += src.at(x * 2 + dx, y * 2 + dy)[channel];
                    }
                }
                expected *= 0.25;
                CHECK(half.at(x, y)[channel] == doctest::Approx(expected).epsilon(1e-6));
            }
        }
    }

    // 3) 非整数倍率（8→6，等价 1.33x）也应工作且不产生 NaN
    const HdrImageF fractional = downsample_box(src, 6, 6);
    REQUIRE(fractional.width == 6);
    for (float value : fractional.pixels) {
        CHECK(std::isfinite(value));
    }

    // 4) 常数图降采样后仍是同一常数（归一化正确，无能量漂移）
    const HdrImageF constant = make_constant(6, 6, 0.75F);
    const HdrImageF constant_half = downsample_box(constant, 3, 3);
    for (float value : constant_half.pixels) {
        CHECK(value == doctest::Approx(0.75).epsilon(1e-6));
    }

    // 5) 目标大于源 → 拒绝（应返回空图，由调用方走升采样路径）
    CHECK(downsample_box(src, 16, 16).width == 0);
}

TEST_CASE("tonemap: Catmull-Rom 升采样——同尺寸恒等、常数保持、值域不越界") {
    const HdrImageF src = make_gradient(4, 4, 0.25);

    // 同尺寸：插值核在整数位置应还原原值（权重 (0,1,0,0)）
    const HdrImageF same = upscale_catmull_rom(src, 4, 4);
    REQUIRE(same.width == 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                CHECK(same.at(x, y)[channel] == doctest::Approx(src.at(x, y)[channel]).epsilon(1e-6));
            }
        }
    }

    // 常数图升采样后仍是同常数（核权重和为 1）
    const HdrImageF constant = make_constant(3, 3, 0.5F);
    const HdrImageF upscaled = upscale_catmull_rom(constant, 9, 9);
    REQUIRE(upscaled.width == 9);
    for (float value : upscaled.pixels) {
        CHECK(value == doctest::Approx(0.5).epsilon(1e-5));
    }

    // 不产生 NaN/Inf
    const HdrImageF big = upscale_catmull_rom(src, 7, 5);
    for (float value : big.pixels) {
        CHECK(std::isfinite(value));
    }
}

TEST_CASE("tonemap: 色差按半径增长，中心无偏移、边缘最大（§4.6 色差）") {
    // 中心：三通道 UV 完全一致（不产生色差）
    const Vec3 center = chroma_sample_uv(0.5, 0.5, 0.5);
    CHECK(center.x == doctest::Approx(0.5));
    CHECK(center.y == doctest::Approx(0.5));
    CHECK(center.z == doctest::Approx(1.0));

    // 半径越大偏移越大（二次增长）
    const double near_offset = std::abs(chroma_sample_uv(0.55, 0.5, 0.5).x - 0.55);
    const double far_offset = std::abs(chroma_sample_uv(1.0, 0.5, 0.5).x - 1.0);
    CHECK(far_offset > near_offset);

    // strength = 0 时退化为无偏移
    const Vec3 none = chroma_sample_uv(0.9, 0.3, 0.0);
    CHECK(none.x == doctest::Approx(0.9));
    CHECK(none.y == doctest::Approx(0.3));

    // 偏移方向沿径向：右侧点只往右偏（y 不变）
    const Vec3 right = chroma_sample_uv(0.8, 0.5, 0.4);
    CHECK(right.x > 0.8);
    CHECK(right.y == doctest::Approx(0.5));
}
