#pragma once

// EHE core —— 全量运行参数（DESIGN §8.1 Config JSON schema）
//
// 设计要点：
//   - 字段与面板一一对应，键名固定（§8.1）；未知键忽略并告警、缺失键取默认值；
//   - 本结构是「唯一参数来源」：UI 读写它、UBO 由它组装、golden 参数文件由它序列化；
//   - core 不依赖图形库，故此处定义后端枚举，render 侧通过 using 别名复用（避免枚举重复定义）。

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ehe/core/math.h"

namespace ehe::core {

/// 渲染后端（DESIGN §8 渲染组）
enum class Backend { Vulkan, OpenGL };

/// 渲染模式（§5.6 粒子经典模式 / §5.4 raymarching）
enum class RenderMode { Raymarch, Particle };

/// 积分精度模式（§4.3：混合 fp64 关键路径 / 纯 fp32）
enum class PrecisionMode { Mixed, Fp32 };

/// 相机模式（§4.7）
enum class CameraMode { Orbit, Fly };

// ---------------------------------------------------------------- 分组参数（对应 §8.1 的各 JSON 对象）

struct RenderConfig {
    Backend backend = Backend::OpenGL;
    RenderMode mode = RenderMode::Raymarch;
    double res_scale = 1.0;          ///< 内部渲染分辨率档：0.5–2.0（>1.0 即 SSAA）
    bool fsr1 = true;                ///< <1.0 时的 FSR1 升频开关（>1.0 自动禁用）
    bool fxaa = true;
    int fps_cap = 60;                ///< 60 / 120 / 0(不限)
    std::string debug_view = "shaded";  ///< §5.5：shaded/steps/classify/g_factor/null_drift
};

struct BlackHoleConfig {
    double mass = 1.0;               ///< 面板展示用；方程在 M=1 下无量纲化
    double spin = 0.0;               ///< a ∈ [0, 0.998]，M1 阶段锁定 0
    double disk_r_in = 6.0;          ///< 盘内半径（M=1；a=0 时 = ISCO）
    double disk_r_out = 20.0;        ///< 盘外半径（M=1）
    double disk_density = 1.0;       ///< 归一化盘密度（进入通量剖面）
    double disk_t_scale = 10000.0;   ///< 盘温标定 T_scale（单位 K；峰值温度，进入黑体 LUT 前需有物理量级）
    double disk_kappa = 2.0;         ///< 吸收系数 κ（§4.5 体合成，建议默认 2）
    /// 盘湍流噪声振幅 ∈ [0,1]（§4.5 fbm 调制 ρ 与发射；V5.8 新增字段）。
    /// 默认 0.35 = 交互观感（有明显湍流纹理）；**golden 基线在参数文件里显式写 0**（§6.1「无噪声动画」），
    /// 使基线与噪声实现解耦、逐位可复现（噪声的 GPU/CPU hash 不可能逐位一致，不能进 NMSE 差分）。
    double disk_noise = 0.35;
};

struct IntegratorConfig {
    int n_max = 300;                 ///< 最大步数（§4.3）
    double h0 = 0.05;                ///< 步长因子
    double h_min = 1e-3;             ///< 步长下限
    double h_max = 0.5;              ///< 步长上限
    PrecisionMode precision = PrecisionMode::Mixed;
};

struct ParticleConfig {
    int count = 100000;
    double size = 1.0;
    std::string velocity_profile = "kepler";
    bool enabled = false;
};

struct PostConfig {
    bool aces = true;
    double exposure = 1.0;
    double chrom_ab = 0.5;
};

struct AudioConfig {
    double volume = 0.5;
    bool muted = false;
};

struct CameraConfig {
    CameraMode mode = CameraMode::Orbit;
    double fov_deg = kFovDefaultDeg;
    double dist = 15.0;
    double azim_deg = 0.0;
    double polar_deg = 75.0;
};

// ---------------------------------------------------------------- 汇总

struct Config {
    RenderConfig render;
    BlackHoleConfig blackhole;
    IntegratorConfig integrator;
    ParticleConfig particle;
    PostConfig post;
    AudioConfig audio;
    CameraConfig camera;

    /// 序列化为 JSON（键名与 §8.1 完全一致）
    nlohmann::json to_json() const;

    /// 从 JSON 构造；未知键追加到 warnings（不抛异常），缺失键取默认值
    static Config from_json(const nlohmann::json& json, std::vector<std::string>* warnings = nullptr);

    /// 读取文件；文件不存在时返回默认值并把提示写入 warnings
    static Config load_from_file(const std::string& path, std::vector<std::string>* warnings = nullptr);

    /// 从配置 JSON 读取 image 块（§6.1 的 width/height）。
    /// 成功返回 true；文件不存在/为空/非法 JSON/无 image 块时返回 false 并保持 width/height 不变。
    /// **绝不抛异常** —— 早期版本在调用方直接 `stream >> json`，空文件会让进程 std::terminate
    /// （退出码 0xC0000409），因此这里统一吞掉异常并交由调用方决定回退默认值。
    static bool read_image_size(const std::string& path, int& width, int& height);

    /// 写出文件（缩进 2）
    bool save_to_file(const std::string& path) const;

    /// 值域检查：越界项写入 issues，并按 DESIGN 约定做必要钳制（如 spin ≤ 0.998）
    void validate_and_clamp(std::vector<std::string>* issues = nullptr);
};

// 枚举 ↔ 字符串（用于 JSON 与 UI；未知字符串返回默认值并可选记录告警）
const char* to_string(Backend value);
const char* to_string(RenderMode value);
const char* to_string(PrecisionMode value);
const char* to_string(CameraMode value);
bool parse_backend(const std::string& text, Backend& out);
bool parse_render_mode(const std::string& text, RenderMode& out);
bool parse_precision(const std::string& text, PrecisionMode& out);
bool parse_camera_mode(const std::string& text, CameraMode& out);

}  // namespace ehe::core
