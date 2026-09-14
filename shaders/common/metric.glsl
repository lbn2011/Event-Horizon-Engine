// EHE —— Kerr-Schild 度规内核（GLSL 450，精度可切换）
//
// ⚠ 本文件是 core/src/metric.cpp 的**逐行移植**：任何改动必须两侧同步，
//   由 `ehe_tests` 的 shader 静态校验 + 目标机 NMSE 差分双重把关。
//
// 约定（DESIGN §4.1）：
//   - 几何化单位 M = 1，号差 (−,+,+,+)，坐标 (t, x, y, z)（Kerr-Schild 笛卡尔形式）
//   - g_μν = η_μν + 2H k_μ k_ν，H = r³/(r⁴ + a²z²)，k 关于 η 零
//   - r 由 (x²+y²)/(r²+a²) + z²/r² = 1 隐式定义（**不是欧氏距离**）

#ifndef EHE_METRIC_GLSL
#define EHE_METRIC_GLSL

// ---------------------------------------------------------------- 精度开关（DESIGN §4.3 / §7）
//
// 为什么要开关：**Terascale 及更早的 GPU 没有 fp64 硬件**，双精度由驱动软件模拟，
//   实测在 AMD Radeon HD 7400M 上单帧可达 2.1 s（32² × 60 步）并触发 Windows TDR 驱动复位。
//   目标机（现代独显）有 fp64 硬件，`mixed` 模式可承受。
//   因此：默认 `mixed`（关键路径 fp64）；受限 GPU 与快速迭代用 `fp32`。
//
// 开启方式：渲染器在 `#version` 之后插入 `#define EHE_FP32_ONLY 1`（见 render/gl 与 sim_params 的精度位）。
// 注意：GLSL 的 fp64 **只保证算术与 sqrt**（exp/log/pow 的 double 重载不可用，
//       tests/src/test_shaders.cpp 有探测用例）——故本文件与 disk.glsl 的超越函数一律下沉到 float。
#ifdef EHE_FP32_ONLY
#define EHE_REAL float
#define EHE_REAL3 vec3
#define EHE_REAL4 vec4
#else
#define EHE_REAL double
#define EHE_REAL3 dvec3
#define EHE_REAL4 dvec4
#endif

// η 缩并：η_μν u^μ v^ν = −u_t v_t + u_x v_x + u_y v_y + u_z v_z（分量顺序 t,x,y,z）
EHE_REAL eta_dot(EHE_REAL4 u, EHE_REAL4 v) {
    return -u.x * v.x + u.y * v.y + u.z * v.z + u.w * v.w;
}

// 分量直乘（无度规）：k_μ u^μ
EHE_REAL plain_dot(EHE_REAL4 u, EHE_REAL4 v) {
    return u.x * v.x + u.y * v.y + u.z * v.z + u.w * v.w;
}

// 度规数据（含一阶导数），对应 core 的 KsPoint
struct KsPoint {
    EHE_REAL3 position;
    EHE_REAL r;
    EHE_REAL H;
    EHE_REAL4 k;         // k_μ（协变）
    EHE_REAL4 k_up;      // k^μ（逆变，关于 η）
    EHE_REAL3 dr_dx;     // ∂_i r
    EHE_REAL3 dH_dx;     // ∂_i H
    EHE_REAL4 dk_dx[3];  // ∂_i k_μ（i = x,y,z）
};

// 由空间坐标解 Kerr-Schild 径向坐标（正根）
EHE_REAL ks_radius(EHE_REAL3 position, EHE_REAL spin) {
    EHE_REAL r2_euclid = dot(position, position);
    if (spin == 0.0) {
        return sqrt(r2_euclid);
    }
    EHE_REAL a2 = spin * spin;
    EHE_REAL s = r2_euclid - a2;
    EHE_REAL disc = sqrt(s * s + 4.0 * a2 * position.z * position.z);
    return sqrt(0.5 * (s + disc));
}

