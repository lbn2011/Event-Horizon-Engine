#include "ehe/render/spirv.h"

#include <cstdio>
#include <mutex>

// 注意头文件路径：glslang 的 SPIRV 目录与 glslang 目录**同级**（都在 SDK 源码根下），
// 故是 <SPIRV/...> 而不是 <glslang/SPIRV/...>（已踩）。
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "ehe/render/shader_source.h"

namespace ehe::render {
namespace {

/// glslang 的进程级初始化只需一次（多线程安全；本项目目前只在渲染线程调用）
bool ensure_glslang_initialized() {
    static std::once_flag flag;
    static bool initialized = false;
    std::call_once(flag, []() { initialized = glslang::InitializeProcess(); });
    return initialized;
}

/// 把 glslang 的日志拼成可读文本（getInfoLog 非 const，故取非 const 引用）
std::string join_log(glslang::TShader& shader, const char* stage_label) {
    std::string log = std::string("[") + stage_label + "] ";
    if (shader.getInfoLog() != nullptr) {
        log += shader.getInfoLog();
    }
    if (shader.getInfoDebugLog() != nullptr) {
        log += shader.getInfoDebugLog();
    }
    return log;
}

/// 阶段的可读名（日志定位用）
const char* stage_label(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Fragment: return "frag";
        case ShaderStage::Compute: return "comp";
        case ShaderStage::Vertex: break;
    }
    return "vert";
}

EShLanguage to_esl(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Fragment: return EShLangFragment;
        case ShaderStage::Compute: return EShLangCompute;
        case ShaderStage::Vertex: break;
    }
    return EShLangVertex;
}

}  // namespace

SpirvResult compile_glsl_to_spirv_stage(const std::string& source, ShaderStage stage,
                                        const std::vector<std::string>& defines) {
    SpirvResult result;
    if (source.empty()) {
        result.log = "源码为空";
        return result;
    }
    if (!ensure_glslang_initialized()) {
        result.log = "glslang 初始化失败";
        return result;
    }

    // 宏必须插在 `#version` 之后（GLSL 要求 #version 最先出现）
    const std::string text = insert_defines_after_version(source, defines);

    const EShLanguage esl = to_esl(stage);
    glslang::TShader shader(esl);
    const char* strings[1] = {text.c_str()};
    shader.setStrings(strings, 1);
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint("main");
    // Vulkan 语义：显式布局（本项目的 shader 已用 layout(binding=…) 声明）
    shader.setEnvInput(glslang::EShSourceGlsl, esl, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault)) {
        result.log = join_log(shader, stage_label(stage));
        return result;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        result.log = join_log(shader, "link");
        return result;
    }

    glslang::SpvOptions options;
    options.disableOptimizer = true;  // ENABLE_OPT=OFF：不做 SPIR-V 优化（正确性优先）
    options.generateDebugInfo = false;
    glslang::GlslangToSpv(*program.getIntermediate(esl), result.words, &options);

    if (result.words.empty() || result.words[0] != 0x07230203U) {
        result.log = "SPIR-V 生成失败或魔数不符";
        result.words.clear();
        return result;
    }
    result.ok = true;
    result.log = join_log(shader, stage_label(stage));
    return result;
}

SpirvResult compile_shader_file_stage(const std::string& path, const std::vector<std::string>& roots,
                                      ShaderStage stage, const std::vector<std::string>& defines) {
    const ShaderLoadResult loaded = load_shader(path, roots);
    if (!loaded.ok) {
        SpirvResult result;
        result.log = loaded.error;
        return result;
    }
    return compile_glsl_to_spirv_stage(loaded.source.text, stage, defines);
}

SpirvResult compile_glsl_to_spirv(const std::string& source, bool fragment_stage,
                                  const std::vector<std::string>& defines) {
    return compile_glsl_to_spirv_stage(source, fragment_stage ? ShaderStage::Fragment
                                                              : ShaderStage::Vertex,
                                       defines);
}

SpirvResult compile_shader_file(const std::string& path, const std::vector<std::string>& roots,
                                bool fragment_stage, const std::vector<std::string>& defines) {
    return compile_shader_file_stage(path, roots,
                                     fragment_stage ? ShaderStage::Fragment : ShaderStage::Vertex,
                                     defines);
}

}  // namespace ehe::render
