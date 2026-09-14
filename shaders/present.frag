// EHE —— 临时呈现 pass（GLSL 450）
//
// 状态：**占位实现**。T1.5 的后处理链（FSR1/FXAA → ACES + 曝光 → 色差，DESIGN §5.4 步骤 2–5）
//       落地后由 `post_final.frag` 取代本文件。
// 当前作用：把 raymarch 输出的 **HDR 线性**结果做曝光 + Reinhard + sRGB 传输函数后送到屏幕，
//           使窗口预览与 `ehe_reftool golden` 的 PNG 预览观感一致（PFM 基线不受影响）。

#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(binding = 2) uniform sampler2D hdr_input;

void main() {
    vec3 hdr = texture(hdr_input, v_uv).rgb;
    // Reinhard（x/(1+x)）保留高光层次，避免硬削顶
    vec3 mapped = hdr / (vec3(1.0) + hdr);
    // sRGB 传输函数（与 core/golden.cpp 的 tonemap_to_srgb8 一致）
    vec3 srgb = mix(mapped * 12.92, 1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055,
                    step(vec3(0.0031308), mapped));
    out_color = vec4(clamp(srgb, vec3(0.0), vec3(1.0)), 1.0);
}
