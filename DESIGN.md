# Event Horizon Engine (EHE) — 开发文档 V5.12

> **本文档是 EHE 项目开发的唯一权威依据。** 原始需求对话（`黑洞渲染模拟：Kerr度规光线....json`）仅为历史参考，
> 凡本文档与对话冲突之处，以本文档为准。本文档不含代码；修改本文档需明确提出并递增版本号。
>
> V1.0（2026-09-13）：第一轮拷问定稿，7 项决策。
> V2.0（2026-09-13）：第二轮拷问定稿，15 项决策，补全数值/验证/UI/工程规格与开发计划。
> V2.1（2026-09-13）：全文审查勘误 7 处（无决策变更）。
> V3.0（2026-09-13）：深化为方程级实现规格；勘误 1 处（盘外径口径统一为 20M）。
> V4.0（2026-09-13）：再深化——度规导数链式分解、静态 tetrad 构造、像素→光线映射、盘四速度显式 u^t、
> 相机模块规格、Config JSON schema、Smoke CLI 规格、工具链部署清单、§13.1 KS↔BL 坐标附录。
> 审查修正 4 处（逆度规精确形式/交叉引用/圆轨道免变换/null 构造）。
> V5.0（2026-09-13）：第四轮——黑体 LUT 生成规格、双后端资源对照表、CMake target 图、粒子初始化/积分/渲染、
> 音频合成参数、shader 文件组织与 include 约定、golden 渲染器规格、L2 测试用例表（12 条）。
> 审查修正 2 处：①golden/smoke 差分基准从 8-bit PNG 改为 PFM（PNG 量化损失 ~1e-2 会淹没 1e-3 阈值）；
> ②依赖表补 stb（PNG 写出），仓库计数 10→11。
> V5.1（2026-09-13）：全文审查勘误 3 处（无内容变更）——标题版本号 V4.0→V5.x 遗漏；
> §6 表 L3 行残留 `--shot out.png` 过时引用；§5.4.1 删除不存在的扩展名 `VK_KHR_main` 改为核心特性表述。
> V5.2（2026-09-14）：**环境事实勘误**——开发机 GL 能力实测为 **OpenGL 4.5 核心**（原文档据卡型规格误判为 ≤4.1），
> 因此 GL 路径可在开发机直接运行验证（Vulkan 仍不可用）。
> V5.3（2026-09-14）：**验证策略确认不重排**（项目所有者决定）——尽管开发机可运行 GL 路径，
> §6 的 L3 目标机冒烟、L4 CI 边界维持原设计不变；开发机可运行 GL 仅视为调试便利，不作为验收依据。
> V5.4（2026-09-14）：**T1.2 落地中发现并修正 4 处规格缺口**（均由真实渲染/数值验证暴露）：
> ① §4.5 盘温 T_scale 缺单位与物理量级（默认 1.0 会使温度落到 LUT 域外，画面全黑红）→ 明确为**开尔文**、默认 10000 K；
> ② §4.5 的发射公式 ε = ρ·g³·LUT **缺绝对尺度**（沿测地线累积上百步后可达 10² 量级）→ 新增归一化常数 `emission_scale`（默认 2.5，仅做数值归一，物理对比仍由 ρ/F(r)/g³ 承载）；
> ③ §6.1 golden 相机 r=15 **落在盘径向范围 [6,20] 内且贴近盘面**（相机嵌在盘介质里，产生满屏弥散辉光）→ 改为 r=30；golden 的 `N_max` 300 → **1000**（参考基准不应受实时步数预算限制；实测步数用尽像素从 106067 降到 4）；
> ④ 黑体 LUT 所用 CIE 1931 2° 配色函数的**数据来源与许可**（BSD-3 项目提取、原始为 CIE 公开数据集）与生成脚本路径入档（§4.5）。
> V5.5（2026-09-14）：**T1.3 GLSL 移植过程中的 5 处规格补充/约束**（均由真实 GPU 运行暴露）：
> ① §5.8 增加 **GLSL fp64 约束**：只保证算术与 `sqrt`，`exp/log/pow` 的 double 重载不可用
>   （`tests/src/test_shaders.cpp` 有探测用例）→ 超越函数一律下沉到 float（影响盘密度/吸收/LUT 索引映射）；
> ② §4.3/§7 **fp32 编译变体成为一等模式**：Terascale 等无 fp64 硬件的 GPU 上，fp64 由驱动软件模拟，
>   实测 AMD HD 7400M 单帧 **2.16 s**（32²×60 步）并触发 Windows TDR 驱动复位崩溃；切 fp32 后同场景 **12.7 ms（≈170×）**。
>   实现方式：`EHE_FP32_ONLY` 宏（渲染器按 `integrator.precision` 注入，宏定义见 §4.3/§7）；
> ③ §5.3 UBO **追加 offset 10（extras）**：emission_scale / thickness_scale / debug_view（字段可增不可改序）；
> ④ §6.3 smoke 两条实现约束：**渲染尺寸必须与窗口尺寸解耦**（Windows 对可见窗口有最小尺寸限制，
>   实测 32×32 被系统拉到 120×32）；**PFM 行序统一**为「内存第 0 行 = 顶部，文件按标准自下而上」（此前 CPU 侧写反导致 NMSE≈1.8）；
> ⑤ §5.8 文件清单新增 `present.frag`（T1.5 前占位：曝光 + Reinhard + sRGB；T1.5 由 `post_final.frag` 取代）。
> V5.6（2026-09-19）：**目标机（Intel 核显）验证协议与 2 处测量/接口补充**：
> ① §6 新增 **`--caps` 能力探测**与**目标机便携包协议**（CI 产出 `ehe-target-bundle.zip`：静态链接 exe +
>   shaders + golden 基线 + 一键脚本 + 指南；目标机免编译直接跑，结果落成**单个报告文件**回传）——
>   这是 §6 L4「GPU 渲染永不进 CI」在工程上的落地方式：CI 无法验的，交给目标机验，但数据必须可回传核对；
> ② §6 L4 补充**计时方法论**：GPU 帧耗时必须 `glFinish` 同步后再测——实测本机 32² fp32「提交时间」12.7 ms
>   而**真实 GPU 时间 254 ms（差 20 倍）**；不同步的计时会把"跑不动"误报成"跑得动"。smoke 的预热帧计时已改为含同步；
> ③ `IRenderer` 增补 `rebuild_pipeline(defines)`（精度切换走管线级重建）与 `capability_report()`——
>   前者是因为**同一进程内二次创建 GL 上下文会失败**（实测 glad 第二次加载返回 0），后者供 `--caps` 采集。
> V5.7（2026-09-19）：**目标机实测完成，§7 新增「实测基线」小节**（Iris Xe 512²/N_max=1000/fp32 = **2516.6 ms/帧**、
>   64²/N_max=60/fp32 = **28.3 ms**；AMD HD 7400M 同配置 2050 ms 且 fp64 ≥64² 会触发 TDR）。
>   **M1 的 L3 门禁达成**：目标机 512² 抹烟与 fp64 CPU 参考 **NMSE = 5.654e-04 < 1e-3**（§6.2 阈值），
>   且经**两条独立路径**核对（目标机自报 + 开发机用 reftool 重算，数值完全一致；两侧 golden 基线 SHA-256 相同）。
>   另：目标机 `[probe] vulkan => OK` 且 `features.shaderFloat64=0` → 证实"核显无原生 fp64"的先验结论。
> V5.8（2026-09-19）：**T1.4 盘体湍流落地，§4.5 增补 2 处实测修正 + 1 个 Config 键**：
>   ① 噪声必须主要作用在**发射系数**上（光学厚盘的源函数 S=ε/κ 与 ρ 无关，只调制密度看不出来）；
>   ② 采样坐标改「极坐标 + 弱垂直变化」（裸 3D 噪声会被长穿盘路径平均掉）；
>   ③ 新增 `blackhole.disk_noise ∈ [0,1]`（默认 0.35；**golden 参数文件显式写 0**，噪声不进 NMSE 差分）；
>   ④ UBO extras.w 复用为噪声振幅（追加语义，不改字段顺序）。
> V5.9（2026-09-19）：**T1.5 后处理链落地（FSR1 除外）**，§4.6 增补 4 处实现要点：
>   ① **链序实现**：raymarch（内部分辨率 FP16）→ 分辨率变换（SSAA 面积加权降采样 / Catmull-Rom 升频，互斥）
>      → FXAA → final（曝光 → **ACES Hill 拟合** → 色差 → sRGB）→ blit 呈现；
>   ② **SSAA 用面积加权**（不是整数倍才可用）：目标像素足迹投回源空间按重叠面积加权，
>      因此 §7 的 1.25x / 1.5x 档位可用；整数倍时退化为等权平均（与手算一致，见单测）；
>   ③ **FXAA 的 HDR 适配**：§4.6 把 FXAA 排在色调映射之前，但它的 luma 阈值是按 LDR 调的——
>      直接吃 HDR 会让亮区全部判成强边缘而过模糊。实现上对**判据用的 luma 先做 Reinhard 压缩**
>      （混合仍用原始 HDR 颜色），链序不破坏；
>   ④ **后处理输出到独立 LDR 目标再 blit 呈现**（不直接写默认帧缓冲）：窗口尺寸可能被系统放大
>      （离屏实测 32×32 → 120×32），直接写会让"成品尺寸 ≠ 输出分辨率"，无法与 CPU 参考逐像素对照。
>   **验证**：新增 `smoke` 的 **post 一致性对照**（GPU 后处理成品 vs `core/tonemap.cpp` 同式 CPU 参考）——
>   三条分辨率路径实测 **最大差 1/255、平均差 0.03/255**；`capture_hdr` 语义修正为
>   "分辨率变换之后、色调映射之前"的输出分辨率 HDR（res_scale=1 时与旧行为**逐位一致**）。
>   FXAA/FSR1 不在此对照内（前者 GPU 专有、后者待 T1.5.2 收尾）。
> V5.10（2026-09-19）：**T1.6.2 落地（GLSL→SPIR-V 运行时编译），并新增 3 条跨 API/CI 约定**：
>   ① **顶点序号内建名两 API 不同**：OpenGL GLSL 是 `gl_VertexID`，**Vulkan GLSL 是 `gl_VertexIndex`**
>      （由 SPIR-V 编译测试抓出，GL 下能跑、VK 下直接编不过）。约定：shader 侧用宏 `EHE_VERTEX_INDEX`，
>      VK 路径编译时注入 `EHE_VULKAN` 抹平差异；其余内建（gl_FragCoord/texture/texelFetch/textureGather）同名，无需处理。
>   ② **SPIR-V 编译进 CI**（§6 L4 的边界因此更严）：GLSL→SPIR-V 是**纯 CPU 行为**，
>      故 VK 侧最易错的一环（绑定布局、std140、精度宏）可由 CI 全覆盖——5 个 shader × 2 种精度
>      全部断言编出合法 SPIR-V（含魔数、字流非空），并断言 fp64/fp32 产物**不同**（精度开关真生效）。
>      新增负例：语法错误/空源码必须失败且不产生字流。
>   ③ **VK 侧精度必须运行时判定**：`shaderFloat64=0` 的设备（如 Intel 核显）**无法创建 fp64 流水线**，
>      VK 后端需在 init 时查询该特性并在不支持时强制 `EHE_FP32_ONLY`（T1.6.1 实现要点）。
>   另：`glslang` 的 SPIRV 目录与 `glslang` 目录**同级**（`<SPIRV/GlslangToSpv.h>`，不是 `<glslang/SPIRV/...>`）。
> V5.11（2026-09-19）：**T1.6.1 Vulkan 渲染链落地**（`render/vk/src/raymarch_chain.cpp`），4 处实现取舍：
>   ① **离屏图像统一用 `VK_IMAGE_LAYOUT_GENERAL`**：该布局同时合法于"颜色附件"与"采样源"，**无需布局过渡**，
>      只需在 pass 之间插 `COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER` 的内存屏障。专用布局更省带宽，
>      但过渡时机易错——首版刻意避开（正确性优先；优化放到实测之后）。
>   ② **final pass 直接画进交换链**（复用后端已有 render pass），因此**不需要额外的 blit pass**；
>      ImGui 随后在同一 render pass 内叠加，与 T0.4 行为一致。
>   ③ **读回**：图像→缓冲拷贝（GENERAL 布局对 `vkCmdCopyImageToBuffer` 合法）→ CPU 侧 half→float 转换
>      （HDR 为 RGBA16F）；LDR 从**最近一帧的交换链图像**回读（final 就画在那里）并做 BGRA→RGB。
>      **Vulkan 图像原点在左上，与 core 的"第 0 行 = 顶部"约定一致，故无需翻转**（GL 侧需要翻转）。
>   ④ **链重建触发条件**：交换链尺寸变化 或 `res_scale` 档位变化 → 离屏目标尺寸随之变化，必须在帧开始前重建整条链。
>   另：`ehe_render` 也需链接 `glslang::glslang-default-resource-limits`（`GetDefaultResources()` 不在主库内）。
> V5.12（2026-09-19 真机首跑）：**Iris Xe 上 `--caps` 抓出两处 VK 建链失败**，各有普适教训：
>   ① **描述符池容量 = 集合数 × 每集合描述符数**（不是集合数）——每集合 3 个 UBO（binding 0/3/4）
>      却只按 1 个/集合分配，`vkAllocateDescriptorSets` 直接 OUT_OF_POOL。开发机无 ICD 编译期
>      查不出这种"运行时资源账目"错误，只有真机能暴露。
>   ② **提交路径的每个 VkResult 都要检查并进错误串**——首建成功、重建全挂的现象，若 begin/end/
>      alloc/bind/map/waitIdle 的返回值被吞，就无法区分"提交参数错"与"device lost 级联"。
>      现约定：`last_error` 一律带 `vk_result_name(result)` 后缀。
>   真机已验证：设备探测 / shaderFloat64=no → 强制 fp32（V5.10 约定③落地）/ 5 shader 编出 SPIR-V ✓。

