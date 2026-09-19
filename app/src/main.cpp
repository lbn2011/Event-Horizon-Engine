// EHE 主程序（T1.3）
//
// 两种运行形态：
//   1. 窗口模式（默认）：GLFW 窗口 + raymarch 管线 + ImGui 面板（调试视图切换、相机交互）
//   2. **smoke 模式**（DESIGN §6.3）：离屏渲染 N 帧 → 写 out.pfm（HDR 差分基准）与 out.png（预览）
//      → 与 golden PFM 做 NMSE 差分 → 打印结果 → 退出码 0/1/2
//
// 后端切换（DESIGN §5.2）：CLIENT_API hint 在窗口创建时固定，切换需销毁重建窗口句柄，进程不退。
//
// 环境说明（DESIGN §2.1 / V5.3）：开发机 GL 4.5 可运行 GL 路径用于开发期观察；
//   **验收仍在目标机执行**；Vulkan 的 raymarch 管线在 T1.6 落地。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <stb_image_write.h>

#include "audio.h"
#include "backend_factory.h"
#include "caps.h"
#include "ehe/core/camera.h"
#include "ehe/core/console.h"
#include "ehe/core/config.h"
#include "ehe/core/golden.h"
#include "ehe/core/image_io.h"
#include "ehe/core/version.h"
#include "ehe/render/IRenderer.h"
#include "ehe/render/backend.h"
#include "ehe/render/sim_params.h"

