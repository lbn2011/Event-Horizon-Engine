// EHE —— 主 raymarch 片元着色器（GLSL 450）
//
// 流程（DESIGN §5.4 步骤 1）：
//   像素 → 局部方向（§4.2）→ 观测者 tetrad → 类光波矢 →
//   RK4 + 自适应步长积分（§4.3）→ 盘体发射-吸收前向合成（§4.5）→ 调试视图分支（§5.5）
//
// ⚠ 与 core/src/golden.cpp 的 render_golden 逐行对应：同样的采样点（步长中点）、
//   同样的终止条件顺序、同样的 g 因子与发射公式。任何一侧改动都必须同步，
//   由 shader↔CPU 一致性（L1/L2）与目标机 NMSE 差分（§6.2）双重把关。

#version 450
// 双精度内建函数重载（pow/exp/log/sqrt 的 EHE_REAL 版本）需显式开启该扩展；
// 仅靠算术运算符不需要，因此缺少本行时症状是"某个数学函数找不到重载"。
#extension GL_ARB_gpu_shader_fp64 : enable

#include "common/simparams.glsl"
#include "common/metric.glsl"
#include "common/disk.glsl"
#include "common/noise.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_ndc;
layout(location = 0) out vec4 out_color;

// 循环上界取编译期常量（便于驱动展开/优化）；运行时由 UBO 的 n_max 生效
const int EHE_MAX_STEPS = 2048;

// v1 背景：纯色（与 golden 的 (0.02, 0.02, 0.03) 一致）；M2 起替换为星空 cubemap（§4.6）
const vec3 EHE_BACKGROUND = vec3(0.02, 0.02, 0.03);

// 盘湍流噪声的空间频率（每单位长度的噪声周期数）。盘半径区间 [6, 20]，
// 取 0.6 得到约 1.7 个长度单位的特征尺寸——比盘厚（σ=0.1r）大、比盘半径小，视觉上合适。
// 不做成 Config 项：§8.1 的 schema 未列该键，保持参数面板简洁；如需调，改这里即可。
#define EHE_NOISE_SCALE 0.6

// 终止结局（与 CPU 侧 TraceOutcome 对齐）
const int EHE_OUTCOME_CAPTURED = 0;
const int EHE_OUTCOME_ESCAPED = 1;
const int EHE_OUTCOME_EXHAUSTED = 2;

// 自适应步长 h = clamp(h₀(r − r₊)/r₊, h_min, h_max)（§4.3）
EHE_REAL adaptive_step(EHE_REAL r, EHE_REAL spin, EHE_REAL h0, EHE_REAL h_min, EHE_REAL h_max) {
    EHE_REAL r_plus = 1.0 + sqrt(1.0 - spin * spin);
    EHE_REAL f = (r - r_plus) / r_plus;
    return clamp(h0 * f, h_min, h_max);
}

// 光子测地线 RK4 单步（与 core/integrator.cpp 的 rk4_step 一致）
void rk4_step(inout EHE_REAL4 x, inout EHE_REAL4 k, EHE_REAL h, EHE_REAL spin) {
    EHE_REAL4 dx1 = k;
    EHE_REAL4 dk1 = geodesic_accel(ks_evaluate(x.yzw, spin), k);

    EHE_REAL4 x2 = x + EHE_REAL4(0.5 * h) * dx1;
    EHE_REAL4 k2 = k + EHE_REAL4(0.5 * h) * dk1;
    EHE_REAL4 dx2 = k2;
    EHE_REAL4 dk2 = geodesic_accel(ks_evaluate(x2.yzw, spin), k2);

    EHE_REAL4 x3 = x + EHE_REAL4(0.5 * h) * dx2;
    EHE_REAL4 k3 = k + EHE_REAL4(0.5 * h) * dk2;
    EHE_REAL4 dx3 = k3;
    EHE_REAL4 dk3 = geodesic_accel(ks_evaluate(x3.yzw, spin), k3);

    EHE_REAL4 x4 = x + EHE_REAL4(h) * dx3;
    EHE_REAL4 k4 = k + EHE_REAL4(h) * dk3;
    EHE_REAL4 dx4 = k4;
    EHE_REAL4 dk4 = geodesic_accel(ks_evaluate(x4.yzw, spin), k4);

    x = x + (EHE_REAL4(h) / 6.0) * (dx1 + 2.0 * dx2 + 2.0 * dx3 + dx4);
    k = k + (EHE_REAL4(h) / 6.0) * (dk1 + 2.0 * dk2 + 2.0 * dk3 + dk4);
}