---

## 1. 项目概述

EHE 是一个实时黑洞渲染模拟器：在 GPU 上对 Kerr 时空做光线测地线积分（raymarching），渲染体积吸积盘、
多普勒聚束、引力红移与黑洞阴影；全 C++ 编写，CMake 构建，Vulkan 1.2 与 OpenGL 4.5 双后端，
ImGui 全参数实时调节，另含粒子"经典模式"与程序化音效。

**原始需求的落地修正**（相对对话 JSON）：

| 原需求 | 落地结果 | 原因 |
|---|---|---|
| C++ 和 Rust 混合，Rust 管物理 | **全 C++**，物理在 GLSL shader，C++ 管调度 | 用户决策，消除 FFI 与原需求自相矛盾 |
| 鼠标/触控 | 鼠标/键盘；**触控降级 backlog** | GLFW 无触控 API，桌面独显场景价值低 |
| DLSS/TSR 升频 | v1 = FSR1 空间升频；TSR 延至 M3 | 体渲染无可靠 motion vector，时间重投影鬼影 |
| 星空引力透镜 | **后置 M2**（用户修正），v1 背景纯色 | 用户明确要求后置 |
| 喷流 / 偏振 / 多波段 / 时间延迟 | 喷流 M2、偏振 M3；多波段/时间延迟 backlog | 喷流物理上依赖 Kerr 自旋轴；偏振需并行传输 |

---

## 2. 环境事实与工具链

### 2.1 环境事实（不可变更的前提）

