// EHE —— 盘湍流噪声与差速旋转（GLSL 450，DESIGN §4.5）
//
// ⚠ 与 core/src/noise.cpp **同算法**（value noise → fbm），但**不是逐位一致**：
//   hash 是非连续函数，GPU 与 CPU 的浮点次序差异会给出完全不同的图案。
//   因此 golden 基线一律取**噪声振幅 0**（§6.1「无噪声动画」），噪声不进 NMSE 差分；
//   噪声自身的正确性由 core 侧单元测试（确定性/值域/平滑性/旋转方向与速度）保证。
//
// 精度约束（§5.8）：fp64 只保证算术与 sqrt，cos/sin 等超越函数在 double 下不可用
//   → 旋转角在 double 里算，三角函数降到 float。

#ifndef EHE_NOISE_GLSL
#define EHE_NOISE_GLSL

// 3D→[0,1) hash（iq 风格；core/noise.cpp 的 hash13 与之同式）
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// 3D value noise（三线性插值 + 平滑步），值域 [0,1]
float value_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 w = f * f * (3.0 - 2.0 * f);

    float n000 = hash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));

    float nx00 = mix(n000, n100, w.x);
    float nx10 = mix(n010, n110, w.x);
    float nx01 = mix(n001, n101, w.x);
    float nx11 = mix(n011, n111, w.x);
    float nxy0 = mix(nx00, nx10, w.y);
    float nxy1 = mix(nx01, nx11, w.y);
    return mix(nxy0, nxy1, w.z);
}

// 分形叠加：**4 倍频**（DESIGN §4.5 的规定值），振幅 0.5 递减，频率 ×2.02，结果归一化
#define EHE_FBM_OCTAVES 4

float fbm(vec3 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    float normalization = 0.0;
    for (int i = 0; i < EHE_FBM_OCTAVES; ++i) {
        sum += amplitude * value_noise(p);
        normalization += amplitude;
        p *= 2.02;  // 非整数倍频，避免格点对齐产生方块感
        amplitude *= 0.5;
    }
    return (normalization > 0.0) ? (sum / normalization) : 0.0;
}

// 开普勒角速度 Ω(r) = r^{−3/2}（M=1, a=0；r ≤ 3 为光子球内，无时序圆轨道 → 0）
EHE_REAL kepler_omega(EHE_REAL r) {
    if (r <= 3.0) {
        return 0.0;
    }
    return 1.0 / (r * sqrt(r));
}

// 密度/发射的噪声调制因子：1 + amplitude·(2·fbm − 1) ∈ [1−a, 1+a]
// amplitude = 0 时**完全短路**（返回 1），保证 golden 基线与噪声实现解耦、可复现。
//
// 采样坐标的选取（实测修正）：盘湍流在**垂直方向近乎相干**，若直接用 3D 位置采样，
// 金标准视角（polar=75°，接近盘面）的穿盘路径很长，沿视线积分会把噪声平滑掉——
// 实测振幅 1.0 时像素只变约 3%，视觉上几乎看不出。因此改用「极坐标 + 弱垂直变化」：
//     p = ( cos(φ)·r, sin(φ)·r, 0.35·z ) · scale,  φ = atan2(y,x) − Ω(r)·t
// 用 (cos,sin) 表示 φ 可避免 2π 处的接缝；φ 上的 −Ω(r)·t 正是差速旋转（内快外慢）。
// 三角函数降 float：fp64 只保证算术与 sqrt（§5.8 约束）。
float disk_noise_factor(EHE_REAL3 position, EHE_REAL r, EHE_REAL time, float amplitude, float scale) {
    if (amplitude <= 0.0) {
        return 1.0;
    }
    float phi = atan(float(position.y), float(position.x)) - float(kepler_omega(r) * time);
    vec3 p = vec3(cos(phi) * float(r), sin(phi) * float(r), float(position.z) * 0.35) * scale;
    return 1.0 + amplitude * (2.0 * fbm(p) - 1.0);
}

#endif  // EHE_NOISE_GLSL
