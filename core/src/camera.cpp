#include "ehe/core/camera.h"

#include <cmath>

namespace ehe::core {
namespace {

/// 世界"上"方向（数学约定：+Z 为自旋轴，DESIGN §4.1 使用 Kerr-Schild 笛卡尔坐标）
const Vec3 kWorldUp{0.0, 0.0, 1.0};

/// 由偏航/俯仰构造单位视线方向（yaw 绕 +Z，pitch 自 XY 平面量起）
Vec3 forward_from_angles(double yaw_rad, double pitch_rad) {
    const double cos_pitch = std::cos(pitch_rad);
    return Vec3{cos_pitch * std::cos(yaw_rad), cos_pitch * std::sin(yaw_rad), std::sin(pitch_rad)};
}

/// 由视线方向反解偏航/俯仰
void angles_from_forward(const Vec3& forward, double& yaw_deg, double& pitch_deg) {
    const double pitch = std::asin(clamp_d(forward.z, -1.0, 1.0));
    const double yaw = std::atan2(forward.y, forward.x);
    yaw_deg = rad_to_deg(yaw);
    pitch_deg = rad_to_deg(pitch);
}

}  // namespace

// ---------------------------------------------------------------- 状态同步

void Camera::apply(const CameraConfig& config) {
    fov_deg_ = clamp_d(config.fov_deg, kFovMinDeg, kFovMaxDeg);

    if (config.mode != mode_) {
        // 模式切换：先把当前模式的状态落盘，再按新模式的参数重建，保持视线方向连续
        mode_ = config.mode;
    }

    if (mode_ == CameraMode::Orbit) {
        dist_ = clamp_d(config.dist, kOrbitDistanceMin, kOrbitDistanceMax);
        azim_deg_ = config.azim_deg;
        polar_deg_ = clamp_d(config.polar_deg, rad_to_deg(kPolarMin), rad_to_deg(kPolarMax));
        sync_free_fly_from_orbit();
    } else {
        // 自由飞行模式下 Config 的 dist/azim/polar 作为初始位置来源
        const double dist = clamp_d(config.dist, kOrbitDistanceMin, kOrbitDistanceMax);
        fly_position_ = spherical_to_cartesian(dist, deg_to_rad(config.azim_deg),
                                              deg_to_rad(clamp_d(config.polar_deg,
                                                                 rad_to_deg(kPolarMin),
                                                                 rad_to_deg(kPolarMax))));
        Vec3 forward = -glm::normalize(fly_position_);
        angles_from_forward(forward, yaw_deg_, pitch_deg_);
    }
}

void Camera::write_to(CameraConfig& config) const {
    config.mode = mode_;
    config.fov_deg = fov_deg_;
    if (mode_ == CameraMode::Orbit) {
        config.dist = dist_;
        config.azim_deg = azim_deg_;
        config.polar_deg = polar_deg_;
    } else {
        const double radius = glm::length(fly_position_);
        config.dist = clamp_d(radius, kOrbitDistanceMin, kOrbitDistanceMax);
        if (radius > 1e-9) {
            config.azim_deg = rad_to_deg(std::atan2(fly_position_.y, fly_position_.x));
            config.polar_deg = rad_to_deg(safe_acos(fly_position_.z / radius));
        }
    }
}

void Camera::set_mode(CameraMode mode) {
    if (mode == mode_) {
        return;
    }
    const Vec3 keep_forward = forward();
    mode_ = mode;
    if (mode_ == CameraMode::Orbit) {
        sync_orbit_from_free_fly();
    } else {
        sync_free_fly_from_orbit();
    }
    // 视线方向尽可能连续（位置切换会导致轻微跳变，方向保持一致是优先项）
    Vec3 restored = keep_forward;
    const double length = glm::length(restored);
    if (length > 1e-9) {
        restored /= length;
        angles_from_forward(restored, yaw_deg_, pitch_deg_);
        // 仅在轨道模式下需要重新约束极角（自由飞行无此限制）
        if (mode_ == CameraMode::Orbit) {
            sync_free_fly_from_orbit();
        }
    }
}

void Camera::sync_free_fly_from_orbit() {
    fly_position_ = spherical_to_cartesian(dist_, deg_to_rad(azim_deg_), deg_to_rad(polar_deg_));
    const Vec3 forward = glm::length(fly_position_) > 1e-9 ? -glm::normalize(fly_position_)
                                                          : Vec3{1.0, 0.0, 0.0};
    angles_from_forward(forward, yaw_deg_, pitch_deg_);
}

void Camera::sync_orbit_from_free_fly() {
    const double radius = glm::length(fly_position_);
    dist_ = clamp_d(radius, kOrbitDistanceMin, kOrbitDistanceMax);
    if (radius > 1e-9) {
        azim_deg_ = rad_to_deg(std::atan2(fly_position_.y, fly_position_.x));
        polar_deg_ = clamp_d(rad_to_deg(safe_acos(fly_position_.z / radius)),
                             rad_to_deg(kPolarMin), rad_to_deg(kPolarMax));
    }
}

// ---------------------------------------------------------------- 状态查询

Vec3 Camera::position() const {
    if (mode_ == CameraMode::Orbit) {
        return spherical_to_cartesian(dist_, deg_to_rad(azim_deg_), deg_to_rad(polar_deg_));
    }
    return fly_position_;
}

Vec3 Camera::forward() const {
    if (mode_ == CameraMode::Orbit) {
        const Vec3 to_origin = -position();
        const double length = glm::length(to_origin);
        return length > 1e-12 ? to_origin / length : Vec3{1.0, 0.0, 0.0};
    }
    return forward_from_angles(deg_to_rad(yaw_deg_), deg_to_rad(pitch_deg_));
}

Vec3 Camera::right() const {
    const Vec3 f = forward();
    Vec3 r = glm::cross(f, kWorldUp);
    const double length = glm::length(r);
    if (length < 1e-9) {
        // 视线与世界上方向平行（极点）：任取一个与 f 正交的方向，避免退化
        const Vec3 fallback = std::abs(f.x) < 0.9 ? Vec3{1.0, 0.0, 0.0} : Vec3{0.0, 1.0, 0.0};
        return glm::normalize(glm::cross(f, fallback));
    }
    return r / length;
}

Vec3 Camera::up() const { return glm::normalize(glm::cross(right(), forward())); }

void Camera::set_fov_deg(double degrees) { fov_deg_ = clamp_d(degrees, kFovMinDeg, kFovMaxDeg); }

double Camera::tan_half_fov() const { return std::tan(deg_to_rad(fov_deg_) * 0.5); }

// ---------------------------------------------------------------- 交互

void Camera::orbit(double delta_azim_deg, double delta_polar_deg) {
    if (mode_ != CameraMode::Orbit) {
        return;
    }
    azim_deg_ += delta_azim_deg;
    if (azim_deg_ > 360.0 || azim_deg_ < -360.0) {
        azim_deg_ = std::fmod(azim_deg_, 360.0);  // azim 环绕无限制，仅做数值收敛
    }
    polar_deg_ = clamp_d(polar_deg_ + delta_polar_deg, rad_to_deg(kPolarMin), rad_to_deg(kPolarMax));
    sync_free_fly_from_orbit();
}

void Camera::zoom(double wheel_steps) {
    if (mode_ != CameraMode::Orbit) {
        return;
    }
    // 对数缩放：每格 ×0.9，手感均匀
    dist_ = clamp_d(dist_ * std::pow(0.9, wheel_steps), kOrbitDistanceMin, kOrbitDistanceMax);
    sync_free_fly_from_orbit();
}

void Camera::fly_move(const Vec3& local_delta) {
    if (mode_ != CameraMode::Fly) {
        return;
    }
    fly_position_ += right() * local_delta.x + up() * local_delta.y + forward() * local_delta.z;
}

void Camera::fly_look(double delta_yaw_deg, double delta_pitch_deg) {
    if (mode_ != CameraMode::Fly) {
        return;
    }
    yaw_deg_ += delta_yaw_deg;
    pitch_deg_ = clamp_d(pitch_deg_ + delta_pitch_deg, -89.0, 89.0);  // §4.7 俯仰钳制
}

// ---------------------------------------------------------------- 光线生成

Camera::RayBasis Camera::ray_basis(double aspect) const {
    RayBasis basis;
    basis.origin = position();
    basis.forward = forward();
    basis.right = right();
    basis.up = up();
    basis.tan_half_fov = tan_half_fov();
    basis.aspect = (aspect > 1e-6) ? aspect : 1.0;
    return basis;
}

Vec3 Camera::ray_direction(double ndc_x, double ndc_y, double aspect) const {
    const RayBasis basis = ray_basis(aspect);
    // §4.2：n̂ = normalize( e_fwd + s_x·tan(fov/2)·aspect·e_x + s_y·tan(fov/2)·e_y )
    const Vec3 direction = basis.forward + basis.right * (ndc_x * basis.tan_half_fov * basis.aspect) +
                           basis.up * (ndc_y * basis.tan_half_fov);
    const double length = glm::length(direction);
    return length > 1e-12 ? direction / length : basis.forward;
}

Camera::Tetrad Camera::static_tetrad_flat() const {
    // 度规无关的笛卡尔正交基：在 a=0 且远场（g → η）时即为静态观测者 tetrad。
    // T1.2 引入 Kerr-Schild 度规后，此处替换为按 g 做 Gram–Schmidt 的实现（接口不变）。
    const Vec3 f = forward();
    const Vec3 r = right();
    const Vec3 u = up();

    Tetrad tetrad;
    tetrad.e_t = Vec4{1.0, 0.0, 0.0, 0.0};        // u_obs = (1,0,0,0)（η 归一化静态观测者）
    tetrad.e_x = Vec4{0.0, r.x, r.y, r.z};
    tetrad.e_y = Vec4{0.0, u.x, u.y, u.z};
    tetrad.e_z = Vec4{0.0, f.x, f.y, f.z};        // 前向空间基（指向黑洞）
    return tetrad;
}

}  // namespace ehe::core