| 事实 | 影响 |
|---|---|
| 开发机无 MSVC、无 gcc/clang；有 CMake 4.4.2 | 工具链全新搭建，走 GNU 路线 |
| 开发机 GPU = AMD Radeon HD 7400M（驱动 15.201.1151.1008） | **实测支持 OpenGL 4.5 核心（GL_VERSION=4.5.13399）**；**无 Vulkan**（缺 vulkan-1.dll）→ **GL 路径可在开发机直接运行验证**，Vulkan 路径仍需目标机 |
| 目标机 = 现代独显（Vulkan 1.2+ / GL 4.6） | 全特性集可用，compute shader 可用 |
| 开发机无 Vulkan SDK | 免 SDK 方案：Vulkan-Headers + volk 动态加载 |

### 2.2 工具链规格

| 组件 | 选型 | 备注 |
|---|---|---|
| C/C++ 编译器 | **LLVM-MinGW**（Clang + lld，单 tarball 免安装） | 备选回退：WinLibs GCC |
| 构建系统 | 系统 CMake 4.4.2 + **Ninja**（单文件下载） | generator: Ninja |
| 语言标准 | **C++20**（不碰 modules） | concepts 约束接口 |
| 版本控制 | git + **MIT License** | 仓库根 = `C:\Users\lbn\Desktop\code\ehe\` |
| 注释/文档 | **中文注释 + 英文标识符**，文档全中文 | |
| 警告 | `-Wall -Wextra -Wpedantic`，开发期不开 `-Werror` | |

---

## 3. 决策总表（15 项，全部经项目所有者确认）

### 第一轮（V1.0）
1. **开发机角色**：仅编译开发，运行验证在目标现代独显机。API 底线 Vulkan 1.2 / GL 4.5。
2. **工具链**：LLVM-MinGW Clang + Ninja + 系统 CMake。
3. **语言**：全 C++。模块边界靠库分离保留（core 零图形依赖 ↔ render 后端）。
4. **物理推进**：先验证 a=0 再开自旋（第二轮细化为 Kerr-Schild 一步到位，见 §4.1）。
5. **v1 渲染范围**：raymarching 体渲染 + 粒子经典模式双模式 + FSR1 空间升频。
6. **依赖获取**：FetchContent 全源码构建；Vulkan = Headers + volk（免 SDK）；glslang 库内嵌，
   启动时 GLSL→SPIR-V；双后端共享同一份 GLSL 450 源码。
7. **后端切换**：双后端静态链接单一 exe，ImGui 切换时销毁重建窗口句柄、原地重建 `IRenderer`，进程不退。

### 第二轮（V2.0）
8. **坐标系**：Kerr-Schild 坐标写完整 Kerr 度规，M1 = a=0 验证（退化为 Schwarzschild）。
9. **积分器**：RK4 + 距离自适应步长；**混合精度**（关键路径 fp64，编译开关 `EHE_FP64_METRIC` 可回退纯 fp32）。
10. **吸积盘**：薄盘几何 + Novikov-Thorne 解析通量剖面 + 黑体色温 + 噪声湍流差速旋转。
11. **相机**：v1 双模式（轨道 + 自由飞行）；触控 backlog，保留 GLFW。
12. **性能目标**：1080p60 原生为目标；内部分辨率手动档；raymarch 步数预算面板化（默认 300）。
13. **验证体系**：四层 = CPU 参考实现 + core 单元测试 + 目标机冒烟脚本 + CI 编译检查。
14. **UI**：参数清单确认（§8）；抗锯齿 = **FXAA + SSAA 双档**（内部档 0.5x–2.0x，>1.0x 为 SSAA，与 FSR1 互斥）。
15. **工程**：C++20；中文注释 + 英文标识符；git + MIT。

---

## 4. 物理与数值规格

> 约定：几何化单位 G = c = 1，质量 M = 1（方程无量纲化）；度规号差 (−,+,+,+)；
> 指标 μ ∈ {t, x, y, z}；仿射参数 λ。

### 4.1 Kerr-Schild 度规（显式形式）

度规写为 Minkowski 背景加 Kerr 修正：

```
g_μν = η_μν + 2H · k_μ k_ν        （η = diag(−1,1,1,1)）
H    = r³ / (r⁴ + a²z²)           （M = 1；一般情形 H = M r³/(r⁴ + a²z²)）
k    = (1, (r x + a y)/(r² + a²), (r y − a x)/(r² + a²), z/r)   （零余矢量）
```

- **r 的隐式定义**（Kerr-Schild 的 r **不是**欧氏距离）：由 (x²+y²)/(r²+a²) + z²/r² = 1 解出。
  显式求根（取正根）：令 R² = x²+y²+z²，则
  ```
  r² = ½ ( R² − a² + √( (R² − a²)² + 4 a² z² ) )
  ```
  a=0 时 r² = R²，退化为欧氏距离——**此为 L2 单元测试用例**。
- **视界**：r₊ = 1 + √(1 − a²)（M=1）；外视界判定用此 r₊。
- **自旋**：a ∈ [0, 0.998]（上限钳制，防极端 Kerr 数值病态）；**M1 阶段 a 锁定 0 灰显**。
- **Christoffel**：Γ^μ_αβ 由上述 g_μν **解析求导**得到（不接受数值差分——误差会直接毁掉阴影轮廓）。
  推导产物（符号化简后的表达式）以注释形式入库，注明推导日期与核对人；建议用符号代数工具
  （SymPy/Mathematica）生成后人工核对【建议默认】。

#### 4.1.1 度规导数（Γ 的原料，显式形式）

Γ^μ_αβ = ½ g^μσ(∂_α g_σβ + ∂_β g_σα − ∂_σ g_αβ)，其中 g^μσ = η^μσ − 2H k^μ k^σ
（Kerr-Schild 逆度规有**精确**封闭形式：因 k 关于 η 是零矢量，求逆不引入分母，k^μ = η^μν k_ν）。偏导按链式分解为三层：

```
∂_i r     = (x_i 因子) / ( r(x²+y²)/(r²+a²)² + z²/r³ )    （隐函数求导）
            分子：∂_x → x/(r²+a²)，∂_y → y/(r²+a²)，∂_z → z/r²；∂_t r = 0
