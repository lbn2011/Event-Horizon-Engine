// EHE 参考工具（ehe_reftool）—— golden 基线生成与 LUT 烘焙
//
// 用法：
//   ehe_reftool lut [--out-dir-tests tests/golden] [--out-dir-shaders shaders]
//       烘焙黑体 LUT（256×1 float32），同时写出两份拷贝（tests/golden 与 shaders），
//       并生成 PNG 预览条（blackbody_lut_preview.png）。
//
//   ehe_reftool golden [--params tests/golden/params.json] [--shot tests/golden/golden]
//       按 golden 参数渲染离屏图：写出 <shot>.pfm（float32，差分基准）与 <shot>.png（预览）。
//
// 说明：DESIGN §6.3/§6.4 —— 差分基准必须是 PFM（8-bit PNG 的量化损失会淹没 1e-3 阈值）；
//       PNG 仅供肉眼查看。本工具属 tests/ 范畴，可依赖 stb（core 保持零图形依赖）。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include "ehe/core/blackbody.h"
#include "ehe/core/config.h"
#include "ehe/core/console.h"
#include "ehe/core/golden.h"
#include "ehe/core/image_io.h"
#include "ehe/core/integrator.h"
#include "ehe/core/sha256.h"

namespace {


bool write_png(const std::string& path, int width, int height, const std::vector<std::uint8_t>& rgb) {
    return stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3) != 0;
}

std::string arg_value(int argc, char** argv, const char* name, const std::string& fallback) {
    const std::string prefix = std::string(name) + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) {
            return arg.substr(prefix.size());
        }
    }
    return fallback;
}

int run_lut(int argc, char** argv) {
    const std::string tests_dir = arg_value(argc, argv, "--out-dir-tests", "tests/golden");
    const std::string shaders_dir = arg_value(argc, argv, "--out-dir-shaders", "shaders");

    const auto lut = ehe::core::build_blackbody_lut();
    const std::string path_tests = tests_dir + "/blackbody_lut.f32";
    const std::string path_shaders = shaders_dir + "/blackbody_lut.f32";

    if (!ehe::core::write_blackbody_lut(path_tests, lut) ||
        !ehe::core::write_blackbody_lut(path_shaders, lut)) {
        std::fprintf(stderr, "[reftool] 写出 LUT 失败（目录是否存在？）\n");
        return 1;
    }

    const std::string hash_tests = ehe::core::sha256_file_hex(path_tests);
    const std::string hash_shaders = ehe::core::sha256_file_hex(path_shaders);
    std::printf("[reftool] LUT 已写出：%s\n", path_tests.c_str());
    std::printf("[reftool] LUT 已写出：%s\n", path_shaders.c_str());
    std::printf("[reftool] SHA-256(tests)   = %s\n", hash_tests.c_str());
    std::printf("[reftool] SHA-256(shaders) = %s\n", hash_shaders.c_str());
    std::printf("[reftool] 两份一致：%s\n", (hash_tests == hash_shaders) ? "是" : "否");

    // PNG 预览条：LUT 横向铺开（256×1 → 256×48）
    const int preview_height = 48;
    std::vector<std::uint8_t> preview(static_cast<std::size_t>(256) * preview_height * 3, 0);
    for (int y = 0; y < preview_height; ++y) {
        for (int x = 0; x < 256; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float linear = lut[3 * x + c];
                // 与 core 的 tonemap 一致的 sRGB 传输函数（此处直接内联，避免依赖 golden.h 的图像结构）
                const double clamped = std::min(1.0, std::max(0.0, static_cast<double>(linear)));
                const double srgb = (clamped <= 0.0031308) ? (12.92 * clamped)
                                                           : (1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055);
                preview[(static_cast<std::size_t>(y) * 256 + x) * 3 + c] =
                    static_cast<std::uint8_t>(std::lround(srgb * 255.0));
            }
        }
    }
    const std::string preview_path = tests_dir + "/blackbody_lut_preview.png";
    if (!write_png(preview_path, 256, preview_height, preview)) {
        std::fprintf(stderr, "[reftool] PNG 预览写出失败\n");
        return 1;
    }
    std::printf("[reftool] 预览条：%s\n", preview_path.c_str());
    return 0;
}

