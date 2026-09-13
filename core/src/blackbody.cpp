#include "ehe/core/blackbody.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

#include "ehe/core/cie1931_2deg.h"

namespace ehe::core {
namespace {

constexpr char kLutMagic[8] = {'E', 'H', 'B', 'L', 'U', 'T', '0', '1'};

/// CIE XYZ → 线性 sRGB 矩阵（sRGB 规范 D65 白点，IEC 61966-2-1）
constexpr double kXyzToSrgb[3][3] = {
    {3.2404542, -1.5371385, -0.4985314},
    {-0.9692660, 1.8760108, 0.0415560},
    {0.0556434, -0.2040259, 1.0572252},
};

}  // namespace

double planck_spectral_radiance(double wavelength_nm, double temperature_k) {
    if (wavelength_nm <= 0.0 || temperature_k <= 0.0) {
        return 0.0;
    }
    const double lambda_m = wavelength_nm * 1e-9;
    const double lambda5 = lambda_m * lambda_m * lambda_m * lambda_m * lambda_m;
    const double numerator = 2.0 * kPlanckConstant * kSpeedOfLight * kSpeedOfLight;
    const double exponent = (kPlanckConstant * kSpeedOfLight) / (lambda_m * kBoltzmannConstant * temperature_k);
    // 大指数时 exp 溢出：物理上该项极小，直接返回 0
    if (exponent > 700.0) {
        return 0.0;
    }
    return (numerator / lambda5) / (std::exp(exponent) - 1.0);
}

Xyz planck_xyz(double temperature_k) {
    Xyz xyz;
    // 1 nm 步长积分（DESIGN §4.5）；CMF 与积分步长一一对应，无需插值
    for (int i = 0; i < cie1931::kSampleCount; ++i) {
        const double wavelength = cie1931::kWavelengthStart + i * cie1931::kWavelengthStep;
        const double radiance = planck_spectral_radiance(wavelength, temperature_k);
        xyz.x += radiance * cie1931::kCmf[3 * i + 0];
        xyz.y += radiance * cie1931::kCmf[3 * i + 1];
        xyz.z += radiance * cie1931::kCmf[3 * i + 2];
    }
    // 数值尺度无物理意义（只关心相对三刺激值），在此统一按 Y 归一便于比较
    if (xyz.y > 0.0) {
        const double scale = 1.0 / xyz.y;
        xyz.x *= scale;
        xyz.y *= scale;
        xyz.z *= scale;
    }
    return xyz;
}

Vec3 xyz_to_linear_srgb(const Xyz& xyz) {
    return Vec3{
        kXyzToSrgb[0][0] * xyz.x + kXyzToSrgb[0][1] * xyz.y + kXyzToSrgb[0][2] * xyz.z,
        kXyzToSrgb[1][0] * xyz.x + kXyzToSrgb[1][1] * xyz.y + kXyzToSrgb[1][2] * xyz.z,
        kXyzToSrgb[2][0] * xyz.x + kXyzToSrgb[2][1] * xyz.y + kXyzToSrgb[2][2] * xyz.z,
    };
}

Vec3 planck_linear_srgb(double temperature_k) {
    Vec3 rgb = xyz_to_linear_srgb(planck_xyz(temperature_k));

    // sRGB 基色是物理可实现色域的近似，极端色温可能出现负分量：钳制并记录（不静默）
    rgb = Vec3{std::max(0.0, rgb.x), std::max(0.0, rgb.y), std::max(0.0, rgb.z)};

    // 按峰值归一（DESIGN §4.5）
    const double peak = std::max({rgb.x, rgb.y, rgb.z});
    if (peak > 0.0) {
        rgb /= peak;
    }
    return rgb;
}

double lut_index_for_temperature(double temperature_k, std::size_t count, double temperature_min,
                                 double temperature_max) {
    const double clamped = clamp_d(temperature_k, temperature_min, temperature_max);
    const double log_min = std::log(temperature_min);
    const double log_max = std::log(temperature_max);
    const double t = (std::log(clamped) - log_min) / (log_max - log_min);
    return t * static_cast<double>(count - 1);
}

double lut_temperature_for_index(std::size_t index, std::size_t count, double temperature_min,
                                 double temperature_max) {
    const double t = static_cast<double>(index) / static_cast<double>(count - 1);
    const double log_min = std::log(temperature_min);
    const double log_max = std::log(temperature_max);
    return std::exp(log_min + t * (log_max - log_min));
}

std::vector<float> build_blackbody_lut(std::size_t count, double temperature_min,
                                       double temperature_max) {
    std::vector<float> lut(3 * count, 0.0F);
    for (std::size_t i = 0; i < count; ++i) {
        const double temperature = lut_temperature_for_index(i, count, temperature_min, temperature_max);
        const Vec3 rgb = planck_linear_srgb(temperature);
        lut[3 * i + 0] = static_cast<float>(rgb.x);
        lut[3 * i + 1] = static_cast<float>(rgb.y);
        lut[3 * i + 2] = static_cast<float>(rgb.z);
    }
    return lut;
}

Vec3 sample_blackbody_lut(const std::vector<float>& lut, double temperature_k, std::size_t count,
                          double temperature_min, double temperature_max) {
    if (lut.size() < 3 * count || count < 2) {
        return Vec3{1.0, 1.0, 1.0};
    }
    const double position = lut_index_for_temperature(temperature_k, count, temperature_min, temperature_max);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = std::min(lower + 1, count - 1);
    const double weight = position - static_cast<double>(lower);

    Vec3 result;
    for (int channel = 0; channel < 3; ++channel) {
        const double a = lut[3 * lower + channel];
        const double b = lut[3 * upper + channel];
        result[channel] = a + (b - a) * weight;
    }
    return result;
}

bool write_blackbody_lut(const std::string& path, const std::vector<float>& lut, std::size_t count) {
    if (lut.size() < 3 * count) {
        return false;
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.write(kLutMagic, sizeof(kLutMagic));
    const auto count32 = static_cast<std::uint32_t>(count);
    stream.write(reinterpret_cast<const char*>(&count32), sizeof(count32));
    stream.write(reinterpret_cast<const char*>(lut.data()),
                 static_cast<std::streamsize>(sizeof(float) * 3 * count));
    return stream.good();
}

bool read_blackbody_lut(const std::string& path, std::vector<float>& lut, std::size_t& count) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    char magic[8] = {};
    stream.read(magic, sizeof(magic));
    if (!stream || std::memcmp(magic, kLutMagic, sizeof(kLutMagic)) != 0) {
        return false;
    }
    std::uint32_t count32 = 0;
    stream.read(reinterpret_cast<char*>(&count32), sizeof(count32));
    if (!stream || count32 == 0) {
        return false;
    }
    count = count32;
    lut.assign(3 * count, 0.0F);
    stream.read(reinterpret_cast<char*>(lut.data()),
                static_cast<std::streamsize>(sizeof(float) * 3 * count));
    return stream.good();
}

double wien_peak_wavelength_nm(double temperature_k) {
    // λ_max · T = b，b = 2.897771955e-3 m·K（SI 2019 定义下的推荐值）
    constexpr double kWienB = 2.897771955e-3;
    if (temperature_k <= 0.0) {
        return 0.0;
    }
    return kWienB / temperature_k * 1e9;
}

}  // namespace ehe::core
