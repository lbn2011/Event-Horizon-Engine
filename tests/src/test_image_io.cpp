// EHE —— HDR 图像 I/O 与差分指标测试（DESIGN §6.2 / §6.4）
//
// 这些是 smoke 模式（§6.3）与目标机验收所依赖的基础设施：PFM 往返、NMSE 定义、行序约定。

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "ehe/core/image_io.h"

using ehe::core::diff_stats;
using ehe::core::HdrImageF;
using ehe::core::nmse;
using ehe::core::read_pfm;
using ehe::core::write_pfm;

namespace {

HdrImageF make_image(int width, int height, float base) {
    HdrImageF image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(3) * width * height, 0.0F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float* pixel = image.at(x, y);
            pixel[0] = base + 0.01F * static_cast<float>(x);
            pixel[1] = base + 0.02F * static_cast<float>(y);
            pixel[2] = base;
        }
    }
    return image;
}

}  // namespace

TEST_CASE("image_io: PFM 写入/读取往返（float32 全精度，行序自下而上）") {
    const std::string path = "ehe_test_pfm.pfm";
    const HdrImageF original = make_image(7, 5, 0.25F);
    REQUIRE(write_pfm(path, original));

    HdrImageF loaded;
    REQUIRE(read_pfm(path, loaded));
    CHECK(loaded.width == 7);
    CHECK(loaded.height == 5);
    REQUIRE(loaded.pixels.size() == original.pixels.size());
    for (std::size_t i = 0; i < original.pixels.size(); ++i) {
        // PFM 不做任何量化，往返必须**逐位相等**
        CHECK(loaded.pixels[i] == original.pixels[i]);
    }
    std::remove(path.c_str());

    HdrImageF missing;
    CHECK_FALSE(read_pfm("ehe_no_such_file.pfm", missing));
}

TEST_CASE("image_io: NMSE 定义与 golden 自比为零（§6.2）") {
    const HdrImageF reference = make_image(16, 16, 0.5F);

    // 与自身比较：NMSE 必须为 0
    const auto self = diff_stats(reference, reference);
    REQUIRE(self.comparable);
    CHECK(self.nmse == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(self.max_abs_diff == doctest::Approx(0.0).epsilon(1e-12));

    // 整体缩放 1.001：NMSE 应约为 1e-6（相对差的平方）
    HdrImageF scaled = reference;
    for (float& value : scaled.pixels) {
        value *= 1.001F;
    }
    const double scaled_nmse = nmse(scaled, reference);
    CHECK(scaled_nmse > 0.0);
    CHECK(scaled_nmse == doctest::Approx(1e-6).epsilon(0.2));

    // 尺寸不一致 → 不可比（返回负值）
    const HdrImageF other = make_image(8, 16, 0.5F);
    CHECK(nmse(other, reference) < 0.0);
}

TEST_CASE("image_io: NMSE 取逐通道最大值（DESIGN §6.2 的判据）") {
    const HdrImageF reference = make_image(8, 8, 1.0F);
    HdrImageF perturbed = reference;
    // 只扰动绿色通道：整体 NMSE 应等于绿色通道的 NMSE
    for (std::size_t i = 0; i < perturbed.pixels.size(); i += 3) {
        perturbed.pixels[i + 1] *= 1.01F;
    }
    const auto stats = diff_stats(perturbed, reference);
    REQUIRE(stats.comparable);
    CHECK(stats.nmse_rgb[1] > 0.0);
    CHECK(stats.nmse == doctest::Approx(stats.nmse_rgb[1]));
    CHECK(stats.nmse >= stats.nmse_rgb[0]);
    CHECK(stats.nmse >= stats.nmse_rgb[2]);
}