KsPoint ks_evaluate(EHE_REAL3 position, EHE_REAL spin) {
    KsPoint p;
    p.position = position;
    EHE_REAL a = spin;
    EHE_REAL a2 = a * a;
    EHE_REAL x = position.x;
    EHE_REAL y = position.y;
    EHE_REAL z = position.z;

    p.r = ks_radius(position, a);
    EHE_REAL r = p.r;
    EHE_REAL r2 = r * r;
    EHE_REAL r3 = r2 * r;

    EHE_REAL denom_h = r2 * r2 + a2 * z * z;
    p.H = r3 / denom_h;

    EHE_REAL denom_q = r2 + a2;
    p.k = EHE_REAL4(1.0, (r * x + a * y) / denom_q, (r * y - a * x) / denom_q, z / r);
    p.k_up = EHE_REAL4(-p.k.x, p.k.y, p.k.z, p.k.w);

    // ∂_i r = 分子/分母（隐函数求导，DESIGN §4.1.1）
    EHE_REAL denom_dr = r * (x * x + y * y) / (denom_q * denom_q) + z * z / r3;
    p.dr_dx = EHE_REAL3(x / denom_q / denom_dr, y / denom_q / denom_dr, z / r2 / denom_dr);

    // ∂_i H = H·(3∂_i r/r − (4r³∂_i r + 2a²z δ_iz)/(r⁴+a²z²))
    EHE_REAL dh_prefix = 4.0 * r3 / denom_h;
    p.dH_dx = EHE_REAL3(
        p.H * (3.0 * p.dr_dx.x / r - dh_prefix * p.dr_dx.x),
        p.H * (3.0 * p.dr_dx.y / r - dh_prefix * p.dr_dx.y),
        p.H * (3.0 * p.dr_dx.z / r - dh_prefix * p.dr_dx.z - 2.0 * a2 * z / denom_h));

    // ∂_i k_μ
    EHE_REAL kx_num = r * x + a * y;
    EHE_REAL ky_num = r * y - a * x;
    EHE_REAL inv_q2 = 1.0 / (denom_q * denom_q);
    for (int i = 0; i < 3; ++i) {
        EHE_REAL dr_i = p.dr_dx[i];
        EHE_REAL delta_ix = (i == 0) ? 1.0 : 0.0;
        EHE_REAL delta_iy = (i == 1) ? 1.0 : 0.0;
        EHE_REAL delta_iz = (i == 2) ? 1.0 : 0.0;
        EHE_REAL dkx =
            ((dr_i * x + r * delta_ix + a * delta_iy) * denom_q - kx_num * 2.0 * r * dr_i) * inv_q2;
        EHE_REAL dky =
            ((dr_i * y + r * delta_iy - a * delta_ix) * denom_q - ky_num * 2.0 * r * dr_i) * inv_q2;
        EHE_REAL dkz = delta_iz / r - z * dr_i / r2;
        p.dk_dx[i] = EHE_REAL4(0.0, dkx, dky, dkz);
    }
    return p;
}

// 度规缩并 g_μν u^μ v^ν（u、v 必须为**逆变**分量）
EHE_REAL metric_dot(KsPoint p, EHE_REAL4 u, EHE_REAL4 v) {
    return eta_dot(u, v) + 2.0 * p.H * plain_dot(p.k, u) * plain_dot(p.k, v);
}

// ∂_α g_σβ 的组装（α = 0 时为 0：稳态）
void metric_partial(KsPoint p, out EHE_REAL dg[4][4][4]) {
    for (int a = 0; a < 4; ++a) {
        for (int s = 0; s < 4; ++s) {
            for (int b = 0; b < 4; ++b) {
                dg[a][s][b] = 0.0;
            }
        }
    }
    for (int alpha = 1; alpha < 4; ++alpha) {
        int i = alpha - 1;
        EHE_REAL dH = p.dH_dx[i];
        EHE_REAL4 dk = p.dk_dx[i];
        for (int sigma = 0; sigma < 4; ++sigma) {
            for (int beta = 0; beta < 4; ++beta) {
                EHE_REAL k_sigma = p.k[sigma];
                EHE_REAL k_beta = p.k[beta];
                dg[alpha][sigma][beta] = 2.0 * dH * k_sigma * k_beta +
                                         2.0 * p.H * (dk[sigma] * k_beta + k_sigma * dk[beta]);
            }
        }
    }
}

