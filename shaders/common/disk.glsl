// EHE —— 吸积盘介质与相对论效应（GLSL 450）
//
// ⚠ 逐行移植自 core/src/golden.cpp 的盘相关函数；核心的解析式来自 DESIGN §4.5。
//   盘温单位是**开尔文**（V5.4 修正），LUT 索引映射必须与 core/blackbody.cpp 完全一致。

#ifndef EHE_DISK_GLSL
#define EHE_DISK_GLSL

// 黑体 LUT 纹理（256×1，RGB，线性采样）
layout(binding = 1) uniform sampler2D blackbody_lut;

#define EHE_LUT_SIZE 256
#define EHE_LUT_T_MIN 1000.0
#define EHE_LUT_T_MAX 40000.0

// 无扭矩内边界的通量剖面 F(r) ∝ (1 − √(r_in/r)) / r³，归一化到内边界附近的峰值
EHE_REAL raw_flux(EHE_REAL r, EHE_REAL r_in) {
    return (1.0 - sqrt(r_in / r)) / (r * r * r);
}

EHE_REAL disk_flux_profile(EHE_REAL r, EHE_REAL r_in) {
    if (r <= r_in) {
        return 0.0;
    }
    EHE_REAL reference = raw_flux(r_in * 1.5, r_in);
    return raw_flux(r, r_in) / reference;
}

// T(r) = T_scale · F(r)^{1/4}
EHE_REAL disk_temperature(EHE_REAL r, EHE_REAL r_in, EHE_REAL t_scale) {
    EHE_REAL flux = disk_flux_profile(r, r_in);
    if (flux <= 0.0) {
        return 0.0;
    }
    // 0.25 次幂用两次 sqrt 实现：GLSL 的 fp64 只保证算术与 sqrt（exp/log/pow 的 EHE_REAL 重载不可用，
    // 见 tests/src/test_shaders.cpp 的 fp64 探测用例与 DESIGN §5.8 的精度约束）
    return t_scale * sqrt(sqrt(flux));
}

// 高斯厚度 + 径向区间截断（|z| > 3σ 视为无介质，采样加速）
//
// 精度说明：垂直高斯包络是**平滑**量，float 精度远超需要；而 exp 的 EHE_REAL 重载在 GLSL
// fp64 下不可用（§5.8 约束），故此处刻意在 float 侧完成指数运算，再回到 EHE_REAL 参与合成。
EHE_REAL disk_density(EHE_REAL r, EHE_REAL z, EHE_REAL r_in, EHE_REAL r_out, EHE_REAL density, EHE_REAL thickness_scale) {
    if (r < r_in || r > r_out || density <= 0.0) {
        return 0.0;
    }
    EHE_REAL sigma = thickness_scale * r;
    if (sigma <= 0.0) {
        return 0.0;
    }
    if (z * z > 9.0 * sigma * sigma) {
        return 0.0;
    }
    float zf = float(z);
    float sigma_f = float(sigma);
    float vertical = exp(-(zf * zf) / (2.0 * sigma_f * sigma_f));
    return density * EHE_REAL(vertical) * disk_flux_profile(r, r_in);
}

// 赤道圆轨道四速度（a = 0）：Ω = r^{−3/2}，u^t = 1/√(1−3/r)
EHE_REAL4 disk_four_velocity(EHE_REAL3 position, EHE_REAL r) {
    EHE_REAL denom = 1.0 - 3.0 / r;
    if (denom <= 0.0) {
        return EHE_REAL4(0.0);
    }
    EHE_REAL omega = 1.0 / (r * sqrt(r));
    EHE_REAL u_t = 1.0 / sqrt(denom);
    EHE_REAL u_phi = omega * u_t;
    return EHE_REAL4(u_t, -position.y * u_phi, position.x * u_phi, 0.0);
}

// 红移因子 g = (k·u)_obs / (k·u)_em（§4.5）
EHE_REAL redshift_factor(EHE_REAL k_dot_u_obs, KsPoint point, EHE_REAL4 k, EHE_REAL4 u_em) {
    EHE_REAL k_dot_u_em = metric_dot(point, k, u_em);
    if (abs(k_dot_u_em) < 1e-12) {
        return 1.0;
    }
    return k_dot_u_obs / k_dot_u_em;
}

// 温度 → 线性 sRGB（与 core/blackbody.cpp 的 lut_index_for_temperature 同构：对数等距 + 线性插值）
//
// 精度说明：索引映射只是把温度投到 256 档 LUT 上，float 精度绰绰有余；
// 且 log 的 EHE_REAL 重载在 GLSL fp64 下不可用（§5.8 约束），故整体在 float 侧完成。
vec3 sample_blackbody(EHE_REAL temperature) {
    float clamped = clamp(float(temperature), float(EHE_LUT_T_MIN), float(EHE_LUT_T_MAX));
    float t = (log(clamped) - log(float(EHE_LUT_T_MIN))) /
              (log(float(EHE_LUT_T_MAX)) - log(float(EHE_LUT_T_MIN)));
    float index = t * float(EHE_LUT_SIZE - 1);
    float u = (index + 0.5) / float(EHE_LUT_SIZE);
    return texture(blackbody_lut, vec2(u, 0.5)).rgb;
}

#endif  // EHE_DISK_GLSL