// g 因子伪彩（§5.5）
vec3 g_factor_pseudocolor(EHE_REAL g) {
    EHE_REAL t = clamp((g - 0.5) / 1.0, 0.0, 1.0);
    return vec3(float(t), float(0.3 * (1.0 - abs(t - 0.5) * 2.0)), float(1.0 - t));
}

// 步数热力图（§5.5）：蓝→青→黄→红
vec3 steps_heatmap(EHE_REAL ratio) {
    EHE_REAL t = clamp(ratio, 0.0, 1.0);
    return vec3(float(clamp(t * 2.0, 0.0, 1.0)),
                float(clamp(t * 2.0 - 0.5, 0.0, 1.0) * (1.0 - clamp(t * 2.0 - 1.0, 0.0, 1.0))),
                float(1.0 - t));
}

void main() {
    // ---------------------------------------------------------------- 参数解包
    const EHE_REAL spin = EHE_REAL(hole.x);
    const EHE_REAL r_in = EHE_REAL(hole.y);
    const EHE_REAL r_out = EHE_REAL(hole.z);
    const EHE_REAL density_param = EHE_REAL(hole.w);
    const EHE_REAL t_scale = EHE_REAL(disk.x);
    const EHE_REAL kappa = EHE_REAL(disk.y);
    const EHE_REAL aspect = EHE_REAL(frame.x);
    const EHE_REAL n_max = frame.z;
    const EHE_REAL h0 = EHE_REAL(frame.w);
    const EHE_REAL h_min = EHE_REAL(steps.x);
    const EHE_REAL h_max = EHE_REAL(steps.y);
    const EHE_REAL eps_h = EHE_REAL(steps.z);
    const EHE_REAL r_escape = EHE_REAL(steps.w);
    const EHE_REAL emission_scale = EHE_REAL(extras.x);
    const EHE_REAL thickness_scale = EHE_REAL(extras.y);
    const int view = int(extras.z + 0.5);
    // V5.8：extras.w = 盘湍流噪声振幅（0 = 关闭，golden 基线即用 0）
    const float noise_amplitude = extras.w;
    const EHE_REAL time_value = EHE_REAL(frame.y);
    const bool disk_enabled = (flags.x & EHE_FLAG_DISK_ENABLED) != 0u;

    // ---------------------------------------------------------------- 光线初始条件（§4.2）
    const EHE_REAL3 cam_position = EHE_REAL3(cam_pos.xyz);
    const EHE_REAL3 forward = EHE_REAL3(cam_basis_z.xyz);
    const EHE_REAL3 right = EHE_REAL3(cam_basis_x.xyz);
    const EHE_REAL3 up = EHE_REAL3(cam_basis_y.xyz);
    const EHE_REAL tan_half_fov = EHE_REAL(cam_dir_fov.w);

    EHE_REAL3 n_hat = normalize(EHE_REAL3(cam_dir_fov.xyz) +
                            EHE_REAL(v_ndc.x) * tan_half_fov * aspect * right +
                            EHE_REAL(v_ndc.y) * tan_half_fov * up);
    // 兜底：极端参数下保持单位长度
    n_hat = normalize(n_hat);

    KsPoint observer_point = ks_evaluate(cam_position, spin);
    ObserverFrame observer = static_observer_frame(observer_point);
    EHE_REAL4 k = photon_from_frame(observer, n_hat);
    const EHE_REAL k_dot_u_obs = metric_dot(observer_point, k, observer.u);

    EHE_REAL4 x = EHE_REAL4(0.0, cam_position);
    const EHE_REAL r_plus = 1.0 + sqrt(1.0 - spin * spin);
    const EHE_REAL capture_radius = r_plus * (1.0 + eps_h);

    // ---------------------------------------------------------------- 主积分循环
    vec3 color = vec3(0.0);
    EHE_REAL alpha = 0.0;
    EHE_REAL max_density = 1e-30;
    EHE_REAL g_at_max_density = 1.0;
    EHE_REAL null_drift = 0.0;
    int outcome = EHE_OUTCOME_EXHAUSTED;
    int step_used = 0;

    for (int step = 0; step < EHE_MAX_STEPS; ++step) {
        step_used = step;
        if (EHE_REAL(step) >= n_max) {
            outcome = EHE_OUTCOME_EXHAUSTED;
            break;
        }

        const EHE_REAL3 position = x.yzw;
        const EHE_REAL r = ks_radius(position, spin);
        const KsPoint point = ks_evaluate(position, spin);
        null_drift = max(null_drift, abs(metric_dot(point, k, k)));

        // ---- 盘体渲染（步长中点采样，发射-吸收前向合成）----
        if (disk_enabled && view != EHE_VIEW_CLASSIFY) {
            const EHE_REAL h = adaptive_step(r, spin, h0, h_min, h_max);
            const EHE_REAL3 mid = position + EHE_REAL3(0.5 * h) * k.yzw;
            const EHE_REAL r_mid = ks_radius(mid, spin);

            // §4.5 采样加速：|z| > 3σ 且步进方向**远离**赤道面时，本步不可能进入盘体 → 免费跳过。
            // （薄盘几何决定的裁剪；阴影轮廓与积分终止逻辑不受影响）
            const EHE_REAL sigma_mid = thickness_scale * r_mid;
            const bool heading_away = (position.z * k.z) > 0.0;
            const bool skip_disk = (abs(mid.z) > 3.0 * sigma_mid) && heading_away;

            EHE_REAL density = skip_disk
                                   ? 0.0
                                   : disk_density(r_mid, mid.z, r_in, r_out, density_param,
                                                  thickness_scale);
            if (density > 1e-6) {
                // 湍流调制（§4.5「调制 ρ 与发射」）。**为什么不能只调制 ρ**：
                // 盘在 κ=2、ρ~1 时是光学厚的，出射强度趋于源函数 S = ε/κ ∝ g³·LUT/κ —— ρ 被约掉，
                // 只调制密度在画面上几乎看不出来（实测振幅 1.0 时 NMSE 仅 4.7e-4 ≈ 像素变化 2%）。
                // 因此噪声主要作用在**发射系数**上（体现湍流引起的发射率起伏），ρ 只做较弱调制（影响不透明度）。
                const float density_noise = max(
                    disk_noise_factor(mid, r_mid, time_value, noise_amplitude * 0.35, EHE_NOISE_SCALE),
                    0.0);
                const float emission_noise = max(
                    disk_noise_factor(mid, r_mid, time_value, noise_amplitude, EHE_NOISE_SCALE * 1.7),
                    0.0);
                density *= EHE_REAL(density_noise);

                const KsPoint mid_point = ks_evaluate(mid, spin);
                const EHE_REAL4 u_em = disk_four_velocity(mid, r_mid);
                const EHE_REAL g = redshift_factor(k_dot_u_obs, mid_point, k, u_em);
                if (density > max_density) {
                    max_density = density;
                    g_at_max_density = g;
                }
                const EHE_REAL temperature = disk_temperature(r_mid, r_in, t_scale) * g;
                const vec3 emitted = sample_blackbody(temperature);
                const EHE_REAL g3 = g * g * g;
                const EHE_REAL luminance =
                    (1.0 - alpha) * emission_scale * density * EHE_REAL(emission_noise) * g3 * h;
                // 吸收项：exp 下沉到 float（§5.8 精度约束）；不影响发射-吸收的物理形态
                const EHE_REAL absorb = 1.0 - EHE_REAL(exp(float(-kappa * density * h)));
                color += vec3(luminance) * emitted;
                alpha += (1.0 - alpha) * absorb;
                if (alpha >= 0.99) {  // 不透明饱和：提前终止
                    outcome = EHE_OUTCOME_ESCAPED;
                    break;
                }
            }
        }

        // ---- 终止条件（§4.3，与 CPU 同序）----
        if (r < capture_radius) {
            outcome = EHE_OUTCOME_CAPTURED;
            break;
        }
        if (r > r_escape) {
            outcome = EHE_OUTCOME_ESCAPED;
            break;
        }

        rk4_step(x, k, adaptive_step(r, spin, h0, h_min, h_max), spin);
    }

    // ---------------------------------------------------------------- 视图输出
    vec3 output_rgb;
    if (view == EHE_VIEW_CLASSIFY) {
        output_rgb = (outcome == EHE_OUTCOME_CAPTURED)  ? vec3(0.0)
                     : (outcome == EHE_OUTCOME_ESCAPED) ? vec3(1.0)
                                                        : vec3(1.0, 0.0, 0.0);
    } else if (view == EHE_VIEW_STEPS) {
        output_rgb = steps_heatmap(EHE_REAL(step_used) / max(1.0, n_max));
    } else if (view == EHE_VIEW_GFACTOR) {
        output_rgb = g_factor_pseudocolor(g_at_max_density);
    } else if (view == EHE_VIEW_NULLDRIFT) {
        EHE_REAL drift = clamp(null_drift / 1e-3, 0.0, 1.0);  // 设计验收阈值 1e-3（§4.4）
        output_rgb = vec3(float(drift));
    } else {
        output_rgb = color;
        if (outcome != EHE_OUTCOME_CAPTURED) {
            output_rgb += (1.0 - float(alpha)) * EHE_BACKGROUND;
        }
    }

    // HDR 原始输出（FP16 附件）：色调映射在后处理链（T1.5）进行
    out_color = vec4(output_rgb, 1.0);
}
