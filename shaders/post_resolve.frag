// EHE —— 分辨率变换 pass（GLSL 450，DESIGN §4.6 步骤 2）
//
// 三种模式（由内部/输出分辨率的比值决定，互斥）：
//   MODE_BOX   = 0：**SSAA 降采样**（res_scale > 1，整数倍盒式平均；比线性插值更锐利、无权重泄露）
//   MODE_UP    = 1：**Catmull-Rom 双三次升采样**（res_scale < 1 时的路径；
//                   FSR1（EASU+RCAS）落地后本模式由 post_fsr1_* 取代）
//   MODE_COPY  = 2：直通（调试用）
//
// 采样一律按像素中心对齐（(i+0.5)/size），避免半像素偏移造成整体模糊。
// 参数用 UBO（**不用 push constant**：那是 Vulkan 专有特性，GL 不支持，双后端需一致）。

#version 450

layout(std140, binding = 3) uniform PostParams {
    ivec4 mode_and_src;   // x = mode, y = src_width, z = src_height, w = ssaa_factor
    ivec4 dst_size;       // x = dst_width, y = dst_height, z/w = 保留
};

layout(binding = 2) uniform sampler2D post_input;  // texture unit 2

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#define MODE_BOX 0
#define MODE_UP 1
#define MODE_COPY 2

vec3 sample_texel(ivec2 coord) {
    ivec2 clamped = clamp(coord, ivec2(0), ivec2(mode_and_src.y - 1, mode_and_src.z - 1));
    return texelFetch(post_input, clamped, 0).rgb;
}

// Catmull-Rom 权重（与 core/tonemap.cpp 的 catmull_rom_weights 同式）
void catmull_rom_weights(float t, out float w[4]) {
    float t2 = t * t;
    float t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

void main() {
    if (mode_and_src.x == MODE_BOX) {
        // SSAA：**面积加权**盒式平均（支持 1.25/1.5/2.0 等非整数倍率，§7 的分辨率档）
        // 与 core/tonemap.cpp 的 downsample_box 同式：目标像素足迹投回源空间，按重叠面积加权。
        const vec2 scale = vec2(float(mode_and_src.y) / float(dst_size.x),
                                float(mode_and_src.z) / float(dst_size.y));
        const vec2 pixel = v_uv * vec2(dst_size.xy);
        const vec2 source0 = pixel * scale;
        const vec2 source1 = (pixel + vec2(1.0)) * scale;
        const ivec2 begin = ivec2(floor(source0));
        const ivec2 end = ivec2(ceil(source1)) - ivec2(1);

        vec3 accumulator = vec3(0.0);
        float total_weight = 0.0;
        // 倍率 ≤ 2.0 时足迹最多触及 3×3 源像素
        for (int sy = begin.y; sy <= end.y; ++sy) {
            const float overlap_y =
                min(source1.y, float(sy + 1)) - max(source0.y, float(sy));
            if (overlap_y <= 0.0) {
                continue;
            }
            for (int sx = begin.x; sx <= end.x; ++sx) {
                const float overlap_x =
                    min(source1.x, float(sx + 1)) - max(source0.x, float(sx));
                if (overlap_x <= 0.0) {
                    continue;
                }
                const float weight = overlap_x * overlap_y;
                accumulator += weight * sample_texel(ivec2(sx, sy));
                total_weight += weight;
            }
        }
        out_color = vec4(total_weight > 0.0 ? accumulator / total_weight : vec3(0.0), 1.0);
        return;
    }

    if (mode_and_src.x == MODE_UP) {
        // Catmull-Rom：源坐标 = (目标像素中心) × 源/目标 − 0.5
        const vec2 scale = vec2(float(mode_and_src.y) / float(dst_size.x),
                                float(mode_and_src.z) / float(dst_size.y));
        const vec2 source =
            (v_uv * vec2(dst_size.xy) + vec2(0.5)) * scale - vec2(0.5);
        const ivec2 base = ivec2(floor(source));
        float wx[4];
        float wy[4];
        catmull_rom_weights(source.x - float(base.x), wx);
        catmull_rom_weights(source.y - float(base.y), wy);

        vec3 accumulator = vec3(0.0);
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                accumulator += (wx[i] * wy[j]) * sample_texel(base + ivec2(i - 1, j - 1));
            }
        }
        out_color = vec4(accumulator, 1.0);
        return;
    }

    out_color = vec4(texture(post_input, v_uv).rgb, 1.0);
}
