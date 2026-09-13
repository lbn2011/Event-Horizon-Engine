// EHE 主程序（T0.4）
//
// 本阶段目标：GLFW 窗口 + ImGui 空面板 + **运行时后端切换**（GL 4.5 / Vulkan 1.2）。
//
// 切换语义（DESIGN §5.2）：CLIENT_API hint 在窗口创建时固定，因此切换后端必须
//   销毁旧渲染器（连带窗口句柄）→ 用新后端创建窗口 → 重新初始化；
//   进程不退出，窗口几何与后续接入的 Config 参数由 app 层保留并回填。
//
// 尺寸变化处理：由各后端在 begin_frame 内部自行查询帧缓冲尺寸并处理
//   （GL 重设 viewport；Vulkan 重建交换链），app 层不介入，避免依赖“当前上下文”这类 GL 概念。
//
// 命令行（T0.4 可用子集；完整规格见 DESIGN §6.3）：
//   --backend=gl|vk      指定初始后端（默认 gl）
//   --width=N --height=N 初始窗口尺寸（默认 1280x720）
//   --vsync=0|1          垂直同步（默认 1）
//   --frames=N           渲染 N 帧后自动退出（目标机验证用；0 = 不限制）
//   --try-backends       依次探测各后端能否初始化并输出结果（目标机自检 / 开发机诊断）
//
// 环境说明（DESIGN §2.1，2026-09-14 实测更正）：开发机 GL 能力为 OpenGL 4.5 核心，
//   GL 后端可在本机直接运行验证；Vulkan 后端因缺 vulkan-1.dll 仍需目标机验证。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <GLFW/glfw3.h>

#include <imgui.h>

#include "backend_factory.h"
#include "ehe/core/version.h"
#include "ehe/render/IRenderer.h"
#include "ehe/render/backend.h"

namespace {

struct Options {
    ehe::render::Backend backend = ehe::render::Backend::OpenGL;
    int width = 1280;
    int height = 720;
    bool vsync = true;
    int frames = 0;
    bool try_backends = false;
};

int parse_int(const char* text, int fallback) {
    if (text == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text) {
        return fallback;
    }
    return static_cast<int>(value);
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strncmp(arg, "--backend=", 10) == 0) {
            ehe::render::Backend parsed{};
            if (ehe::app::parse_backend(arg + 10, parsed)) {
                options.backend = parsed;
            } else {
                std::fprintf(stderr, "[main] 未知后端 '%s'（可用 gl / vk）\n", arg + 10);
            }
        } else if (std::strncmp(arg, "--width=", 8) == 0) {
            options.width = parse_int(arg + 8, options.width);
        } else if (std::strncmp(arg, "--height=", 9) == 0) {
            options.height = parse_int(arg + 9, options.height);
        } else if (std::strncmp(arg, "--vsync=", 8) == 0) {
            options.vsync = parse_int(arg + 8, 1) != 0;
        } else if (std::strncmp(arg, "--frames=", 9) == 0) {
            options.frames = parse_int(arg + 9, 0);
        } else if (std::strcmp(arg, "--try-backends") == 0) {
            options.try_backends = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf("用法: ehe [--backend=gl|vk] [--width=N] [--height=N] [--vsync=0|1] "
                        "[--frames=N] [--try-backends]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "[main] 忽略未知参数 '%s'\n", arg);
        }
    }
    return options;
}

