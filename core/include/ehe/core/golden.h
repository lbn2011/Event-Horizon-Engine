#pragma once

// EHE core —— golden 参考渲染器（DESIGN §6.4、§4.5）
//
// 作用：在 CPU 上用**与 shader 同一套方程**离屏渲染，产出 golden 基线（PFM/PNG），
//       供目标机冒烟做 NMSE 差分（§6.2/§6.3）。
//
// 覆盖范围（T1.2.6）：
//   - 体积吸积盘：薄盘高斯厚度 + 无扭矩通量剖面 + 黑体 LUT 色温 + 发射-吸收前向合成
//   - 相对论效应：逐采样点 g 因子（引力 + 多普勒），强度按 g³、色温按 g·T
//   - 三终止条件（§4.3）与调试视图（§5.5 的 classify / g_factor 子集）
//
// 本模块为纯计算（无图形 API、无文件 I/O）；PFM/PNG 写出在 tests 的 reftool 中完成。

#include <cstddef>
#include <vector>

#include "ehe/core/blackbody.h"
#include "ehe/core/camera.h"
#include "ehe/core/config.h"
#include "ehe/core/image_io.h"
#include "ehe/core/integrator.h"

namespace ehe::core {

/// 吸积盘参数（对应 Config.blackhole 的盘字段）
struct DiskParams {
    double r_in = 6.0;
    double r_out = 20.0;
    double density = 1.0;        ///< 归一化密度（进入通量剖面与体合成）
    double t_scale = 1.0;        ///< 温度标定 T_scale
    double kappa = 2.0;          ///< 吸收系数 κ（§4.5 建议默认 2）
    double thickness_scale = 0.1;///< 半厚度 σ_d = 0.1·r（§4.5）

    /// 发射强度归一化常数（**规格补充**，见 DESIGN V5.4 说明）
    /// 原因：§4.5 的 ε = ρ·g³·LUT 只定义了相对关系，没有绝对尺度；沿测地线累积上百步后
    /// 数值可达 10² 量级，直接输出会让 HDR 图整体削顶。此处把基线亮度归一到 [0, ~5]，
    /// 物理上的亮暗对比（内热外冷、多普勒聚束）仍由 ρ、F(r)、g³ 承载。
    double emission_scale = 2.5;

    bool enabled = true;
};

/// 调试视图（§5.5 子集）
enum class DebugView { Shaded, Classify, GFactor };

/// golden 渲染参数
struct GoldenParams {
    int width = 256;
    int height = 256;
    CameraConfig camera;
    IntegratorParams integrator;
    DiskParams disk;
    DebugView debug_view = DebugView::Shaded;

    /// 黑体 LUT（由 build_blackbody_lut 生成；为空时使用解析黑体色）
    std::vector<float> lut;
    std::size_t lut_size = kBlackbodyLutSize;

    /// 背景色（v1 为纯色，M2 起换星空 cubemap）
    Vec3 background{0.0, 0.0, 0.0};

    bool compute_g_factor = true;
};

/// 渲染统计（诊断与测试用）
struct RenderStats {
    std::size_t pixels = 0;
    std::size_t captured = 0;
    std::size_t escaped = 0;
    std::size_t exhausted = 0;
    double mean_steps = 0.0;
    double max_null_drift = 0.0;
    double min_g = 0.0;
    double max_g = 0.0;
};

// HDR 图像缓冲统一使用 core::HdrImageF（core/image_io.h，行序自下而上，与 PFM 一致）
using HdrImage = HdrImageF;

/// 盘相关解析式（T1.4 的 GLSL 版本必须与此逐行一致）
double disk_flux_profile(double r, const DiskParams& disk);       ///< F(r) ∝ (1 − √(r_in/r))/r³
double disk_temperature(double r, const DiskParams& disk);        ///< T(r) = T_scale · F(r)^{1/4}
double disk_density(double r, double z, const DiskParams& disk);  ///< 高斯厚度 + 径向区间截断

/// 盘流体四速度（赤道圆轨道，a = 0）：Ω = r^{−3/2}，u^t = 1/√(1−3/r)
/// 返回 KS 笛卡尔分量 u^μ = (u^t, u^x, u^y, u^z)（§13.1：a=0 时两坐标图一致）
Vec4 disk_four_velocity(const Vec3& position, double r);

/// 红移因子 g = (k·u)_obs / (k·u)_em（§4.5）
double redshift_factor(double k_dot_u_obs, const Vec4& k, const Vec4& u_em, const KsPoint& point);

/// 主渲染入口
HdrImage render_golden(const GoldenParams& params, RenderStats* stats = nullptr);

/// HDR 线性 → sRGB8（供 PNG 预览；ACES 等色调映射在 T1.5 接入）
std::vector<std::uint8_t> tonemap_to_srgb8(const HdrImage& image, double exposure = 1.0);

}  // namespace ehe::core