∂_i H     = H · ( 3 ∂_i r / r − (4r³ ∂_i r + 2a²z δ_iz) / (r⁴ + a²z²) )
∂_i k_μ   = 由 k 的定义式逐分量求导（含 ∂_i r 项）
```

- **z=0 奇点**：a>0 且 z→0 时 r² → R² − a²，环奇点 r=0 在赤道面上；终止条件 1（视界外 ε=0.01）
  保证积分永不抵达，无需额外处理，但 L2 测试须覆盖 z=0 平面的 ∂_i r 连续性。
- **验证**：∂_i r 与 ∂_i H 用中心差分在 100 个随机点（r ∈ [2, 50]）做 L2 数值回归，
  相对误差 < 1e-6（fp64 参考实现）【建议默认】。
- 度规求值隔离为 shader 函数块（`metric_accel()` 语义角色），CPU 参考实现与之方程对齐、逐行对照。

### 4.2 光线初始条件（逆向追光）

- **逆向追光（backward tracing）**：光线从相机出发，沿 −λ 方向（向过去）积分。
- **静态观测者 tetrad**：相机固连于坐标静止观测者，u_obs = (u^t, 0,0,0)，u^t = 1/√(−g_tt)。
  空间三基由坐标基 (∂_x, ∂_y, ∂_z) 对 g 做 Gram–Schmidt 正交归一化得到。
  tetrad 正交性 g(e_(a), e_(b)) = η_ab 纳入 L2 测试（容差 1e-12，fp64 参考路径）。
  【建议默认：M1 用静态 tetrad，M2 评估是否换 ZAMO 随转系】
- **像素 → 光线方向**：取视线前向空间基 e_(fwd)（指向黑洞），像素 NDC (s_x, s_y) ∈ [−1,1]²，
  ```
  n̂  = normalize( e_(fwd) + s_x·tan(fov/2)·aspect·e_(x) + s_y·tan(fov/2)·e_(y) )   （tetrad 内单位空间矢量）
  k^μ = u_obs^μ + n̂^μ
  ```
  类光性由构造保证：g(k,k) = g(u,u) + g(n̂,n̂) = −1 + 1 = 0（**禁止**把 e_(t) 与空间基混合后直接
  normalize——那样既不类光也无物理意义）；积分中漂移量 |k·k| 入调试视图 §5.5。
- 场景以黑洞为原点，坐标量级 ≤ 100；相机位置/tetrad 以 uniform 传入（UBO，见 §5.3）。

### 4.3 测地线积分

- **方程**：d²x^μ/dλ² = −Γ^μ_αβ (dx^α/dλ)(dx^β/dλ)，8 维一阶状态 (x^μ, k^μ)。
- **积分器**：经典 RK4。
- **步长策略**：h = clamp(h₀ · f(r, θ), h_min, h_max)。
  【建议默认】f(r) = (r − r₊)/r₊（近视界线性收缩），h₀=0.05、h_min=1e-3、h_max=0.5；
  三者面板可调（积分器组），调参记录写入 Config。
- **终止条件**（命中即停，优先级从上到下）：
  1. r < r₊ · (1 + ε)，ε = 0.01 → 命中视界，返回黑（阴影）
  2. r > r_escape = 100 → 逃逸，采样背景（v1 纯色；M2 星空 cubemap）
  3. 步数 ≥ N_max（默认 300，面板可调）→ 按逃逸处理
- **精度**：混合精度——r 求根、度规/Christoffel 求值 fp64，状态向量 (x,k) 累加 fp32；
  shader 编译开关 `EHE_FP64_METRIC`（默认 ON）可整体回退纯 fp32 用于性能对比。

### 4.4 验证基准（a=0 必须复现的解析值）

| 量 | 解析值 | 验收容差 |
|---|---|---|
| 光子球半径 | 3（M=1，即 1.5 r_s） | 数值积分轨道临界逼近 ±0.5% |
| 无穷远观测者阴影半径 | 3√3 ≈ 5.196（M=1，即 (3√3/2) r_s） | 图像测量 ±1 像素 @512² 参考图 |
| 盘内半径（ISCO，a=0） | 6（M=1，即 3 r_s） | 盘模型常数一致 |
| 引力红移（盘边缘典型） | g 因子单调、方向正确 | CPU/GPU 相对误差 < 2% |
| 类光归一化保持 | g_μν k^μ k^ν = 0 | 全程漂移 < 1e-3（fp32 累加路径） |

### 4.5 吸积盘模型

- **几何**：赤道面薄盘 + 有限厚度：密度 ρ(x) = ρ₀ · n(x) · exp(−z² / (2 σ_d²))，
  σ_d = 0.1 · r【建议默认】；径向区间 [r_in, r_out] 默认 [6, 20]（M=1），面板可调。
- **通量剖面**：a=0 工作形式取带无扭矩内边界的 Shakura–Sunyaev 剖面
  ```
  F(r) ∝ Ṁ · r⁻³ · ( 1 − √(r_in / r) )
  ```
  亮度 ∝ F(r)，Ṁ 归一化进面板"盘密度"。完整 Page–Thorne 相对论修正因子含 a 相关项，
  列为 M2 随 Kerr 解锁一并评估的精化项【建议默认】。
- **颜色**：局部温度 T(r) = T_scale · F(r)^{1/4}；**T_scale 单位为开尔文（峰值温度），默认 10000 K**
  （V5.4 修正：原文档未标单位，默认 1.0 会使温度落到 LUT 域 [1000, 40000] K 之外，画面退化为全黑红）；
  温度 → RGB 用**预烘焙 256×1 LUT**，golden 渲染器与 GPU shader **共用同一 LUT 文件**
  （tests/ 与 shaders/ 各持一份拷贝，CI 校验 SHA-256 一致）【建议默认】。
  **LUT 生成规格**（core/BlackbodyLut，离线工具）：
  - 温度域 [1000 K, 40000 K]，256 点对数等距采样（覆盖红外偏红到蓝白）
  - 每点：Planck 谱 B_λ(T) 在 [380, 780] nm 以 1 nm 步长数值积分 → 乘 CIE 1931 配色函数
    → XYZ → 线性 sRGB（标准 3×3 矩阵）→ 按峰值归一
  - **CMF 数据来源（V5.4 补充）**：`core/include/ehe/core/cie1931_2deg.h`（380–780 nm @1 nm，401 点，
    由 `tests/tools/gen_cie1931_header.py` 生成）；原始数据为 CIE 1931 2° 标准观察者公开数据集，
    提取自 colour-science/colour（**BSD-3-Clause**）的 `cmfs.py`（抽样校验 380/555/600 nm 与 CIE 公布值一致）
  - 输出二进制 `blackbody_lut.f32`（256×3 float32）+ PNG 预览条
  - shader 侧按 log(T) 映射 UV 采样（与生成端同一映射函数，公式写入共享头注释）
- **湍流**：hash 型 value noise，**4 倍频 fbm** 调制 ρ 与发射【建议默认】；
  时间由 UBO `time` 驱动，图案按开普勒角速度 Ω(r) = r^{−3/2}（M=1）差速旋转（内快外慢）。
  **V5.8 实测修正（两处，均由画面数据倒推）**：
  ① **噪声必须主要作用在发射系数上，不能只调制密度**：盘在 κ=2、ρ~1 时是光学厚的，
     出射强度趋于源函数 S = ε/κ ∝ g³·LUT/κ——**ρ 被约掉**。实测"只调制 ρ"时画面变化仅 NMSE 4.7e-4
     （像素约 2%，肉眼不可辨）；改为"ε 主导 + ρ 弱调制（0.35 倍）"后 NMSE 6.3e-3（约 13 倍），纹理清晰可见。
  ② **采样坐标用「极坐标 + 弱垂直变化」而非裸 3D 坐标**：盘湍流在垂直方向近乎相干，
     而金标准视角（polar=75°，接近盘面）穿盘路径长，裸 3D 噪声会被沿视线平均掉。
     取 `p = ( cos(φ)·r, sin(φ)·r, 0.35·z )·scale`，φ = atan2(y,x) − Ω(r)·t（用 cos/sin 表示以避免 2π 接缝）。
  - 噪声振幅：Config 键 `disk_noise ∈ [0,1]`，**默认 0.35**（交互观感）；**golden 参数文件里显式写 0**
    （§6.1「无噪声动画」），使基线与噪声实现解耦——GPU 与 CPU 的 hash 不可能逐位一致，噪声**不进 NMSE 差分**，
    其正确性由 core 侧单元测试（确定性/值域/平滑性/旋转方向与速度）保证。
  - 空间频率 scale = 0.6（`EHE_NOISE_SCALE`，shader 常量；对应约 1.7 长度单位的特征尺寸）。
- **盘流体四速度**（赤道圆轨道，顺行）：Ω = 1 / (r^{3/2} + a)（M=1；a=0 退化为 r^{−3/2}）；
  u^t = (r^{3/2} + a) / √(r³ − 3r² + 2a·r^{3/2})（a=0 退化为 1/√(1−3/r)，光子球 r=3 处发散，
  与物理一致——时序圆轨道在光子球内不存在）；u^φ = Ω · u^t。
  **坐标关系（已验算，见附录 §13.1）**：BL→KS 变换中 φ 仅差一个 r 的函数，故坐标方向 ∂_φ 两图相同；
  对 u^r=0 的圆轨道，四速度分量在两图数值相同，KS 笛卡尔下速度方向就是纯方位向 (−y, x, 0)·u^φ——
  **盘圆轨道可直接使用上式，无需变换**。真正必须走 Jacobian 的是含径向分量/径向导数的量
  （M2 喷流、吸积流），详见 §13.1。
- **相对论效应**：g = (k·u)_obs / (k·u)_em；远场静态观测者 (k·u)_obs = −1（归一化后）。
  (k·u)_em 在**每个盘采样点**用当地 k^μ（积分态已有）与盘四速度现场计算，无需额外存储。
  强度聚束 ∝ g³，频率偏移作用于色温采样（T_eff = g · T(r)，再查 LUT）；
  朝向观测者侧亮且蓝，背侧暗且红。
- **体渲染**：raymarch 途中穿越盘体积时按发射-吸收模型前向合成：
  ```
  C ← C + (1 − α) · ε · Δs
  α ← α + (1 − α) · (1 − exp(−κ ρ Δs))
  ```
  ε = emission_scale · ρ · g³ · LUT(g·T(r))；κ 为吸收系数常数【建议默认 κ=2，入 Config】；α ≥ 0.99 提前终止。
  **emission_scale（V5.4 补充）**：上式只定义相对关系，缺绝对尺度（沿测地线累积上百步后数值可达 10² 量级，
  直接输出会使 HDR 图整体削顶）。取默认 2.5 把基线峰值归一到 [0, ~5]（实测峰值 3.15）；
  物理亮暗对比（内热外冷、多普勒聚束）仍由 ρ、F(r)、g³ 承载，该常数只做数值归一。
- **采样加速**【建议默认】：当前位置 |z| > 3σ_d 且步进方向远离赤道面时跳过盘密度/噪声求值
  （薄盘几何决定的免费裁剪，阴影轮廓计算不受影响）。

### 4.6 后处理链

HDR 线性管线（FP16 render target）→（FSR1 升频或 SSAA box 降采样，互斥二选一）→ FXAA →
ACES 色调映射（Stephen Hill 拟合【建议默认】）+ 曝光 → 色差（边缘 RGB 径向分离，强度面板可调）→
ImGui 合成。

### 4.7 相机模块规格（core/Camera）

- **轨道模式**（默认）：球坐标 (dist, azim, polar) 绕原点；dist ∈ [8, 80]（M=1，滚轮对数缩放），
  polar ∈ [0.05, π−0.05]（钳制防极点万向锁），azim 环绕无限制；左键拖拽旋转、滚轮缩放。
- **自由飞行模式**：位置 + 偏航/俯仰（俯仰 ±89° 钳制）；WASD 平移（速度面板可感：慢/中/快三档
  【建议默认】）、Shift 加速 ×5、右键拖拽转视角。两模式切换时尽量保持视线方向连续【建议默认】。
- **输出**：相机位置 + 静态 tetrad（§4.2）+ tan(fov/2)、aspect；FOV 默认 40°，范围 [20°, 90°]。
- **数值**：相机计算全程 fp64（CPU 侧无性能顾虑），上传 UBO 时转 fp32。

---

## 5. 架构与模块规格

### 5.1 模块分解（静态库）

```text
ehe/
├── CMakeLists.txt            # 顶层：C++20、选项、FetchContent、子目录、警告
├── cmake/                    # llvm-mingw toolchain 文件、Deps.cmake（依赖版本 pin）
├── core/                     # 静态库 · 纯 C++，零图形依赖
│   ├── Config                # 全部面板参数的结构体 + 序列化（JSON）
│   ├── Camera                # 轨道/自由飞行双模式，输出 tetrad + 每像素光线方向
│   ├── BlackHoleParams       # M、a、盘参数
│   ├── ReferenceIntegrator   # 【验证核心】CPU fp64 Kerr-Schild 测地线积分器 + golden 渲染
│   ├── BlackbodyLut          # Planck→sRGB LUT 离线生成器（与 shader 共用输出文件）
│   └── units/math            # 几何化单位常量、vec/mat（与 GLSL 语义对齐）
├── render/                   # 静态库 · 后端无关
│   ├── IRenderer.h           # 抽象接口（生命周期/resize/render/模式与参数注入）
│   ├── ShaderSource          # 共享 GLSL 450 源（内嵌或 shaders/ 目录加载）
│   └── PostChain             # FSR1/FXAA/ACES/色差 的声明与参数（实现各后端自带）
├── render/vk/                # 静态库 · Vulkan 1.2 + volk（+ 可选 VMA）
├── render/gl/                # 静态库 · OpenGL 4.5 + glad
├── app/                      # 可执行文件 ehe
│   ├── main                  # 启动、Config 加载、后端创建
│   ├── Window                # GLFW 封装（切后端销毁重建窗口句柄，保留几何/状态）
│   ├── Ui                    # ImGui 面板 + 左上角监控 overlay（分离）
│   ├── Audio                 # miniaudio 程序化音效
│   └── Smoke                 # 冒烟模式（--smoke：渲 N 帧截图退出，供目标机验证）
├── shaders/                  # 双后端共享 GLSL 450（raymarch、粒子、后处理、blackbody_lut）
└── tests/                    # doctest 单元测试 + CPU 参考验证 + golden 工具与基线图
```

### 5.2 关键接口规则

- `core` 不 include 任何图形/窗口头文件（编译期强制：core target 不链接 glfw/glad/volk）。
- `IRenderer` 实例由工厂按 Config.backend 创建；切换 = 销毁旧实例 → **销毁并重建 GLFW 窗口句柄**
  （`CLIENT_API` hint 在窗口创建时固定；进程不退，窗口几何/Config/UI 状态全量保留）→ 创建新实例。
- 双后端共用 **同一份 GLSL 450 源**：GL 直接编译文本；VK 由 glslang（库形式链入 render-vk）
  启动时编译为 SPIR-V。**GLSL 子集规范**：禁用 GL 专有内建、禁用 `gl_` 前缀扩展变量、
  uniform 一律走 UBO（块布局 std140，两后端共享绑定序号约定）、不用 push constant（GL 无对应物）。
- 物理参数每帧以单个 UBO（`SimParams`）下发，杜绝 CPU-GPU 往返。

### 5.3 SimParams UBO 布局（std140，binding = 0）【建议默认，字段可增不可改序】
| 偏移(vec4) | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | cam_pos | vec3 + pad | 相机位置（Kerr-Schild 坐标） |
| 1–3 | cam_basis_x/y/z | 3×vec4 | tetrad 空间基（w 分量留空） |
| 4 | cam_dir + tan_half_fov | vec3 + float | 视线中心方向；FOV |
| 5 | aspect + time + n_max + h0 | 4×float | 纵横比、动画时间、步数上限、步长因子 |
| 6 | h_min + h_max + eps_h + r_escape | 4×float | 步长上下限、视界 ε、逃逸半径 |
| 7 | spin_a + r_in + r_out + disk_density | 4×float | 黑洞/盘参数 |
| 8 | t_scale + kappa + exposure + chrom_ab | 4×float | 盘温、吸收系数、曝光、色差强度 |
| 9 | flags + res_scale + particle_count + pad | uint×4 | 位标志：FSR1/FXAA/ACES/模式/精度开关 |

### 5.4 渲染管线（raymarch 模式）

1. **Fullscreen raymarch pass**：每像素光线（§4.2）→ §4.3 积分 → §4.5 盘体渲染累积 →
   HDR FP16 颜色 + （可选）调试视图。
2. **分辨率变换**：内部档 <1.0x 时 FSR1（EASU+RCAS）升至输出；>1.0x 时 box 降采样（SSAA）。互斥。
3. **FXAA** → 4. **ACES + 曝光** → 5. **色差** → 6. **ImGui/overlay 合成** → swapchain。

#### 5.4.1 双后端资源规格（对照表）

| 资源 | Vulkan 后端 | OpenGL 后端 |
|---|---|---|
| HDR 颜色 | R16G16B16A16_SFLOAT color attachment（内部分辨率） | FP16 FBO（GL_RGBA16F） |
| UBO | descriptor set 0, binding 0（dynamic offset 不用） | glBindBufferBase(GL_UNIFORM_BUFFER, 0) |
| 黑体 LUT | sampled image, set 0 binding 1，线性采样 | texture unit 1，GL_LINEAR |
| 全屏绘制 | 全屏三角形（gl_VertexID，无 vertex buffer） | 同左（gl_VertexID，GL 4.5 支持） |
| GPU 计时 | VkQueryPool TIMESTAMP（帧首尾双时间戳） | GL_TIME_ELAPSED query |
| 同步 | frames-in-flight = 2，image-available/render-finished 信号量 | glFinish 仅 smoke 模式；正常靠 vsync |
| 帧率上限 | present mode FIFO（60）/ MAILBOX 或 IMMEDIATE（120/不限） | glfwSwapInterval(1/0) + 软件限速 |
| 粒子 | compute pipeline + SSBO（set 0 binding 2） | compute shader + SSBO（binding 2） |
| shader | glslang 运行时编 SPIR-V（Vulkan 1.2 核心特性范围内） | 直接编译 GLSL 450 文本 |

**CMake target 依赖**：`ehe_app(exe) → ehe_render_vk, ehe_render_gl → ehe_render → ehe_core`；
`ehe_tests(exe) → ehe_core`；`ehe_core` 零外部链接（仅标准库 + GLM + nlohmann/json + doctest[仅测试]）。

### 5.5 调试视图（面板下拉切换）【建议默认】

`shaded`（正常）/ `steps`（步数热力图）/ `classify`（命中=黑、逃逸=白、步尽=红）/
`g_factor`（g 因子伪彩）/ `null_drift`（|k·k| 漂移）。调试视图是定位"像黑洞但物理错"的第一工具。

### 5.6 粒子模式（"经典模式"）

- **初始化**：环带 [r_in, r_out] 均匀 + 厚度高斯（σ=0.5M）；初速 = 牛顿开普勒圆速 v=√(M/r)
  方位向 ± 5% 各向同性扰动【建议默认】。
- **积分**：leapfrog KDK（辛、二阶、每粒子每步 1 次力求值）；compute shader 每帧一步，
  牛顿引力 a = −M/r²·r̂ + 盘平面弱恢复力（防离散逃逸）【建议默认】；r < 2M 或 r > 50M 回收重生。
- **渲染**：点精灵，尺寸随距离透视衰减，加色混合进 HDR 缓冲（在色调映射前）；
  可叠加在 raymarch 背景上或独立显示（面板选择）。
- 定位：科普演示；**UI 必须明示"粒子为牛顿近似，与 GR 模式不具物理一致性"**。
- 面板参数：数量（1k–1M 对数档）、大小、初速分布、开关。

### 5.7 音频

- miniaudio 单头文件，48 kHz 单声道 data callback；
- **合成**：棕噪声（白噪声积分 + 泄漏 x ← 0.998x + 0.02w）→ 单极点低通；
  截止 = 40 Hz + 200 Hz · norm(盘密度)，增益 ∝ √norm(盘密度)【建议默认】；
  面板音量/静音；音频线程独立，不进渲染关键路径。

### 5.8 Shader 文件组织（shaders/，GLSL 450）

```text
shaders/
├── common/
│   ├── metric.glsl      # Kerr-Schild 度规/导数/Γ（对照 core/ReferenceIntegrator 逐行）
│   ├── disk.glsl        # 盘密度/通量/g 因子/体合成
│   ├── noise.glsl       # hash/value noise/fbm
│   └── simparams.glsl   # UBO 块声明（与 §5.3 布局一致，唯一来源）
├── fullscreen.vert      # 全屏三角形（3 顶点无 VBO，gl_VertexID 生成）
├── post_resolve.frag    # 分辨率变换（SSAA 面积加权降采样 / Catmull-Rom 升频，V5.9）
├── post_fxaa.frag       # FXAA（HDR 适配：边缘判据用 Reinhard 压缩后的 luma）
├── post_final.frag      # 曝光 → ACES（Hill 拟合）→ 色差 → sRGB
│   （双后端共用：GL 直接编译 GLSL 450；VK 经 render/spirv.cpp 编译为 SPIR-V 1.5，
│     并注入 EHE_VULKAN 以抹平 gl_VertexIndex / gl_VertexID 的差异）
├── raymarch.frag        # 主积分循环 + 调试视图分支
├── particle.comp / particle.vert / particle.frag
├── post_fsr1_*.glsl     # FSR1 EASU/RCAS（GPUOpen 头文件）
├── post_fxaa.frag
└── post_final.frag      # ACES + 曝光 + 色差
```

**精度约定（V5.5）**：GLSL 的 fp64 只保证**算术运算与 `sqrt`**；`exp`/`log`/`pow` 的 double 重载
在 glslang 与常见驱动下均不可用（探测用例见 `tests/src/test_shaders.cpp`）。
因此 shader 侧规则是：**关键精度路径只用 +−×÷ 与 sqrt（fbm/密度/吸收/LUT 索引等超越运算一律在 float 侧完成）**；
`0.25` 次幂用 `sqrt(sqrt(x))` 实现。另：fp64 需显式开启 `GL_ARB_gpu_shader_fp64`，且**无 fp64 硬件的 GPU 上不可用**
（见 §7 的 fp32 模式）。

**include 约定**：GL 450 无原生 `#include`（`GL_ARB_shading_language_include` 不普及），
由 render/ShaderSource 在 C++ 侧做文本递归展开后交给两后端——GL 直接编译、VK 过 glslang；
展开后源码长度写入日志，展开心智成本为零、调试行号以展开后为准【建议默认】。