/// 探测各后端能否初始化（目标机自检；开发机预期全部失败）
int try_backends() {
    int ok_count = 0;
    for (const auto backend : {ehe::render::Backend::OpenGL, ehe::render::Backend::Vulkan}) {
        auto renderer = ehe::app::create_renderer(backend);
        ehe::render::RendererConfig cfg{};
        cfg.backend = backend;
        cfg.width = 320;
        cfg.height = 240;
        cfg.title = "EHE backend probe";
        cfg.vsync = false;

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

/// 面板（T0.4 只放最小内容：后端切换 + 帧率；完整参数面板见 DESIGN §8，T1.7 接入）
void draw_panel(const Options& options, ehe::render::IRenderer& renderer, bool& request_switch,
                ehe::render::Backend& target_backend) {
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 190), ImGuiCond_FirstUseEver);
    ImGui::Begin("EHE 控制面板（T0.4 骨架）");

    ImGui::Text("版本: %s [%s]", ehe::core::version_string(), ehe::core::build_flags());

    int width = 0;
    int height = 0;
    renderer.framebuffer_size(width, height);
    ImGui::Text("帧缓冲: %dx%d", width, height);
    ImGui::Text("帧率: %.1f FPS", static_cast<double>(ImGui::GetIO().Framerate));

    ImGui::Separator();
    const char* items[] = {"OpenGL 4.5", "Vulkan 1.2"};
    int current = renderer.backend() == ehe::render::Backend::Vulkan ? 1 : 0;
    ImGui::TextUnformatted("渲染后端");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    if (ImGui::Combo("##backend", &current, items, 2)) {
        const auto picked =
            current == 1 ? ehe::render::Backend::Vulkan : ehe::render::Backend::OpenGL;
        if (picked != renderer.backend()) {
            request_switch = true;
            target_backend = picked;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("切换将销毁并重建窗口句柄（CLIENT_API 创建时固定），进程不退出");
    }

    ImGui::Separator();
    ImGui::TextDisabled("T1.7 起接入完整参数面板（DESIGN §8）");
    ImGui::TextDisabled("初始后端: %s / vsync=%d", ehe::render::backend_name(options.backend),
                        options.vsync ? 1 : 0);
    ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);

    std::printf("Event Horizon Engine %s [%s]\n", ehe::core::version_string(),
                ehe::core::build_flags());

    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "[main] GLFW 初始化失败\n");
        return 1;
    }
    std::printf("[main] GLFW: %s\n", glfwGetVersionString());

    if (options.try_backends) {
        const int result = try_backends();
        glfwTerminate();
        return result;
    }

    ehe::render::Backend current_backend = options.backend;
    ehe::render::RendererConfig cfg{};
    cfg.backend = current_backend;
    cfg.width = options.width;
    cfg.height = options.height;
    cfg.title = "Event Horizon Engine";
    cfg.vsync = options.vsync;

    auto renderer = ehe::app::create_renderer(current_backend);
    if (renderer == nullptr || !renderer->init(cfg)) {
        std::fprintf(stderr,
                     "[main] 后端 %s 初始化失败。\n"
                     "        请检查显卡驱动；Vulkan 需要系统存在 vulkan-1.dll。\n"
                     "        可用 --try-backends 查看两后端探测结果。\n",
                     ehe::render::backend_name(current_backend));
        glfwTerminate();
        return 1;
    }

    int rendered_frames = 0;
    bool running = true;
    while (running && !renderer->should_close()) {
        glfwPollEvents();
        renderer->begin_frame();

        bool request_switch = false;
        ehe::render::Backend target_backend = current_backend;
        draw_panel(options, *renderer, request_switch, target_backend);

        renderer->end_frame();

        ++rendered_frames;
        if (options.frames > 0 && rendered_frames >= options.frames) {
            running = false;
        }

        if (request_switch) {
            std::printf("[main] 切换后端：%s -> %s（销毁并重建窗口句柄）\n",
                        ehe::render::backend_name(current_backend),
                        ehe::render::backend_name(target_backend));

            int last_width = 0;
            int last_height = 0;
            renderer->framebuffer_size(last_width, last_height);

            renderer->shutdown();  // 连同窗口句柄一起销毁
            renderer.reset();

            current_backend = target_backend;
            cfg.backend = current_backend;
            if (last_width > 0 && last_height > 0) {
                cfg.width = last_width;
                cfg.height = last_height;
            }

            renderer = ehe::app::create_renderer(current_backend);
            if (renderer == nullptr || !renderer->init(cfg)) {
                std::fprintf(stderr, "[main] 切换后端失败，程序退出\n");
                glfwTerminate();
                return 1;
            }
            rendered_frames = 0;
        }
    }

    renderer->shutdown();
    renderer.reset();
    glfwTerminate();
    std::printf("[main] 正常退出（本会话渲染 %d 帧）\n", rendered_frames);
    return 0;
}
