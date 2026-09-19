// EHE —— 粒子点精灵绘制：片元着色器（GLSL 450，DESIGN §5.6）
//
// 圆点软边 + 黑体上色 + 加色混合（blend ONE ONE 由管线侧配置，色调映射前进 HDR）。
// LUT 采样与 common/disk.glsl 的 sample_blackbody 同构（对数等距 + 线性插值），
// 但粒子侧刻意全 float——不拉入盘模型的 fp64 依赖（§5.8 精度约束只影响 raymarch）。

#version 450

layout(binding = 1) uniform sampler2D blackbody_lut;

layout(location = 0) in float v_temperature;
layout(location = 0) out vec4 out_color;

const float EHE_LUT_SIZE = 256.0;
const float EHE_LUT_T_MIN = 1000.0;
const float EHE_LUT_T_MAX = 40000.0;
const float EHE_PARTICLE_BRIGHTNESS = 0.35;  // 单粒子发射亮度（加色叠加大数量时自然 HDR）

void main() {
    // 圆点软边（gl_PointCoord ∈ [0,1]²，中心 0.5）
    const float d = length(gl_PointCoord - vec2(0.5));
    const float fade = smoothstep(0.5, 0.15, d);
    if (fade <= 0.0) {
        discard;
    }

    // 温度 → 黑体色（与 core/blackbody.cpp 的索引映射同构：对数等距）
    const float clamped = clamp(v_temperature, EHE_LUT_T_MIN, EHE_LUT_T_MAX);
    const float t = (log(clamped) - log(EHE_LUT_T_MIN)) /
                    (log(EHE_LUT_T_MAX) - log(EHE_LUT_T_MIN));
    const float u = (t * (EHE_LUT_SIZE - 1.0) + 0.5) / EHE_LUT_SIZE;
    const vec3 emitted = texture(blackbody_lut, vec2(u, 0.5)).rgb;

    out_color = vec4(emitted * (EHE_PARTICLE_BRIGHTNESS * fade), fade);
}