---

## 6. 验证体系（四层）

| 层 | 内容 | 运行位置 | 时机 |
|---|---|---|---|
| L1 CPU 参考实现 | core 内 fp64 版同一积分器：解析基准校验（§4.4）、golden image 离线渲染、shader 移植蓝本 | 开发机 | M1 最早期 |
| L2 单元测试 | doctest：Config 序列化往返、Camera 基矢正交性、RK4 收敛阶（对谐振子/开普勒问题验证 4 阶）、r 求根 a=0 退化 | 开发机 | 持续 |
| L3 目标机冒烟 | `ehe --smoke --frames 60 --shot out` → 与 golden 的 PFM 差分（§6.3） | 目标机 | 每个里程碑 |

> 策略说明（V5.3）：开发机实测可运行 **GL 4.5** 路径（见 §2.1），但**验证策略不因此重排**——
> L3 仍在目标机执行、L4 边界不变。开发机运行 GL 仅用于开发期快速观察，**不作为验收依据**。
| L4 CI 编译检查 | 无 GPU runner：编译 + L1/L2 + LUT 一致性校验；**GPU 渲染永不进 CI**（显式接受的边界） | CI | 每次推送 |

### 6.1 Golden image 固化参数【建议默认，存 tests/golden/params.json】

512×512；相机 **r=30**、倾角 75°、方位角 0；FOV 40°；a=0；盘 [6, 20]、disk_density=1、**t_scale=10000 K**；
**N_max=1000**、h₀=0.05、h_min=1e-3、h_max=0.5；无噪声动画（time=0）；无后处理（纯 HDR 输出做差分）。
（V5.4 修正：原 r=15 落在盘径向范围 [6,20] 内且距盘面过近，相机实际嵌在盘介质里；原 N_max=300 为实时预算，
不适用于参考基准——实测 512² 下 40% 像素步数用尽，改 1000 后降到 4 个像素。）

