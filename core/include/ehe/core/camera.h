#pragma once

// EHE core —— 相机（DESIGN §4.7 相机模块规格、§4.2 光线初始条件）
//
// 双模式：
//   - 轨道（默认）：球坐标 (dist, azim, polar) 绕黑洞；dist ∈ [8,80] 对数缩放，
//     polar ∈ [0.05, π−0.05]（防极点万向锁），azim 无限制；
//   - 自由飞行：位置 + 偏航/俯仰（俯仰 ±89° 钳制）。
// 切换模式时保持视线方向连续（§4.7 建议默认）。
//
// 精度：全程 fp64（CPU 侧无性能顾虑，DESIGN §4.7）；上传 UBO 时转 fp32。

#include "ehe/core/config.h"
#include "ehe/core/math.h"

namespace ehe::core {

class Camera {
public:
    Camera() = default;
    explicit Camera(const CameraConfig& config) { apply(config); }

    /// 从 Config 同步状态（模式切换时保持视线方向连续）
    void apply(const CameraConfig& config);

    /// 把当前状态回写到 Config（UI 持久化用）
    void write_to(CameraConfig& config) const;

    CameraMode mode() const { return mode_; }
    void set_mode(CameraMode mode);

    // ---------------------------------------------------------------- 状态查询
    Vec3 position() const;
    Vec3 forward() const;  ///< 单位视线方向（指向场景）
    Vec3 right() const;    ///< 与 forward 正交的右向（右手系：right = normalize(cross(forward, world_up))）
    Vec3 up() const;       ///< right × forward

    double fov_deg() const { return fov_deg_; }
    void set_fov_deg(double degrees);
    double tan_half_fov() const;

    // 轨道参数（仅轨道模式有意义）
    double orbit_distance() const { return dist_; }
    double orbit_azimuth_deg() const { return azim_deg_; }
    double orbit_polar_deg() const { return polar_deg_; }

    // ---------------------------------------------------------------- 交互操作
    /// 轨道旋转（左键拖拽；polar 自动钳制）
    void orbit(double delta_azim_deg, double delta_polar_deg);

    /// 轨道缩放（滚轮；对数缩放，自动钳制到 [8, 80]）
    void zoom(double wheel_steps);

    /// 自由飞行平移：local_delta 为相机局部坐标系下的位移方向（已含速度与 dt 的乘积）
    void fly_move(const Vec3& local_delta);

    /// 自由飞行转视角（右键拖拽；俯仰钳制 ±89°）
    void fly_look(double delta_yaw_deg, double delta_pitch_deg);

    // ---------------------------------------------------------------- 光线生成（§4.2）
    struct RayBasis {
        Vec3 origin;
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        double tan_half_fov = 0.0;
        double aspect = 1.0;
    };

    /// 组装光线基矢（app/render 每帧调用；aspect = width / height）
    RayBasis ray_basis(double aspect) const;

    /// 像素 NDC (s_x, s_y ∈ [−1,1]) → 单位空间方向 n̂（§4.2 公式）；
    /// 中心像素 (0,0) 恰为 forward
    Vec3 ray_direction(double ndc_x, double ndc_y, double aspect) const;

    /// 静态观测者 tetrad（§4.2）
    /// 说明：笛卡尔正交基在 a=0 且远场时即 η 归一化的 tetrad；
    ///       完整的「按 Kerr-Schild 度量做 Gram–Schmidt」版本依赖度规模块，随 T1.2 接入
    ///       （接口保持稳定，届时仅替换实现）。
    struct Tetrad {
        Vec4 e_t;  ///< 时间基（u_obs 方向）
        Vec4 e_x;  ///< 与视线前向对齐的空间基
        Vec4 e_y;
        Vec4 e_z;
    };
    Tetrad static_tetrad_flat() const;

private:
    void sync_free_fly_from_orbit();   ///< 轨道 → 自由飞行（保持视线方向）
    void sync_orbit_from_free_fly();   ///< 自由飞行 → 轨道（保持视线方向）

    CameraMode mode_ = CameraMode::Orbit;

    // 轨道状态
    double dist_ = 15.0;
    double azim_deg_ = 0.0;
    double polar_deg_ = 75.0;

    // 自由飞行状态
    Vec3 fly_position_{0.0, 0.0, 15.0};
    double yaw_deg_ = 0.0;
    double pitch_deg_ = 0.0;

    double fov_deg_ = kFovDefaultDeg;
};

}  // namespace ehe::core
