#pragma once

// EHE render —— ShaderSource：文本 include 展开与加载（DESIGN §5.8）
//
// 为什么需要：GLSL 450 没有原生 #include（`GL_ARB_shading_language_include` 不普及），
//   因此由 C++ 侧做**文本级递归展开**后再交给两后端——GL 直接编译、VK 过 glslang。
//
// 约定：
//   - `#include "path"` 行会被替换为被包含文件的全文（保持其余行不动，便于对行号）；
//   - 被包含文件自身的 include guard（`#ifndef EHE_XXX_GLSL`）继续生效，重复包含无害；
//   - 循环包含会被检出并报错（不做静默截断）；
//   - 展开后的总行数写日志（§5.8：展开心智成本为零、调试行号以展开后为准）。

#include <string>
#include <vector>

namespace ehe::render {

struct ShaderSource {
    std::string text;                        ///< 展开后的 GLSL 源码
    std::vector<std::string> included_files; ///< 参与展开的文件（含主文件），用于日志与依赖追踪
    int line_count = 0;                      ///< 展开后的行数
};

struct ShaderLoadResult {
    bool ok = false;
    std::string error;  ///< 失败原因（文件缺失 / 循环包含 / 语法前置检查）
    ShaderSource source;
};

/// 加载 shader 并展开 include。
/// @param path          入口文件路径
/// @param search_roots  include 的额外搜索根（按顺序尝试；入口文件所在目录始终优先）
ShaderLoadResult load_shader(const std::string& path,
                             const std::vector<std::string>& search_roots = {});

/// 在 `#version` 行之后插入 `#define <name> 1`（GLSL 要求 #version 必须最先出现）。
/// 找不到 #version 时保持原样（由编译期报错兜住）。
std::string insert_defines_after_version(const std::string& source,
                                         const std::vector<std::string>& defines);

/// 展开 include（不读盘，供测试注入内存中的源码）
ShaderLoadResult expand_shader(const std::string& name, const std::string& text,
                               const std::vector<std::string>& search_roots);

}  // namespace ehe::render
