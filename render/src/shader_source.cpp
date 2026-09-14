#include "ehe/render/shader_source.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace ehe::render {
namespace {

std::string directory_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string{} : path.substr(0, slash);
}

bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

int count_lines(const std::string& text) {
    int lines = 0;
    for (const char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    return lines + (text.empty() ? 0 : 1);
}

/// 解析 `#include "..."`：返回 include 路径；非 include 行返回空
std::string parse_include(const std::string& line) {
    std::size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos || line[pos] != '#') {
        return {};
    }
    pos = line.find_first_not_of(" \t", pos + 1);
    if (pos == std::string::npos || line.compare(pos, 7, "include") != 0) {
        return {};
    }
    pos = line.find_first_not_of(" \t", pos + 7);
    if (pos == std::string::npos || (line[pos] != '"' && line[pos] != '<')) {
        return {};
    }
    const char open = line[pos];
    const char close = (open == '"') ? '"' : '>';
    const std::size_t end = line.find(close, pos + 1);
    if (end == std::string::npos) {
        return {};
    }
    return line.substr(pos + 1, end - pos - 1);
}

}  // namespace

std::string insert_defines_after_version(const std::string& source,
                                         const std::vector<std::string>& defines) {
    if (defines.empty()) {
        return source;
    }
    // 定位真正的 `#version` 行（它前面可能有注释行，不能简单按"第一行"处理）
    std::size_t position = 0;
    std::size_t insert_at = std::string::npos;
    while (position <= source.size()) {
        const std::size_t end = source.find('\n', position);
        const std::string line = source.substr(position, end - position);
        if (line.find("#version") != std::string::npos) {
            insert_at = (end == std::string::npos) ? source.size() : end + 1;
            break;
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    if (insert_at == std::string::npos) {
        return source;
    }
    std::string block;
    for (const std::string& define : defines) {
        block += "#define " + define + " 1\n";
    }
    return source.substr(0, insert_at) + block + source.substr(insert_at);
}

ShaderLoadResult expand_shader(const std::string& name, const std::string& text,
                               const std::vector<std::string>& search_roots) {
    ShaderLoadResult result;

    // 递归展开：active 用于检测循环包含
    struct Expander {
        const std::vector<std::string>& roots;
        ShaderLoadResult& out;
        std::vector<std::string> active;

        bool expand(const std::string& file_name, const std::string& body, std::string& into,
                    const std::string& base_dir) {
            for (const std::string& in_progress : active) {
                if (in_progress == file_name) {
                    out.error = "检测到循环 include：" + file_name;
                    return false;
                }
            }
            active.push_back(file_name);
            out.source.included_files.push_back(file_name);

            std::istringstream lines(body);
            std::string line;
            while (std::getline(lines, line)) {
                const std::string include_path = parse_include(line);
                if (include_path.empty()) {
                    into += line;
                    into += '\n';
                    continue;
                }

                // 解析顺序：当前文件所在目录 → include 搜索根
                std::string resolved;
                std::string content;
                bool found = false;
                std::vector<std::string> candidates;
                if (!base_dir.empty()) {
                    candidates.push_back(base_dir + "/" + include_path);
                }
                for (const std::string& root : roots) {
                    candidates.push_back(root + "/" + include_path);
                }
                for (const std::string& candidate : candidates) {
                    if (read_text_file(candidate, content)) {
                        resolved = candidate;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    out.error = "无法解析 include \"" + include_path + "\"（来自 " + file_name +
                                "，已尝试 " + std::to_string(candidates.size()) + " 个路径）";
                    return false;
                }

                into += "// >>> include begin: " + include_path + "\n";
                if (!expand(resolved, content, into, directory_of(resolved))) {
                    return false;
                }
                into += "// <<< include end: " + include_path + "\n";
            }
            active.pop_back();
            return true;
        }
    };

    Expander expander{search_roots, result, {}};
    std::string output;
    if (!expander.expand(name, text, output, directory_of(name))) {
        result.ok = false;
        return result;
    }

    result.source.text = output;
    result.source.line_count = count_lines(output);
    result.ok = true;
    return result;
}

ShaderLoadResult load_shader(const std::string& path, const std::vector<std::string>& search_roots) {
    std::string content;
    if (!read_text_file(path, content)) {
        ShaderLoadResult result;
        result.error = "无法打开 shader 文件：" + path;
        return result;
    }
    ShaderLoadResult result = expand_shader(path, content, search_roots);
    if (result.ok) {
        std::printf("[shader] %s：展开后 %d 行（含 %zu 个文件）\n", path.c_str(),
                    result.source.line_count, result.source.included_files.size());
    } else {
        std::fprintf(stderr, "[shader] %s 展开失败：%s\n", path.c_str(), result.error.c_str());
    }
    return result;
}

}  // namespace ehe::render
