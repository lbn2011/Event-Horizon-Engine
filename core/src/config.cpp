#include "ehe/core/config.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "ehe/core/units.h"

namespace ehe::core {
namespace {

/// 未知键告警收集：记录形如 "render.unknown_key" 的路径，便于用户定位
void warn(std::vector<std::string>* warnings, const std::string& message) {
    if (warnings != nullptr) {
        warnings->push_back(message);
    }
}

/// 记录某一组 JSON 对象里出现的未知键
void collect_unknown_keys(const nlohmann::json& object,
                          const std::vector<std::string>& known,
                          const std::string& group,
                          std::vector<std::string>* warnings) {
    if (!object.is_object()) {
        warn(warnings, group + ": 期望 JSON 对象，已忽略该组并使用默认值");
        return;
    }
    for (const auto& item : object.items()) {
        bool matched = false;
        for (const std::string& key : known) {
            if (item.key() == key) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            warn(warnings, "未知键已忽略：" + group + "." + item.key());
        }
    }
}

template <typename T>
void read_number(const nlohmann::json& object, const char* key, T& target,
                 std::vector<std::string>* warnings, const std::string& group) {
    if (!object.contains(key)) {
        return;  // 缺失 → 保持默认值
    }
    const nlohmann::json& value = object.at(key);
    if (value.is_number()) {
        target = value.get<T>();
        return;
    }
    if (value.is_boolean() && std::is_same_v<T, bool>) {
        target = value.get<bool>();
        return;
    }
    warn(warnings, group + "." + key + ": 类型不符（期望数值），已使用默认值");
}

template <typename T>
void read_bool(const nlohmann::json& object, const char* key, T& target,
               std::vector<std::string>* warnings, const std::string& group) {
    if (!object.contains(key)) {
        return;
    }
    const nlohmann::json& value = object.at(key);
    if (value.is_boolean()) {
        target = value.get<bool>();
    } else {
        warn(warnings, group + "." + key + ": 类型不符（期望布尔），已使用默认值");
    }
}

void read_string(const nlohmann::json& object, const char* key, std::string& target,
                 std::vector<std::string>* warnings, const std::string& group) {
    if (!object.contains(key)) {
        return;
    }
    const nlohmann::json& value = object.at(key);
    if (value.is_string()) {
        target = value.get<std::string>();
    } else {
        warn(warnings, group + "." + key + ": 类型不符（期望字符串），已使用默认值");
    }
}

/// 枚举字段：接受字符串形式，未知取值告警并保留默认
template <typename Enum, typename Parser>
void read_enum(const nlohmann::json& object, const char* key, Enum& target, Parser parser,
               std::vector<std::string>* warnings, const std::string& group) {
    if (!object.contains(key)) {
        return;
    }
    const nlohmann::json& value = object.at(key);
    if (!value.is_string()) {
        warn(warnings, group + "." + key + ": 类型不符（期望字符串），已使用默认值");
        return;
    }
    Enum parsed{};
    const std::string text = value.get<std::string>();
    if (parser(text, parsed)) {
        target = parsed;
    } else {
        warn(warnings, group + "." + key + ": 未知取值 '" + text + "'，已使用默认值");
    }
}

}  // namespace

// ---------------------------------------------------------------- 枚举转换

const char* to_string(Backend value) { return value == Backend::Vulkan ? "vk" : "gl"; }
const char* to_string(RenderMode value) { return value == RenderMode::Particle ? "particle" : "raymarch"; }
const char* to_string(PrecisionMode value) { return value == PrecisionMode::Fp32 ? "fp32" : "mixed"; }
const char* to_string(CameraMode value) { return value == CameraMode::Fly ? "fly" : "orbit"; }

bool parse_backend(const std::string& text, Backend& out) {
    if (text == "vk" || text == "vulkan") {
        out = Backend::Vulkan;
        return true;
    }
    if (text == "gl" || text == "opengl") {
        out = Backend::OpenGL;
        return true;
    }
    return false;
}

bool parse_render_mode(const std::string& text, RenderMode& out) {
    if (text == "raymarch") {
        out = RenderMode::Raymarch;
        return true;
    }
    if (text == "particle") {
        out = RenderMode::Particle;
        return true;
    }
    return false;
}

bool parse_precision(const std::string& text, PrecisionMode& out) {
    if (text == "mixed") {
        out = PrecisionMode::Mixed;
        return true;
    }
    if (text == "fp32") {
        out = PrecisionMode::Fp32;
        return true;
    }
    return false;
}

bool parse_camera_mode(const std::string& text, CameraMode& out) {
    if (text == "orbit") {
        out = CameraMode::Orbit;
        return true;
    }
    if (text == "fly") {
        out = CameraMode::Fly;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 序列化

nlohmann::json Config::to_json() const {
    nlohmann::json json;
    json["render"] = {
        {"backend", to_string(render.backend)},
        {"mode", to_string(render.mode)},
        {"res_scale", render.res_scale},
        {"fsr1", render.fsr1},
        {"fxaa", render.fxaa},
        {"fps_cap", render.fps_cap},
        {"debug_view", render.debug_view},
    };
    json["blackhole"] = {
        {"mass", blackhole.mass},
        {"spin", blackhole.spin},
        {"disk_r_in", blackhole.disk_r_in},
        {"disk_r_out", blackhole.disk_r_out},
        {"disk_density", blackhole.disk_density},
        {"disk_t_scale", blackhole.disk_t_scale},
        {"disk_kappa", blackhole.disk_kappa},
    };
    json["integrator"] = {
        {"n_max", integrator.n_max},
        {"h0", integrator.h0},
        {"h_min", integrator.h_min},
        {"h_max", integrator.h_max},
        {"precision", to_string(integrator.precision)},
    };
    json["particle"] = {
        {"count", particle.count},
        {"size", particle.size},
        {"velocity_profile", particle.velocity_profile},
        {"enabled", particle.enabled},
    };
    json["post"] = {
        {"aces", post.aces},
        {"exposure", post.exposure},
        {"chrom_ab", post.chrom_ab},
    };
    json["audio"] = {
        {"volume", audio.volume},
        {"muted", audio.muted},
    };
    json["camera"] = {
        {"mode", to_string(camera.mode)},
        {"fov_deg", camera.fov_deg},
        {"dist", camera.dist},
        {"azim_deg", camera.azim_deg},
        {"polar_deg", camera.polar_deg},
    };
    return json;
}

Config Config::from_json(const nlohmann::json& json, std::vector<std::string>* warnings) {
    Config config;
    if (!json.is_object()) {
        warn(warnings, "顶层不是 JSON 对象，全部使用默认值");
        return config;
    }

    // render
    if (const auto it = json.find("render"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g, {"backend", "mode", "res_scale", "fsr1", "fxaa", "fps_cap", "debug_view"},
                             "render", warnings);
        read_enum(g, "backend", config.render.backend, parse_backend, warnings, "render");
        read_enum(g, "mode", config.render.mode, parse_render_mode, warnings, "render");
        read_number(g, "res_scale", config.render.res_scale, warnings, "render");
        read_bool(g, "fsr1", config.render.fsr1, warnings, "render");
        read_bool(g, "fxaa", config.render.fxaa, warnings, "render");
        read_number(g, "fps_cap", config.render.fps_cap, warnings, "render");
        read_string(g, "debug_view", config.render.debug_view, warnings, "render");
    }

    // blackhole
    if (const auto it = json.find("blackhole"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g,
                             {"mass", "spin", "disk_r_in", "disk_r_out", "disk_density", "disk_t_scale",
                              "disk_kappa"},
                             "blackhole", warnings);
        read_number(g, "mass", config.blackhole.mass, warnings, "blackhole");
        read_number(g, "spin", config.blackhole.spin, warnings, "blackhole");
        read_number(g, "disk_r_in", config.blackhole.disk_r_in, warnings, "blackhole");
        read_number(g, "disk_r_out", config.blackhole.disk_r_out, warnings, "blackhole");
        read_number(g, "disk_density", config.blackhole.disk_density, warnings, "blackhole");
        read_number(g, "disk_t_scale", config.blackhole.disk_t_scale, warnings, "blackhole");
        read_number(g, "disk_kappa", config.blackhole.disk_kappa, warnings, "blackhole");
    }

    // integrator
    if (const auto it = json.find("integrator"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g, {"n_max", "h0", "h_min", "h_max", "precision"}, "integrator", warnings);
        read_number(g, "n_max", config.integrator.n_max, warnings, "integrator");
        read_number(g, "h0", config.integrator.h0, warnings, "integrator");
        read_number(g, "h_min", config.integrator.h_min, warnings, "integrator");
        read_number(g, "h_max", config.integrator.h_max, warnings, "integrator");
        read_enum(g, "precision", config.integrator.precision, parse_precision, warnings, "integrator");
    }

    // particle
    if (const auto it = json.find("particle"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g, {"count", "size", "velocity_profile", "enabled"}, "particle", warnings);
        read_number(g, "count", config.particle.count, warnings, "particle");
        read_number(g, "size", config.particle.size, warnings, "particle");
        read_string(g, "velocity_profile", config.particle.velocity_profile, warnings, "particle");
        read_bool(g, "enabled", config.particle.enabled, warnings, "particle");
    }

    // post
    if (const auto it = json.find("post"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g, {"aces", "exposure", "chrom_ab"}, "post", warnings);
        read_bool(g, "aces", config.post.aces, warnings, "post");
        read_number(g, "exposure", config.post.exposure, warnings, "post");
        read_number(g, "chrom_ab", config.post.chrom_ab, warnings, "post");
    }

    // audio
    if (const auto it = json.find("audio"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g, {"volume", "muted"}, "audio", warnings);
        read_number(g, "volume", config.audio.volume, warnings, "audio");
        read_bool(g, "muted", config.audio.muted, warnings, "audio");
    }

    // camera
    if (const auto it = json.find("camera"); it != json.end()) {
        const nlohmann::json& g = *it;
        collect_unknown_keys(g, {"mode", "fov_deg", "dist", "azim_deg", "polar_deg"}, "camera", warnings);
        read_enum(g, "mode", config.camera.mode, parse_camera_mode, warnings, "camera");
        read_number(g, "fov_deg", config.camera.fov_deg, warnings, "camera");
        read_number(g, "dist", config.camera.dist, warnings, "camera");
        read_number(g, "azim_deg", config.camera.azim_deg, warnings, "camera");
        read_number(g, "polar_deg", config.camera.polar_deg, warnings, "camera");
    }

    // 顶层未知键
    collect_unknown_keys(json, {"render", "blackhole", "integrator", "particle", "post", "audio", "camera"},
                         "顶层", warnings);

    config.validate_and_clamp(warnings);
    return config;
}

Config Config::load_from_file(const std::string& path, std::vector<std::string>* warnings) {
    std::ifstream stream(path);
    if (!stream) {
        warn(warnings, "配置文件不存在，使用默认值：" + path);
        return Config{};
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();
    std::string text = buffer.str();

    // BOM 容错：Windows 记事本 / PowerShell 5.1 的 Set-Content 会把文件存成「UTF-8 带 BOM」，
    // 而 JSON 解析器遇到首字节的 EF BB BF 会直接报错。这里剥掉 BOM（并提示），避免手工编辑参数文件踩坑。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
        warn(warnings, "配置文件带 UTF-8 BOM，已自动剥离：" + path);
    }

    try {
        const nlohmann::json json = nlohmann::json::parse(text);
        return from_json(json, warnings);
    } catch (const std::exception& error) {
        warn(warnings, std::string("配置解析失败，使用默认值：") + error.what());
        return Config{};
    }
}

bool Config::save_to_file(const std::string& path) const {
    std::ofstream stream(path);
    if (!stream) {
        std::fprintf(stderr, "[config] 无法写入 %s\n", path.c_str());
        return false;
    }
    stream << to_json().dump(2) << '\n';
    return true;
}

void Config::validate_and_clamp(std::vector<std::string>* issues) {
    // 自旋：按 DESIGN §4.1 钳制到 [0, 0.998]
    if (blackhole.spin < 0.0 || blackhole.spin > units::kMaxSpin) {
        warn(issues, "blackhole.spin 超出 [0, 0.998]，已钳制");
        blackhole.spin = clamp_d(blackhole.spin, 0.0, units::kMaxSpin);
    }

    // 内部分辨率档（§7：0.5–2.0）
    if (render.res_scale < 0.5 || render.res_scale > 2.0) {
        warn(issues, "render.res_scale 超出 [0.5, 2.0]，已钳制");
        render.res_scale = clamp_d(render.res_scale, 0.5, 2.0);
    }

    // 积分参数（§4.3：步数上限 > 0，步长上下限有序）
    if (integrator.n_max < 1) {
        warn(issues, "integrator.n_max < 1，已设为 1");
        integrator.n_max = 1;
    }
    if (integrator.h_min <= 0.0 || integrator.h_max <= integrator.h_min) {
        warn(issues, "integrator 步长上下限无效，已恢复默认（h_min=1e-3, h_max=0.5）");
        integrator.h_min = 1e-3;
        integrator.h_max = 0.5;
    }
    if (integrator.h0 <= 0.0) {
        warn(issues, "integrator.h0 <= 0，已恢复默认 0.05");
        integrator.h0 = 0.05;
    }

    // 盘几何：内 < 外，且内半径不小于 ISCO 下限的合理性检查（§4.5 默认 [6, 20]）
    if (blackhole.disk_r_in <= 0.0 || blackhole.disk_r_out <= blackhole.disk_r_in) {
        warn(issues, "盘半径无效（需 0 < r_in < r_out），已恢复默认 [6, 20]");
        blackhole.disk_r_in = 6.0;
        blackhole.disk_r_out = 20.0;
    }

    // 相机（§4.7）
    if (camera.fov_deg < kFovMinDeg || camera.fov_deg > kFovMaxDeg) {
        warn(issues, "camera.fov_deg 超出 [20, 90]，已钳制");
        camera.fov_deg = clamp_d(camera.fov_deg, kFovMinDeg, kFovMaxDeg);
    }
    if (camera.dist < kOrbitDistanceMin || camera.dist > kOrbitDistanceMax) {
        warn(issues, "camera.dist 超出 [8, 80]，已钳制");
        camera.dist = clamp_d(camera.dist, kOrbitDistanceMin, kOrbitDistanceMax);
    }
    if (camera.polar_deg < rad_to_deg(kPolarMin) || camera.polar_deg > rad_to_deg(kPolarMax)) {
        warn(issues, "camera.polar_deg 超出防万向锁范围，已钳制");
        camera.polar_deg = clamp_d(camera.polar_deg, rad_to_deg(kPolarMin), rad_to_deg(kPolarMax));
    }
    if (camera.mode == CameraMode::Orbit && camera.dist > units::kEscapeRadius) {
        warn(issues, "camera.dist 超过逃逸半径，阴影将退化为点，建议 ≤ 80");
    }
}

}  // namespace ehe::core