namespace {

struct Options {
    ehe::core::Backend backend = ehe::core::Backend::OpenGL;
    int width = 1280;
    int height = 720;
    bool vsync = true;
    bool try_backends = false;
    bool caps = false;
    bool time_mixed = false;
    int probe_size = 64;
    bool smoke = false;
    int smoke_frames = 60;
    int window_frames = 0;  // 窗口模式限帧（0 = 不限），便于开发/CI 下有限步验证
    std::string shot = "out";
    std::string config_path = "tests/golden/params.json";
    std::string golden_path = "tests/golden/golden.pfm";
    std::string shader_root;
    std::string precision;         // 空 = 用配置文件的 precision；可选 fp32 / mixed（命令行覆盖）
    double time = 0.0;             // 动画时间（秒）。smoke 用它做确定性输入；窗口模式作为起始偏移
    bool animation = true;         // 窗口模式是否让 time 随实际时间推进（smoke 恒为固定值）
    double nmse_threshold = 1e-3;  // DESIGN §6.2 初值
    std::string persist_path = "ehe.config.json";  // 会话持久化（§8.1：退出/切后端保存）
};

/// 窗口模式 UI 状态（不进 Config schema 的运行时量）
struct UiState {
    bool show_overlay = true;   ///< 监控 overlay 独立开关（§8）
    int fly_speed = 1;          ///< 自由飞行速度档（§4.7：慢/中/快三档）
    bool rebuild_requested = false;  ///< 精度模式等需要重建管线的改动
};

int parse_int(const char* text, int fallback) {
    if (text == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    return (end == text) ? fallback : static_cast<int>(value);
}

double parse_double(const char* text, double fallback) {
    if (text == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    return (end == text) ? fallback : value;
}

/// 解析 `--key=value`；匹配则返回 value，否则返回 nullptr
const char* value_of(const char* arg, const char* key) {
    const std::size_t length = std::strlen(key);
    if (std::strncmp(arg, key, length) == 0 && arg[length] == '=') {
        return arg + length + 1;
    }
    return nullptr;
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (const char* v = value_of(arg, "--backend")) {
            ehe::core::Backend parsed{};
            if (ehe::app::parse_backend(v, parsed)) {
                options.backend = parsed;
            } else {
                std::fprintf(stderr, "[main] 未知后端 '%s'（可用 gl / vk）\n", v);
            }
        } else if (const char* v = value_of(arg, "--width")) {
            options.width = parse_int(v, options.width);
        } else if (const char* v = value_of(arg, "--height")) {
            options.height = parse_int(v, options.height);
        } else if (const char* v = value_of(arg, "--vsync")) {
            options.vsync = parse_int(v, 1) != 0;
        } else if (const char* v = value_of(arg, "--frames")) {
            options.smoke_frames = parse_int(v, options.smoke_frames);
            options.window_frames = options.smoke_frames;
        } else if (const char* v = value_of(arg, "--shot")) {
            options.shot = v;
        } else if (const char* v = value_of(arg, "--config")) {
            options.config_path = v;
        } else if (const char* v = value_of(arg, "--golden")) {
            options.golden_path = v;
        } else if (const char* v = value_of(arg, "--shader-root")) {
            options.shader_root = v;
        } else if (const char* v = value_of(arg, "--precision")) {
            options.precision = v;
            if (options.precision != "fp32" && options.precision != "mixed") {
                std::fprintf(stderr, "[main] --precision 只接受 fp32 / mixed，收到 '%s'（已忽略）\n", v);
                options.precision.clear();
            }
        } else if (const char* v = value_of(arg, "--nmse")) {
            options.nmse_threshold = parse_double(v, options.nmse_threshold);
        } else if (const char* v = value_of(arg, "--time")) {
            options.time = parse_double(v, options.time);
        } else if (std::strcmp(arg, "--no-animation") == 0) {
            options.animation = false;
        } else if (std::strcmp(arg, "--smoke") == 0) {
            options.smoke = true;
        } else if (std::strcmp(arg, "--try-backends") == 0) {
            options.try_backends = true;
        } else if (std::strcmp(arg, "--caps") == 0) {
            options.caps = true;
        } else if (std::strcmp(arg, "--time-mixed") == 0) {
            options.time_mixed = true;
        } else if (const char* v = value_of(arg, "--probe-size")) {
            options.probe_size = parse_int(v, options.probe_size);
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf(
                "用法:\n"
                "  ehe [--backend=gl|vk] [--width=N] [--height=N] [--vsync=0|1] [--shader-root=DIR]\n"
                "  ehe --smoke [--frames=N=60] [--shot=BASE=out] [--config=PATH] [--golden=PATH.pfm]\n"
                "            [--nmse=THRESHOLD=1e-3] [--backend=gl|vk]\n"
                "  ehe --try-backends\n"
                "  ehe --caps [--config=PATH] [--probe-size=N] [--time-mixed]\n");
            std::exit(0);
            std::exit(0);
        } else {
            std::fprintf(stderr, "[main] 忽略未知参数 '%s'\n", arg);
        }
    }
    return options;
}

// ---------------------------------------------------------------- backend 探测

int try_backends() {
    int ok_count = 0;
    for (const auto backend : {ehe::core::Backend::OpenGL, ehe::core::Backend::Vulkan}) {
        auto renderer = ehe::app::create_renderer(backend);
        ehe::render::RendererConfig cfg{};
        cfg.backend = backend;
        cfg.width = 320;
        cfg.height = 240;
        cfg.title = "EHE backend probe";
        cfg.vsync = false;
        cfg.visible = false;

        const bool ok = renderer != nullptr && renderer->init(cfg);
        std::printf("[probe] %-7s => %s\n", ehe::render::backend_name(backend), ok ? "OK" : "FAILED");
        if (ok) {
            ++ok_count;
            renderer->shutdown();
        }
    }
    std::printf("[probe] 可用后端数：%d\n", ok_count);
    if (ok_count == 0) {
        std::printf("[probe] 无可用后端，请检查显卡驱动（Vulkan 需系统存在 vulkan-1.dll）。\n");
    }
    return ok_count > 0 ? 0 : 1;
}

// ---------------------------------------------------------------- smoke 模式（DESIGN §6.3）

bool write_preview_png(const std::string& path, const ehe::core::HdrImageF& image, double exposure) {
    const auto bytes = ehe::core::tonemap_to_srgb8(image, exposure);
    return stbi_write_png(path.c_str(), image.width, image.height, 3, bytes.data(),
                          image.width * 3) != 0;
}

int run_smoke(const Options& options) {
    // 1) 加载 golden 参数（复用 Config 的容错解析：未知键告警、缺失键默认、值域钳制）
    std::vector<std::string> warnings;
    // 命令行精度覆盖：目标机上不必再改 JSON（也避免"改写参数文件"那一整类失败）
    ehe::core::Config config = ehe::core::Config::load_from_file(options.config_path, &warnings);
    if (!options.precision.empty()) {
        config.integrator.precision = (options.precision == "fp32")
                                          ? ehe::core::PrecisionMode::Fp32
                                          : ehe::core::PrecisionMode::Mixed;
        std::printf("[smoke] 精度模式由命令行指定：%s\n", options.precision.c_str());
    }
    for (const std::string& warning : warnings) {
        std::fprintf(stderr, "[smoke][config] %s\n", warning.c_str());
    }

    // 图像尺寸来自参数文件的 image 块（§6.1 固化 512²；开发期可用更小尺寸快速验证）。
    // 用 core 的 read_image_size（内部已做 BOM 剥离 + 异常吞掉）：
    // 踩坑：早期这里直接 `stream >> json`，配置文件为空时未捕获异常 → 进程 std::terminate
    //      （退出码 0xC0000409 / -1073740791），而 Config::load_from_file 那边看起来"已经容错"。
    int image_width = 512;
    int image_height = 512;
    ehe::core::Config::read_image_size(options.config_path, image_width, image_height);

    ehe::render::RendererConfig cfg{};
    cfg.backend = options.backend;
    cfg.width = image_width;
    cfg.height = image_height;
    cfg.title = "EHE smoke";
    cfg.vsync = false;
    cfg.visible = false;  // 离屏
    cfg.shader_root = options.shader_root;
    cfg.render_width = image_width;    // 与 golden 严格同尺寸（窗口尺寸受系统最小值限制，不能依赖）
    cfg.render_height = image_height;
    if (config.integrator.precision == ehe::core::PrecisionMode::Fp32) {
        cfg.shader_defines.push_back("EHE_FP32_ONLY");
        std::printf("[smoke] 精度模式：fp32（EHE_FP32_ONLY）\n");
    }

    auto renderer = ehe::app::create_renderer(options.backend);
    if (renderer == nullptr || !renderer->init(cfg)) {
        std::fprintf(stderr, "[smoke] 后端 %s 初始化失败（退出码 2）\n",
                     ehe::render::backend_name(options.backend));
        return 2;
    }
    if (!options.shader_root.empty()) {
        renderer->set_shader_root(options.shader_root);
    }
    if (!renderer->pipeline_ready()) {
        std::fprintf(stderr, "[smoke] 管线未就绪：%s\n", renderer->last_error().c_str());
        renderer->shutdown();
        return 2;
    }

    // 2) 预热 N 帧后取末帧（§6.3）
    ehe::core::Camera camera(config.camera);
    const ehe::core::Vec3 forward = camera.forward();
    double last_frame_ms = 0.0;
    ehe::render::SimParams params =
        ehe::render::make_sim_params(config, camera, forward, 1.0, options.time);
    renderer->set_params(params);
    if (options.time != 0.0) {
        std::printf("[smoke] 动画时间 time=%.3f s（用于湍流图案的确定性对照）\n", options.time);
    }

    for (int frame = 0; frame < std::max(1, options.smoke_frames); ++frame) {
        const auto begin = std::chrono::steady_clock::now();
        renderer->begin_frame();
        renderer->end_frame();
        // 必须 finish()：否则只测到**提交时间**。实测本机 32² fp32 提交 12.7 ms、真实 GPU 时间 254 ms，
        // 差 20 倍——不同步的计时会把"很慢"误报成"很快"（DESIGN §5.4.1：仅冒烟/探测允许同步）。
        renderer->finish();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - begin)
                              .count();
        std::fprintf(stderr, "[smoke] 预热帧 %d/%d 耗时 %.1f ms（含 GPU 同步）\n", frame + 1,
                     std::max(1, options.smoke_frames), ms);
        last_frame_ms = ms;
    }

    std::printf("[smoke] 预热帧耗时 %.1f ms（%dx%d, n_max=%d, %s）\n", last_frame_ms, image_width,
                image_height, config.integrator.n_max,
                (config.integrator.precision == ehe::core::PrecisionMode::Fp32) ? "fp32"
                                                                               : "mixed(fp64)");
    std::fprintf(stderr, "[smoke] 回读 HDR 结果...\n");
    std::vector<float> rgb;
    int width = 0;
    int height = 0;
    if (!renderer->capture_hdr(rgb, width, height)) {
        std::fprintf(stderr, "[smoke] HDR 回读失败：%s\n", renderer->last_error().c_str());
        renderer->shutdown();
        return 2;
    }

    // 后处理链的 LDR 成品（在 shutdown 之前抓，否则上下文已销毁）
    std::vector<unsigned char> post_rgb;
    int post_width = 0;
    int post_height = 0;
    const bool has_post = renderer->capture_ldr(post_rgb, post_width, post_height);
    if (!has_post) {
        std::fprintf(stderr, "[smoke] 后处理输出回读失败（跳过 post 一致性校验）：%s\n",
                     renderer->last_error().c_str());
    }
    renderer->shutdown();

    ehe::core::HdrImageF image;
    image.width = width;
    image.height = height;
    image.pixels = std::move(rgb);

    // 3) 写出 PFM（差分基准）与 PNG（预览）
    const std::string pfm_path = options.shot + ".pfm";
    const std::string png_path = options.shot + ".png";
    if (!ehe::core::write_pfm(pfm_path, image)) {
        std::fprintf(stderr, "[smoke] PFM 写出失败：%s\n", pfm_path.c_str());
        return 2;
    }
    if (!write_preview_png(png_path, image, config.post.exposure)) {
        std::fprintf(stderr, "[smoke] PNG 写出失败：%s\n", png_path.c_str());
        return 2;
    }
    std::printf("[smoke] 输出 %s（%dx%d，%s，%d 帧）与 %s\n", pfm_path.c_str(), width, height,
                ehe::render::backend_name(options.backend), options.smoke_frames, png_path.c_str());

    // 3b) 后处理一致性：GPU 后处理成品 vs CPU 同式参考（core/tonemap.cpp）
    //
    // 为什么需要：后处理作用在 golden **之前**（PFM 是 post 前的 HDR，§6.1），不进 NMSE 差分，
    // 因此它的正确性只能靠这条对照来证。色差开启时 CPU 侧未做同样采样，故跳过该情形。
    if (has_post) {
        const std::string post_png = options.shot + "_post.png";
        stbi_write_png(post_png.c_str(), post_width, post_height, 3, post_rgb.data(),
                       post_width * 3);
        std::printf("[smoke] 后处理成品 %s（%dx%d）\n", post_png.c_str(), post_width, post_height);

        if (config.post.chrom_ab > 0.0) {
            std::printf("[smoke] 色差已开启（%.2f）→ 跳过与 CPU 参考的逐像素对照（CPU 未做同式采样）\n",
                        config.post.chrom_ab);
        } else {
            const auto reference = ehe::core::tonemap_to_srgb8(image, config.post.exposure,
                                                               config.post.aces);
            if (reference.size() == post_rgb.size()) {
                int max_difference = 0;
                double sum_difference = 0.0;
                for (std::size_t i = 0; i < post_rgb.size(); ++i) {
                    const int difference = std::abs(static_cast<int>(post_rgb[i]) -
                                                    static_cast<int>(reference[i]));
                    max_difference = std::max(max_difference, difference);
                    sum_difference += difference;
                }
                const double mean_difference =
                    sum_difference / static_cast<double>(post_rgb.size());
                std::printf("[smoke] post 一致性（GPU vs CPU 同式 ACES/曝光/sRGB）："
                            "最大差=%d/255  平均差=%.3f/255\n",
                            max_difference, mean_difference);
                // 容差 3/255：GPU 用 FP16 中间值 + 逐点浮点次序不同，8 位量化后允许 ±1~2 的抖动
                if (max_difference > 3) {
                    std::fprintf(stderr, "[smoke] ⚠️ 后处理与 CPU 参考偏差过大（> 3/255）\n");
                }
            } else {
                std::fprintf(stderr, "[smoke] post 尺寸不匹配（%zu vs %zu），跳过对照\n",
                             post_rgb.size(), reference.size());
            }
        }
    }

    // 4) 与 golden 差分（§6.2：逐通道 NMSE 取最大）
    ehe::core::HdrImageF golden;
    if (!ehe::core::read_pfm(options.golden_path, golden)) {
        std::fprintf(stderr, "[smoke] 无法读取 golden：%s（退出码 2）\n", options.golden_path.c_str());
        return 2;
    }
    const ehe::core::DiffStats stats = ehe::core::diff_stats(image, golden);
    if (!stats.comparable) {
        std::fprintf(stderr, "[smoke] 图像不可比（%dx%d vs %dx%d，或 golden 全零）\n", image.width,
                     image.height, golden.width, golden.height);
        return 2;
    }
    std::printf("[smoke] NMSE=%.6e（阈值 %.1e）  R=%.3e G=%.3e B=%.3e\n", stats.nmse,
                options.nmse_threshold, stats.nmse_rgb[0], stats.nmse_rgb[1], stats.nmse_rgb[2]);
    std::printf("[smoke] 最大绝对差=%.6e  平均绝对差=%.6e\n", stats.max_abs_diff,
                stats.mean_abs_diff);
    if (std::abs(config.render.res_scale - 1.0) > 1e-6) {
        // golden 基线定义在 res_scale = 1.0（§6.1）；SSAA/升频是对同一场景的**不同采样**，
        // 与 1.0× 基线本就不同（实测 SSAA 2.0x ≈ 2.5e-3、升频 0.5x ≈ 2.6e-3），
        // 因此此时的 NMSE 只作参考量，不构成"实现错误"。
        std::printf("[smoke] 注意：res_scale=%.2f ≠ 1.0，golden 基线定义在 1.0×，"
                    "此处 NMSE 仅作参考（非实现正确性判据）\n",
                    config.render.res_scale);
    }

    if (stats.nmse <= options.nmse_threshold) {
        std::printf("[smoke] 通过\n");
        return 0;
    }
    std::printf("[smoke] 超阈值（退出码 1）\n");
    return 1;
}

// ---------------------------------------------------------------- 窗口模式

/// 自由飞行速度档（§4.7：慢/中/快三档，单位/秒）
double fly_speed_units_per_second(int speed_level) {
    static constexpr double kSpeeds[] = {5.0, 15.0, 40.0};
    return kSpeeds[std::clamp(speed_level, 0, 2)];
}

void handle_camera_input(GLFWwindow* window, ehe::core::Camera& camera, double& last_x,
                         double& last_y, bool& dragging, double frame_dt_seconds,
                         int fly_speed_level) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        dragging = false;
        return;  // ImGui 捕获鼠标时不做相机交互（面板拖拽不转视角）
    }
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);

    const bool orbit_mode = camera.mode() == ehe::core::CameraMode::Orbit;
    const int orbit_button = GLFW_MOUSE_BUTTON_LEFT;
    const int fly_button = GLFW_MOUSE_BUTTON_RIGHT;
    const int drag_button = orbit_mode ? orbit_button : fly_button;

    const bool button_down = glfwGetMouseButton(window, drag_button) == GLFW_PRESS;
    if (button_down && !dragging) {
        dragging = true;
        last_x = x;
        last_y = y;
    } else if (!button_down) {
        dragging = false;
    }
    if (dragging && button_down) {
        if (orbit_mode) {
            camera.orbit(-(x - last_x) * 0.3, (y - last_y) * 0.3);
        } else {
            camera.fly_look(-(x - last_x) * 0.15, -(y - last_y) * 0.15);
        }
        last_x = x;
        last_y = y;
    }

    // 自由飞行平移（§4.7：WASD + Shift ×5；ImGui 捕获键盘时不响应）
    if (!orbit_mode && !io.WantCaptureKeyboard && frame_dt_seconds > 0.0) {
        double speed = fly_speed_units_per_second(fly_speed_level) * frame_dt_seconds;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
            speed *= 5.0;
        }
        double fly_forward = 0.0;
        double fly_strafe = 0.0;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            fly_forward += 1.0;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            fly_forward -= 1.0;
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            fly_strafe += 1.0;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
            fly_strafe -= 1.0;
        }
        if (fly_forward != 0.0 || fly_strafe != 0.0) {
            camera.fly_move(ehe::core::Vec3{fly_strafe * speed, 0.0, fly_forward * speed});
        }
    }
}

