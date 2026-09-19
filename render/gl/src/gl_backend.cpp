// EHE —— OpenGL 4.5 后端实现（T0.4 空窗口 → T1.3 raymarch 管线）
//
// 职责：
//   1. GLFW 窗口 + GL 4.5 core context + glad 加载 + ImGui（glfw/opengl3）
//   2. raymarch 管线（DESIGN §5.4.1 的 GL 列）：
//      FP16 FBO（GL_RGBA16F，内部分辨率）→ UBO(binding 0) + 黑体 LUT(unit 1)
//      → 全屏三角形（gl_VertexID，无 VBO）→ 呈现 pass（T1.5 前为临时色调映射）
//   3. HDR 回读（smoke 模式 PFM 输出，§6.3）
//
// 环境说明（DESIGN §2.1）：开发机 GL 4.5 可运行本后端，用于开发期观察；
//   **验收仍在目标机执行**（V5.3 策略）。

#include "ehe/render/gl/gl_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glad/glad.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "ehe/core/blackbody.h"
#include "ehe/render/backend.h"
#include "ehe/render/shader_source.h"
#include "ehe/render/sim_params.h"

namespace ehe::render::gl {
namespace {

/// 在 `#version` 之后插入 `#define`（GLSL 要求 #version 必须是第一行非注释内容）
std::string inject_defines(const std::string& source, const std::vector<std::string>& defines) {
    if (defines.empty()) {
        return source;
    }
    const std::size_t newline = source.find('\n');
    std::string prefix = (newline == std::string::npos) ? source + "\n" : source.substr(0, newline + 1);
    std::string rest = (newline == std::string::npos) ? std::string{} : source.substr(newline + 1);
    for (const std::string& define : defines) {
        prefix += "#define " + define + " 1\n";
    }
    return prefix + rest;
}

/// shader 根目录探测顺序（从 CWD 或可执行文件所在目录出发均可命中）
const char* const kShaderRootCandidates[] = {"shaders", "../shaders", "../../shaders",
                                            "../../../shaders"};

class GLBackend final : public IRenderer {
public:
    Backend backend() const override { return Backend::OpenGL; }

    // ---------------------------------------------------------------- 生命周期

    bool init(const RendererConfig& cfg) override {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, cfg.visible ? GLFW_TRUE : GLFW_FALSE);

        window_ = glfwCreateWindow(cfg.width, cfg.height, cfg.title, nullptr, nullptr);
        if (window_ == nullptr) {
            std::fprintf(stderr, "[gl] 创建窗口/GL 4.5 上下文失败（驱动不支持 4.5 核心 profile？）\n");
            return false;
        }
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(cfg.vsync ? 1 : 0);

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
        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true) || !ImGui_ImplOpenGL3_Init("#version 450")) {
            std::fprintf(stderr, "[gl] ImGui 后端初始化失败\n");
            destroy_window();
            return false;
        }

        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::printf("[gl] 上下文就绪：GL_VERSION=%s GL_RENDERER=%s\n",
                    version != nullptr ? version : "?", renderer != nullptr ? renderer : "?");