int run_golden(int argc, char** argv) {
    const std::string params_path = arg_value(argc, argv, "--params", "tests/golden/params.json");
    const std::string shot_base = arg_value(argc, argv, "--shot", "tests/golden/golden");

    std::ifstream stream(params_path);
    if (!stream) {
        std::fprintf(stderr, "[reftool] 找不到参数文件：%s\n", params_path.c_str());
        return 1;
    }
    nlohmann::json json;
    stream >> json;

    // 复用 Config 的解析与容错（未知键告警、缺失键默认、值域钳制）
    std::vector<std::string> warnings;
    const ehe::core::Config config = ehe::core::Config::from_json(json, &warnings);
    for (const std::string& warning : warnings) {
        std::fprintf(stderr, "[reftool][config] %s\n", warning.c_str());
    }

    ehe::core::GoldenParams params;
    params.width = json.value("image", nlohmann::json::object()).value("width", 512);
    params.height = json.value("image", nlohmann::json::object()).value("height", 512);
    params.camera = config.camera;
    params.integrator = ehe::core::make_integrator_params(config);
    params.disk.r_in = config.blackhole.disk_r_in;
    params.disk.r_out = config.blackhole.disk_r_out;
    params.disk.density = config.blackhole.disk_density;
    params.disk.t_scale = config.blackhole.disk_t_scale;
    params.disk.kappa = config.blackhole.disk_kappa;
    params.disk.enabled = config.blackhole.disk_density > 0.0;
    params.background = ehe::core::Vec3{0.02, 0.02, 0.03};

    const std::string view = config.render.debug_view;
    if (view == "classify") {
        params.debug_view = ehe::core::DebugView::Classify;
        params.background = ehe::core::Vec3{0.0, 0.0, 0.0};
    } else if (view == "g_factor") {
        params.debug_view = ehe::core::DebugView::GFactor;
    }

    // 与 golden 约定一致：time = 0（无噪声动画）、不做后处理（§6.1）
    params.lut = ehe::core::build_blackbody_lut();

    std::printf("[reftool] 渲染 %dx%d（视图=%s, a=%.3f, 相机 r=%.1f/倾角=%.1f°）...\n", params.width,
                params.height, view.c_str(), params.integrator.spin, params.camera.dist,
                params.camera.polar_deg);

    ehe::core::RenderStats stats;
    const auto image = ehe::core::render_golden(params, &stats);

    const std::string pfm_path = shot_base + ".pfm";
    const std::string png_path = shot_base + ".png";
    if (!ehe::core::write_pfm(pfm_path, image)) {
        std::fprintf(stderr, "[reftool] PFM 写出失败：%s\n", pfm_path.c_str());
        return 1;
    }
    const auto srgb = ehe::core::tonemap_to_srgb8(image, config.post.exposure);
    if (!write_png(png_path, image.width, image.height, srgb)) {
        std::fprintf(stderr, "[reftool] PNG 写出失败：%s\n", png_path.c_str());
        return 1;
    }

    std::printf("[reftool] 命中视界=%zu 逃逸=%zu 步数用尽=%zu 平均步数=%.1f\n", stats.captured,
                stats.escaped, stats.exhausted, stats.mean_steps);
    double peak = 0.0;
    double sum = 0.0;
    for (const float value : image.pixels) {
        peak = std::max(peak, static_cast<double>(value));
        sum += static_cast<double>(value);
    }
    std::printf("[reftool] HDR 峰值=%.4f 均值=%.6f\n", peak,
                sum / static_cast<double>(image.pixels.size()));
    std::printf("[reftool] g 因子范围=[%.4f, %.4f] 类光漂移=%.3e\n", stats.min_g, stats.max_g,
                stats.max_null_drift);
    std::printf("[reftool] 输出：%s（差分基准）与 %s（预览）\n", pfm_path.c_str(), png_path.c_str());
    return 0;
}

