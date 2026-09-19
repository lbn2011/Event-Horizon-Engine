// EHE —— SPIR-V 生成测试（Vulkan 侧 shader 路径，**无需 GPU**）
//
// 为什么这条测试很关键：DESIGN §6 L4 规定"GPU 渲染永不进 CI"，但 **SPIR-V 编译是纯 CPU 行为**——
// 于是 Vulkan 侧最容易出错的一环（GLSL→SPIR-V：绑定布局、std140、精度宏）可以在 CI 里被完全覆盖。
// 这样 VK 后端在目标机上第一次运行前，shader 侧已经被验证过。

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "ehe/render/spirv.h"

namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203U;

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

struct ShaderEntry {
    const char* file;
    bool fragment;
};

const ShaderEntry kShaders[] = {
    {"fullscreen.vert", false},
    {"raymarch.frag", true},
    {"post_resolve.frag", true},
    {"post_fxaa.frag", true},
    {"post_final.frag", true},
};

}  // namespace

TEST_CASE("spirv: 全部 shader 在 mixed(fp64) 与 fp32 两种精度下都能编出合法 SPIR-V") {
    const std::string root = find_shader_root();
    REQUIRE_FALSE(root.empty());
    const std::vector<std::string> roots = {root, "shaders"};

    for (const ShaderEntry& entry : kShaders) {
        for (const bool fp32_only : {false, true}) {
            // VK 路径必须注入 EHE_VULKAN：顶点序号内建名两 API 不同（gl_VertexIndex vs gl_VertexID）
            std::vector<std::string> defines = {"EHE_VULKAN"};
            if (fp32_only) {
                defines.push_back("EHE_FP32_ONLY");
            }
            const ehe::render::SpirvResult result =
                ehe::render::compile_shader_file(root + "/" + entry.file, roots, entry.fragment,
                                                 defines);
            INFO("shader=" << entry.file << " fp32=" << fp32_only);
            if (!result.ok) {
                std::fprintf(stderr, "[spirv-test] %s（fp32=%d）编译失败：\n%s\n", entry.file,
                             fp32_only ? 1 : 0, result.log.c_str());
            }
            CHECK(result.ok);
            REQUIRE_FALSE(result.words.empty());
            CHECK(result.words[0] == kSpirvMagic);
            // 除魔数外至少有版本/生成者/边界等头部字
            CHECK(result.words.size() > 5);
        }
    }
}

TEST_CASE("spirv: 非法源码必须失败且不产生字流（负例）") {
    // 语法错误
    const ehe::render::SpirvResult broken =
        ehe::render::compile_glsl_to_spirv("#version 450\nvoid main() { this is not glsl }\n", true);
    CHECK_FALSE(broken.ok);
    CHECK(broken.words.empty());
    CHECK_FALSE(broken.log.empty());

    // 空源码
    const ehe::render::SpirvResult empty = ehe::render::compile_glsl_to_spirv("", true);
    CHECK_FALSE(empty.ok);
    CHECK(empty.words.empty());
}

TEST_CASE("spirv: 精度宏确实改变了生成结果（mixed 与 fp32 不是同一份 SPIR-V）") {
    const std::string root = find_shader_root();
    REQUIRE_FALSE(root.empty());
    const std::vector<std::string> roots = {root, "shaders"};

    const ehe::render::SpirvResult mixed = ehe::render::compile_shader_file(
        root + "/raymarch.frag", roots, true, {"EHE_VULKAN"});
    const ehe::render::SpirvResult fp32 = ehe::render::compile_shader_file(
        root + "/raymarch.frag", roots, true, {"EHE_VULKAN", "EHE_FP32_ONLY"});
    REQUIRE(mixed.ok);
    REQUIRE(fp32.ok);

    // 字流长度/内容必须不同：fp64 变体会声明 Float64 能力并产生更多指令
    CHECK(mixed.words != fp32.words);
    CHECK(mixed.words.size() > fp32.words.size());
}