void on_scroll(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    auto* accumulator = static_cast<double*>(glfwGetWindowUserPointer(window));
    if (accumulator != nullptr) {
        *accumulator += yoffset;
    }
}

void draw_overlay(const Options& options, ehe::render::IRenderer& renderer, int internal_width,
                  int internal_height, double frame_ms) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 240.0F, 16.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(224, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("EHE 监控", nullptr, ImGuiWindowFlags_NoCollapse);
    int out_width = 0;
    int out_height = 0;
    renderer.framebuffer_size(out_width, out_height);
    ImGui::Text("FPS        %.1f", static_cast<double>(io.Framerate));
    ImGui::Text("帧时间     %.2f ms", frame_ms);
    ImGui::Text("分辨率     %dx%d x %dx%d", internal_width, internal_height, out_width, out_height);
    ImGui::Text("后端       %s", ehe::render::backend_name(options.backend));
    const double gpu_ms = renderer.gpu_frame_ms();
    if (gpu_ms >= 0.0) {
        ImGui::Text("GPU        %.2f ms", gpu_ms);
    } else {
        ImGui::TextDisabled("GPU        n/a");
    }
    const double steps = renderer.last_avg_steps();
    if (steps >= 0.0) {
        ImGui::Text("平均步数   %.1f", steps);
    } else {
        ImGui::TextDisabled("平均步数   n/a");
    }
    ImGui::End();
}