### 6.2 差分指标

NMSE = Σ(I − G)² / Σ G²，逐通道计算取最大；阈值初值 1e-3（首次目标机对比后允许一次性校准并记录原因）。

### 6.3 Smoke 模式 CLI 规格

```
ehe --smoke [--frames N=60] [--shot path=out] [--backend vk|gl] [--config params.json]
```
行为：加载 golden 参数（默认 tests/golden/params.json）→ 无窗口离屏渲染 N 帧（预热后取末帧）→
写出 `out.pfm`（**HDR float 原始输出，差分基准**）与 `out.png`（过 ACES 的预览，仅供肉眼）→
与 golden 的 PFM 做 NMSE 差分 → stdout 打印结果 →
退出码 0（通过）/ 1（超阈值）/ 2（运行错误）。供 L3 与手动验证共用。

### 6.4 Golden 渲染器规格（core/ReferenceIntegrator）

- 输入 `tests/golden/params.json`（§6.1）；与 shader 同一套方程，全程 fp64。
- 多线程：`std::thread` 硬件并发数分块扫描线；512² × 300 步量级预计分钟级，可接受【建议默认】。
- 输出：PFM（RGB float32，写入器 ~20 行零依赖，格式简单且 IM 工具可读）+ PNG 预览（stb_image_write）。
- **为何 PFM 而非 PNG 做差分**：8-bit PNG 在 HDR 范围量化损失达 1e-2 量级，会淹没 NMSE 1e-3 阈值；
  PFM 保留 float 全精度，差分才有物理意义。

