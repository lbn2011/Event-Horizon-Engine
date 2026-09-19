#include "ehe/core/tonemap.h"

#include <algorithm>
#include <cmath>

namespace ehe::core {
namespace {

/// Hill 拟合的 3×3 矩阵（sRGB 线性 ↔ ACEScg）
struct Mat3Row {
    double r[3];
};

constexpr Mat3Row kAcesInput[3] = {
    {{0.59719, 0.35458, 0.04823}},
    {{0.07600, 0.90834, 0.01566}},
    {{0.02840, 0.13383, 0.83777}},
};

constexpr Mat3Row kAcesOutput[3] = {
    {{1.60475, -0.53108, -0.07367}},
    {{-0.10208, 1.10813, -0.00605}},
    {{-0.00327, -0.07276, 1.07602}},
};

Vec3 multiply(const Mat3Row matrix[3], const Vec3& v) {
    return Vec3{matrix[0].r[0] * v.x + matrix[0].r[1] * v.y + matrix[0].r[2] * v.z,
                matrix[1].r[0] * v.x + matrix[1].r[1] * v.y + matrix[1].r[2] * v.z,
                matrix[2].r[0] * v.x + matrix[2].r[1] * v.y + matrix[2].r[2] * v.z};
}

/// 拟合的 RRT + ODT（Hill 版多项式）
Vec3 rrt_and_odt_fit(const Vec3& v) {
    const Vec3 numerator{v.x * (v.x + 0.0245786) - 0.000090537,
                         v.y * (v.y + 0.0245786) - 0.000090537,
                         v.z * (v.z + 0.0245786) - 0.000090537};
    const Vec3 denominator{v.x * (0.983729 * v.x + 0.4329510) + 0.238081,
                           v.y * (0.983729 * v.y + 0.4329510) + 0.238081,
                           v.z * (0.983729 * v.z + 0.4329510) + 0.238081};
    return Vec3{numerator.x / denominator.x, numerator.y / denominator.y,
                numerator.z / denominator.z};
}

/// Catmull-Rom 权重（四点，t ∈ [0,1]）
void catmull_rom_weights(double t, double w[4]) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

const float* pixel_clamped(const HdrImageF& image, int x, int y) {
    x = std::clamp(x, 0, image.width - 1);
    y = std::clamp(y, 0, image.height - 1);
    return image.at(x, y);
}

}  // namespace

double linear_to_srgb(double linear) {
    if (linear <= 0.0031308) {
        return 12.92 * linear;
    }
    return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double srgb_to_linear(double encoded) {
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

Vec3 aces_fitted(const Vec3& linear_rgb) {
    Vec3 color = multiply(kAcesInput, linear_rgb);
    color = rrt_and_odt_fit(color);
    color = multiply(kAcesOutput, color);
    return Vec3{std::clamp(color.x, 0.0, 1.0), std::clamp(color.y, 0.0, 1.0),
                std::clamp(color.z, 0.0, 1.0)};
}

HdrImageF downsample_box(const HdrImageF& src, int out_width, int out_height) {
    // 面积加权盒式平均：目标像素足迹 [x0, x1) × [y0, y1) 投到源空间，按重叠面积加权。
    // 支持非整数倍率（1.25 / 1.5 / 2.0，见 §7 的分辨率档），且对整数倍率退化为等权平均。
    HdrImageF dst;
    if (src.width <= 0 || src.height <= 0 || out_width <= 0 || out_height <= 0 ||
        out_width > src.width || out_height > src.height) {
        return dst;
    }
    dst.width = out_width;
    dst.height = out_height;
    dst.pixels.assign(static_cast<std::size_t>(3) * out_width * out_height, 0.0F);

    const double scale_x = static_cast<double>(src.width) / out_width;
    const double scale_y = static_cast<double>(src.height) / out_height;

    for (int y = 0; y < out_height; ++y) {
        const double source_y0 = y * scale_y;
        const double source_y1 = (y + 1) * scale_y;
        const int iy0 = static_cast<int>(std::floor(source_y0));
        const int iy1 = std::min(static_cast<int>(std::ceil(source_y1)) - 1, src.height - 1);

        for (int x = 0; x < out_width; ++x) {
            const double source_x0 = x * scale_x;
            const double source_x1 = (x + 1) * scale_x;
            const int ix0 = static_cast<int>(std::floor(source_x0));
            const int ix1 = std::min(static_cast<int>(std::ceil(source_x1)) - 1, src.width - 1);

            double accumulator[3] = {0.0, 0.0, 0.0};
            double total_weight = 0.0;
            for (int sy = iy0; sy <= iy1; ++sy) {
                const double overlap_y =
                    std::min(source_y1, static_cast<double>(sy + 1)) -
                    std::max(source_y0, static_cast<double>(sy));
                if (overlap_y <= 0.0) {
                    continue;
                }
                for (int sx = ix0; sx <= ix1; ++sx) {
                    const double overlap_x =
                        std::min(source_x1, static_cast<double>(sx + 1)) -
                        std::max(source_x0, static_cast<double>(sx));
                    if (overlap_x <= 0.0) {
                        continue;
                    }
                    const double weight = overlap_x * overlap_y;
                    const float* pixel = src.at(sx, sy);
                    accumulator[0] += weight * pixel[0];
                    accumulator[1] += weight * pixel[1];
                    accumulator[2] += weight * pixel[2];
                    total_weight += weight;
                }
            }
            float* out = dst.at(x, y);
            if (total_weight > 0.0) {
                const double inverse = 1.0 / total_weight;
                out[0] = static_cast<float>(accumulator[0] * inverse);
                out[1] = static_cast<float>(accumulator[1] * inverse);
                out[2] = static_cast<float>(accumulator[2] * inverse);
            }
        }
    }
    return dst;
}

HdrImageF upscale_catmull_rom(const HdrImageF& src, int out_width, int out_height) {
    HdrImageF dst;
    if (src.width <= 0 || src.height <= 0 || out_width <= 0 || out_height <= 0) {
        return dst;
    }
    dst.width = out_width;
    dst.height = out_height;
    dst.pixels.assign(static_cast<std::size_t>(3) * out_width * out_height, 0.0F);

    // 像素中心对齐：源像素中心映射到目标归一化坐标
    const double scale_x = static_cast<double>(src.width) / out_width;
    const double scale_y = static_cast<double>(src.height) / out_height;

    for (int y = 0; y < out_height; ++y) {
        const double sy = (y + 0.5) * scale_y - 0.5;
        const int iy = static_cast<int>(std::floor(sy));
        const double fy = sy - iy;
        double wy[4];
        catmull_rom_weights(fy, wy);

        for (int x = 0; x < out_width; ++x) {
            const double sx = (x + 0.5) * scale_x - 0.5;
            const int ix = static_cast<int>(std::floor(sx));
            const double fx = sx - ix;
            double wx[4];
            catmull_rom_weights(fx, wx);

            double accumulator[3] = {0.0, 0.0, 0.0};
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    const double weight = wx[i] * wy[j];
                    const float* pixel = pixel_clamped(src, ix - 1 + i, iy - 1 + j);
                    accumulator[0] += weight * pixel[0];
                    accumulator[1] += weight * pixel[1];
                    accumulator[2] += weight * pixel[2];
                }
            }
            float* out = dst.at(x, y);
            out[0] = static_cast<float>(accumulator[0]);
            out[1] = static_cast<float>(accumulator[1]);
            out[2] = static_cast<float>(accumulator[2]);
        }
    }
    return dst;
}

Vec3 chroma_sample_uv(double uv_x, double uv_y, double strength) {
    // 以画面中心为原点做径向缩放：中心三通道一致，边缘分离最大
    const double centered_x = uv_x - 0.5;
    const double centered_y = uv_y - 0.5;
    const double radius = std::sqrt(centered_x * centered_x + centered_y * centered_y);
    const double scale = 1.0 + strength * radius * radius;  // 二次增长：边缘更明显、中心无偏移
    return Vec3{0.5 + centered_x * scale, 0.5 + centered_y * scale, scale};
}

}  // namespace ehe::core
