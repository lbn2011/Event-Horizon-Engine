#include "audio.h"

// miniaudio 单头实现（只在本翻译单元展开一次，DESIGN §5.7 / §9）。
// MINIAUDIO_IMPLEMENTATION 必须定义（恰一个 TU）：否则只有声明没有实现，
// 链接期报 ma_device_init 等符号未定义（Deps.cmake 的 ehe_miniaudio 是 INTERFACE
// target，只提供头文件路径，实现要自己展开）。
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_ID  // 不需要内嵌资源加载
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace ehe::app {

void AudioEngine::data_callback(ma_device* device, void* output, const void* /*input*/,
                                std::uint32_t frame_count) {
    auto* self = static_cast<AudioEngine*>(device->pUserData);
    if (self == nullptr) {
        return;
    }
    auto* samples = static_cast<float*>(output);

    const double volume = self->volume_.load();
    const bool muted = self->muted_.load();
    const double norm = self->density_norm_.load();

    // §5.7：截止 = 40 Hz + 200 Hz·norm(盘密度)；增益 ∝ √norm
    const double sample_rate = static_cast<double>(device->sampleRate);
    const double cutoff = 40.0 + 200.0 * norm;
    // 单极点低通系数：α = ω·dt / (ω·dt + 1)（等价 RC 一阶，数值稳定形式）
    const double omega_dt = 6.283185307179586 * cutoff / sample_rate;
    const double alpha = omega_dt / (omega_dt + 1.0);
    const double gain = std::sqrt(norm) * volume * (muted ? 0.0 : 1.0);

    for (std::uint32_t i = 0; i < frame_count; ++i) {
        // 白噪声：内联 LCG（音频线程内自用状态，无全局锁、无标准库开销）
        self->rng_state_ = self->rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        const double white =
            static_cast<double>(static_cast<std::int32_t>(self->rng_state_ >> 33)) / 2147483648.0;
        // 棕噪声：白噪声泄漏积分（§5.7：x ← 0.998x + 0.02w）
        self->brown_state_ = 0.998 * self->brown_state_ + 0.02 * white;
        // 单极点低通（×8 提升合成信号量级：泄漏积分的稳态幅度 ~0.02/(1-0.998)=10，实际动态小）
        self->lowpass_state_ += alpha * (self->brown_state_ * 8.0 - self->lowpass_state_);
        samples[i] = static_cast<float>(self->lowpass_state_ * gain);
    }
}

bool AudioEngine::init(const std::uint32_t sample_rate) {
    if (device_ != nullptr) {
        return true;
    }
    auto* config = new ma_device_config();
    *config = ma_device_config_init(ma_device_type_playback);
    config->playback.format = ma_format_f32;
    config->playback.channels = 1;   // 单声道（§5.7）
    config->sampleRate = sample_rate;
    config->dataCallback = &AudioEngine::data_callback;
    config->pUserData = this;

    auto* device = new ma_device();
    if (ma_device_init(nullptr, config, device) != MA_SUCCESS) {
        std::fprintf(stderr, "[audio] miniaudio 设备初始化失败（音频静默禁用）\n");
        delete device;
        delete config;
        return false;
    }
    if (ma_device_start(device) != MA_SUCCESS) {
        std::fprintf(stderr, "[audio] miniaudio 设备启动失败（音频静默禁用）\n");
        ma_device_uninit(device);
        delete device;
        delete config;
        return false;
    }
    device_ = device;
    device_config_ = config;
    std::printf("[audio] 音频就绪：%u Hz 单声道（棕噪声 + 低通合成，§5.7）\n", sample_rate);
    return true;
}

void AudioEngine::shutdown() {
    if (device_ != nullptr) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
    if (device_config_ != nullptr) {
        delete device_config_;
        device_config_ = nullptr;
    }
}

}  // namespace ehe::app