### 6.5 L2 测试用例清单（doctest）

| 用例 | 断言 |
|---|---|
| config_roundtrip | 序列化→反序列化全字段相等；未知键告警、缺失键默认 |
| camera_tetrad_orthonormal | g(e_(a), e_(b)) = η_ab，误差 < 1e-12（fp64） |
| ks_radius_a0 | r² 求根在 a=0 时等于欧氏距离²（随机 1e4 点） |
| metric_derivative_numeric | ∂r/∂H 解析式 vs 中心差分，100 随机点 rel err < 1e-6 |
| ks_metric_a0_limit | a=0 度规分量 = 解析 Schwarzschild（EF 形式）参考值 |
| rk4_convergence | 谐振子/开普勒问题步长减半误差 ÷16（4 阶） |
| photon_capture_bc | 临界碰撞参数 b_crit = 3√3 ± 0.5%（变 b 扫捕获阈值） |
| shadow_radius_image | golden 图阴影半径 3√3 ± 1px @512² |
| null_norm_init | 初始 k 类光 |k·k| < 1e-12（fp64 构造路径） |
| g_factor_monotonic | a=0 盘 g 因子随半径单调且 0 < g ≤ 1 |
| lut_monotonic_peak | 黑体 LUT 峰值波长随 T 单调左移（维恩位移） |
| lut_files_identical | tests/ 与 shaders/ 两份 LUT 的 SHA-256 一致（同 CI 校验） |

**黄金标准**：M1 完成判定 = L1 五项解析基准全过 + 目标机冒烟图与 golden 差分通过。

---

## 7. 性能预算

- 目标：**1080p60 @ 原生内部渲染**（raymarch，默认盘参数，N_max=300，目标机现代独显）。
- 内部分辨率档：0.5x / 0.67x / 0.75x / 1.0x / 1.25x / 1.5x / 2.0x；
  <1.0x 配 FSR1（可关），>1.0x 即 SSAA，FSR1 自动禁用。
- 帧率上限：60 / 120 / 不限（面板）。无自动 DRS（显式排除，防反馈振荡）。

### 7.1 实测基线（2026-09-19，供后续性能优化对照）

| 平台 | 配置 | 单帧耗时（含 `glFinish` 同步） |
|---|---|---|
| **Intel Iris Xe**（i5-1135G7 核显，目标机） | 64², N_max=60, fp32 | **28.3 ms** |
| 同上 | **512², N_max=1000, fp32** | **2516.6 ms** |
| AMD HD 7400M（2012 移动卡，开发机） | 64², N_max=60, fp32 | 2050 ms |
| 同上 | 64², N_max=60, **mixed(fp64)** | 2164 ms（且 ≥64² 会触发 Windows TDR 驱动复位） |

**结论（影响 §7 的优化路线）**：
1. 核显（Iris Xe）在 512²/N_max=1000/fp32 下 **2.5 s/帧**——即当前实现在核显上**远达不到 1080p60**，
   §7 的 60 fps 目标必须依赖**降内部分辨率（0.5x + FSR1）与步数预算（N_max 由 1000 降到 ~200–300）**；
   2. 64²/N_max=60 只要 28.3 ms → 说明成本几乎全部来自「像素数 × 步数」，
   优化优先级：**先用 FSR1 降内部分辨率，再压 N_max / 自适应步长**（二者线性收益）。
3. **fp64 在无硬件支持的 GPU 上不可用**：核显上 fp64 仅可编译（GL 侧软件模拟；Vulkan 直接 `shaderFloat64=0`）
   → 性能相关结论一律以 fp32 为准，§4.3 的"混合精度"在目标机上的实际默认值应为 fp32。

---

## 8. UI 规格（验收依据）

**主面板（ImGui，可折叠分组）**：

| 分组 | 参数 |
|---|---|
| 渲染 | 后端(Vulkan/OpenGL)、模式(raymarch/粒子)、内部分辨率档(0.5–2.0x)、FSR1 开关、抗锯齿(FXAA 开关/SSAA 档)、帧率上限(60/120/不限)、调试视图(§5.5) |
| 黑洞 | 质量 M、自旋 a（M1 灰显锁 0）、盘内半径、盘外半径、盘密度、盘温 T_scale、**盘湍流噪声振幅 `disk_noise`（0–1，V5.8 增补）+ 动画开关** |
| 积分器 | 最大步数 N_max、步长因子 h₀、步长上下限 h_min/h_max、精度模式(混合 fp64 / 纯 fp32) |
| 粒子 | 数量(1k–1M 对数档)、大小、初速分布、粒子开关 |
| 后处理 | ACES 开关、曝光、色差强度 |
| 音频 | 音量、静音 |
| 相机 | 模式(轨道/自由飞行)、FOV、距离、（轨道：方位角/极角） |

**监控 overlay**（左上角，与主面板分离，可独立开关）：
FPS、帧时间 ms、内部分辨率×输出分辨率、当前后端、GPU 耗时（timer query）、raymarch 平均步数。

### 8.1 Config JSON schema（与面板一一对应，键名固定）

```json
{
  "render":  { "backend": "vk|gl", "mode": "raymarch|particle", "res_scale": 1.0,
               "fsr1": true, "fxaa": true, "fps_cap": 60, "debug_view": "shaded" },
  "blackhole": { "mass": 1.0, "spin": 0.0, "disk_r_in": 6.0, "disk_r_out": 20.0,
                 "disk_density": 1.0, "disk_t_scale": 10000.0, "disk_kappa": 2.0,
                 "disk_noise": 0.35 },
  "integrator": { "n_max": 300, "h0": 0.05, "h_min": 0.001, "h_max": 0.5,
                  "precision": "mixed|fp32" },
  "particle": { "count": 100000, "size": 1.0, "velocity_profile": "kepler", "enabled": false },
  "post":    { "aces": true, "exposure": 1.0, "chrom_ab": 0.5 },
  "audio":   { "volume": 0.5, "muted": false },
  "camera":  { "mode": "orbit|fly", "fov_deg": 40.0, "dist": 15.0,
               "azim_deg": 0.0, "polar_deg": 75.0 }
}
```

规则：未知键忽略并告警；缺失键取默认值；启动加载、退出/切换后端时保存（`ehe.config.json`）。

---

## 9. 依赖清单（全部 FetchContent 源码构建，版本 pin 于 `cmake/Deps.cmake`）

