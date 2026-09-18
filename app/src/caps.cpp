// EHE app —— 能力探测实现（--caps）
//
// 输出三块内容，全部是「目标机能不能跑 / 用哪种精度 / 能到多少帧」的硬数据：
//   1. GL 与 Vulkan 的能力清单（API 版本、设备、关键扩展或设备特性、上限、功能性探测）
//   2. 两种精度模式（mixed=fp64 关键路径 / fp32）各自能否编译出管线；不能则打印编译错误
//   3. 能编译的模式实测帧耗时（每帧 finish 同步，取平均/最小/最大）
//
// 重要实现约束：**同一进程内只创建一次图形上下文**，切换精度走 `rebuild_pipeline`
//   （实测第二次 gladLoadGLLoader 会返回 0 → 上下文创建路径不可复用）。
//
// 为什么需要它：DESIGN §6 L4 规定 GPU 渲染不进 CI，GPU 结论只能在目标机采集；
//   本探针 + tools/run_target_tests.ps1 把采集结果落成一份文本，便于回传核对。

#include "caps.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "backend_factory.h"
#include "ehe/core/camera.h"
#include "ehe/core/config.h"
#include "ehe/render/IRenderer.h"
#include "ehe/render/sim_params.h"

namespace ehe::app {
namespace {

struct Timings {
    bool ready = false;
    double mean_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    std::string error;
};

/// 在当前管线上渲染若干帧并计时（每帧 finish 同步，得到真实 GPU 时间）
Timings time_frames(ehe::render::IRenderer& renderer, const ehe::core::Config& config,
                    int frames) {
    Timings timings;

    ehe::core::Camera camera(config.camera);
    ehe::render::SimParams params =
        ehe::render::make_sim_params(config, camera, camera.forward(), 1.0, 0.0);
    renderer.set_params(params);

    double total = 0.0;
    timings.min_ms = 1e30;
    for (int frame = 0; frame < std::max(1, frames); ++frame) {
        const auto begin = std::chrono::steady_clock::now();
        renderer.begin_frame();
        renderer.end_frame();
        renderer.finish();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        total += ms;
        timings.min_ms = std::min(timings.min_ms, ms);
        timings.max_ms = std::max(timings.max_ms, ms);
    }
    timings.mean_ms = total / static_cast<double>(std::max(1, frames));
    timings.ready = true;
    return timings;
}

/// 探测单个后端：清单 + 两种精度模式的可编译性与帧耗时
void probe_backend(const ehe::core::Config& config, ehe::core::Backend backend,
                   const CapOptions& options) {
    std::printf("\n================== %s ==================\n", ehe::render::backend_name(backend));

    ehe::render::RendererConfig cfg{};
    cfg.backend = backend;
    cfg.width = options.probe_size;
    cfg.height = options.probe_size;
    cfg.render_width = options.probe_size;
    cfg.render_height = options.probe_size;
    cfg.title = "EHE caps";
    cfg.vsync = false;
    cfg.visible = false;
    cfg.shader_root = options.shader_root;  // 必须在 init 前（init 内构建管线）

    auto renderer = create_renderer(backend);
    if (renderer == nullptr || !renderer->init(cfg)) {
        std::printf("[caps] %s 初始化失败（驱动/运行时缺失）\n", ehe::render::backend_name(backend));
        return;
    }

    // 1) 能力清单（用默认精度建好的管线即可给出上下文信息）
    std::printf("%s", renderer->capability_report().c_str());

    // 2) 两种精度模式（管线级重建，不重建上下文）
    //
    // ⚠ 顺序与安全策略：**先 fp32 后 mixed**，且 mixed 默认只做「能否编译」判定、不测帧耗时。
    //   原因（本机实测证据）：无 fp64 硬件的 GPU 上 fp64 由驱动软件模拟——AMD HD 7400M 上
    //   32²×60 步单帧 2.16 s；64² 时探针直接挂死（GPU/驱动 20 分钟无响应，只能强杀）。
    //   因此把 fp64 计时做成显式 opt-in（--time-mixed），避免在目标机上复现同样的挂死。
    struct ModeEntry {
        const char* label;
        bool fp32_only;
    };
    const ModeEntry modes[] = {{"fp32", true}, {"mixed(fp64)", false}};

    for (const ModeEntry& mode : modes) {
        const bool time_this_mode = mode.fp32_only || options.time_mixed;
        const std::vector<std::string> defines =
            mode.fp32_only ? std::vector<std::string>{"EHE_FP32_ONLY"} : std::vector<std::string>{};
        const bool ready = renderer->rebuild_pipeline(defines);
        if (!ready) {
            std::printf("[caps] %-7s %-12s 管线不可用：%s\n", ehe::render::backend_name(backend),
                        mode.label, renderer->last_error().c_str());
            continue;
        }
        if (!time_this_mode) {
            std::printf("[caps] %-7s %-12s 管线就绪（**未计时**：fp64 在无硬件支持的 GPU 上为软件模拟，"
                        "计时可能挂死驱动；需实测请加 --time-mixed）\n",
                        ehe::render::backend_name(backend), mode.label);
            continue;
        }
        const Timings timings = time_frames(*renderer, config, options.probe_frames);
        std::printf("[caps] %-7s %-12s 管线就绪  %d×%d 帧耗时 mean=%.1f ms  min=%.1f  max=%.1f%s\n",
                    ehe::render::backend_name(backend), mode.label, options.probe_size,
                    options.probe_size, timings.mean_ms, timings.min_ms, timings.max_ms,
                    mode.fp32_only ? "" : "  ← fp64 计时（可能极慢）");
    }

    renderer->shutdown();
}

}  // namespace

int run_caps(const CapOptions& options) {
    std::vector<std::string> warnings;
    const ehe::core::Config config =
        ehe::core::Config::load_from_file(options.config_path, &warnings);
    for (const std::string& warning : warnings) {
        std::fprintf(stderr, "[caps][config] %s\n", warning.c_str());
    }

    std::printf("EHE 能力探测（--caps）\n");
    std::printf("参数文件：%s（渲染 %d×%d，每模式 %d 帧，每帧 finish 同步）\n", options.config_path.c_str(),
                options.probe_size, options.probe_size, options.probe_frames);

    for (const auto backend : {ehe::core::Backend::OpenGL, ehe::core::Backend::Vulkan}) {
        probe_backend(config, backend, options);
    }

    std::printf("\n[caps] 判读提示：\n");
    std::printf("[caps]   - mixed(fp64) 能编译但帧耗时远大于 fp32 → 该机 fp64 为软件模拟，"
                "smoke/交互都建议 \"precision\": \"fp32\"\n");
    std::printf("[caps]   - Vulkan 报 shaderFloat64=0（如 Intel 核显）→ Vulkan 侧无法走 fp64，"
                "T1.6 起默认走 fp32\n");
    std::printf("[caps]   - 两个后端都失败 → 先装/更新显卡驱动（Vulkan 需系统存在 vulkan-1.dll）\n");
    return 0;
}

}  // namespace ehe::app
