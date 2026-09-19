// EHE —— FXAA pass（GLSL 450，DESIGN §4.6 步骤 3）
//
// 链序说明：§4.6 把 FXAA 排在**色调映射之前**（输入是 HDR 线性值）。
// 但 FXAA 的边缘判据依赖 luma 阈值与对比度，这些常量是按 LDR 调过的——直接喂 HDR 会让
// 亮区（盘面可达 10^0 量级、峰值 2.5）全部判成"强边缘"而过模糊。
// 因此这里对**边缘判据用的 luma 先做一次 Reinhard 压缩**（luma' = luma/(1+luma)），
// 而混合用的仍是原始 HDR 颜色——探测行为接近 LDR，输出仍是 HDR，链序不破坏。

#version 450

layout(binding = 2) uniform sampler2D post_input;  // texture unit 2

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#define FXAA_SPAN_MAX 8.0
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_REDUCE_MIN (1.0 / 128.0)

/// 压缩后的亮度（Reinhard），使 LDR 调过的阈值可用于 HDR 输入
float compressed_luma(vec3 rgb) {
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    return luma / (1.0 + luma);
}

struct LumaNeighborhood {
    float north_west;
    float north;
    float north_east;
    float west;
    float middle;
    float east;
    float south_west;
    float south;
    float south_east;
};

LumaNeighborhood sample_neighborhood(vec2 uv, vec2 texel) {
    LumaNeighborhood n;
    n.north_west = compressed_luma(texture(post_input, uv + vec2(-1.0, -1.0) * texel).rgb);
    n.north = compressed_luma(texture(post_input, uv + vec2(0.0, -1.0) * texel).rgb);
    n.north_east = compressed_luma(texture(post_input, uv + vec2(1.0, -1.0) * texel).rgb);
    n.west = compressed_luma(texture(post_input, uv + vec2(-1.0, 0.0) * texel).rgb);
    n.middle = compressed_luma(texture(post_input, uv).rgb);
    n.east = compressed_luma(texture(post_input, uv + vec2(1.0, 0.0) * texel).rgb);
    n.south_west = compressed_luma(texture(post_input, uv + vec2(-1.0, 1.0) * texel).rgb);
    n.south = compressed_luma(texture(post_input, uv + vec2(0.0, 1.0) * texel).rgb);
    n.south_east = compressed_luma(texture(post_input, uv + vec2(1.0, 1.0) * texel).rgb);
    return n;
}

void main() {
    const ivec2 size = textureSize(post_input, 0);
    const vec2 texel = 1.0 / vec2(size);
    const LumaNeighborhood n = sample_neighborhood(v_uv, texel);

    const float luma_min = min(n.middle,
                               min(min(n.north, n.south), min(n.west, n.east)));
    const float luma_max = max(n.middle,
                               max(max(n.north, n.south), max(n.west, n.east)));
    const float luma_range = luma_max - luma_min;

    // 对比度不足 → 不是边缘，直接输出（早期退出，省掉后续采样）
    if (luma_range < max(FXAA_REDUCE_MIN, luma_max * FXAA_REDUCE_MUL)) {
        out_color = vec4(texture(post_input, v_uv).rgb, 1.0);
        return;
    }

    const vec2 direction = vec2(-((n.north + n.south) - (n.west + n.east)),
                                ((n.north_west + n.north_east) - (n.south_west + n.south_east)));
    const float direction_reduce = max(
        (n.north_west + n.north + n.north_east + n.west + n.east + n.south_west + n.south +
         n.south_east) *
            (1.0 / 12.0),
        1.0 / 128.0);
    const float inverse_direction_min = 1.0 / (min(abs(direction.x), abs(direction.y)) + direction_reduce);

    vec2 offset = clamp(direction * inverse_direction_min, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * texel;

    const vec3 rgb_a = 0.5 * (texture(post_input, v_uv + offset * (1.0 / 3.0 - 0.5)).rgb +
                              texture(post_input, v_uv + offset * (2.0 / 3.0 - 0.5)).rgb);
    const vec3 rgb_b = rgb_a * 0.5 +
                       0.25 * (texture(post_input, v_uv + offset * -0.5).rgb +
                               texture(post_input, v_uv + offset * 0.5).rgb);
    const float luma_b = compressed_luma(rgb_b);
    out_color = vec4((luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b, 1.0);
}