        shader_defines_ = cfg.shader_defines;
        render_width_ = cfg.render_width;
        render_height_ = cfg.render_height;
        if (!cfg.shader_root.empty()) {
            shader_root_ = cfg.shader_root;  // 必须在 build_pipeline 之前（见 IRenderer.h 说明）
        }
        build_pipeline();  // 失败不致命：窗口与面板仍可用，错误经 last_error 暴露给 UI
        return true;
    }

    void shutdown() override {
        if (window_ != nullptr) {
            destroy_pipeline();
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
        glfwGetFramebufferSize(window_, &width_, &height_);
        if (!valid()) {
            return;  // 最小化：跳过本帧
        }
        ensure_targets();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void end_frame() override {
        if (!valid()) {
            return;
        }

        if (pipeline_ready_) {
            draw_frame();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width_, height_);
            glClearColor(0.05F, 0.05F, 0.07F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
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

    // ---------------------------------------------------------------- 管线接口

    void set_params(const SimParams& params) override { params_ = params; }

    void set_shader_root(const std::string& root) override { shader_root_ = root; }

    bool pipeline_ready() const override { return pipeline_ready_; }

    const std::string& last_error() const override { return last_error_; }

    bool capture_ldr(std::vector<unsigned char>& rgb, int& width, int& height) override {
        if (!pipeline_ready_ || width_ <= 0 || height_ <= 0) {
            return false;
        }
        // 重跑完整链（raymarch → resolve → fxaa → final）后回读**最终 LDR 目标**（输出分辨率）。
        // 为什么不读默认帧缓冲：窗口尺寸可能被系统放大（离屏冒烟实测 32x32 → 120x32），
        // 那样抓到的成品尺寸与渲染尺寸不一致，无法与 CPU 参考逐像素对照。
        draw_frame();

        width = output_width_;
        height = output_height_;
        rgb.assign(static_cast<std::size_t>(3) * width * height, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, final_fbo_);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

        // glReadPixels 自下而上；core 约定第 0 行 = 顶部 → 逐行翻转
        const std::size_t row_bytes = static_cast<std::size_t>(3) * width;
        std::vector<unsigned char> flipped(rgb.size());
        for (int y = 0; y < height; ++y) {
            const unsigned char* source =
                rgb.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes;
            std::copy(source, source + row_bytes,
                      flipped.data() + static_cast<std::size_t>(y) * row_bytes);
        }
        rgb.swap(flipped);
        return true;
    }

    void finish() override {
        if (window_ != nullptr) {
            glFinish();  // 仅探测/冒烟路径调用（DESIGN §5.4.1）
        }
    }

    bool rebuild_pipeline(const std::vector<std::string>& shader_defines) override {
        if (window_ == nullptr) {
            last_error_ = "上下文未创建，无法重建管线";
            return false;
        }
        shader_defines_ = shader_defines;
        destroy_pipeline();
        build_pipeline();
        return pipeline_ready_;
    }

    std::string capability_report() const override {
        if (window_ == nullptr) {
            return "[GL] 上下文未创建";
        }
        auto get_string = [](GLenum name) -> const char* {
            const auto* text = reinterpret_cast<const char*>(glGetString(name));
            return (text != nullptr) ? text : "?";
        };

        std::string report;
        char line[512] = {};
        std::snprintf(line, sizeof(line), "[GL] version=%s\n[GL] renderer=%s\n[GL] vendor=%s\n[GL] glsl=%s\n",
                      get_string(GL_VERSION), get_string(GL_RENDERER), get_string(GL_VENDOR),
                      get_string(GL_SHADING_LANGUAGE_VERSION));
        report += line;

        // 关键扩展（core profile 必须用 glGetStringi 枚举）
        const char* wanted[] = {"GL_ARB_gpu_shader_fp64", "GL_ARB_gpu_shader_int64",
                                "GL_ARB_shader_storage_buffer_object", "GL_ARB_compute_shader",
                                "GL_ARB_direct_state_access", "GL_ARB_shader_image_load_store",
                                "GL_ARB_texture_filter_anisotropic", "GL_EXT_texture_filter_anisotropic"};
        GLint extension_count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
        for (const char* name : wanted) {
            bool found = false;
            for (GLint i = 0; i < extension_count && !found; ++i) {
                const auto* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
                found = (extension != nullptr) && (std::strcmp(extension, name) == 0);
            }
            std::snprintf(line, sizeof(line), "[GL] ext %-42s %s\n", name, found ? "YES" : "no");
            report += line;
        }

        GLint max_texture = 0;
        GLint max_ubo = 0;
        GLint max_compute = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &max_ubo);
        glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &max_compute);
        std::snprintf(line, sizeof(line),
                      "[GL] limits max_texture=%d max_uniform_block=%d max_compute_invocations=%d\n",
                      max_texture, max_ubo, max_compute);
        report += line;

        // 功能性探测：FP16 颜色附件是否真的可用（我们的 HDR 目标正是 RGBA16F）
        GLuint texture = 0;
        GLuint framebuffer = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 16, 16, 0, GL_RGBA, GL_FLOAT, nullptr);
        glGenFramebuffers(1, &framebuffer);
        GLint previous_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        std::snprintf(line, sizeof(line), "[GL] rgba16f_color_attachment=%s (status=0x%04X)\n",
                      (status == GL_FRAMEBUFFER_COMPLETE) ? "COMPLETE" : "INCOMPLETE", status);
        report += line;

        std::snprintf(line, sizeof(line), "[GL] pipeline_ready=%d\n", pipeline_ready_ ? 1 : 0);
        report += line;
        if (!pipeline_ready_ && !last_error_.empty()) {
            report += "[GL] pipeline_error: " + last_error_ + "\n";
        }
        return report;
    }

    bool capture_hdr(std::vector<float>& rgb, int& width, int& height) override {
        if (!pipeline_ready_ || fbo_ == 0 || internal_width_ <= 0 || internal_height_ <= 0) {
            return false;
        }
        width = output_width_;
        height = output_height_;
        rgb.assign(static_cast<std::size_t>(3) * width * height, 0.0F);

        // 以当前参数重绘：raymarch + 分辨率变换 → **输出分辨率的 HDR**（与 draw_frame 同一路径）。
        // 语义：PFM 差分基准是"分辨率变换之后、色调映射/FXAA 之前"的 HDR（§6.1「无后处理」），
        // 这样 res_scale != 1.0 时也能与 golden 直接可比（res_scale==1 时与内部缓冲逐位相同）。
        const GLuint source_texture = render_hdr_output();
        const bool resolved = (source_texture == post_texture_[0]);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolved ? post_fbo_[0] : fbo_);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_FLOAT, rgb.data());
        const GLenum error = glGetError();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (error != GL_NO_ERROR) {
            std::fprintf(stderr, "[gl] glReadPixels 失败（GL error 0x%04X）\n", error);
            return false;
        }
        // glReadPixels 自下而上返回；core 的图像约定是"第 0 行 = 顶部"，故此处逐行翻转
        std::vector<float> flipped(rgb.size());
        const std::size_t row_floats = static_cast<std::size_t>(3) * width;
        for (int y = 0; y < height; ++y) {
            const float* source = rgb.data() + static_cast<std::size_t>(height - 1 - y) * row_floats;
            std::copy(source, source + row_floats, flipped.data() + static_cast<std::size_t>(y) * row_floats);
        }
        rgb.swap(flipped);
        return true;
    }

