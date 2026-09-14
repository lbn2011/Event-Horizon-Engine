#pragma once

// EHE core —— 图像 I/O 与差分指标（DESIGN §6.2 / §6.3 / §6.4）
//
// 为什么是 PFM：8-bit PNG 在 HDR 范围的量化损失达 1e-2 量级，会淹没 NMSE 1e-3 阈值；
//   PFM 保留 float32 全精度，差分才有物理意义（§6.4）。
//
// 行序约定（V5.5 统一）：**内存中的第 0 行 = 图像顶部**（与 PNG/stb 一致）；
//   PFM 文件按标准自下而上存储——write_pfm/read_pfm 内部负责翻转。
//   OpenGL 的 glReadPixels 自下而上返回，故 GL 侧回读后需自行翻转（见 gl 后端 capture_hdr）。

#include <cstddef>
#include <string>
#include <vector>

namespace ehe::core {

/// 线性 HDR 图像（float32，3 通道，**第 0 行 = 图像顶部**）
struct HdrImageF {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;  ///< 3 × width × height

    float* at(int x, int y) { return &pixels[3 * (static_cast<std::size_t>(y) * width + x)]; }
    const float* at(int x, int y) const {
        return &pixels[3 * (static_cast<std::size_t>(y) * width + x)];
    }
};

/// 写 PFM（格式：`PF\n<w> <h>\n-1.0\n` + float32 小端数据，行序自下而上）
bool write_pfm(const std::string& path, const HdrImageF& image);

/// 读 PFM（仅支持 float32 小端；失败时返回 false 并保持输出不变）
bool read_pfm(const std::string& path, HdrImageF& image);

/// NMSE = Σ(I − G)² / Σ G²，逐通道计算后取最大值（DESIGN §6.2）
/// 尺寸不一致或 G 全零时返回负值表示不可比。
double nmse(const HdrImageF& image, const HdrImageF& reference);

/// 差分统计（供 smoke 输出详细信息）
struct DiffStats {
    double nmse = -1.0;        ///< 逐通道最大 NMSE
    double nmse_rgb[3] = {0.0, 0.0, 0.0};
    double max_abs_diff = 0.0;
    double mean_abs_diff = 0.0;
    bool comparable = false;
};

DiffStats diff_stats(const HdrImageF& image, const HdrImageF& reference);

}  // namespace ehe::core
