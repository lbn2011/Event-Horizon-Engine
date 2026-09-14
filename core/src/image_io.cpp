#include "ehe/core/image_io.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ehe::core {

bool write_pfm(const std::string& path, const HdrImageF& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() < static_cast<std::size_t>(3) * image.width * image.height) {
        return false;
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream << "PF\n" << image.width << " " << image.height << "\n-1.0\n";
    // PFM 标准：文件第一行是**图像底部**；内存中是自上而下，故按行倒序写出
    for (int y = image.height - 1; y >= 0; --y) {
        stream.write(reinterpret_cast<const char*>(image.at(0, y)),
                     static_cast<std::streamsize>(sizeof(float) * 3 * image.width));
    }
    return stream.good();
}

bool read_pfm(const std::string& path, HdrImageF& image) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    std::string magic;
    stream >> magic;
    if (magic != "PF") {
        return false;
    }
    int width = 0;
    int height = 0;
    double scale = 0.0;
    stream >> width >> height >> scale;
    if (!stream || width <= 0 || height <= 0) {
        return false;
    }
    if (scale >= 0.0) {
        return false;  // 仅支持小端（scale < 0）
    }
    stream.get();  // 消费行尾换行

    HdrImageF loaded;
    loaded.width = width;
    loaded.height = height;
    loaded.pixels.assign(static_cast<std::size_t>(3) * width * height, 0.0F);
    // 文件自下而上 → 内存自上而下：逐行倒序读入
    for (int y = height - 1; y >= 0; --y) {
        stream.read(reinterpret_cast<char*>(loaded.at(0, y)),
                    static_cast<std::streamsize>(sizeof(float) * 3 * width));
    }
    if (!stream) {
        return false;
    }
    image = std::move(loaded);
    return true;
}

DiffStats diff_stats(const HdrImageF& image, const HdrImageF& reference) {
    DiffStats stats;
    if (image.width != reference.width || image.height != reference.height ||
        image.pixels.size() != reference.pixels.size() || image.pixels.empty()) {
        return stats;
    }

    double sum_squared_error[3] = {0.0, 0.0, 0.0};
    double sum_reference[3] = {0.0, 0.0, 0.0};
    double max_abs = 0.0;
    double sum_abs = 0.0;

    const std::size_t count = image.pixels.size();
    for (std::size_t i = 0; i < count; ++i) {
        const int channel = static_cast<int>(i % 3);
        const double a = image.pixels[i];
        const double b = reference.pixels[i];
        const double delta = a - b;
        sum_squared_error[channel] += delta * delta;
        sum_reference[channel] += b * b;
        max_abs = std::max(max_abs, std::abs(delta));
        sum_abs += std::abs(delta);
    }

    bool ok = true;
    for (int c = 0; c < 3; ++c) {
        if (sum_reference[c] <= 0.0) {
            ok = false;
            stats.nmse_rgb[c] = 0.0;
        } else {
            stats.nmse_rgb[c] = sum_squared_error[c] / sum_reference[c];
        }
    }
    stats.comparable = ok;
    stats.nmse = ok ? std::max({stats.nmse_rgb[0], stats.nmse_rgb[1], stats.nmse_rgb[2]}) : -1.0;
    stats.max_abs_diff = max_abs;
    stats.mean_abs_diff = sum_abs / static_cast<double>(count);
    return stats;
}

double nmse(const HdrImageF& image, const HdrImageF& reference) {
    return diff_stats(image, reference).nmse;
}

}  // namespace ehe::core
