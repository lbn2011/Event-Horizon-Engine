// EHE —— 噪声函数（GLSL 450）
//
// 用途（T1.4）：盘的湍流细节 fbm + 差速旋转动画（由 UBO 的 time 驱动）。
// v1 先提供 hash/value noise/fbm 三个基础件，噪声层数固定 5（DESIGN §4.5）。

#ifndef EHE_NOISE_GLSL
#define EHE_NOISE_GLSL

// 3D→1D hash（iq 风格，无纹理依赖，跨后端一致）
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// 3D value noise（三线性插值 + 平滑步）
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

// 分形叠加（DESIGN §4.5：fbm 5 层）
float fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    float normalization = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * value_noise(p);
        normalization += amplitude;
        p *= 2.02;  // 非整数倍频，避免格点对齐产生方块感
        amplitude *= 0.5;
    }
    return (normalization > 0.0) ? (sum / normalization) : 0.0;
}

#endif  // EHE_NOISE_GLSL
