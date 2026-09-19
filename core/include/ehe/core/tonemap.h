#pragma once

// EHE core —— 后处理数学的 CPU 参考实现（DESIGN §4.6）
//
// 用途：`shaders/post_*.frag` 的**逐式对照**（同算法），并作为可单测的规格载体——
//   后处理链作用在 golden 之前（golden 是 post 之前的 HDR 输出，§6.1），因此**不进 NMSE 差分**，
//   它的正确性必须由本文件的单元测试保证（单调性/值域/锚点/恒等性质）。
//
// 链序（§4.6）：分辨率变换（FSR1 升频 或 SSAA box 降采样，互斥）→ FXAA → ACES + 曝光 → 色差。

#include <vector>

#include "ehe/core/image_io.h"
#include "ehe/core/math.h"

namespace ehe::core {

// ---------------------------------------------------------------- 色调映射（§4.6）

/// sRGB 传输函数（线性 → 编码）
double linear_to_srgb(double linear);
/// sRGB 逆传输函数（编码 → 线性）
double srgb_to_linear(double encoded);

/// ACES 色调映射（**Stephen Hill 拟合**：输入/输出矩阵 + 拟合 RRT/ODT 曲线）
Vec3 aces_fitted(const Vec3& linear_rgb);

// ---------------------------------------------------------------- 分辨率变换（§4.6）

/// SSAA 降采样：**面积加权盒式平均**（支持 1.25/1.5/2.0 这类非整数倍率，§7 的分辨率档）
/// 算法与 `shaders/post_resolve.frag` 的 MODE_BOX 同式：目标像素的足迹映射回源空间，
/// 按重叠面积加权平均（每个目标像素最多触及 ceil(ratio)+1 个源像素）。
/// @param src 高内部分辨率的 HDR 图（第 0 行 = 顶部，与 HdrImageF 约定一致）
HdrImageF downsample_box(const HdrImageF& src, int out_width, int out_height);

/// Catmull-Rom 双三次升采样（res_scale < 1 时的路径；FSR1 落地后由 EASU+RCAS 取代）
HdrImageF upscale_catmull_rom(const HdrImageF& src, int out_width, int out_height);

/// 色差：按半径做 RGB 径向分离。返回该像素三通道各自应采样的 UV（中心处三者相同）
Vec3 chroma_sample_uv(double uv_x, double uv_y, double strength);

}  // namespace ehe::core
