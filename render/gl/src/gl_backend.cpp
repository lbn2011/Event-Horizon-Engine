// EHE —— OpenGL 4.5 后端实现（T0.4）
//
// 职责：创建 GLFW 窗口 + GL 4.5 core context、加载 glad、初始化 ImGui（GLFW + OpenGL3 后端）、
//       每帧清屏并绘制 ImGui 面板。
//
// 规格依据：DESIGN §5.2（生命周期与切换）、§5.4.1（GL 侧资源约定，FP16 FBO 等在 T1.3 接入）
//
// 环境说明（DESIGN §2.1，2026-09-14 实测更正）：开发机 GL 能力为 **OpenGL 4.5 核心**，
// 本后端**可在开发机直接运行验证**；Vulkan 后端因缺少 vulkan-1.dll 仍需目标机验证。

#include "ehe/render/gl/gl_backend.h"

#include <cstdio>

#include <glad/glad.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "ehe/render/backend.h"

namespace ehe::render::gl {
namespace {

class GLBackend final : public IRenderer {
public:
    Backend backend() const override { return Backend::OpenGL; }

    bool init(const RendererConfig& cfg) override {
        // 目标机底线 GL 4.5（DESIGN §2.1）；开发机 4.1 会在此失败，属预期行为
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        window_ = glfwCreateWindow(cfg.width, cfg.height, cfg.title, nullptr, nullptr);
        if (window_ == nullptr) {
            std::fprintf(stderr, "[gl] 创建窗口/GL 4.5 上下文失败（驱动不支持 4.5 核心profile？）\n");
            return false;
        }
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(cfg.vsync ? 1 : 0);

        // glad2 生成器提供 gladLoadGLLoader(GLADloadproc)；用 GLFW 的取址函数显式装载，
        // 避免依赖 glad 内置的平台 loader（wgl），与「窗口由 GLFW 管理」保持一致
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            std::fprintf(stderr, "[gl] glad 加载 GL 函数失败\n");
            destroy_window();
            return false;
        }

        IMGUI_CHECKVERSION();
        if (ImGui::GetCurrentContext() == nullptr) {
            ImGui::CreateContext();
        }
        ImGui::StyleColorsDark();
        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
            std::fprintf(stderr, "[gl] ImGui GLFW 后端初始化失败\n");
            destroy_window();
            return false;
        }
        if (!ImGui_ImplOpenGL3_Init("#version 450")) {
            std::fprintf(stderr, "[gl] ImGui OpenGL3 后端初始化失败\n");
            ImGui_ImplGlfw_Shutdown();
            destroy_window();
            return false;
        }

        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("[gl] 初始化完成：GL_VERSION=%s GL_RENDERER=%s\n",
                    version != nullptr ? version : "?",
                    renderer != nullptr ? renderer : "?");
        return true;
    }

    void shutdown() override {
        if (window_ != nullptr) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            if (ImGui::GetCurrentContext() != nullptr) {
                ImGui::DestroyContext();
            }
        }
        destroy_window();
    }

    void resize(int width, int height) override {
        width_ = width;
        height_ = height;
    }

    void begin_frame() override {
        if (window_ == nullptr) {
            return;
        }
        // 尺寸由后端自行查询（避免 app 层依赖“当前 GL 上下文”这类概念）
        glfwGetFramebufferSize(window_, &width_, &height_);
        if (!valid()) {
            return;  // 最小化：跳过本帧
        }
        glViewport(0, 0, width_, height_);
        glClearColor(0.02F, 0.02F, 0.03F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void end_frame() override {
        if (!valid()) {
            return;
        }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }

    bool should_close() const override { return valid() && (glfwWindowShouldClose(window_) != 0); }

    void framebuffer_size(int& width, int& height) const override {
        width = width_;
        height = height_;
    }

private:
    bool valid() const { return window_ != nullptr && width_ > 0 && height_ > 0; }

    void destroy_window() {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
    }

    GLFWwindow* window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace

std::unique_ptr<IRenderer> create_renderer() { return std::make_unique<GLBackend>(); }

}  // namespace ehe::render::gl
