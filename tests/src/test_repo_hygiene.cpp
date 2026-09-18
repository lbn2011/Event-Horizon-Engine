// EHE —— 仓库卫生（可自动检查的约定）
//
// 为什么要有这个文件：有些"约定"靠文档提醒不住，必须让 CI 拦住。这里放的都是**踩过坑**的规则。
//   目前两条：
//     1. PowerShell 脚本必须带 UTF-8 BOM（否则 Windows PowerShell 5.1 按 GBK 解码 → 中文错乱 → 解析失败）
//     2. 目标机便携包依赖的文件必须存在（脚本/指南），否则包出来是残的
//
// 关联：DESIGN §6 L4（CI 编译检查）、docs/TARGET_MACHINE_TESTS.md

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

/// 仓库根目录探测（与 shader 测试同一套：从 CWD 或可执行目录出发均可命中）
std::string find_repo_root() {
    for (const char* candidate : {".", "..", "../..", "../../.."}) {
        const std::string probe = std::string(candidate) + "/scripts/run_target_tests.ps1";
        std::FILE* handle = std::fopen(probe.c_str(), "rb");
        if (handle != nullptr) {
            std::fclose(handle);
            return candidate;
        }
    }
    return {};
}

bool has_utf8_bom(const std::string& path) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        return false;
    }
    unsigned char header[3] = {0, 0, 0};
    const std::size_t read = std::fread(header, 1, 3, handle);
    std::fclose(handle);
    return read == 3 && header[0] == 0xEF && header[1] == 0xBB && header[2] == 0xBF;
}

bool file_exists(const std::string& path) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        return false;
    }
    std::fclose(handle);
    return true;
}

/// 读取整个文件（二进制）
std::string read_file_bytes(const std::string& path) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        return {};
    }
    std::string data;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        data.append(buffer, read);
    }
    std::fclose(handle);
    return data;
}

/// 严格 UTF-8 校验（拒绝非法字节序列 / 截断序列 / 过长编码）
/// 为什么需要：脚本里若混入 GBK 字节（例如被以错误编码改写），BOM 可能仍然正常，
/// 但那个字符串在运行时就是乱码——只有严格校验才能拦住。
bool is_valid_utf8(const std::string& text) {
    std::size_t i = 0;
    const std::size_t size = text.size();
    while (i < size) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        int extra = 0;
        unsigned int codepoint = 0;
        if (byte < 0x80) {
            ++i;
            continue;
        } else if ((byte & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = byte & 0x1FU;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = byte & 0x0FU;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = byte & 0x07U;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(extra) >= size) {
            return false;
        }
        for (int k = 1; k <= extra; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3FU);
        }
        // 过长编码与代理区检查
        const unsigned int minimum = (extra == 1) ? 0x80U : (extra == 2) ? 0x800U : 0x10000U;
        if (codepoint < minimum || codepoint > 0x10FFFFU) {
            return false;
        }
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            return false;
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

}  // namespace

TEST_CASE("repo: PowerShell 脚本必须是「UTF-8 带 BOM 且严格合法 UTF-8」") {
    const std::string root = find_repo_root();
    REQUIRE_FALSE(root.empty());

    const std::vector<std::string> scripts = {
        "/scripts/run_target_tests.ps1",
    };
    for (const std::string& script : scripts) {
        const std::string path = root + script;
        INFO("脚本: " << path);
        CHECK(file_exists(path));
        CHECK(has_utf8_bom(path));  // PS 5.1 对无 BOM 的 .ps1 按 GBK 解码 → 中文错乱 → 解析失败

        const std::string bytes = read_file_bytes(path);
        REQUIRE_FALSE(bytes.empty());
        // 跳过 BOM 后必须是严格合法 UTF-8（拦住"混合编码"：BOM 正常但个别字符串是 GBK 字节）
        const std::string body = (bytes.size() >= 3) ? bytes.substr(3) : bytes;
        CHECK(is_valid_utf8(body));
    }
}

TEST_CASE("repo: 目标机便携包所需文件齐备（脚本 + 指南 + 参数 + 基线）") {
    const std::string root = find_repo_root();
    REQUIRE_FALSE(root.empty());

    // CI 的打包步骤引用的文件；少一个就会出现"包出来后跑不了"
    const std::vector<std::string> required = {
        "/scripts/run_target_tests.ps1",
        "/docs/TARGET_MACHINE_TESTS.md",
        "/shaders/raymarch.frag",
        "/shaders/fullscreen.vert",
        "/shaders/present.frag",
        "/shaders/blackbody_lut.f32",
        "/tests/golden/params.json",
        "/tests/golden/params_tiny.json",
        "/tests/golden/params_small.json",
        "/tests/golden/golden.pfm",
        "/tests/golden/golden_tiny.pfm",
        "/tests/golden/golden_small.pfm",
    };
    for (const std::string& relative : required) {
        const std::string path = root + relative;
        INFO("缺失: " << path);
        CHECK(file_exists(path));
    }
}
