// EHE —— 粒子点精灵绘制：顶点着色器（GLSL 450，DESIGN §5.6）
//
// Vertex pulling：无顶点缓冲，gl_VertexID 直接索引粒子 SSBO（与 raymarch 的
// gl_VertexID 全屏三角形同风格，§5.4.1「无 VBO」约定）。
// 输出：透视投影位置 + 距离衰减点径 + 半径→温度 varying（frag 黑体上色用）。

#version 450

// ⚠ 跨 API 差异（与 fullscreen.vert 同款，SPIR-V 编译测试抓出来的）：
//   顶点序号内建 OpenGL GLSL 叫 gl_VertexID，Vulkan GLSL 叫 gl_VertexIndex；
//   VK 后端注入 EHE_VULKAN 宏抹平。vertex pulling 索引 SSBO 用的就是它。
#ifdef EHE_VULKAN
#define EHE_VERTEX_INDEX gl_VertexIndex
#else
#define EHE_VERTEX_INDEX gl_VertexID
#endif

#include "common/simparams.glsl"

layout(std430, binding = 2) readonly buffer ParticleBuffer {
    vec4 particle_pos_seed[];  // xyz = 位置, w = 种子（draw 侧不使用）
};

layout(location = 0) out float v_temperature;  // K（盘温剖面，frag 采 LUT）

void main() {
    const vec3 position = particle_pos_seed[EHE_VERTEX_INDEX].xyz;

    // 世界 → 相机（tetrad 空间基，UBO cam_basis_x/y/z；与 raymarch.frag 的光线构造同约定）
    const vec3 rel = position - cam_pos.xyz;
    const float depth = dot(rel, cam_basis_z.xyz);  // 前向深度（正 = 在相机前方）
    if (depth <= 0.05) {
        // 相机身后：推出裁剪空间（不产生图元）
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_temperature = 0.0;
        gl_PointSize = 1.0;
        return;
    }

    const float tan_half = cam_dir_fov.w;
    const float aspect = frame.x;
    const vec2 ndc = vec2(dot(rel, cam_basis_x.xyz) / (tan_half * aspect),
                          dot(rel, cam_basis_y.xyz) / tan_half) / depth;
    gl_Position = vec4(ndc, 0.5, 1.0);

    // 点径：透视衰减（内部渲染高度 × 粒子大小系数 / 深度），钳制到合理范围
    const float size_scale = uintBitsToFloat(flags.w);
    const float internal_height = 720.0;  // 归一系数：size=1、depth=15 时约 2px（观感标定）
    gl_PointSize = clamp(size_scale * internal_height * 0.004 / depth, 1.0, 24.0);

    // 温度剖面（与 common/disk.glsl 的 disk_temperature 同构的 float 版，科普近似）：
    // T(r) = T_scale · ((1 − √(r_in/r)) / r³)^{1/4}，归一化到 r = 1.5·r_in 的峰值
    const float r = max(length(position), 1e-3);
    const float r_in = hole.y;
    const float t_scale = disk.x;
    float flux = (1.0 - sqrt(r_in / r)) / (r * r * r);
    const float reference = (1.0 - sqrt(1.0 / 1.5)) / (1.5 * r_in * 1.5 * r_in * 1.5 * r_in);
    flux = (r > r_in) ? flux / max(reference, 1e-9) : 0.0;
    v_temperature = (flux > 0.0) ? t_scale * sqrt(sqrt(flux)) : 1000.0;
}