private:
    bool valid() const { return window_ != nullptr && width_ > 0 && height_ > 0; }

    void destroy_window() {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
    }

    // ---------------------------------------------------------------- 管线构建

    std::string resolve_shader_root() {
        if (!shader_root_.empty()) {
            return shader_root_;
        }
        for (const char* candidate : kShaderRootCandidates) {
            std::ifstream probe(std::string(candidate) + "/raymarch.frag");
            if (probe) {
                std::printf("[gl] shader 根目录：%s\n", candidate);
                return candidate;
            }
        }
        std::fprintf(stderr, "[gl] 未找到 shaders 目录，回退到 'shaders'\n");
        return "shaders";
    }

    bool compile_program(const char* vert_rel, const char* frag_rel, GLuint& program,
                         const std::string& root) {
        const std::vector<std::string> roots = {root, "shaders", "../shaders", "../../shaders"};
        ShaderLoadResult vert = load_shader(root + "/" + vert_rel, roots);
        ShaderLoadResult frag = load_shader(root + "/" + frag_rel, roots);
        if (!vert.ok || !frag.ok) {
            last_error_ = vert.ok ? frag.error : vert.error;
            return false;
        }

        std::string define_log = "（无，默认 mixed/fp64）";
        if (!shader_defines_.empty()) {
            define_log.clear();
            for (const std::string& define : shader_defines_) {
                define_log += define;
                define_log += " ";
            }
        }
        std::printf("[gl] 编译宏：%s\n", define_log.c_str());

        if (vert.ok) {
            vert.source.text = insert_defines_after_version(vert.source.text, shader_defines_);
        }
        if (frag.ok) {
            frag.source.text = insert_defines_after_version(frag.source.text, shader_defines_);
        }

        auto compile_stage = [&](GLenum stage, const std::string& source, const char* label) -> GLuint {
            const GLuint shader = glCreateShader(stage);
            const char* text = source.c_str();
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::string log(static_cast<std::size_t>(std::max(1, length)), '\0');
                glGetShaderInfoLog(shader, length, nullptr, log.data());
                last_error_ = std::string(label) + " 编译失败：\n" + log;
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        };

        const GLuint vs = compile_stage(GL_VERTEX_SHADER, vert.source.text, vert_rel);
        const GLuint fs = vs != 0 ? compile_stage(GL_FRAGMENT_SHADER, frag.source.text, frag_rel) : 0;
        if (vs == 0 || fs == 0) {
            if (vs != 0) {
                glDeleteShader(vs);
            }
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(1, length)), '\0');
            glGetProgramInfoLog(program, length, nullptr, log.data());
            last_error_ = std::string("链接失败（") + frag_rel + "）：\n" + log;
            glDeleteProgram(program);
            program = 0;
            return false;
        }
        std::printf("[gl] 程序就绪：%s + %s（展开后 %d + %d 行）\n", vert_rel, frag_rel,
                    vert.source.line_count, frag.source.line_count);
        return true;
    }

    bool load_lut_texture(const std::string& root) {
        std::vector<float> lut;
        std::size_t count = 0;
        const std::string path = root + "/blackbody_lut.f32";
        if (!core::read_blackbody_lut(path, lut, count)) {
            last_error_ = "无法读取黑体 LUT：" + path + "（请先运行 ehe_reftool lut）";
            return false;
        }

        if (lut_texture_ == 0) {
            glGenTextures(1, &lut_texture_);
        }
        glBindTexture(GL_TEXTURE_2D, lut_texture_);
        // LUT 为 256×1 线性 RGB；线性采样须与 core 的插值方式一致
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, static_cast<GLsizei>(count), 1, 0, GL_RGB,
                     GL_FLOAT, lut.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::printf("[gl] 黑体 LUT 已上传：%zu 点\n", count);
        return true;
    }

    void build_pipeline() {
        pipeline_ready_ = false;
        last_error_.clear();

        const std::string root = resolve_shader_root();
        // 四个 pass（§4.6 链序）：raymarch → resolve（分辨率变换）→ fxaa → final（ACES+曝光+色差）
        if (!compile_program("fullscreen.vert", "raymarch.frag", raymarch_program_, root) ||
            !compile_program("fullscreen.vert", "post_resolve.frag", resolve_program_, root) ||
            !compile_program("fullscreen.vert", "post_fxaa.frag", fxaa_program_, root) ||
            !compile_program("fullscreen.vert", "post_final.frag", final_program_, root)) {
            std::fprintf(stderr, "[gl] 管线构建失败：%s\n", last_error_.c_str());
            return;
        }
        if (!load_lut_texture(root)) {
            std::fprintf(stderr, "[gl] %s\n", last_error_.c_str());
            return;
        }

        // 全屏三角形：无顶点缓冲，仅需一个空 VAO（core profile 要求绑定 VAO）
        glGenVertexArrays(1, &vao_);

        glGenBuffers(1, &ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(SimParams)), nullptr,
                     GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);  // §5.4.1：GL 用 binding 0
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        // resolve 与 final 各自的小 UBO（binding 3 / 4，见对应 shader 的声明）
        glGenBuffers(1, &post_ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, post_ubo_);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(PostParams)), nullptr,
                     GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, post_ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        glGenBuffers(1, &final_ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, final_ubo_);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(sizeof(FinalParams)), nullptr,
                     GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, final_ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        pipeline_ready_ = true;
        std::printf("[gl] 后处理管线就绪：raymarch → resolve → fxaa → final（UBO %zu 字节）\n",
                    sizeof(SimParams));
    }

    void destroy_pipeline() {
        if (fbo_ != 0) {
            glDeleteFramebuffers(1, &fbo_);
            fbo_ = 0;
        }
        if (color_texture_ != 0) {
            glDeleteTextures(1, &color_texture_);
            color_texture_ = 0;
        }
        // 后处理中间目标（输出分辨率 ×2）
        for (int i = 0; i < 2; ++i) {
            if (post_fbo_[i] != 0) {
                glDeleteFramebuffers(1, &post_fbo_[i]);
                post_fbo_[i] = 0;
            }
            if (post_texture_[i] != 0) {
                glDeleteTextures(1, &post_texture_[i]);
                post_texture_[i] = 0;
            }
        }
        if (final_fbo_ != 0) {
            glDeleteFramebuffers(1, &final_fbo_);
            final_fbo_ = 0;
        }
        if (final_texture_ != 0) {
            glDeleteTextures(1, &final_texture_);
            final_texture_ = 0;
        }
        if (lut_texture_ != 0) {
            glDeleteTextures(1, &lut_texture_);
            lut_texture_ = 0;
        }
        if (ubo_ != 0) {
            glDeleteBuffers(1, &ubo_);
            ubo_ = 0;
        }
        if (post_ubo_ != 0) {
            glDeleteBuffers(1, &post_ubo_);
            post_ubo_ = 0;
        }
        if (final_ubo_ != 0) {
            glDeleteBuffers(1, &final_ubo_);
            final_ubo_ = 0;
        }
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
            vao_ = 0;
        }
        if (raymarch_program_ != 0) {
            glDeleteProgram(raymarch_program_);
            raymarch_program_ = 0;
        }
        if (resolve_program_ != 0) {
            glDeleteProgram(resolve_program_);
            resolve_program_ = 0;
        }
        if (fxaa_program_ != 0) {
            glDeleteProgram(fxaa_program_);
            fxaa_program_ = 0;
        }
        if (final_program_ != 0) {
            glDeleteProgram(final_program_);
            final_program_ = 0;
        }
        internal_width_ = 0;
        internal_height_ = 0;
        output_width_ = 0;
        output_height_ = 0;
        pipeline_ready_ = false;
    }

    /// 按 res_scale 计算内部分辨率与输出分辨率，并（必要时）重建 FP16 目标
    void ensure_targets() {
        if (!pipeline_ready_) {
            return;
        }
        const float res_scale = unpack_float(params_.flags[1]);
        const double scale = (res_scale > 0.0F) ? static_cast<double>(res_scale) : 1.0;
        // 显式渲染尺寸优先（离屏 smoke 用）；否则跟随窗口帧缓冲
        const int base_w = (render_width_ > 0) ? render_width_ : width_;
        const int base_h = (render_height_ > 0) ? render_height_ : height_;
        const int want_w = std::max(1, static_cast<int>(std::lround(base_w * scale)));
        const int want_h = std::max(1, static_cast<int>(std::lround(base_h * scale)));
        if (want_w == internal_width_ && want_h == internal_height_ && base_w == output_width_ &&
            base_h == output_height_ && fbo_ != 0 && post_fbo_[0] != 0 && final_fbo_ != 0) {
            return;
        }

        auto create_target = [](GLuint& fbo, GLuint& texture, int w, int h) {
            if (fbo == 0) {
                glGenFramebuffers(1, &fbo);
            }
            if (texture == 0) {
                glGenTextures(1, &texture);
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            // §5.4.1：GL 侧 HDR 颜色 = GL_RGBA16F
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return status;
        };

        const GLenum status = create_target(fbo_, color_texture_, want_w, want_h);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            last_error_ = "FP16 FBO 不完整（status=0x" + std::to_string(status) + "）";
            std::fprintf(stderr, "[gl] %s\n", last_error_.c_str());
            pipeline_ready_ = false;
            return;
        }
        // 后处理中间目标固定在**输出分辨率**（SSAA 降采样/升频的结果尺寸）
        for (int i = 0; i < 2; ++i) {
            const GLenum post_status =
                create_target(post_fbo_[i], post_texture_[i], base_w, base_h);
            if (post_status != GL_FRAMEBUFFER_COMPLETE) {
                last_error_ = "后处理 FBO 不完整（status=0x" + std::to_string(post_status) + "）";
                std::fprintf(stderr, "[gl] %s\n", last_error_.c_str());
                pipeline_ready_ = false;
                return;
            }
        }

        // 最终 LDR 目标（RGBA8）：后处理链写这里，再 blit 到默认帧缓冲显示。
        // 这样"后处理输出的尺寸 = 输出分辨率"与窗口尺寸解耦——
        // 离屏冒烟时窗口会被系统拉大（实测 32×32 → 120×32），若直接写默认帧缓冲就抓不到正确尺寸的成品。
        {
            if (final_fbo_ == 0) {
                glGenFramebuffers(1, &final_fbo_);
            }
            if (final_texture_ == 0) {
                glGenTextures(1, &final_texture_);
            }
            glBindTexture(GL_TEXTURE_2D, final_texture_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, base_w, base_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, final_fbo_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   final_texture_, 0);
            const GLenum final_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (final_status != GL_FRAMEBUFFER_COMPLETE) {
                last_error_ = "最终 LDR FBO 不完整（status=0x" + std::to_string(final_status) + "）";
                std::fprintf(stderr, "[gl] %s\n", last_error_.c_str());
                pipeline_ready_ = false;
                return;
            }
        }

        internal_width_ = want_w;
        internal_height_ = want_h;
        output_width_ = base_w;
        output_height_ = base_h;
    }

    /// raymarch pass → FP16 FBO（HDR 原始输出，不做色调映射）
    void draw_raymarch_to_fbo() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, internal_width_, internal_height_);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(SimParams)), &params_);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        glUseProgram(raymarch_program_);

        glActiveTexture(GL_TEXTURE1);  // §5.4.1：LUT 固定 texture unit 1
        glBindTexture(GL_TEXTURE_2D, lut_texture_);

        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    /// raymarch + （必要时）分辨率变换 → **输出分辨率的 HDR**。
    /// capture_hdr（差分基准）与 draw_frame（继续 FXAA/final）共用此路径，保证两者看到同一张图。
    /// @return 源纹理（res_scale==1 时为 color_texture_ 本身，否则为 post_texture_[0]）
    GLuint render_hdr_output() {
        draw_raymarch_to_fbo();

        const float res_scale = unpack_float(params_.flags[1]);
        const bool ssaa = res_scale > 1.001F;     // >1.0 → SSAA 降采样
        const bool upscale = res_scale < 0.999F;  // <1.0 → 升频（FSR1 落地前走 Catmull-Rom）
        if (!ssaa && !upscale) {
            return color_texture_;  // 原生分辨率：省一次全屏读写
        }

        PostParams post{};
        post.mode_and_src[0] = ssaa ? 0 : 1;  // MODE_BOX / MODE_UP
        post.mode_and_src[1] = internal_width_;
        post.mode_and_src[2] = internal_height_;
        post.mode_and_src[3] = 1;
        post.dst_size[0] = output_width_;
        post.dst_size[1] = output_height_;

        glBindFramebuffer(GL_FRAMEBUFFER, post_fbo_[0]);
        glViewport(0, 0, output_width_, output_height_);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBindBuffer(GL_UNIFORM_BUFFER, post_ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(PostParams)), &post);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glUseProgram(resolve_program_);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, color_texture_);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);
        return post_texture_[0];
    }

    /// 完整一帧（§4.6 链序）：raymarch → 分辨率变换 → FXAA → final（ACES+曝光+色差）→ 呈现
    void draw_frame() {
        ensure_targets();
        if (!pipeline_ready_) {
            return;
        }
        GLuint source_texture = render_hdr_output();

        const bool fxaa = (params_.flags[0] & kFlagFxaa) != 0U;
        const bool aces = (params_.flags[0] & kFlagAces) != 0U;

        // ---- pass 3：FXAA（在色调映射之前，§4.6；输入仍是 HDR 线性）----
        if (fxaa) {
            // 目标必须与源不同：源可能是 post_texture_[0]（做过分辨率变换）或 color_texture_
            const GLuint fxaa_target_fbo =
                (source_texture == post_texture_[0]) ? post_fbo_[1] : post_fbo_[0];
            const GLuint fxaa_target_texture =
                (source_texture == post_texture_[0]) ? post_texture_[1] : post_texture_[0];

            glBindFramebuffer(GL_FRAMEBUFFER, fxaa_target_fbo);
            glViewport(0, 0, output_width_, output_height_);
            glUseProgram(fxaa_program_);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, source_texture);
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glUseProgram(0);

            source_texture = fxaa_target_texture;
        }

        // ---- pass 4：final（曝光 → ACES → 色差 → sRGB）→ 输出分辨率的 LDR 目标 ----
        FinalParams final_params{};
        final_params.exposure_and_chroma[0] = params_.disk[2];  // exposure
        final_params.exposure_and_chroma[1] = params_.disk[3];  // chroma_ab
        final_params.flags[0] = aces ? 1.0F : 0.0F;

        glBindFramebuffer(GL_FRAMEBUFFER, final_fbo_);
        glViewport(0, 0, output_width_, output_height_);
        glBindBuffer(GL_UNIFORM_BUFFER, final_ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(FinalParams)),
                        &final_params);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glUseProgram(final_program_);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, source_texture);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);

        // ---- 呈现：把 LDR 目标 blit 到默认帧缓冲（窗口尺寸可能不同于输出尺寸，用线性缩放）----
        glBindFramebuffer(GL_READ_FRAMEBUFFER, final_fbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, output_width_, output_height_, 0, 0, width_, height_,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ---------------------------------------------------------------- 成员

    GLFWwindow* window_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    GLuint raymarch_program_ = 0;
    GLuint resolve_program_ = 0;
    GLuint fxaa_program_ = 0;
    GLuint final_program_ = 0;
    GLuint vao_ = 0;
    GLuint ubo_ = 0;
    GLuint post_ubo_ = 0;
    GLuint final_ubo_ = 0;
    GLuint fbo_ = 0;
    GLuint color_texture_ = 0;
    GLuint post_fbo_[2] = {0, 0};
    GLuint post_texture_[2] = {0, 0};
    GLuint final_fbo_ = 0;
    GLuint final_texture_ = 0;
    GLuint lut_texture_ = 0;
    int internal_width_ = 0;
    int internal_height_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;

    SimParams params_{};
    std::string shader_root_;
    std::vector<std::string> shader_defines_;
    int render_width_ = 0;
    int render_height_ = 0;
    std::string last_error_;
    bool pipeline_ready_ = false;
};

}  // namespace

std::unique_ptr<IRenderer> create_renderer() { return std::make_unique<GLBackend>(); }

}  // namespace ehe::render::gl
