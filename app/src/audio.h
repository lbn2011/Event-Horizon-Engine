#pragma once

// EHE —— 程序化音效（DESIGN §5.7，T1.8.3）
//
// miniaudio 单声道 data callback（48 kHz）+ 音频线程独立（不进渲染关键路径）：
//   合成 = 棕噪声（白噪声泄漏积分 x ← 0.998x + 0.02w）→ 单极点低通；
//   截止 = 40 Hz + 200 Hz · norm(盘密度)；增益 ∝ √norm(盘密度)【建议默认】。
//
// 线程模型：callback 在 miniaudio 音频线程执行，所有共享参数走 std::atomic；
//   主线程只在面板改动时写原子量，无锁。

#include <atomic>
#include <cstdint>

// miniaudio 前置声明（完整定义仅在 audio.cpp 内 include——单头文件很大，别扩散编译时延）
struct ma_device;
struct ma_device_config;

namespace ehe::app {

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine() { shutdown(); }

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// 启动音频设备；失败返回 false（静默降级——音频不可用不影响程序）
    bool init(std::uint32_t sample_rate = 48000);

    void shutdown();

    bool ready() const { return device_ != nullptr; }

    /// 音量 [0, 1]
    void set_volume(double volume) { volume_.store(volume < 0.0 ? 0.0 : (volume > 1.0 ? 1.0 : volume)); }

    /// 静音开关
    void set_muted(bool muted) { muted_.store(muted); }

    /// 盘密度归一值 [0, 1]（驱动截止频率与增益，§5.7）
    void set_density_norm(double norm) {
        density_norm_.store(norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm));
    }

private:
    /// 签名必须是 ma_device_data_proc：void(*)(ma_device*, void* output, const void* input,
    /// uint32 frame_count)——缺 input 参数会直接编译错（回调函数指针类型不兼容）。
    static void data_callback(ma_device* device, void* output, const void* input,
                              std::uint32_t frame_count);

    struct ma_device* device_ = nullptr;
    struct ma_device_config* device_config_ = nullptr;  ///< init 内 new（ma_device 不保留完整 config）

    // —— 音频线程读取的参数（atomic；主线程写）——
    std::atomic<double> volume_{0.5};
    std::atomic<bool> muted_{false};
    std::atomic<double> density_norm_{1.0};

    // —— 合成状态（仅音频线程触碰）——
    double brown_state_ = 0.0;   ///< 棕噪声泄漏积分状态
    double lowpass_state_ = 0.0; ///< 单极点低通状态
    std::uint64_t rng_state_ = 0x9E3779B97F4A7C15ULL;  ///< 白噪声 LCG 状态（音频线程内自用）
};

}  // namespace ehe::app
