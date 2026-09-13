// EHE —— Camera 单元测试（DESIGN §6.5：camera_tetrad_orthonormal）
//
// 覆盖：tetrad 正交归一性（η）、像素→光线映射（§4.2 公式）、交互钳制（§4.7）、模式切换视线连续。

#include <doctest/doctest.h>

#include <cmath>

#include <glm/glm.hpp>

#include "ehe/core/camera.h"
#include "ehe/core/config.h"

using ehe::core::Camera;
using ehe::core::CameraConfig;
using ehe::core::CameraMode;
using ehe::core::Vec4;

namespace {

/// 平直时空度量 η = diag(−1, 1, 1, 1)（tetrad 正交性判据，DESIGN §6.5）
double eta_dot(const Vec4& a, const Vec4& b) { return -a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

}  // namespace

TEST_CASE("camera_tetrad_orthonormal: g(e_a, e_b) = η_ab（容差 1e-12）") {
    CameraConfig config;
    config.dist = 15.0;
    config.azim_deg = 37.0;
    config.polar_deg = 75.0;

    Camera camera(config);
    const auto tetrad = camera.static_tetrad_flat();

    CHECK(eta_dot(tetrad.e_t, tetrad.e_t) == doctest::Approx(-1.0).epsilon(1e-12));
    CHECK(eta_dot(tetrad.e_x, tetrad.e_x) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(eta_dot(tetrad.e_y, tetrad.e_y) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(eta_dot(tetrad.e_z, tetrad.e_z) == doctest::Approx(1.0).epsilon(1e-12));

    // 交叉项全为 0
    CHECK(std::abs(eta_dot(tetrad.e_t, tetrad.e_x)) < 1e-12);
    CHECK(std::abs(eta_dot(tetrad.e_t, tetrad.e_y)) < 1e-12);
    CHECK(std::abs(eta_dot(tetrad.e_t, tetrad.e_z)) < 1e-12);
    CHECK(std::abs(eta_dot(tetrad.e_x, tetrad.e_y)) < 1e-12);
    CHECK(std::abs(eta_dot(tetrad.e_x, tetrad.e_z)) < 1e-12);
    CHECK(std::abs(eta_dot(tetrad.e_y, tetrad.e_z)) < 1e-12);
}

TEST_CASE("camera: 视线基矢正交归一，前向指向黑洞（轨道模式）") {
    CameraConfig config;
    config.dist = 12.0;
    config.azim_deg = 0.0;
    config.polar_deg = 90.0;  // 赤道面内
    Camera camera(config);

    const auto basis = camera.ray_basis(16.0 / 9.0);
    CHECK(glm::length(basis.forward) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(glm::length(basis.right) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(glm::length(basis.up) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(std::abs(glm::dot(basis.forward, basis.right)) < 1e-12);
    CHECK(std::abs(glm::dot(basis.forward, basis.up)) < 1e-12);
    CHECK(std::abs(glm::dot(basis.right, basis.up)) < 1e-12);

    // 轨道相机始终看向原点：forward 应与位置反向平行
    const auto position = camera.position();
    const auto to_origin = glm::normalize(-position);
    CHECK(glm::length(basis.forward - to_origin) < 1e-12);
}

TEST_CASE("camera: 像素 → 光线映射（§4.2 公式）") {
    CameraConfig config;
    config.dist = 20.0;
    config.polar_deg = 75.0;
    config.fov_deg = 40.0;
    Camera camera(config);

    const double aspect = 16.0 / 9.0;
    const auto basis = camera.ray_basis(aspect);

    // 中心像素恰为前向
    const auto center = camera.ray_direction(0.0, 0.0, aspect);
    CHECK(glm::length(center - basis.forward) < 1e-12);

    // 任意像素都必须是单位向量
    for (const double sx : {-1.0, -0.35, 0.6, 1.0}) {
        for (const double sy : {-1.0, 0.0, 0.8}) {
            const auto direction = camera.ray_direction(sx, sy, aspect);
            CHECK(glm::length(direction) == doctest::Approx(1.0).epsilon(1e-12));
        }
    }

    // FOV 越大，边缘光线偏离前向越多（单调性检查）
    Camera narrow(config);
    narrow.set_fov_deg(20.0);
    Camera wide(config);
    wide.set_fov_deg(90.0);
    const double narrow_angle = std::acos(ehe::core::clamp_d(
        glm::dot(narrow.ray_direction(1.0, 0.0, aspect), narrow.forward()), -1.0, 1.0));
    const double wide_angle = std::acos(ehe::core::clamp_d(
        glm::dot(wide.ray_direction(1.0, 0.0, aspect), wide.forward()), -1.0, 1.0));
    CHECK(wide_angle > narrow_angle);
}

TEST_CASE("camera: 轨道交互钳制（dist/polar/fov，DESIGN §4.7）") {
    Camera camera{CameraConfig{}};

    camera.zoom(1000.0);  // 疯狂拉近
    CHECK(camera.orbit_distance() == doctest::Approx(8.0));
    camera.zoom(-1000.0);  // 疯狂推远
    CHECK(camera.orbit_distance() == doctest::Approx(80.0));

    camera.orbit(0.0, 1000.0);
    // 注意：DESIGN §4.7 的 polar 界限 0.05 是**弧度**（≈2.8648°），Config 用角度表示
    const double polar_min_deg = ehe::core::rad_to_deg(ehe::core::kPolarMin);
    const double polar_max_deg = ehe::core::rad_to_deg(ehe::core::kPolarMax);
    CHECK(camera.orbit_polar_deg() == doctest::Approx(polar_max_deg).epsilon(1e-9));
    camera.orbit(0.0, -1000.0);
    CHECK(camera.orbit_polar_deg() == doctest::Approx(polar_min_deg).epsilon(1e-9));

    camera.set_fov_deg(5.0);
    CHECK(camera.fov_deg() == doctest::Approx(20.0));
    camera.set_fov_deg(179.0);
    CHECK(camera.fov_deg() == doctest::Approx(90.0));
}

TEST_CASE("camera: 模式切换保持视线方向连续（§4.7）") {
    CameraConfig config;
    config.dist = 18.0;
    config.azim_deg = 45.0;
    config.polar_deg = 70.0;
    Camera camera(config);

    // 正面视线方向
    const auto forward_before = camera.forward();
    // 转到自由飞行再切回轨道
    camera.set_mode(CameraMode::Fly);
    const auto forward_fly = camera.forward();
    CHECK(glm::length(forward_before - forward_fly) < 1e-9);

    camera.set_mode(CameraMode::Orbit);
    const auto forward_after = camera.forward();
    CHECK(glm::length(forward_before - forward_after) < 1e-9);
    CHECK(camera.orbit_distance() == doctest::Approx(18.0).epsilon(1e-9));
}

TEST_CASE("camera: 自由飞行交互（移动/俯仰钳制）") {
    CameraConfig config;
    config.mode = ehe::core::CameraMode::Fly;
    config.dist = 15.0;
    config.polar_deg = 75.0;
    Camera camera(config);

    const auto start = camera.position();
    camera.fly_move({0.0, 0.0, 1.0});  // 沿视线前向移动 1 单位
    const auto moved = camera.position();
    CHECK(glm::length(moved - start) == doctest::Approx(1.0).epsilon(1e-9));

    camera.fly_look(0.0, 1000.0);
    CHECK(glm::length(camera.forward()) == doctest::Approx(1.0).epsilon(1e-12));
    // 俯仰被钳制在 ±89°：forward.z 不得达到 1
    CHECK(camera.forward().z < 0.9999);
}