// 测地线加速度 a^μ = −Γ^μ_αβ u^α u^β（直接缩并，对应 core 的 geodesic_accel）
EHE_REAL4 geodesic_accel(KsPoint p, EHE_REAL4 u) {
    EHE_REAL dg[4][4][4];
    metric_partial(p, dg);

    EHE_REAL term1[4];
    EHE_REAL term3[4];
    for (int sigma = 0; sigma < 4; ++sigma) {
        EHE_REAL acc1 = 0.0;
        EHE_REAL acc3 = 0.0;
        for (int alpha = 0; alpha < 4; ++alpha) {
            for (int beta = 0; beta < 4; ++beta) {
                acc1 += dg[alpha][sigma][beta] * u[alpha] * u[beta];
                acc3 += dg[sigma][alpha][beta] * u[alpha] * u[beta];
            }
        }
        term1[sigma] = acc1;
        term3[sigma] = acc3;
    }

    EHE_REAL4 accel = EHE_REAL4(0.0);
    for (int mu = 0; mu < 4; ++mu) {
        EHE_REAL sum = 0.0;
        for (int sigma = 0; sigma < 4; ++sigma) {
            EHE_REAL eta_up = (mu == sigma) ? ((mu == 0) ? -1.0 : 1.0) : 0.0;
            EHE_REAL g_up = eta_up - 2.0 * p.H * p.k_up[mu] * p.k_up[sigma];
            sum += g_up * (term1[sigma] - 0.5 * term3[sigma]);
        }
        accel[mu] = -sum;
    }
    return accel;
}

// 静态观测者 tetrad（按 g 对 u 的正交补做 Gram–Schmidt，对应 core 的 static_observer_frame）
struct ObserverFrame {
    EHE_REAL4 u;
    EHE_REAL4 e_x;
    EHE_REAL4 e_y;
    EHE_REAL4 e_z;
};

ObserverFrame static_observer_frame(KsPoint p) {
    ObserverFrame f;
    EHE_REAL g_tt = metric_dot(p, EHE_REAL4(1, 0, 0, 0), EHE_REAL4(1, 0, 0, 0));
    EHE_REAL u_t = 1.0 / sqrt(-g_tt);
    f.u = EHE_REAL4(u_t, 0.0, 0.0, 0.0);

    EHE_REAL4 basis[3] =
        EHE_REAL4[3](EHE_REAL4(0, 1, 0, 0), EHE_REAL4(0, 0, 1, 0), EHE_REAL4(0, 0, 0, 1));
    EHE_REAL4 accepted[3];
    for (int i = 0; i < 3; ++i) {
        EHE_REAL4 v = basis[i];
        EHE_REAL gu = metric_dot(p, f.u, v);
        v = v + EHE_REAL4(gu) * f.u;
        for (int j = 0; j < i; ++j) {
            EHE_REAL gv = metric_dot(p, accepted[j], v);
            v = v - EHE_REAL4(gv) * accepted[j];
        }
        EHE_REAL norm2 = metric_dot(p, v, v);
        EHE_REAL scale = (abs(norm2) > 1e-30) ? 1.0 / sqrt(norm2) : 0.0;
        accepted[i] = v * scale;
    }
    f.e_x = accepted[0];
    f.e_y = accepted[1];
    f.e_z = accepted[2];
    return f;
}

// 由观测者 tetrad 与局部单位方向构造类光波矢 k^μ = u_obs^μ + n̂^μ（§4.2）
EHE_REAL4 photon_from_frame(ObserverFrame f, EHE_REAL3 n_hat) {
    return f.u + n_hat.x * f.e_x + n_hat.y * f.e_y + n_hat.z * f.e_z;
}

#endif  // EHE_METRIC_GLSL
