// EHE —— Config 单元测试（DESIGN §6.5：config_roundtrip）
//
// 覆盖：序列化往返、未知键告警、缺失键默认、值域钳制、文件读写。

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "ehe/core/config.h"

using ehe::core::Backend;
using ehe::core::CameraMode;
using ehe::core::Config;
using ehe::core::PrecisionMode;
using ehe::core::RenderMode;

namespace {

bool has_warning(const std::vector<std::string>& warnings, const std::string& fragment) {
    for (const std::string& item : warnings) {
        if (item.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("config_roundtrip: 序列化 → 反序列化全字段相等") {
    Config original;
    // 改动每个分组的若干字段，确保往返不是"默认值恰好相等"
    original.render.backend = Backend::Vulkan;
    original.render.mode = RenderMode::Particle;
    original.render.res_scale = 0.67;
    original.render.fsr1 = false;
    original.render.fxaa = false;
    original.render.fps_cap = 120;
    original.render.debug_view = "g_factor";

    original.blackhole.mass = 2.5;
    original.blackhole.spin = 0.5;
    original.blackhole.disk_r_in = 7.0;
    original.blackhole.disk_r_out = 25.0;
    original.blackhole.disk_density = 1.5;
    original.blackhole.disk_t_scale = 0.8;
    original.blackhole.disk_kappa = 3.0;

    original.integrator.n_max = 512;
    original.integrator.h0 = 0.02;
    original.integrator.h_min = 1e-4;
    original.integrator.h_max = 0.25;
    original.integrator.precision = PrecisionMode::Fp32;

    original.particle.count = 4096;
    original.particle.size = 2.0;
    original.particle.velocity_profile = "uniform";
    original.particle.enabled = true;

    original.post.aces = false;
    original.post.exposure = 1.3;
    original.post.chrom_ab = 0.2;

    original.audio.volume = 0.1;
    original.audio.muted = true;

    original.camera.mode = CameraMode::Fly;
    original.camera.fov_deg = 55.0;
    original.camera.dist = 22.0;
    original.camera.azim_deg = 33.0;
    original.camera.polar_deg = 60.0;

    std::vector<std::string> warnings;
    const Config restored = Config::from_json(original.to_json(), &warnings);

    CHECK(warnings.empty());
    CHECK(restored.render.backend == original.render.backend);
    CHECK(restored.render.mode == original.render.mode);
    CHECK(restored.render.res_scale == doctest::Approx(original.render.res_scale));
    CHECK(restored.render.fsr1 == original.render.fsr1);
    CHECK(restored.render.fxaa == original.render.fxaa);
    CHECK(restored.render.fps_cap == original.render.fps_cap);
    CHECK(restored.render.debug_view == original.render.debug_view);

    CHECK(restored.blackhole.mass == doctest::Approx(original.blackhole.mass));
    CHECK(restored.blackhole.spin == doctest::Approx(original.blackhole.spin));
    CHECK(restored.blackhole.disk_r_in == doctest::Approx(original.blackhole.disk_r_in));
    CHECK(restored.blackhole.disk_r_out == doctest::Approx(original.blackhole.disk_r_out));
    CHECK(restored.blackhole.disk_density == doctest::Approx(original.blackhole.disk_density));
    CHECK(restored.blackhole.disk_t_scale == doctest::Approx(original.blackhole.disk_t_scale));
    CHECK(restored.blackhole.disk_kappa == doctest::Approx(original.blackhole.disk_kappa));

    CHECK(restored.integrator.n_max == original.integrator.n_max);
    CHECK(restored.integrator.h0 == doctest::Approx(original.integrator.h0));
    CHECK(restored.integrator.precision == original.integrator.precision);

    CHECK(restored.particle.count == original.particle.count);
    CHECK(restored.particle.velocity_profile == original.particle.velocity_profile);
    CHECK(restored.particle.enabled == original.particle.enabled);

    CHECK(restored.post.exposure == doctest::Approx(original.post.exposure));
    CHECK(restored.audio.muted == original.audio.muted);
    CHECK(restored.camera.mode == original.camera.mode);
    CHECK(restored.camera.fov_deg == doctest::Approx(original.camera.fov_deg));
    CHECK(restored.camera.polar_deg == doctest::Approx(original.camera.polar_deg));
}

TEST_CASE("config: 未知键忽略并告警（含顶层与分组内）") {
    nlohmann::json json = Config{}.to_json();
    json["render"]["unknown_option"] = 42;
    json["mystery_group"] = {{"a", 1}};

    std::vector<std::string> warnings;
    const Config config = Config::from_json(json, &warnings);
    (void)config;

    CHECK(has_warning(warnings, "render.unknown_option"));
    CHECK(has_warning(warnings, "mystery_group"));
}

TEST_CASE("config: 缺失键取默认值") {
    nlohmann::json json;
    json["render"] = {{"backend", "vk"}};  // 只给一个字段

    std::vector<std::string> warnings;
    const Config config = Config::from_json(json, &warnings);

    CHECK(config.render.backend == Backend::Vulkan);
    CHECK(config.render.res_scale == doctest::Approx(1.0));       // 默认
    CHECK(config.integrator.n_max == 300);                         // 整组缺失 → 默认
    CHECK(config.camera.fov_deg == doctest::Approx(40.0));
    CHECK(warnings.empty());
}

TEST_CASE("config: 自旋超限被钳制到 [0, 0.998]（DESIGN §4.1）") {
    nlohmann::json json = Config{}.to_json();
    json["blackhole"]["spin"] = 2.0;

    std::vector<std::string> warnings;
    const Config config = Config::from_json(json, &warnings);

    CHECK(config.blackhole.spin == doctest::Approx(0.998));
    CHECK(has_warning(warnings, "spin"));
}

TEST_CASE("config: 无效值域被钳制/恢复默认并告警") {
    nlohmann::json json = Config{}.to_json();
    json["render"]["res_scale"] = 9.0;
    json["integrator"]["n_max"] = 0;
    json["integrator"]["h_min"] = 1.0;
    json["integrator"]["h_max"] = 0.5;
    json["blackhole"]["disk_r_in"] = 30.0;   // 大于外半径
    json["camera"]["fov_deg"] = 200.0;
    json["camera"]["dist"] = 1.0;
    json["camera"]["polar_deg"] = 0.0;

    std::vector<std::string> warnings;
    const Config config = Config::from_json(json, &warnings);

    CHECK(config.render.res_scale == doctest::Approx(2.0));
    CHECK(config.integrator.n_max == 1);
    CHECK(config.integrator.h_min == doctest::Approx(1e-3));
    CHECK(config.integrator.h_max == doctest::Approx(0.5));
    CHECK(config.blackhole.disk_r_in == doctest::Approx(6.0));
    CHECK(config.blackhole.disk_r_out == doctest::Approx(20.0));
    CHECK(config.camera.fov_deg == doctest::Approx(90.0));
    CHECK(config.camera.dist == doctest::Approx(8.0));
    CHECK(config.camera.polar_deg > 0.0);
    CHECK(warnings.size() >= 6);
}

TEST_CASE("config: 类型不符时告警并保留默认") {
    nlohmann::json json = Config{}.to_json();
    json["render"]["fps_cap"] = "sixty";       // 期望数值
    json["integrator"]["precision"] = 42;      // 期望字符串
    json["render"]["backend"] = "mystery";     // 未知取值

    std::vector<std::string> warnings;
    const Config config = Config::from_json(json, &warnings);

    CHECK(config.render.fps_cap == 60);
    CHECK(config.integrator.precision == PrecisionMode::Mixed);
    CHECK(config.render.backend == Backend::OpenGL);
    CHECK(warnings.size() == 3);
}

TEST_CASE("config: 文件读写往返（不存在的文件返回默认值并告警）") {
    const std::string path = "ehe_test_config.json";

    std::vector<std::string> read_warnings;
    const Config missing = Config::load_from_file(path, &read_warnings);
    CHECK(missing.integrator.n_max == 300);
    CHECK(has_warning(read_warnings, "配置文件不存在"));

    Config written;
    written.integrator.n_max = 777;
    written.render.debug_view = "steps";
    REQUIRE(written.save_to_file(path));

    std::vector<std::string> second_warnings;
    const Config loaded = Config::load_from_file(path, &second_warnings);
    CHECK(second_warnings.empty());
    CHECK(loaded.integrator.n_max == 777);
    CHECK(loaded.render.debug_view == "steps");

    std::remove(path.c_str());
}

TEST_CASE("config: 空文件 / 非法 JSON / BOM 都必须安全回退（绝不抛异常）") {
    // 背景：目标机实测出现「配置文件为空 → ehe.exe 以 0xC0000409 崩溃」。
    // 根因是调用方直接 `stream >> json` 未兜异常；这里把这些边界固化成用例。
    const std::string empty_path = "ehe_test_empty_config.json";
    const std::string bom_path = "ehe_test_bom_config.json";
    const std::string bad_path = "ehe_test_bad_config.json";

    {
        std::ofstream out(empty_path, std::ios::binary);  // 空文件
    }
    {
        std::ofstream out(bom_path, std::ios::binary);
        out << "\xEF\xBB\xBF";  // UTF-8 BOM（记事本存出来的样子）
        out << R"({"image": {"width": 96, "height": 48}, "integrator": {"n_max": 123}})";
    }
    {
        std::ofstream out(bad_path, std::ios::binary);
        out << R"({"image": {"width": )";  // 故意截断
    }

    // 1) 空文件：回退默认值 + 告警，且不抛
    std::vector<std::string> warnings;
    const Config from_empty = Config::load_from_file(empty_path, &warnings);
    CHECK(from_empty.integrator.n_max == Config{}.integrator.n_max);
    CHECK_FALSE(warnings.empty());

    // 2) 带 BOM 的合法 JSON：必须正常解析
    warnings.clear();
    const Config from_bom = Config::load_from_file(bom_path, &warnings);
    CHECK(from_bom.integrator.n_max == 123);

    // 3) 非法 JSON：回退默认值，不抛
    warnings.clear();
    const Config from_bad = Config::load_from_file(bad_path, &warnings);
    CHECK(from_bad.integrator.n_max == Config{}.integrator.n_max);
    CHECK_FALSE(warnings.empty());

    // 4) read_image_size：失败时不得改动输出参数；成功时读出并接受 BOM
    int width = 512;
    int height = 512;
    CHECK_FALSE(Config::read_image_size(empty_path, width, height));
    CHECK(width == 512);
    CHECK(height == 512);
    CHECK_FALSE(Config::read_image_size(bad_path, width, height));
    CHECK(width == 512);
    CHECK_FALSE(Config::read_image_size("ehe_no_such_config.json", width, height));
    CHECK(width == 512);
    CHECK(Config::read_image_size(bom_path, width, height));
    CHECK(width == 96);
    CHECK(height == 48);

    std::remove(empty_path.c_str());
    std::remove(bom_path.c_str());
    std::remove(bad_path.c_str());
}
