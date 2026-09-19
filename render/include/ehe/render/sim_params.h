#pragma once

// EHE render —— SimParams UBO 的 C++ 镜像（DESIGN §5.3，std140）
//
// ⚠ 与 shaders/common/simparams.glsl **逐字段对齐**（顺序不可改，字段可增）：
//   offset 0–9 为 §5.3 原布局；offset 10（extras）为 V5.5 追加：
//     x = emission_scale（V5.4 规格缺口②：发射需绝对尺度归一）
//     y = thickness_scale（盘半厚度系数 σ_d = k·r，避免把 §4.5 的常数写死在 shader）
//     z = debug_view（§5.5 五种调试视图之一）
//     w = disk_noise（盘湍流噪声振幅，0 = 关闭；V5.8 追加，对应 Config.blackhole.disk_noise）
//
// 上传方式（双后端对照，§5.4.1）：GL 用 glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo)；
//   VK 用 descriptor set 0 binding 0。两侧必须使用同一个 sizeof(SimParams) 校验。

#include <cstdint>

#include "ehe/core/camera.h"
#include "ehe/core/config.h"
#include "ehe/core/math.h"

namespace ehe::render {

/// 调试视图编号（与 simparams.glsl 的 EHE_VIEW_* 一致）
enum class DebugView : std::uint32_t {
    Shaded = 0,
    Steps = 1,
    Classify = 2,
    GFactor = 3,
    NullDrift = 4,
};

/// flags.x 位定义（与 simparams.glsl 的 EHE_FLAG_* 一致）
enum SimFlags : std::uint32_t {
    kFlagFsr1 = 1U << 0,
    kFlagFxaa = 1U << 1,
    kFlagAces = 1U << 2,
    kFlagParticle = 1U << 3,       ///< 渲染模式 = 粒子（独立显示，跳过 raymarch 背景）
    kFlagFp32Only = 1U << 4,
    kFlagDiskEnabled = 1U << 5,
    kFlagParticleEnabled = 1U << 6,  ///< 粒子启用（raymarch 模式下叠加显示，§5.6）
    kFlagAnimate = 1U << 7,          ///< 动画推进（time 随帧推进 + 粒子积分步进；app 层置位）
};

/// std140 布局的 UBO 数据（每个 vec4 = 16 字节，总 176 字节）
struct SimParams {
    // [0]
    float cam_pos[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    // [1][2][3] tetrad 空间基（w 留空）
    float cam_basis_x[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    float cam_basis_y[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    float cam_basis_z[4] = {0.0F, 0.0F, 1.0F, 0.0F};
    // [4] 视线方向 + tan(fov/2)
    float cam_dir_fov[4] = {0.0F, 0.0F, -1.0F, 0.36397F};
    // [5] aspect / time / n_max / h0
    float frame[4] = {1.7778F, 0.0F, 300.0F, 0.05F};
    // [6] h_min / h_max / eps_h / r_escape
    float steps[4] = {1e-3F, 0.5F, 0.01F, 100.0F};
    // [7] spin_a / r_in / r_out / disk_density
    float hole[4] = {0.0F, 6.0F, 20.0F, 1.0F};
    // [8] t_scale / kappa / exposure / chrom_ab
    float disk[4] = {10000.0F, 2.0F, 1.0F, 0.5F};
    // [9] flags / res_scale / particle_count / particle_size
    /// w = particle_size（V5.18 启用原 pad 槽位：位打包 float，§5.6 面板参数「大小」）
    std::uint32_t flags[4] = {kFlagDiskEnabled, 0x3F800000U /* 1.0f */, 0U, 0U};
    // [10] emission_scale / thickness_scale / debug_view / pad
    float extras[4] = {2.5F, 0.1F, 0.0F, 0.0F};
};

static_assert(sizeof(SimParams) == 11 * 16, "SimParams 必须严格等于 11 个 std140 vec4（176 字节）");

/// std140 的一个 vec4 槽位在 shader 侧是 `uvec4`，其中 flags.y/z 需要携带浮点量
/// （res_scale、particle_count）。C++ 侧用位打包写入，shader 侧用 uintBitsToFloat 读出。
std::uint32_t pack_float(float value);
float unpack_float(std::uint32_t bits);

/// 依据 Config 与相机状态组装 UBO
/// @param camera       相机（提供位置与 tetrad 基）
/// @param camera_forward 视线中心方向（局部笛卡尔分量）
/// @param aspect       宽高比
/// @param time         动画时间（秒；M1 恒为 0，T1.4 起驱动噪声动画）
SimParams make_sim_params(const core::Config& config, const core::Camera& camera,
                          const core::Vec3& camera_forward, double aspect, double time);

// ---------------------------------------------------------------- 后处理 UBO（§4.6）

/// 分辨率变换 pass 的参数（`shaders/post_resolve.frag`，std140 binding = 3）
struct PostParams {
    std::int32_t mode_and_src[4] = {0, 0, 0, 1};  ///< x = mode(0 box/1 up/2 copy), y = src_w, z = src_h, w = 保留
    std::int32_t dst_size[4] = {0, 0, 0, 0};      ///< x = dst_w, y = dst_h
};
static_assert(sizeof(PostParams) == 32, "PostParams 必须是 2 个 std140 ivec4（32 字节）");

/// 最终合成 pass 的参数（`shaders/post_final.frag`，std140 binding = 4）
struct FinalParams {
    float exposure_and_chroma[4] = {1.0F, 0.0F, 0.0F, 0.0F};  ///< x = 曝光, y = 色差强度
    float flags[4] = {1.0F, 0.0F, 0.0F, 0.0F};               ///< x = 1 启用 ACES
};
static_assert(sizeof(FinalParams) == 32, "FinalParams 必须是 2 个 std140 vec4（32 字节）");

/// 设置 flags 中的某一位
void set_flag(SimParams& params, std::uint32_t flag, bool enabled);
bool has_flag(const SimParams& params, std::uint32_t flag);

/// 设置调试视图
void set_debug_view(SimParams& params, DebugView view);
DebugView debug_view_of(const SimParams& params);

}  // namespace ehe::render
