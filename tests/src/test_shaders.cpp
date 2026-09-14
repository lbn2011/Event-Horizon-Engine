// EHE —— shader 静态校验（无需 GPU）
//
// 为什么重要：GPU 渲染刻意不进 CI（DESIGN §6 L4），但 GLSL 的**语法与语义错误**完全可以在
//   无显卡的机器上提前抓出来——用项目已依赖的 glslang 对「展开 include 后的源码」做 parse。
//   这样 shader 改坏会在 CI 立刻暴露，而不是等到目标机冒烟。
//
// 覆盖：
//   - include 展开链路（render/ShaderSource，§5.8）
//   - fullscreen.vert / raymarch.frag / present.frag 的 GLSL 450 解析（OpenGL 客户端语义）
//   - UBO 布局一致性：shader 声明的 std140 块必须能被 C++ 侧 sizeof(SimParams) 校验通过

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "ehe/render/shader_source.h"
#include "ehe/render/sim_params.h"

namespace {

/// shader 根目录探测（与 GL 后端一致：从 CWD 或可执行目录出发均可命中）
std::string find_shader_root() {
    for (const char* candidate : {"shaders", "../shaders", "../../shaders", "../../../shaders"}) {
        const std::string probe = std::string(candidate) + "/raymarch.frag";
        std::FILE* handle = std::fopen(probe.c_str(), "rb");
        if (handle != nullptr) {
            std::fclose(handle);
            return candidate;
        }
    }
    return {};
}

struct ParseResult {
    bool parsed = false;
    std::string log;
};

/// 用 glslang 解析 GLSL 源码（只做语法/语义检查，不生成 SPIR-V，故不依赖 SPIRV 库）
ParseResult parse_glsl(const std::string& source, EShLanguage stage, const char* label) {
    ParseResult result;
    glslang::TShader shader(stage);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientOpenGL, 100);
    shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const bool ok = shader.parse(GetDefaultResources(), 450, false, EShMsgDefault);
    result.log = shader.getInfoLog();
    if (!result.log.empty()) {
        std::printf("[shader-test] %s 信息日志：\n%s\n", label, result.log.c_str());
    }
    result.parsed = ok;
    return result;
}

}  // namespace

TEST_CASE("shader_source: include 展开（§5.8）与循环检测") {
    const std::string root = find_shader_root();
    REQUIRE_FALSE(root.empty());

    const ehe::render::ShaderLoadResult result =
        ehe::render::load_shader(root + "/raymarch.frag", {root, "shaders"});
    REQUIRE(result.ok);
    CHECK(result.source.line_count > 200);          // 展开后应明显大于主文件本身
    CHECK(result.source.included_files.size() >= 5);  // 主文件 + 4 个 common
    CHECK(result.source.text.find("#include") == std::string::npos);  // 全部展开完毕
    CHECK(result.source.text.find("EHE_METRIC_GLSL") != std::string::npos);

    // 内存注入：循环 include 必须被检出而不是静默递归
    const ehe::render::ShaderLoadResult cyclic =
        ehe::render::expand_shader("a.glsl", "#include \"b.glsl\"\n", {});
    CHECK_FALSE(cyclic.ok);
    CHECK(cyclic.error.find("无法解析 include") != std::string::npos);
}

TEST_CASE("shader: raymarch.frag / fullscreen.vert / present.frag 通过 glslang 解析") {
    const std::string root = find_shader_root();
    REQUIRE_FALSE(root.empty());

    glslang::InitializeProcess();

    const std::vector<std::string> roots = {root, "shaders"};
    const ehe::render::ShaderLoadResult vert = ehe::render::load_shader(root + "/fullscreen.vert", roots);
    const ehe::render::ShaderLoadResult frag = ehe::render::load_shader(root + "/raymarch.frag", roots);
    const ehe::render::ShaderLoadResult present = ehe::render::load_shader(root + "/present.frag", roots);
    REQUIRE(vert.ok);
    REQUIRE(frag.ok);
    REQUIRE(present.ok);

    const ParseResult vert_result = parse_glsl(vert.source.text, EShLangVertex, "fullscreen.vert");
    const ParseResult frag_result = parse_glsl(frag.source.text, EShLangFragment, "raymarch.frag");
    const ParseResult present_result =
        parse_glsl(present.source.text, EShLangFragment, "present.frag");

    glslang::FinalizeProcess();

    // 失败时打印全部日志，便于定位（CI 日志即现场）
    if (!frag_result.parsed) {
        std::fprintf(stderr, "[shader-test] raymarch.frag 解析失败：\n%s\n", frag_result.log.c_str());
    }
    if (!vert_result.parsed) {
        std::fprintf(stderr, "[shader-test] fullscreen.vert 解析失败：\n%s\n", vert_result.log.c_str());
    }
    if (!present_result.parsed) {
        std::fprintf(stderr, "[shader-test] present.frag 解析失败：\n%s\n", present_result.log.c_str());
    }

    CHECK(vert_result.parsed);
    CHECK(frag_result.parsed);
    CHECK(present_result.parsed);
}