// ---------------------------------------------------------------- 新增子命令：compare / png

/// `compare <a.pfm> <b.pfm> [--threshold=T]`：按 §6.2 计算 NMSE，退出码 0（通过）/1（超阈值）
int run_compare(int argc, char** argv) {
    std::vector<std::string> positional;
    double threshold = 1e-3;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--threshold=", 0) == 0) {
            threshold = std::strtod(arg.substr(12).c_str(), nullptr);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2) {
        std::fprintf(stderr, "用法：ehe_reftool compare <a.pfm> <b.pfm> [--threshold=1e-3]\n");
        return 2;
    }

    ehe::core::HdrImageF a;
    ehe::core::HdrImageF b;
    if (!ehe::core::read_pfm(positional[0], a) || !ehe::core::read_pfm(positional[1], b)) {
        std::fprintf(stderr, "[reftool] PFM 读取失败\n");
        return 2;
    }
    const ehe::core::DiffStats stats = ehe::core::diff_stats(a, b);
    if (!stats.comparable) {
        std::fprintf(stderr, "[reftool] 图像不可比（%dx%d vs %dx%d）\n", a.width, a.height, b.width,
                     b.height);
        return 2;
    }
    std::printf("[reftool] %s vs %s（%dx%d）\n", positional[0].c_str(), positional[1].c_str(), a.width,
                a.height);
    std::printf("[reftool] NMSE=%.6e（阈值 %.1e）  R=%.3e G=%.3e B=%.3e\n", stats.nmse, threshold,
                stats.nmse_rgb[0], stats.nmse_rgb[1], stats.nmse_rgb[2]);
    std::printf("[reftool] 最大绝对差=%.6e  平均绝对差=%.6e\n", stats.max_abs_diff,
                stats.mean_abs_diff);
    if (stats.nmse <= threshold) {
        std::printf("[reftool] 通过\n");
        return 0;
    }
    std::printf("[reftool] 超阈值\n");
    return 1;
}

/// `png <in.pfm> <out.png> [--exposure=E]`：把 PFM 转成 PNG 预览（供肉眼查看）
int run_png(int argc, char** argv) {
    std::vector<std::string> positional;
    double exposure = 1.0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--exposure=", 0) == 0) {
            exposure = std::strtod(arg.substr(11).c_str(), nullptr);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2) {
        std::fprintf(stderr, "用法：ehe_reftool png <in.pfm> <out.png> [--exposure=1.0]\n");
        return 2;
    }

    ehe::core::HdrImageF image;
    if (!ehe::core::read_pfm(positional[0], image)) {
        std::fprintf(stderr, "[reftool] PFM 读取失败：%s\n", positional[0].c_str());
        return 2;
    }
    const auto bytes = ehe::core::tonemap_to_srgb8(image, exposure);
    if (!write_png(positional[1], image.width, image.height, bytes)) {
        std::fprintf(stderr, "[reftool] PNG 写出失败：%s\n", positional[1].c_str());
        return 2;
    }
    std::printf("[reftool] 已写出 %s（%dx%d，曝光 %.2f）\n", positional[1].c_str(), image.width,
                image.height, exposure);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    ehe::core::enable_utf8_console();  // 中文输出需 UTF-8 控制台（否则乱码）
    if (argc < 2) {
        std::printf("用法: ehe_reftool <lut|golden|compare|png> [参数...]\n");
        return 2;
    }
    const std::string command = argv[1];
    if (command == "lut") {
        return run_lut(argc, argv);
    }
    if (command == "golden") {
        return run_golden(argc, argv);
    }
    if (command == "compare") {
        return run_compare(argc, argv);
    }
    if (command == "png") {
        return run_png(argc, argv);
    }
    std::fprintf(stderr, "[reftool] 未知子命令：%s\n", command.c_str());
    return 2;
}
