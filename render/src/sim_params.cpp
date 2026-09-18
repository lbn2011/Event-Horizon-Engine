#include "ehe/render/sim_params.h"

#include <cmath>
#include <cstring>

#include "ehe/core/camera.h"
#include "ehe/core/units.h"

namespace ehe::render {
namespace {

void store_vec3(float (&target)[4], const core::Vec3& value) {
    target[0] = static_cast<float>(value.x);
    target[1] = static_cast<float>(value.y);
    target[2] = static_cast<float>(value.z);
    target[3] = 0.0F;
}

}  // namespace

std::uint32_t pack_float(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float unpack_float(std::uint32_t bits) {
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

SimParams make_sim_params(const core::Config& config, const core::Camera& camera,
                          const core::Vec3& camera_forward, double aspect, double time) {
    SimParams params;

    const core::Vec3 position = camera.position();
    store_vec3(params.cam_pos, position);

    // tetrad 空间基：与 core 的光线构造保持一致（§4.2）
    const core::Vec3 forward = glm::normalize(camera_forward);
    core::Vec3 right = glm::cross(forward, core::Vec3{0.0, 0.0, 1.0});
    if (glm::length(right) < 1e-9) {
        right = glm::cross(forward, core::Vec3{1.0, 0.0, 0.0});
    }
    right = glm::normalize(right);
    const core::Vec3 up = glm::normalize(glm::cross(right, forward));

    store_vec3(params.cam_basis_x, right);
    store_vec3(params.cam_basis_y, up);
    store_vec3(params.cam_basis_z, forward);
    store_vec3(params.cam_dir_fov, forward);
    params.cam_dir_fov[3] = static_cast<float>(std::tan(core::deg_to_rad(camera.fov_deg()) * 0.5));

    params.frame[0] = static_cast<float>(aspect);
    params.frame[1] = static_cast<float>(time);
    params.frame[2] = static_cast<float>(config.integrator.n_max);
    params.frame[3] = static_cast<float>(config.integrator.h0);

    params.steps[0] = static_cast<float>(config.integrator.h_min);
    params.steps[1] = static_cast<float>(config.integrator.h_max);
    params.steps[2] = static_cast<float>(core::units::kHorizonEpsilon);
    params.steps[3] = static_cast<float>(core::units::kEscapeRadius);

    params.hole[0] = static_cast<float>(config.blackhole.spin);
    params.hole[1] = static_cast<float>(config.blackhole.disk_r_in);
    params.hole[2] = static_cast<float>(config.blackhole.disk_r_out);
    params.hole[3] = static_cast<float>(config.blackhole.disk_density);

    params.disk[0] = static_cast<float>(config.blackhole.disk_t_scale);
    params.disk[1] = static_cast<float>(config.blackhole.disk_kappa);
    params.disk[2] = static_cast<float>(config.post.exposure);
    params.disk[3] = static_cast<float>(config.post.chrom_ab);

    std::uint32_t flags = 0;
    if (config.render.fsr1) {
        flags |= kFlagFsr1;
    }
    if (config.render.fxaa) {
        flags |= kFlagFxaa;
    }
    if (config.post.aces) {
        flags |= kFlagAces;
    }
    if (config.render.mode == core::RenderMode::Particle) {
        flags |= kFlagParticle;
    }
    if (config.integrator.precision == core::PrecisionMode::Fp32) {
        flags |= kFlagFp32Only;
    }
    if (config.blackhole.disk_density > 0.0) {
        flags |= kFlagDiskEnabled;
    }
    params.flags[0] = flags;
    params.flags[1] = pack_float(static_cast<float>(config.render.res_scale));   // 位打包（见头文件说明）
    params.flags[2] = pack_float(static_cast<float>(config.particle.count));

    params.extras[0] = 2.5F;    // emission_scale：V5.4 的数值归一（与 golden 默认一致）
    params.extras[1] = 0.1F;    // thickness_scale：σ_d = 0.1·r（§4.5）
    params.extras[2] = 0.0F;    // debug_view = Shaded
    // V5.8：盘湍流噪声振幅（0 = 关闭；golden 基线用 0，见 Config::BlackHoleParams::disk_noise）
    params.extras[3] = static_cast<float>(config.blackhole.disk_noise);
    return params;
}

void set_flag(SimParams& params, std::uint32_t flag, bool enabled) {
    if (enabled) {
        params.flags[0] |= flag;
    } else {
        params.flags[0] &= ~flag;
    }
}

bool has_flag(const SimParams& params, std::uint32_t flag) {
    return (params.flags[0] & flag) != 0U;
}

void set_debug_view(SimParams& params, DebugView view) {
    params.extras[2] = static_cast<float>(static_cast<std::uint32_t>(view));
}

DebugView debug_view_of(const SimParams& params) {
    return static_cast<DebugView>(static_cast<std::uint32_t>(params.extras[2] + 0.5F));
}

}  // namespace ehe::render
