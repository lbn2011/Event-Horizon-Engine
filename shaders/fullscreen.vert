// EHE —— 全屏三角形顶点着色器（GLSL 450）
//
// 无 VBO：3 个顶点由 gl_VertexID 生成，覆盖裁剪空间全屏（DESIGN §5.4.1）。
// 输出 v_uv（[0,1]）与 v_ndc（[−1,1]，y 向上），片元侧按需选用。

#version 450

// ⚠ 跨 API 差异（SPIR-V 编译测试抓出来的）：
//   顶点序号内建在 **OpenGL GLSL 里叫 `gl_VertexID`，在 Vulkan GLSL 里叫 `gl_VertexIndex`**。
//   VK 后端编译时注入 `EHE_VULKAN`（见 render/spirv.cpp / vk_backend），此宏负责抹平差异。
//   其余片元内建（gl_FragCoord / texture / texelFetch / textureGather）两 API 同名，无需处理。
#ifdef EHE_VULKAN
#define EHE_VERTEX_INDEX gl_VertexIndex
#else
#define EHE_VERTEX_INDEX gl_VertexID
#endif

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_ndc;

void main() {
    // 顶点顺序：(-1,-1) → (3,-1) → (-1,3)，三角形覆盖整个 [-1,1]²
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[EHE_VERTEX_INDEX];
    v_ndc = p;
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
