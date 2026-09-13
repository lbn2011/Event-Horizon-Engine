#include "ehe/core/sha256.h"

#include <cstdio>
#include <fstream>

namespace ehe::core {
namespace {

// FIPS 180-4 规定的初始哈希值与轮常量
constexpr std::uint32_t kInitialState[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

inline std::uint32_t rotate_right(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

void process_block(const std::uint8_t* block, std::uint32_t state[8]) {
    std::uint32_t w[64] = {};
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24U) |
               (static_cast<std::uint32_t>(block[4 * i + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8U) |
               static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::vector<std::uint8_t> sha256(const std::uint8_t* data, std::size_t size) {
    std::uint32_t state[8];
    for (int i = 0; i < 8; ++i) {
        state[i] = kInitialState[i];
    }

    // 填充：0x80 + 若干 0 + 64 位大端长度
    const std::size_t padded_size = ((size + 8) / 64 + 1) * 64;
    std::vector<std::uint8_t> buffer(padded_size, 0);
    for (std::size_t i = 0; i < size; ++i) {
        buffer[i] = data[i];
    }
    buffer[size] = 0x80U;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8U;
    for (int i = 0; i < 8; ++i) {
        buffer[padded_size - 1 - i] =
            static_cast<std::uint8_t>((bit_length >> (8U * i)) & 0xffU);
    }

    for (std::size_t offset = 0; offset < padded_size; offset += 64) {
        process_block(&buffer[offset], state);
    }

    std::vector<std::uint8_t> digest(32);
    for (int i = 0; i < 8; ++i) {
        digest[4 * i + 0] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xffU);
        digest[4 * i + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xffU);
        digest[4 * i + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xffU);
        digest[4 * i + 3] = static_cast<std::uint8_t>(state[i] & 0xffU);
    }
    return digest;
}

std::string sha256_hex(const std::vector<std::uint8_t>& digest) {
    static const char* kHex = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(kHex[(byte >> 4U) & 0x0fU]);
        text.push_back(kHex[byte & 0x0fU]);
    }
    return text;
}

std::string sha256_file_hex(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
    return sha256_hex(sha256(data.data(), data.size()));
}

}  // namespace ehe::core
