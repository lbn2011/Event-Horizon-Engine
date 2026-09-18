// EHE —— SimParams UBO（std140, binding = 0）
//
// ⚠ 本文件是 UBO 布局的**唯一来源**，C++ 侧镜像见 render/include/ehe/render/sim_params.h。
//   二者字段顺序/offset 必须逐项一致（DESIGN §5.3：字段可增不可改序）。
//
// V5.5 追加：offset 10（extras）——emission_scale / thickness_scale / debug_view / 保留。
//           追加原因见 DESIGN V5.4 的规格缺口②（发射需绝对尺度归一）与 V5.5 的调试视图选择。

#ifndef EHE_SIMPARAMS_GLSL
#define EHE_SIMPARAMS_GLSL

layout(std140, binding = 0) uniform SimParams {
    vec4 cam_pos;        // [0]  xyz = 相机位置（Kerr-Schild 笛卡尔）, w = 保留
    vec4 cam_basis_x;    // [1]  xyz = tetrad 空间基 e_x, w = 保留
    vec4 cam_basis_y;    // [2]  xyz = e_y
    vec4 cam_basis_z;    // [3]  xyz = e_z
    vec4 cam_dir_fov;    // [4]  xyz = 视线中心方向（局部单位矢量）, w = tan(fov/2)
    vec4 frame;          // [5]  x = aspect, y = time, z = n_max, w = h0
    vec4 steps;          // [6]  x = h_min, y = h_max, z = eps_h, w = r_escape
    vec4 hole;           // [7]  x = spin_a, y = r_in, z = r_out, w = disk_density
    vec4 disk;           // [8]  x = t_scale, y = kappa, z = exposure, w = chrom_ab
    uvec4 flags;         // [9]  x = 位标志, y = res_scale, z = particle_count, w = 保留
    vec4 extras;         // [10] x = emission_scale, y = thickness_scale, z = debug_view,
                         //      w = disk_noise（盘湍流噪声振幅，0 = 关闭；V5.8 追加）
};

// flags.x 的位定义（与 C++ 侧 ehe::render::SimFlags 保持一致）
#define EHE_FLAG_FSR1          (1u << 0)
#define EHE_FLAG_FXAA          (1u << 1)
#define EHE_FLAG_ACES          (1u << 2)
#define EHE_FLAG_PARTICLE      (1u << 3)
#define EHE_FLAG_FP32_ONLY     (1u << 4)
#define EHE_FLAG_DISK_ENABLED  (1u << 5)

// 调试视图编号（与 C++ 侧 ehe::render::DebugView 一致）
#define EHE_VIEW_SHADED      0
#define EHE_VIEW_STEPS       1
#define EHE_VIEW_CLASSIFY    2
#define EHE_VIEW_GFACTOR     3
#define EHE_VIEW_NULLDRIFT   4

#endif  // EHE_SIMPARAMS_GLSL