void draw_panel(const Options& options, ehe::core::Camera& camera, ehe::render::IRenderer& renderer,
                ehe::render::SimParams& params, bool& request_switch,
                ehe::core::Backend& target_backend, ehe::core::Config& config, bool& animation,
                double animation_time, UiState& ui, ehe::app::AudioEngine& audio) {
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("EHE 控制面板");

    ImGui::Text("版本 %s [%s]", ehe::core::version_string(), ehe::core::build_flags());
    int width = 0;
    int height = 0;
    renderer.framebuffer_size(width, height);
    ImGui::Text("窗口 %dx%d", width, height);
    if (!renderer.pipeline_ready()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
        ImGui::TextWrapped("管线未就绪：%s", renderer.last_error().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Checkbox("显示监控 overlay（§8）", &ui.show_overlay);

    // ================================================================ 渲染组
    if (ImGui::CollapsingHeader("渲染", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* backend_items[] = {"OpenGL 4.5", "Vulkan 1.2"};
        int backend_index = renderer.backend() == ehe::core::Backend::Vulkan ? 1 : 0;
        if (ImGui::Combo("后端", &backend_index, backend_items, 2)) {
            const auto picked =
                backend_index == 1 ? ehe::core::Backend::Vulkan : ehe::core::Backend::OpenGL;
            if (picked != renderer.backend()) {
                request_switch = true;
                target_backend = picked;
            }
        }
        const char* mode_items[] = {"raymarch（相对论光线追踪）", "particle（经典粒子，牛顿近似）"};
        int mode_index = config.render.mode == ehe::core::RenderMode::Particle ? 1 : 0;
        if (ImGui::Combo("模式", &mode_index, mode_items, 2)) {
            config.render.mode =
                mode_index == 1 ? ehe::core::RenderMode::Particle : ehe::core::RenderMode::Raymarch;
        }

        const char* scale_items[] = {"0.5x", "0.67x", "0.75x", "1.0x（原生）", "1.25x", "1.5x", "2.0x"};
        const double scale_values[] = {0.5, 0.67, 0.75, 1.0, 1.25, 1.5, 2.0};
        int scale_index = 3;
        for (int i = 0; i < 7; ++i) {
            if (std::abs(config.render.res_scale - scale_values[i]) < 1e-6) {
                scale_index = i;
                break;
            }
        }
        if (ImGui::Combo("内部分辨率", &scale_index, scale_items, 7)) {
            config.render.res_scale = scale_values[scale_index];
        }
        ImGui::TextDisabled(">1.0 = SSAA 降采样；<1.0 = 升频（FSR1 开关生效）");
        ImGui::Checkbox("FSR1 升频（<1.0x 时）", &config.render.fsr1);
        ImGui::Checkbox("FXAA（色调映射前）", &config.render.fxaa);

        const char* fps_items[] = {"60", "120", "不限"};
        const int fps_values[] = {60, 120, 0};
        int fps_index = 0;
        for (int i = 0; i < 3; ++i) {
            if (config.render.fps_cap == fps_values[i]) {
                fps_index = i;
                break;
            }
        }
        if (ImGui::Combo("帧率上限", &fps_index, fps_items, 3)) {
            config.render.fps_cap = fps_values[fps_index];
        }

        const char* views[] = {"shaded", "steps", "classify", "g_factor", "null_drift"};
        int current = static_cast<int>(ehe::render::debug_view_of(params));
        if (ImGui::Combo("调试视图", &current, views, 5)) {
            ehe::render::set_debug_view(params, static_cast<ehe::render::DebugView>(current));
        }
        ImGui::TextDisabled("classify / steps 用于自查积分器行为（§5.5）");
    }

    // ================================================================ 黑洞组
    if (ImGui::CollapsingHeader("黑洞", ImGuiTreeNodeFlags_DefaultOpen)) {
        float mass = static_cast<float>(config.blackhole.mass);
        if (ImGui::SliderFloat("质量 M", &mass, 0.1F, 10.0F, "%.2f")) {
            config.blackhole.mass = static_cast<double>(mass);
        }
        ImGui::TextDisabled("方程在 M=1 下无量纲化；此处仅面板展示");

        // M1 阶段自旋锁定为 0（§8：灰显）
        ImGui::BeginDisabled(true);
        float spin = static_cast<float>(config.blackhole.spin);
        ImGui::SliderFloat("自旋 a", &spin, 0.0F, 0.998F, "%.3f");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("M1 锁定 0（Kerr 随 M2 解锁）");

        float r_in = static_cast<float>(config.blackhole.disk_r_in);
        if (ImGui::SliderFloat("盘内半径", &r_in, 2.5F, 12.0F, "%.1f")) {
            config.blackhole.disk_r_in = static_cast<double>(r_in);
        }
        float r_out = static_cast<float>(config.blackhole.disk_r_out);
        if (ImGui::SliderFloat("盘外半径", &r_out, 10.0F, 40.0F, "%.1f")) {
            config.blackhole.disk_r_out = static_cast<double>(r_out);
        }
        float density = static_cast<float>(config.blackhole.disk_density);
        if (ImGui::SliderFloat("盘密度", &density, 0.0F, 2.0F, "%.2f")) {
            config.blackhole.disk_density = static_cast<double>(density);
        }
        float t_scale = static_cast<float>(config.blackhole.disk_t_scale);
        if (ImGui::SliderFloat("盘温 T_scale", &t_scale, 2000.0F, 40000.0F, "%.0f K")) {
            config.blackhole.disk_t_scale = static_cast<double>(t_scale);
        }
        float kappa = static_cast<float>(config.blackhole.disk_kappa);
        if (ImGui::SliderFloat("吸收 κ", &kappa, 0.1F, 8.0F, "%.2f")) {
            config.blackhole.disk_kappa = static_cast<double>(kappa);
        }
        float noise = static_cast<float>(config.blackhole.disk_noise);
        if (ImGui::SliderFloat("湍流噪声振幅", &noise, 0.0F, 1.0F, "%.2f")) {
            config.blackhole.disk_noise = static_cast<double>(noise);
        }
        ImGui::Checkbox("盘动画（time 推进）", &animation);
        ImGui::TextDisabled("图案按 Ω(r)=r^-3/2 差速旋转；golden 基线用关闭 + 噪声 0");
        ImGui::Text("t = %.2f s", animation_time);
    }

    // ================================================================ 积分器组
    if (ImGui::CollapsingHeader("积分器")) {
        int n_max = config.integrator.n_max;
        if (ImGui::SliderInt("最大步数 N", &n_max, 30, 2048)) {
            config.integrator.n_max = n_max;
        }
        float h0 = static_cast<float>(config.integrator.h0);
        if (ImGui::SliderFloat("步长因子 h0", &h0, 0.005F, 0.2F, "%.3f")) {
            config.integrator.h0 = static_cast<double>(h0);
        }
        float h_min = static_cast<float>(config.integrator.h_min);
        if (ImGui::SliderFloat("步长下限 h_min", &h_min, 0.0001F, 0.01F, "%.4f")) {
            config.integrator.h_min = static_cast<double>(h_min);
        }
        float h_max = static_cast<float>(config.integrator.h_max);
        if (ImGui::SliderFloat("步长上限 h_max", &h_max, 0.05F, 1.0F, "%.2f")) {
            config.integrator.h_max = static_cast<double>(h_max);
        }
        const char* precision_items[] = {"mixed（关键路径 fp64）", "纯 fp32"};
        int precision_index =
            config.integrator.precision == ehe::core::PrecisionMode::Fp32 ? 1 : 0;
        if (ImGui::Combo("精度模式", &precision_index, precision_items, 2)) {
            config.integrator.precision =
                precision_index == 1 ? ehe::core::PrecisionMode::Fp32
                                     : ehe::core::PrecisionMode::Mixed;
            ui.rebuild_requested = true;  // shader 宏变化 → 重建管线（不重建上下文）
        }
    }

    // ================================================================ 粒子组
    if (ImGui::CollapsingHeader("粒子")) {
        ImGui::Checkbox("启用粒子", &config.particle.enabled);
        // 数量：1k–1M 对数档（§5.6）
        float count_log = std::log10(static_cast<double>(std::max(1000, config.particle.count)));
        if (ImGui::SliderFloat("数量（对数）", &count_log, 3.0F, 6.0F, "10^%.1f")) {
            const double raw = std::pow(10.0, static_cast<double>(count_log));
            config.particle.count = std::max(1000, static_cast<int>(raw / 1000.0 + 0.5) * 1000);
        }
        ImGui::TextDisabled("当前 %d 粒子", config.particle.count);
        float size = static_cast<float>(config.particle.size);
        if (ImGui::SliderFloat("大小", &size, 0.2F, 4.0F, "%.2f")) {
            config.particle.size = static_cast<double>(size);
        }
        const char* profile_items[] = {"kepler（开普勒圆速 ±5%）"};
        int profile_index = 0;
        ImGui::Combo("初速分布", &profile_index, profile_items, 1);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.75F, 0.2F, 1.0F));
        ImGui::TextWrapped("⚠ 粒子为牛顿近似，与 GR 模式不具物理一致性（§5.6 科普演示定位）");
        ImGui::PopStyleColor();
    }

    // ================================================================ 后处理组
    if (ImGui::CollapsingHeader("后处理", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("ACES 色调映射", &config.post.aces);
        float exposure = static_cast<float>(config.post.exposure);
        if (ImGui::SliderFloat("曝光", &exposure, 0.05F, 4.0F, "%.2f")) {
            config.post.exposure = static_cast<double>(exposure);
        }
        float chroma_ab = static_cast<float>(config.post.chrom_ab);
        if (ImGui::SliderFloat("色差", &chroma_ab, 0.0F, 1.0F, "%.2f")) {
            config.post.chrom_ab = static_cast<double>(chroma_ab);
        }
    }

    // ================================================================ 音频组
    if (ImGui::CollapsingHeader("音频")) {
        float volume = static_cast<float>(config.audio.volume);
        if (ImGui::SliderFloat("音量", &volume, 0.0F, 1.0F, "%.2f")) {
            config.audio.volume = static_cast<double>(volume);
            audio.set_volume(volume);
        }
        ImGui::Checkbox("静音", &config.audio.muted);
        audio.set_muted(config.audio.muted);
        if (!audio.ready()) {
            ImGui::TextDisabled("音频设备不可用（静默降级）");
        }
        ImGui::TextDisabled("合成：棕噪声 + 低通，音色随盘密度变化（§5.7）");
    }

    // ================================================================ 相机组
    if (ImGui::CollapsingHeader("相机", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* camera_items[] = {"轨道（绕黑洞）", "自由飞行（WASD + 右键转视角）"};
        int camera_index = camera.mode() == ehe::core::CameraMode::Fly ? 1 : 0;
        if (ImGui::Combo("相机模式", &camera_index, camera_items, 2)) {
            camera.set_mode(camera_index == 1 ? ehe::core::CameraMode::Fly
                                              : ehe::core::CameraMode::Orbit);
        }
        double fov = camera.fov_deg();
        float fov_lo = static_cast<float>(ehe::core::kFovMinDeg);
        float fov_hi = static_cast<float>(ehe::core::kFovMaxDeg);
        if (ImGui::SliderScalar("FOV", ImGuiDataType_Double, &fov, &fov_lo, &fov_hi, "%.1f deg")) {
            camera.set_fov_deg(fov);
        }
        if (camera.mode() == ehe::core::CameraMode::Orbit) {
            float dist = static_cast<float>(camera.orbit_distance());
            if (ImGui::SliderFloat("距离", &dist, static_cast<float>(ehe::core::kOrbitDistanceMin),
                                   static_cast<float>(ehe::core::kOrbitDistanceMax), "%.1f")) {
                camera.zoom(std::log2(dist / camera.orbit_distance()));  // 对数缩放语义
            }
            ImGui::Text("方位 %.1f°  极角 %.1f°", camera.orbit_azimuth_deg(),
                        camera.orbit_polar_deg());
            ImGui::TextDisabled("左键拖拽旋转 / 滚轮缩放");
        } else {
            const char* speed_items[] = {"慢（5/s）", "中（15/s）", "快（40/s）"};
            ImGui::Combo("飞行速度", &ui.fly_speed, speed_items, 3);
            ImGui::TextDisabled("WASD 平移 · Shift 加速 x5 · 右键拖拽转视角");
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("初始后端 %s / vsync=%d / 配置 %s", ehe::render::backend_name(options.backend),
                        options.vsync ? 1 : 0, options.config_path.c_str());
    ImGui::End();
}

int run_window(const Options& options) {
    std::vector<std::string> warnings;
    // §8.1 启动加载顺序：显式 --config > ehe.config.json（上次会话）> 默认路径。
    // 显式 --config 也照常加载（golden 校验/参数文件场景），退出时仍保存会话状态。
    std::string load_path = options.config_path;
    if (std::ifstream(options.persist_path)) {
        load_path = options.persist_path;  // 上次会话状态优先于默认参数文件
        std::printf("[main] 发现会话配置 %s（优先于 %s 加载）\n", options.persist_path.c_str(),
                    options.config_path.c_str());
    }
    ehe::core::Config config = ehe::core::Config::load_from_file(load_path, &warnings);
    if (!options.precision.empty()) {
        config.integrator.precision = (options.precision == "fp32")
                                          ? ehe::core::PrecisionMode::Fp32
                                          : ehe::core::PrecisionMode::Mixed;
        std::printf("[main] 精度模式由命令行指定：%s\n", options.precision.c_str());
    }
    for (const std::string& warning : warnings) {
        std::fprintf(stderr, "[main][config] %s\n", warning.c_str());
    }
    config.render.backend = options.backend;

    /// 保存会话状态（§8.1：退出/切换后端时保存；相机状态经 write_to 回写）
    const auto persist_config = [&config, &options](const ehe::core::Camera& camera) {
        camera.write_to(config.camera);
        if (!config.save_to_file(options.persist_path)) {
            std::fprintf(stderr, "[main] 会话配置保存失败：%s\n", options.persist_path.c_str());
        }
    };

    ehe::render::Backend current_backend = options.backend;
    ehe::render::RendererConfig cfg{};
    cfg.backend = current_backend;
    cfg.width = options.width;
    cfg.height = options.height;
    cfg.title = "Event Horizon Engine";
    cfg.vsync = options.vsync;
    cfg.shader_root = options.shader_root;
    if (config.integrator.precision == ehe::core::PrecisionMode::Fp32) {
        cfg.shader_defines.push_back("EHE_FP32_ONLY");
        std::printf("[main] 精度模式：fp32（EHE_FP32_ONLY）\n");
    }

    auto renderer = ehe::app::create_renderer(current_backend);
    if (renderer == nullptr || !renderer->init(cfg)) {
        std::fprintf(stderr,
                     "[main] 后端 %s 初始化失败。\n"
                     "        请检查显卡驱动；Vulkan 需要系统存在 vulkan-1.dll。\n"
                     "        可用 --try-backends 查看两后端探测结果。\n",
                     ehe::render::backend_name(current_backend));
        return 1;
    }
    if (!options.shader_root.empty()) {
        renderer->set_shader_root(options.shader_root);
    }

    ehe::core::Camera camera(config.camera);
    double last_x = 0.0;
    double last_y = 0.0;
    bool dragging = false;
    double zoom_accumulator = 0.0;

    ehe::app::AudioEngine audio;  // 音频独立线程（§5.7）；初始化失败静默降级
    audio.init();
    audio.set_volume(config.audio.volume);
    audio.set_muted(config.audio.muted);

    UiState ui;

    auto bind_window = [&]() {
        GLFWwindow* current = glfwGetCurrentContext();
        if (current != nullptr) {
            glfwSetWindowUserPointer(current, &zoom_accumulator);
            glfwSetScrollCallback(current, on_scroll);
        }
        return current;
    };
    GLFWwindow* window = bind_window();

    bool running = true;
    int rendered = 0;
    double last_frame_ms = 0.0;
    auto frame_clock = std::chrono::steady_clock::now();
    // 动画时间：以 --time 为起始偏移，开动画时随实际时间推进（§4.5 差速旋转由 UBO time 驱动）
    const double animation_start = glfwGetTime();
    bool animation = options.animation;
    while (running && !renderer->should_close()) {
        glfwPollEvents();
        if (window != nullptr) {
            handle_camera_input(window, camera, last_x, last_y, dragging, last_frame_ms * 0.001,
                                ui.fly_speed);
            if (zoom_accumulator != 0.0) {
                camera.zoom(zoom_accumulator);
                zoom_accumulator = 0.0;
            }
        }

        renderer->begin_frame();

        int width = 0;
        int height = 0;
        renderer->framebuffer_size(width, height);
        const double aspect = (height > 0) ? static_cast<double>(width) / height : 1.0;
        const double animation_time =
            options.time + (animation ? (glfwGetTime() - animation_start) : 0.0);
        ehe::render::SimParams params =
            ehe::render::make_sim_params(config, camera, camera.forward(), aspect, animation_time);
        // 动画/粒子步进开关（app 层运行时状态，不进 schema）
        ehe::render::set_flag(params, ehe::render::kFlagAnimate, animation);

        bool request_switch = false;
        ehe::core::Backend target_backend = current_backend;
        draw_panel(options, camera, *renderer, params, request_switch, target_backend, config,
                   animation, animation_time, ui, audio);
        renderer->set_params(params);  // 面板改动当帧生效

        int internal_width = 0;
        int internal_height = 0;
        renderer->render_resolution(internal_width, internal_height);
        if (ui.show_overlay) {
            draw_overlay(options, *renderer, internal_width, internal_height, last_frame_ms);
        }

        renderer->end_frame();

        ++rendered;
        {
            const auto now = std::chrono::steady_clock::now();
            last_frame_ms = std::chrono::duration<double, std::milli>(now - frame_clock).count();
            frame_clock = now;
            if (rendered <= 5) {
                std::fprintf(stderr, "[main] 第 %d 帧耗时 %.1f ms（管线就绪=%d）\n", rendered,
                             last_frame_ms, renderer->pipeline_ready() ? 1 : 0);
            }
        }

        // 帧率上限（§8 渲染组）：vsync 之外再做软限速（cap > 实际帧率时 sleep 不触发）
        if (config.render.fps_cap > 0 && last_frame_ms > 0.0) {
            const double budget_ms = 1000.0 / static_cast<double>(config.render.fps_cap);
            if (last_frame_ms < budget_ms - 0.5) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double, std::milli>(budget_ms - last_frame_ms - 0.5));
            }
        }

        if (options.window_frames > 0 && rendered >= options.window_frames) {
            running = false;
        }

        // 精度模式改动 → 重建管线（不重建上下文；GL/VK 同路径，见 IRenderer::rebuild_pipeline）
        if (ui.rebuild_requested) {
            ui.rebuild_requested = false;
            std::vector<std::string> defines;
            if (config.integrator.precision == ehe::core::PrecisionMode::Fp32) {
                defines.push_back("EHE_FP32_ONLY");
            }
            std::printf("[main] 精度切换 → 重建管线（%s）\n",
                        config.integrator.precision == ehe::core::PrecisionMode::Fp32 ? "fp32"
                                                                                      : "mixed");
            if (!renderer->rebuild_pipeline(defines)) {
                std::fprintf(stderr, "[main] 管线重建失败：%s\n", renderer->last_error().c_str());
            }
        }

        if (request_switch) {
            std::printf("[main] 切换后端：%s -> %s（销毁并重建窗口句柄）\n",
                        ehe::render::backend_name(current_backend),
                        ehe::render::backend_name(target_backend));
            int last_width = 0;
            int last_height = 0;
            renderer->framebuffer_size(last_width, last_height);
            persist_config(camera);  // §8.1：切换后端时保存

            renderer->shutdown();
            renderer.reset();

            current_backend = target_backend;
            cfg.backend = current_backend;
            if (last_width > 0 && last_height > 0) {
                cfg.width = last_width;
                cfg.height = last_height;
            }
            renderer = ehe::app::create_renderer(current_backend);
            if (renderer == nullptr || !renderer->init(cfg)) {
                // 切换失败不退出（DESIGN §5.2）：回退到原后端继续跑，错误显示在面板上
                std::fprintf(stderr, "[main] 切换到 %s 失败，回退到 %s\n",
                             ehe::render::backend_name(current_backend),
                             ehe::render::backend_name(options.backend));
                current_backend = options.backend;
                cfg.backend = current_backend;
                if (last_width > 0 && last_height > 0) {
                    cfg.width = last_width;
                    cfg.height = last_height;
                }
                renderer = ehe::app::create_renderer(current_backend);
                if (renderer == nullptr || !renderer->init(cfg)) {
                    std::fprintf(stderr, "[main] 回退到初始后端仍失败，程序退出\n");
                    audio.shutdown();
                    return 1;
                }
            }
            if (!options.shader_root.empty()) {
                renderer->set_shader_root(options.shader_root);
            }
            window = bind_window();
        }
    }

    persist_config(camera);  // §8.1：退出时保存会话状态
    audio.shutdown();
    renderer->shutdown();
    renderer.reset();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // 先把控制台切到 UTF-8：本程序的字符串字面量是 UTF-8，而中文 Windows 控制台默认 GBK(936)
    // —— 不切换的话所有中文输出都是乱码（与调用方式无关：双击、脚本调起都一样）
    ehe::core::enable_utf8_console();
    // 无缓冲 stdout：图形路径崩溃时缓冲会丢，日志即现场（smoke/开发期定位必需）
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const Options options = parse_args(argc, argv);
    std::printf("Event Horizon Engine %s [%s]\n", ehe::core::version_string(),
                ehe::core::build_flags());

    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "[main] GLFW 初始化失败\n");
        return 1;
    }
    std::printf("[main] GLFW: %s\n", glfwGetVersionString());

    int exit_code = 0;
    if (options.caps) {
        ehe::app::CapOptions cap_options{};
        cap_options.config_path = options.config_path;
        cap_options.time_mixed = options.time_mixed;
        cap_options.probe_size = options.probe_size;
        cap_options.shader_root = options.shader_root;
        exit_code = ehe::app::run_caps(cap_options);
    } else if (options.try_backends) {
        exit_code = try_backends();
    } else if (options.smoke) {
        exit_code = run_smoke(options);
    } else {
        exit_code = run_window(options);
    }

    glfwTerminate();
    return exit_code;
}