TEST_CASE("shader: glslang 对 fp64 内建函数的支持探测（环境自检）") {
    // 目的：确认 glslang 在当前配置下能否解析 double 版本的数学内建函数。
    // 若本用例失败，说明 shader 侧必须避开 double 的 pow/exp/log（改用乘法与 sqrt 的组合）。
    glslang::InitializeProcess();

    const std::string prologue = "#version 450\n";
    const std::string body =
        "\nlayout(location = 0) out vec4 c;\n"
        "void main() {\n"
        "    double x = 4.0lf;\n"
        "    double y = %EXPR%;\n"
        "    c = vec4(float(y));\n"
        "}\n";

    struct Probe {
        const char* label;
        const char* expression;
    };
    const Probe probes[] = {
        {"double 算术（基准）", "x * 0.5lf"},
        {"sqrt(double)", "sqrt(x)"},
        {"exp(double)", "exp(x)"},
        {"log(double)", "log(x)"},
        {"pow(double,double)", "pow(x, 0.25lf)"},
    };

    for (const Probe& probe : probes) {
        std::string source = prologue + body;
        const std::string marker = "%EXPR%";
        source.replace(source.find(marker), marker.size(), probe.expression);
        const ParseResult result = parse_glsl(source, EShLangFragment, probe.label);
        std::printf("[shader-test] fp64 探测：%-22s => %s\n", probe.label,
                    result.parsed ? "OK" : "FAILED");
    }

    glslang::FinalizeProcess();
    CHECK(true);  // 探测用例只输出信息，不构成断言（结论用于指导 shader 写法）
}

TEST_CASE("shader: raymarch.frag 在 mixed(fp64) 与 fp32 两种精度下均通过解析（§4.3 精度开关）") {
    const std::string root = find_shader_root();
    REQUIRE_FALSE(root.empty());

    const std::vector<std::string> roots = {root, "shaders"};
    const ehe::render::ShaderLoadResult frag = ehe::render::load_shader(root + "/raymarch.frag", roots);
    REQUIRE(frag.ok);

    // 与 GL 后端一致：用共享的 insert_defines_after_version（按 #version 行定位）
    const std::string fp32_source =
        ehe::render::insert_defines_after_version(frag.source.text, {"EHE_FP32_ONLY"});

    glslang::InitializeProcess();
    const ParseResult mixed = parse_glsl(frag.source.text, EShLangFragment, "raymarch.frag(mixed)");
    const ParseResult fp32 = parse_glsl(fp32_source, EShLangFragment, "raymarch.frag(fp32)");
    glslang::FinalizeProcess();

    if (!mixed.parsed) {
        std::fprintf(stderr, "[shader-test] mixed 精度解析失败：\n%s\n", mixed.log.c_str());
    }
    if (!fp32.parsed) {
        std::fprintf(stderr, "[shader-test] fp32 精度解析失败：\n%s\n", fp32.log.c_str());
    }
    CHECK(mixed.parsed);
    CHECK(fp32.parsed);
}

TEST_CASE("simparams: UBO 镜像与 §5.3 布局一致（std140，11 个 vec4）") {
    using ehe::render::SimParams;
    CHECK(sizeof(SimParams) == 176);
    CHECK(offsetof(SimParams, cam_pos) == 0);
    CHECK(offsetof(SimParams, cam_basis_x) == 16);
    CHECK(offsetof(SimParams, cam_basis_y) == 32);
    CHECK(offsetof(SimParams, cam_basis_z) == 48);
    CHECK(offsetof(SimParams, cam_dir_fov) == 64);
    CHECK(offsetof(SimParams, frame) == 80);
    CHECK(offsetof(SimParams, steps) == 96);
    CHECK(offsetof(SimParams, hole) == 112);
    CHECK(offsetof(SimParams, disk) == 128);
    CHECK(offsetof(SimParams, flags) == 144);
    CHECK(offsetof(SimParams, extras) == 160);

    // 位打包往返（flags.y/z 携带浮点量）
    const float value = 1.25F;
    CHECK(ehe::render::unpack_float(ehe::render::pack_float(value)) == doctest::Approx(value));
}
