# shaders/ —— 双后端共享的 GLSL 450 源码（DESIGN §5.8）
#
# 规划文件：
#   common/simparams.glsl   UBO 块声明（与 DESIGN §5.3 布局一致，唯一来源）
#   common/metric.glsl      Kerr-Schild 度规/导数/Γ（对照 core/ReferenceIntegrator 逐行）
#   common/disk.glsl        盘密度/通量/g 因子/体合成
#   common/noise.glsl       hash/value noise/fbm
#   fullscreen.vert         全屏三角形（gl_VertexID，无 VBO）
#   raymarch.frag           主积分循环 + 调试视图分支
#   particle.comp/.vert/.frag
#   post_fsr1_*.glsl / post_fxaa.frag / post_final.frag
#
# include 约定：GL 450 无原生 #include，由 render/ShaderSource 在 C++ 侧做文本递归展开，
# 展开后的源码同时供 GL 直接编译与 Vulkan 侧 glslang 编译。
#
# 本目录当前为空骨架：shader 从 T1.3 起逐个落地。
