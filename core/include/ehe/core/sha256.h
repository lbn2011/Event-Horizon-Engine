#pragma once

// EHE core —— SHA-256（用于 LUT 双份拷贝一致性校验，DESIGN §4.5 / §6.5）
//
// 说明：此处是为「校验产物一致性」自带的极简实现，不用于任何安全用途。
//       已通过标准空串 KAT 验证（见 tests/src/test_blackbody.cpp）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ehe::core {

/// 计算 SHA-256 摘要（32 字节）
std::vector<std::uint8_t> sha256(const std::uint8_t* data, std::size_t size);

/// 小写十六进制摘要字符串
std::string sha256_hex(const std::vector<std::uint8_t>& digest);

/// 直接对文件内容求摘要；读取失败返回空字符串
std::string sha256_file_hex(const std::string& path);

}  // namespace ehe::core
