#pragma once

// EHE core —— 黑体辐射色温映射与 LUT（DESIGN §4.5）
//
// 规格要点：
//   - 局部温度 T(r) = T_scale · F(r)^{1/4}
//   - 温度 → RGB 用预烘焙 256×1 LUT：Planck 谱在 [380, 780] nm 以 1 nm 步长积分
//     → 乘 CIE 1931 配色函数（core/cie1931_2deg.h）→ CIE XYZ → 线性 sRGB → 按峰值归一
//   - 温度域 [1000, 40000] K，256 点**对数等距**；shader 侧按 log(T) 映射 UV 采样，
//     两侧必须使用同一映射函数（本文件的 lut_index_for_temperature / lut_temperature_for_index）
//   - golden 渲染器与 GPU shader 共用同一 LUT 文件；两份拷贝（tests/golden、shaders）由
//     SHA-256 校验一致（CI 与单元测试各查一次）

#include <cstddef>
#include <string>
#include <vector>

#include "ehe/core/math.h"

namespace ehe::core {

/// LUT 温度域与规模（DESIGN §4.5）
inline constexpr double kBlackbodyTemperatureMin = 1000.0;
inline constexpr double kBlackbodyTemperatureMax = 40000.0;
inline constexpr std::size_t kBlackbodyLutSize = 256;

/// 光谱积分范围（nm）
inline constexpr double kSpectrumMinNm = 380.0;
inline constexpr double kSpectrumMaxNm = 780.0;

/// 物理常数（SI，仅本模块用于 Planck 公式）
inline constexpr double kPlanckConstant = 6.62607015e-34;      ///< h (J·s)
inline constexpr double kSpeedOfLight = 2.99792458e8;          ///< c (m/s)
inline constexpr double kBoltzmannConstant = 1.380649e-23;     ///< k_B (J/K)

/// Planck 黑体谱辐射亮度 B_λ(T)（SI：W·sr⁻¹·m⁻²·m⁻¹），λ 以 nm 传入
double planck_spectral_radiance(double wavelength_nm, double temperature_k);

/// 黑体谱的三刺激值（在 [380, 780] nm 上以 1 nm 步长积分）
struct Xyz {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Xyz planck_xyz(double temperature_k);

/// CIE XYZ → **线性** sRGB（未做伽马，DESIGN §4.6 的色调映射在渲染侧统一处理）
Vec3 xyz_to_linear_srgb(const Xyz& xyz);

/// 黑体色（线性 sRGB，按最大通道归一，保留色相同时便于 HDR 管线缩放）
Vec3 planck_linear_srgb(double temperature_k);

/// 生成 LUT：长度 = 3 × count，按温度升序，每 3 个为 (r, g, b) 线性 sRGB
std::vector<float> build_blackbody_lut(std::size_t count = kBlackbodyLutSize,
                                       double temperature_min = kBlackbodyTemperatureMin,
                                       double temperature_max = kBlackbodyTemperatureMax);

/// 温度 ↔ LUT 索引的对数映射（shader 侧必须使用同一公式）
double lut_index_for_temperature(double temperature_k, std::size_t count = kBlackbodyLutSize,
                                 double temperature_min = kBlackbodyTemperatureMin,
                                 double temperature_max = kBlackbodyTemperatureMax);
double lut_temperature_for_index(std::size_t index, std::size_t count = kBlackbodyLutSize,
                                 double temperature_min = kBlackbodyTemperatureMin,
                                 double temperature_max = kBlackbodyTemperatureMax);

/// 采样 LUT（线性插值；温度自动钳制到温度域）
Vec3 sample_blackbody_lut(const std::vector<float>& lut, double temperature_k,
                          std::size_t count = kBlackbodyLutSize,
                          double temperature_min = kBlackbodyTemperatureMin,
                          double temperature_max = kBlackbodyTemperatureMax);

/// 写出 LUT 二进制文件（float32 小端，布局：count 组 RGB，温度升序）
/// 文件头约定：首 8 字节为 magic "EHBLUT01" + uint32 count，随后为数据
bool write_blackbody_lut(const std::string& path, const std::vector<float>& lut,
                         std::size_t count = kBlackbodyLutSize);

/// 读取 LUT（校验 magic 与长度）
bool read_blackbody_lut(const std::string& path, std::vector<float>& lut, std::size_t& count);

/// Wien 位移定律给出的峰值波长（nm）：λ_max = b / T，b = 2.897771955e-3 m·K
double wien_peak_wavelength_nm(double temperature_k);

}  // namespace ehe::core
