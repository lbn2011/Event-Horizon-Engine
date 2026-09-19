// EHE —— 最终合成 pass（GLSL 450，DESIGN §4.6 步骤 4–5）
//
// 链序：曝光 → **ACES（Stephen Hill 拟合）** → 色差 → sRGB 编码
// 与 `core/tonemap.cpp` 的 aces_fitted / linear_to_srgb **同式**（同算法，CPU 侧有单测）。
//
// 色差说明：按像素半径对三通道分别取采样 UV（中心无偏移、边缘最大）。
// 由于 ACES 与曝光都是**逐通道单调**运算，"先色调映射再分离"与"分离后各自色调映射"等价，
// 故这里对三个通道各自采样后独立走同一条链——实现更简单且不引入额外 pass。

#version 450

layout(binding = 2) uniform sampler2D post_input;   // texture unit 2
layout(binding = 4) uniform FinalParams {
    vec4 exposure_and_chroma;   // x = exposure, y = chroma_ab, z/w = 保留
    vec4 flags;                 // x = 1 表示启用 ACES（0 则只做曝光 + sRGB）
};

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// ACES：sRGB 线性 → ACEScg → 拟合 RRT/ODT → 回 sRGB 线性（Hill 拟合，与 core 同式）
vec3 aces_fitted(vec3 color) {
    const mat3 input_matrix = mat3(0.59719, 0.35458, 0.04823,
                                   0.07600, 0.90834, 0.01566,
                                   0.02840, 0.13383, 0.83777);
    const mat3 output_matrix = mat3(1.60475, -0.53108, -0.07367,
                                    -0.10208, 1.10813, -0.00605,
                                    -0.00327, -0.07276, 1.07602);
    color = input_matrix * color;
    const vec3 a = color * (color + 0.0245786) - 0.000090537;
    const vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    color = output_matrix * (a / b);
    return clamp(color, vec3(0.0), vec3(1.0));
}

float linear_to_srgb(float linear) {
    return (linear <= 0.0031308) ? (12.92 * linear)
                                 : (1.055 * pow(linear, 1.0 / 2.4) - 0.055);
}

/// 色差：按半径对 UV 做二次径向缩放（与 core/tonemap.cpp 的 chroma_sample_uv 同式）
vec2 chroma_uv(vec2 uv, float strength) {
    const vec2 centered = uv - vec2(0.5);
    const float radius_squared = dot(centered, centered);
    return vec2(0.5) + centered * (1.0 + strength * radius_squared);
}

void main() {
    const float exposure = exposure_and_chroma.x;
    const float chroma = exposure_and_chroma.y;
    const bool use_aces = flags.x > 0.5;

    vec3 hdr;
    if (chroma > 0.0) {
        const vec2 uv = chroma_uv(v_uv, chroma);
        hdr = vec3(texture(post_input, uv).r, texture(post_input, uv).g,
                   texture(post_input, uv).b);
    } else {
        hdr = texture(post_input, v_uv).rgb;
    }

    hdr *= exposure;
    vec3 mapped = use_aces ? aces_fitted(hdr) : hdr / (vec3(1.0) + hdr);  // 关 ACES 时退化为 Reinhard
    out_color = vec4(vec3(linear_to_srgb(mapped.r), linear_to_srgb(mapped.g),
                          linear_to_srgb(mapped.b)),
                     1.0);
}