| 依赖 | 用途 | 形态 |
|---|---|---|
| GLFW | 窗口/输入 | 源码构建 |
| GLM | 数学（CPU 侧） | header-only |
| ImGui | 面板（docking 分支不强制） | 源码 |
| glad | GL 4.5 加载器 | 生成文件入库 |
| Vulkan-Headers + volk | Vulkan 免 SDK | header + 源码 |
| glslang | GLSL→SPIR-V（库形式链入 render-vk） | 源码构建 |
| miniaudio | 音效 | 单头文件 |
| doctest | 单元测试 | 单头文件 |
| nlohmann/json | Config 序列化 | header-only |
| FidelityFX FSR1 | 空间升频（`ffx_a.h` / `ffx_fsr1.h`，GPUOpen） | 头文件入库 |
| stb | PNG 写出（`stb_image_write.h`，smoke/golden 预览） | 单头文件 |

注：Vulkan-Headers 与 volk 为两个独立 FetchContent 仓库（共 11 个仓库）；
ImGui 需同时编译 `imgui_impl_glfw` + `imgui_impl_opengl3` + `imgui_impl_vulkan` 三个后端文件。

### 9.1 工具链部署规格（M0/T0.1 执行清单）

- **LLVM-MinGW**：mstorsjo/llvm-mingw GitHub Releases，`llvm-mingw-<ver>-ucrt-x86_64.zip`，
  解压至 `C:\tools\llvm-mingw`【建议默认路径】；版本号 pin 在 `cmake/llvm-mingw-toolchain.cmake` 注释中，
  升级需变更流程。`bin\` 加入 PATH（或 toolchain 文件内绝对路径引用，优先后者——不污染全局环境）。
- **Ninja**：ninja-build/ninja Releases `ninja-win.zip`，放入 `C:\tools\ninja\`。
- **验证**：`clang++ --version` 输出目标 `x86_64-w64-mingw32`；`cmake -G Ninja` 空项目配置通过。
- **CI 同源**：CI runner 用同一版本号的 LLVM-MinGW 压缩包（URL 写入 CI 配置），禁止"本地新版、CI 旧版"。

---

## 10. 里程碑开发计划

### M0 — 工具链与骨架（验收：空窗口双后端可编译）
- T0.1 部署 LLVM-MinGW + Ninja；`clang++ --version`、CMake toolchain 链路验证
- T0.2 git init + MIT LICENSE + 目录骨架（§5.1）
- T0.3 FetchContent 全部依赖（11 个仓库，§9）拉通编译
- T0.4 GLFW 空窗口 + ImGui 空面板，GL/VK 两后端各初始化一次（目标机人工确认）

### M1 — v1（验收：§6 黄金标准 + §8 面板全参数生效）
- T1.1 core：Config/Camera（轨道先行）/BlackHoleParams + doctest 基线
- T1.2 **L1 CPU 参考实现**：Kerr-Schild 积分器（a=0）+ §4.4 解析基准 + golden image 渲染器 + 黑体 LUT 生成器
- T1.3 GLSL raymarch 内核移植（对照 L1 逐行）+ fullscreen quad 管线（GL 先行）
- T1.4 盘体渲染（通量剖面 + 黑体 LUT + 噪声差速）+ g 因子红移/聚束
- T1.5 后处理链（HDR→FSR1/SSAA→FXAA→ACES→色差）
- T1.6 Vulkan 后端对齐 + 运行时实例重建切换
- T1.7 面板全参数 + 监控 overlay + 调试视图 + 自由飞行相机
- T1.8 粒子模式（compute）+ 音效
- T1.9 目标机冒烟（L3）通过 → **v1 发布**

### M2 — Kerr 解锁 + 后置视觉项
- 自旋 a 解锁（UI 解禁）、极端 Kerr 数值钳制验证
- Page–Thorne 完整相对论通量修正因子评估（§4.5）
- 相对论喷流（沿自旋轴双极体积噪声）
- 星空 cubemap 背景 + 引力透镜（逃逸光线采样 cubemap）
- L1 基准扩展：a>0 阴影偏心轮廓校验（与文献图比对）；静态 tetrad → ZAMO 评估（§4.2）

### M3 — 高级视觉
- TSR/TAAU 时间升频（需解决体渲染重投影鬼影：以测地线终点的角坐标重投影替代 motion vector）
- 偏振 EHT 视图（偏振矢量沿测地线并行传输，叠加线段 overlay）

### Backlog（显式不做进里程碑）
多波段观测、光传播时间延迟、触控支持、自动 DRS。

---

## 11. 编码与工程规范

- C++20；`-Wall -Wextra -Wpedantic`；命名：类型 `PascalCase`、函数/变量 `snake_case`、
  成员后缀 `_`、宏/常量 `EHE_UPPER_SNAKE`。
- 中文注释（物理公式必须注明来源/推导要点），英文标识符；公开接口头注释块。
- 每个静态库一个 `CMakeLists.txt`；依赖版本集中 pin 于 `cmake/Deps.cmake`；禁止全局 `include_directories`。
- commit 信息中文、祈使句；里程碑打 tag（`v1.0-m1` 等）。

---

## 12. 风险登记

| 风险 | 等级 | 缓解 |
|---|---|---|
| 开发机零 GPU 验证，"像黑洞但物理错" | **高** | L1 CPU 参考 + golden 差分（§6）+ 调试视图（§5.5） |
| 混合精度 fp64 在目标卡开销未知 | 中 | `EHE_FP64_METRIC` 开关回退纯 fp32；面板精度模式实时对比 |
| GLSL 双后端行为差异（精度/内建函数） | 中 | §5.2 子集规范 + L3 双后端互相对比截图 |
| Christoffel 解析表达式推导错误 | 中 | 符号工具生成 + 人工核对 + L1 基准兜底（§4.1） |
| clang-mingw 与个别 C 依赖构建脚本摩擦 | 中 | 回退 WinLibs GCC（已评估备选） |
| 粒子/GR 模式画面不一致误导用户 | 低 | UI 明示牛顿近似（§5.6） |
| Kerr a→1 数值病态 | 低 | a 钳制 0.998 + 步长自适应 |

---

## 13. 变更管理

本文档为唯一权威来源。任何范围/决策变更：提出 → 确认 → 修改本文档并递增版本号（V2.x），
在文首追加版本行。不接受以聊天记录、口头约定为依据的开发变更。
标注【建议默认】的条目是实现细节：可直接按此实现；如实现中发现更优解，修改时同样递增版本号并注明。

### 13.1 附录：Boyer–Lindquist ↔ Kerr-Schild 坐标关系（M2 必读）

变换（M=1，Δ = r² − 2r + a²，r± = 1 ± √(1−a²)）：

```
t_KS = t_BL + ∫ 2r/Δ dr
φ_KS = φ_BL + ∫ a/Δ dr = φ_BL + a/(r₊−r₋) · ln((r−r₊)/(r−r₋))
x = √(r²+a²) sinθ cosφ_KS,   y = √(r²+a²) sinθ sinφ_KS,   z = r cosθ
```

**关键性质（已验算）**：φ 的差只依赖 r，故在固定事件上坐标方向 ∂_φ 两图相同；
混合发生在 ∂_r 与 ∂_t：∂/∂r_BL = ∂/∂r_KS + (2r/Δ)∂/∂t_KS + (a/Δ)∂/∂φ_KS。

推论：
1. **盘圆轨道（u^r = 0）**：四速度分量两图数值相同，KS 笛卡尔下 u = u^t∂_t + u^φ(−y∂_x + x∂_y)，
   无需任何变换（§4.5 据此直接使用 BL 的 Ω、u^t 公式）。
2. **含径向分量的量（M2 喷流/吸积流、任何 ∂_r 导数）**：必须经上述 Jacobian 变换，
   径向分量会混入 t 与 φ 方向；遗漏将导致喷流方向错误且无直观画面异常，属高危静默错误。
3. a=0 时 Δ = r(r−2)，变换退化为 Eddington–Finkelstein 型时间平移，空间坐标完全一致。
