# EHE 任务拆解 V1.13

> 执行清单，与 `DESIGN.md`（规格权威）配套使用。冲突时以 DESIGN.md 为准。
> 每条任务：可勾选状态、验收标准、依赖、产出物。完成后勾 `[x]` 并注明日期。
> 版本：V1.0（基于 DESIGN.md V5.1 拆出）→ V1.1（协作设施）→ V1.2（看板）→ V1.3（T0.1）
> → V1.4（T0.2）→ V1.5（T0.3）→ V1.6（T0.4 + GL 能力勘误）→ V1.7（T1.1）
> → V1.8（T1.2 度规内核/RK4/解析基准）→ V1.9（T1.2 完成）→ V1.10（T1.3 移植完成）→ V1.11（T1.3 目标机门禁通过）
> → V1.12（T1.4 盘体湍流完成）→ V1.13（T1.5 后处理链完成，FSR1 待收尾）。

图例：🏁 = 里程碑验收门；⛓ = 有前置依赖；产出物用 `代码格式` 标注。

---

## M0 — 工具链与骨架（DESIGN §10/M0，验收：空窗口双后端可编译）

### T0.1 工具链部署（DESIGN §9.1）—— ✅ 2026-09-13
- [x] T0.1.1 下载 `llvm-mingw-20260908-ucrt-x86_64.zip`（190,677,197 字节），解压 `C:\tools\llvm-mingw`
  - 实测：`clang version 23.1.1`；`clang -dumpmachine` → `x86_64-w64-windows-gnu`
    （与 `x86_64-w64-mingw32` 等价，验收口径按实测三元组）
- [x] T0.1.2 下载 `ninja-win.zip`（v1.13.2），解压 `C:\tools\ninja`；`ninja --version` → 1.13.2
- [x] T0.1.3 编写 `cmake/llvm-mingw-toolchain.cmake`
  - 绝对路径引用编译器（`EHE_LLVM_MINGW_ROOT` 可覆盖），不污染 PATH
  - **新增 `EHE_STATIC_RUNTIME`（默认 ON）**：llvm-mingw 默认动态链接 libc++，直接运行报
    `0xC0000135`；改静态后产物仅依赖系统 DLL（KERNEL32 + UCRT），可拷贝到目标机直接运行
- [x] T0.1.4 空 CMake 项目全链路验证通过（探针工程，非仓库源码）
  - `cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=... -DCMAKE_MAKE_PROGRAM=C:/tools/ninja/ninja.exe`
  - 编译 + 链接 + 运行均通过；探针覆盖 concepts/constexpr、libc++ 容器、cmath、异常
  - 实测输出：`cpp_std=202002`、`libcxx=230101`

> 下载通道记录（网络受限时复用）：gh 直连 release 下载在本机极慢/卡死（约 1MB/min）；
> 镜像 `https://v4.gh-proxy.org/https://github.com/<o>/<r>/releases/download/<tag>/<asset>` 实测
> 1.45 MB/s。解压用 Python `zipfile`（`tar.exe`/`curl.exe` 本机缺失，`Add-Type` 被沙箱策略拦截）。

### T0.2 仓库骨架 ⛓T0.1 —— ✅ 2026-09-13
- [x] T0.2.1 `git init` + `LICENSE`（MIT）+ `.gitignore`(build/、.cache/、ehe.config.json、*.pfm) —— 2026-09-13
- [x] T0.2.2 目录骨架（DESIGN §5.1）—— core/ render/{,vk,gl}/ app/ shaders/ tests/ cmake/ 全部就位，含占位源码
- [x] T0.2.3 顶层 `CMakeLists.txt`：C++20、警告集（`ehe_warnings`）、选项（`EHE_FP64_METRIC` 默认 ON、
  `EHE_BUILD_TESTS`）、`ehe_options` 编译期开关、输出目录集中、构建摘要打印
  - target 依赖实测：`ehe_app → ehe_render_vk / ehe_render_gl → ehe_render → ehe_core`（与 §5.4.1 一致）
  - **验收通过**：`cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=... -DCMAKE_MAKE_PROGRAM=C:/tools/ninja/ninja.exe`
    配置 + 编译 12/12 全绿、零警告；`ehe.exe` 输出
    `Event Horizon Engine 0.1.0 [fp64_metric=1] / backends compiled: 2 (vulkan, opengl)`；`ehe_tests.exe` 运行正常
  - 产物：`build/bin/ehe.exe`（1.4MB，静态运行时）、四个静态库
- [x] T0.2.4 协作设施：issue 模板（bug_report/task）+ PR 模板 + CONTRIBUTING.md 分支策略 —— 2026-09-13
- [x] T0.2.5 本地 `main` 初始提交 —— 2026-09-13
- [x] T0.2.6 建 `dev` 分支 —— 2026-09-13（远端已建并同步至 main）
- [x] T0.2.7 GitHub 远程仓库：`lbn2011/Event-Horizon-Engine`（Public）—— 2026-09-13
  - 旧仓库按用户决定删除；完整克隆备份在 `C:\Users\lbn\Desktop\code\Event-Horizon-Engine-legacy-backup`
  - ⚠ 遗留：git 直连推送受本机代理阻断（502），当前改动经 GitHub API 同步；网络恢复后验证 `git push`
- [x] T0.2.8 任务看板与 issue 体系（Projects v2，issue #3–#22 + 里程碑 M0–M3）—— 2026-09-13

### T0.3 依赖拉通（11 个仓库）⛓T0.2 —— ✅ 2026-09-13
- [x] `cmake/Deps.cmake`：11 个仓库 FetchContent 声明 + 版本 pin
  - glfw 3.5.1 / glm 1.0.3 / imgui v1.92.9 / Vulkan-Headers + volk + glslang = vulkan-sdk-1.4.357.0 /
    doctest v2.5.3 / nlohmann-json v3.12.0 / miniaudio 0.11.25 / FSR1 v1.0.2 / stb（提交 pin）
- [x] 逐个验证编译：全部通过（详见下方验收记录）
- [x] glad 生成入库：`third_party/glad`（`glad --profile core --api gl=4.5 --generator c`；注册表经本地服务注入）
- [x] FSR1/stb/miniaudio 按头文件使用；ImGui 三后端文件（glfw + opengl3 + vulkan）编译进 `ehe_imgui`

**验收记录（2026-09-13）**
- 配置：11/11 依赖拉取完成（首次配置 713s），`-DFETCHCONTENT_BASE_DIR=<持久目录>` 使依赖跨 build 复用（缓存放 `C:\Users\lbn\Downloads\ehe-deps`，213.6MB）
- 构建：**54/54 target 全部成功**，`libglslang.a`、`libehe_imgui.a`、`libglfw3.a`、`libglad.a`、`libvolk.a`、`libminiaudio.a`、`libdoctest_with_main.a` 等全部产出
- 运行：`ehe.exe` 正常；`ehe_tests.exe` **doctest 2.5.3，2 用例 / 4 断言全通过**
- 踩坑与修复（均已写入 Deps.cmake 注释与 CONTRIBUTING）：
  1. **FetchContent 子进程不读仓库级 .git/config** → 镜像改写须用 `GIT_CONFIG_*` 环境变量注入
  2. **CMake 默认执行 `git submodule update`，而本机 Git shell 脚本不可用** → 全部依赖 `GIT_SUBMODULES ""`；
     经查 glslang 新版已自带 `SPIRV/spirv.hpp11` 且 `ENABLE_OPT=OFF`，确实无需子模块
  3. **glslang 在未指定构建类型时默认 Debug** → 产出 `libglslangd.a`，与空配置的 `libglslang.a` 不匹配；
     顶层显式设默认 `CMAKE_BUILD_TYPE=RelWithDebInfo`
  4. `glslang::SPIRV` 在 `ENABLE_OPT=OFF` 下是空壳库 → 不链接（glslang 主库已含 SPIRV 源码）
  5. `imgui_impl_vulkan.cpp` 需要 vulkan 头 → Vulkan-Headers/volk 声明前移到 ImGui 之前，并启用 `IMGUI_IMPL_VULKAN_USE_VOLK`
  6. 关闭 `ENABLE_GLSLANG_BINARIES`（运行时用库，不需要 glslang/glslangValidator 工具，省 73MB 产物）

### T0.5 CI 编译检查（DESIGN §6 L4）—— ✅ 2026-09-14（审查发现缺失后补建）
- [x] `.github/workflows/ci.yml`：同源 LLVM-MinGW + Ninja → 配置 → 构建 → **全量测试（L2 + L1 基准）**
      → **LUT 可复现性校验**（重烘焙后 `git diff --exit-code`）→ GL 自检（`continue-on-error`）
- [x] GPU 渲染刻意不进 CI（§6 明确边界）
- [x] 首次运行抓出并修复 3 个真实缺陷：Action 需钉 SHA（仓库启用 `sha_pinning_required`）、
      LLVM-MinGW 解压目录改名、choco 的 PATH 在同一步内不生效（ninja 需绝对路径）
- **验收**：push / PR 触发均 **success**（干净机器上 43 用例 / 34962 断言通过 + LUT 校验通过）

### T0.4 空窗口双后端 ⛓T0.3
- [x] T0.4.1 GL 后端：GLFW 窗口 + GL 4.5 core context + glad（`gladLoadGLLoader`）+ ImGui（glfw/opengl3）
- [x] T0.4.2 Vulkan 后端：volk → instance(1.2) → 物理/逻辑设备 → 自管 swapchain + render pass +
  framebuffer → 命令缓冲/同步（2 帧并行）→ ImGui（`imgui_impl_vulkan`，配合 volk）
- [x] T0.4.3 `IRenderer` 抽象 + app 层工厂 + 面板下拉切换（销毁窗口句柄 → 新后端重建，进程不退）；
  尺寸自适应由各后端在 `begin_frame` 内处理（GL 重设 viewport；VK 重建交换链），最小化时跳帧
- [x] T0.4.4 运行参数：`--backend` / `--width` / `--height` / `--vsync` / `--frames=N`（限帧）/
  `--try-backends`（后端自检）

**验收记录（2026-09-14）**
- 构建：11/11 全绿（`ehe.exe` 链接双后端）
- **开发机运行验证（GL 路径）**：`--try-backends` → `opengl => OK`，`GL_VERSION=4.5.13399 Core Profile`、
  `GL_RENDERER=AMD Radeon HD 7400M Series`；`--frames=2` 跑通窗口 + ImGui 面板 + 渲染循环并正常退出
- **重要环境勘误（已同步 DESIGN V5.2）**：开发机 GL 能力**实测为 4.5 核心**，非文档原判的 ≤4.1
  → GL 路径运行时验证可在开发机完成；Vulkan 因缺 `vulkan-1.dll`（无 ICD）仍需目标机
- 单元测试：doctest 2 用例 / 4 断言全通过
- ⏳ 待办（目标机）：`--backend=vk` 起窗、面板内 GL↔VK 切换不崩、窗口几何保留 —— 🏁 M0 门禁待此确认

---

## M1 — v1（DESIGN §10/M1，🏁 验收 = §6 黄金标准 + §8 面板全参数生效）

### T1.1 core 基础 ⛓T0.4 —— ✅ 2026-09-14
- [x] T1.1.1 `Config`：§8.1 schema 全字段 + nlohmann/json 序列化 + 容错（未知键告警/缺失默认/类型不符告警）
      + 值域钳制（spin→0.998、res_scale→[0.5,2.0]、步长有序性、盘几何、相机范围）+ 文件读写
- [x] T1.1.2 `units.h` / `math.h`：几何化单位常量（§4.4 解析基准：r_s=2、光子球=3、阴影=3√3、ISCO=6）
      + 视界公式 r₊ = M+√(M²−a²) + 球坐标/角度/钳制工具（全程 fp64）
- [x] T1.1.3 `Camera`：轨道与自由飞行双模式、模式切换保持视线连续、对数缩放、极角防万向锁、
      像素→光线（§4.2 公式，中心像素即前向）、静态 tetrad 输出（度量感知版随 T1.2）
- [x] T1.1.4 测试接入：doctest 用例 `config_roundtrip`、`camera_tetrad_orthonormal` 等

**验收记录（2026-09-14）**
- `ehe_tests`：**15 个用例 / 102 个断言全部通过**
- 覆盖：Config 往返（含逐字段相等）、未知键/缺失键/类型不符处理、自旋与值域钳制、文件读写往返；
  tetrad 正交性 g(e_a,e_b)=η_ab（容差 1e-12）、基矢正交归一、像素→光线映射（中心=前向、单位长度、FOV 单调性）、
  轨道钳制（dist/polar/fov）、模式切换视线连续、自由飞行移动与俯仰钳制
- 过程中测试抓到一处**单位误解**：DESIGN §4.7 的 polar 界限 0.05 是弧度（≈2.8648°），
  已在测试中按 `rad_to_deg(kPolarMin)` 校正（代码本身正确）

> 说明：静态 tetrad 当前为「笛卡尔正交基（η 归一化，a=0/远场极限）」，
> 完整的按 Kerr-Schild 度量 Gram–Schmidt 版本随度规模块在 T1.2 接入，接口保持不变（DESIGN §4.2）。

### T1.2 L1 CPU 参考实现 ⛓T1.1（本阶段最重要，DESIGN §6/L1）—— ✅ 2026-09-14（L1 门通过）
- [x] T1.2.1 Kerr-Schild r 求根 + 度规分量（§4.1，fp64）—— `core/metric.{h,cpp}`
  - 测试：`ks_radius_a0`（1000 随机点退化欧氏距离）、`ks_radius` 隐式定义满足性（a ∈ {0,0.3,0.7,0.998}）
  - 测试：`ks_metric_a0_limit`（a=0 精确退化为 Schwarzschild：g_tt=−1+2/r、g_ti=2x_i/r²、g_ij=δ_ij+2x_ix_j/r³）
- [x] T1.2.2 度规导数链式三层（§4.1.1）+ Christoffel 组装
  - 测试：`metric_derivative_numeric`（∂r/∂H/∂k 解析 vs 中心差分，rel err < 1e-6，60 点 × 3 轴 × 3 个自旋）
  - 测试：`christoffel` 后两指标对称性（< 1e-12）
- [x] T1.2.3 RK4 积分器 + 自适应步长 + 终止条件（§4.3）—— `core/integrator.{h,cpp}`
  - 测试：`rk4_convergence`（步长减半误差比 ≈16，实测落于 [12,20]）、`null_norm` 全程类光性漂移 < 1e-6（设计验收 1e-3）
  - 测试：三终止条件（正对中心→Captured、朝外→Escaped、步数用尽→Exhausted）、`adaptive_step` 上下限与单调性
  - 测试：守恒量漂移（E、L_z 相对漂移 < 1e-7）
- [x] T1.2.4 解析基准验证：光子球、b_crit 捕获扫描、阴影角（§4.4）
  - **实测（2026-09-14，fp64）**：
    - 临界冲击参数 `b_crit = 5.1961524441`，解析 `3√3 = 5.1961524227` → **相对误差 4.1e-09**（设计容差 ±0.5%）
    - 阴影角（观测者 r=15）`0.32835918 rad` vs 解析 `0.32835918 rad` → **相对误差 2.9e-09**（设计容差 1%）
- [x] T1.2.5 黑体 LUT 生成器（§4.5 生成规格）—— `core/blackbody.{h,cpp}` + `core/cie1931_2deg.h`
  - CMF 数据：CIE 1931 2° @1nm（380–780，401 点），由 `tests/tools/gen_cie1931_header.py` 从
    BSD-3 项目提取并生成常量头；抽样校验 380/555/600 nm 与 CIE 公布值一致
  - LUT：256 点对数等距 [1000, 40000] K，二进制 `blackbody_lut.f32`（magic+count+float32×3）
  - **两份拷贝 SHA-256 实测一致**：`c95b51c58a33a2d5121702c9733a07dcc02f1216739e163424298b76c0acc136`（tests/golden 与 shaders）
  - 测试：`lut_monotonic_peak`（Wien 位移 + 红蓝比单调）、`lut_files_identical`、LUT 往返读写、越界钳制
- [x] T1.2.6 golden 渲染器（§6.4）—— `core/golden.{h,cpp}` + 工具 `ehe_reftool`
  - 盘体渲染（中点采样，抑制层状伪影）+ 无扭矩通量剖面 + 黑体色温 + g 因子（引力+多普勒，强度 g³、色温 g·T）
  - 三终止条件 + 调试视图（shaded / classify / g_factor）；PFM（差分基准）+ PNG（Reinhard 预览）输出
  - 测试：`shadow_radius_image`（图像测量 vs 解析 **0.245%**）、`g_factor_monotonic`（范围 [0.51, 1.46] 跨越 1）、
    shaded 非零且无 NaN、类光漂移 1.15e-08
- [x] T1.2.7 🏁 **L1 门**：§4.4 五项基准全过 + golden PFM 入库 `tests/golden/`
  - 五项：光子球（直接动力学验证：r=3M 切向光子停留 ≥10 步且最小半径 >0.6·r₊）、阴影半径（图像 0.245%）、
    ISCO 常量（6M）、g 因子（单调/跨 1）、类光归一化（1.15e-08 ≪ 1e-3）
  - golden 基线：`tests/golden/golden.pfm`（512², 3.07 MB）+ `golden.png` 预览
  - **渲染过程中暴露并修正 4 处规格缺口 → DESIGN 升 V5.4**（见文档版本行）

**验收记录（2026-09-14，本轮）**
- `ehe_tests`：**30 个用例 / 5984 个断言全部通过**（含上轮 T1.1 的 15 用例）
- 关键：解析基准的数值精度远超设计容差，说明 Kerr-Schild 度规（含逆度规无分母形式）、
  解析导数链与 Christoffel 组装、RK4 + 自适应步长三者均正确
- 修复的两个真问题（均由测试暴露）：
  1. `is_captured` 起点半径 1000 > 逃逸半径 100 → 光线第一步即被判逃逸，二分永不捕获；
     现按起点距离自动放大逃逸半径并注明该耦合
  2. 冲击参数带符号（表示绕轴旋转方向），临界值需取模长——否则 b_crit 得到 −5.196 而非 +5.196

> 说明：`photon_capture_bc` 用的是「二分 y₀ + 由守恒量精确计算 b」的方式，
> 因此结论不受起点半径有限的 O(M/D) 误差影响；光子球半径 3M 由 b_crit = 3√3 M 间接锁定
> （临界冲击参数与光子球半径在 Schwarzschild 下是同一物理量的两种表述）。

### T1.3 GLSL 内核移植（GL 先行）⛓T1.2 —— ✅ 2026-09-14（shader↔CPU 数值一致性已验；肉眼对齐待目标机）
- [x] T1.3.1 `shaders/common/` 四文件（simparams/metric/disk/noise）+ `fullscreen.vert`/`raymarch.frag`/`present.frag`，
      对照 T1.2 逐行移植；5 种调试视图在 shader 内实现（§5.5）
- [x] T1.3.2 ShaderSource 文本 include 展开器（§5.8 约定）+ `insert_defines_after_version`（精度宏注入，按 `#version` 行定位）
- [x] T1.3.3 fullscreen triangle + raymarch.frag 主循环 + UBO 上传（§5.3 布局 + V5.5 的 extras 槽）
      + FP16 FBO（内部分辨率 = 渲染尺寸 × res_scale）+ 黑体 LUT 纹理（unit 1）+ 呈现 pass
- [x] T1.3.4 调试视图 5 种（§5.5）：shaded / steps / classify / g_factor / null_drift（面板下拉切换）
- [x] T1.3.5 **shader 静态校验（无需 GPU）**：glslang 解析展开后的源码，mixed 与 fp32 两种精度都要过
      → 已进 CI（§6 L4 的"GPU 渲染永不进 CI"与"shader 错误必须早暴露"由此兼容）
- [x] T1.3.6 **shader↔CPU 数值一致性（本机 GL 4.5）**：同参数（32², n_max=60）双侧渲染后差分
      → **NMSE = 1.63e-04**（阈值 1e-3）**通过**；CPU 侧 512² 基线可**逐位复现**（NMSE = 0）
- [x] T1.3.7 🏁 **目标机（Intel Iris Xe）实测通过**（2026-09-19）：
      **512² 抹烟 NMSE = 5.654e-04 < 1e-3**（fp32；单帧 2516.6 ms）；64² = 1.760e-04（28.3 ms/帧）
      独立核对：目标机自报值 == 开发机 reftool 重算值；两侧 golden.pfm SHA-256 一致
      遗留：窗口模式肉眼对齐（`-Windowed`）与 Vulkan 后端运行时切换（issue #27）
      - 已知待查：本机 128² 及以上在 `capture_hdr` 处崩溃 → **目标机 512² 回读正常**，确认为老驱动特例
**关键发现（已写入 DESIGN V5.5）**
- GLSL fp64 只保证算术与 `sqrt`（exp/log/pow 的 double 重载不可用）→ 超越函数下沉 float
- 无 fp64 硬件的 GPU（Terascale）上 fp64 片元 2.16 s/帧并触发 TDR 崩溃；fp32 变体 12.7 ms/帧（≈170×）
- smoke 渲染尺寸必须与窗口尺寸解耦（Windows 强制窗口最小尺寸：32×32 被拉到 120×32）
- PFM 行序必须统一（CPU 侧原写反 → NMSE≈1.8；修正后 1.6e-4）


### T1.3 附带（目标机验证设施，2026-09-19 增补）—— ✅ 完成
- [x] `ehe --caps`：能力探测（GL/VK 版本与设备、关键扩展/设备特性、rgba16f 附件功能性检查、
      上限值；两种精度模式的可编译性 + fp32 帧耗时；`--time-mixed` 可选测 fp64）
      - 踩坑记录：同进程二次创建 GL 上下文会失败 → 精度切换改走 `rebuild_pipeline`（T1.7 也复用）
      - 安全策略：fp64 计时默认 **关闭**（无 fp64 硬件的 GPU 上实测会挂死驱动，只能强杀）
- [x] **目标机便携包**（CI 产出 `ehe-target-bundle.zip`）：静态链接 exe + shaders + golden 基线 + 脚本 + 指南
- [x] `tools/run_target_tests.ps1`：一键跑完（环境信息 → --caps → 小尺寸抹烟 → 512² 抹烟 → 后端探测），
      全部输出落成 `target_report.txt` 单文件回传
- [x] `docs/TARGET_MACHINE_TESTS.md`：目标机验证指南（含 Intel 核显的预期与判读表）
- [x] 计时方法论修正：smoke 预热帧计时加入 `finish()` 同步（否则低估约 20 倍）

### T1.4 盘体渲染 + 相对论效应 ⛓T1.3 —— ✅ 2026-09-19
- [x] T1.4.1 通量剖面 + 高斯厚度 + 采样裁剪（§4.5）（含"|z|>3σ 且步进远离赤道面则跳过"的免费裁剪）
- [x] T1.4.2 黑体 LUT 纹理绑定（binding 1）+ log(T) UV 映射（T1.3 已随盘体一起落地）
- [x] T1.4.3 g 因子（每采样点现场计算）→ 聚束 g³ + T_eff 色温偏移（T1.3 已落地，NMSE 已验）
- [x] T1.4.4 噪声 fbm(4 倍频) + 差速旋转动画（UBO time 驱动）
      - **两处实测修正**（写进 DESIGN V5.8）：噪声主要作用于**发射系数**（光学厚盘源函数与 ρ 无关——
        只调制 ρ 时画面仅变 NMSE 4.7e-4，改 ε 主导后 6.3e-3，13 倍且肉眼可见）；
        采样坐标改**极坐标 + 弱垂直变化**（裸 3D 噪声被长穿盘路径平均掉）
      - `core/noise.{h,cpp}` 提供 CPU 对照实现 + 5 个单测（确定性/值域/平滑性/4 倍频/差速旋转方向与速度）
      - 新增 Config 键 `blackhole.disk_noise`（默认 0.35；golden 参数文件显式 0）
      - 验证：**噪声关闭时输出与引入该功能前逐位一致**（PFM SHA 相同，零回归）；
        噪声开启时同参数两次运行逐位一致（确定性）；`--time` 改变图案

### T1.5 后处理链 ⛓T1.3 —— ✅ 2026-09-19（T1.5.2 的 FSR1 待收尾）
- [x] T1.5.1 FP16 HDR 缓冲 + 内部分辨率档（0.5x–2.0x）+ 输出分辨率 LDR 目标 + blit 呈现（V5.9）
- [x] T1.5.2 SSAA **面积加权**降采样 / Catmull-Rom 升频互斥逻辑（1.25x/1.5x 等非整数档可用）
      ⏳ **FSR1（EASU+RCAS）待收尾**：官方 ffx_a.h / ffx_fsr1.h 支持 GLSL 模式（A_GPU+A_GLSL 宏 + 回调），
      需 vendor 头文件 + 常量缓冲计算 + 两个 pass 接进 GL 链（下一条增量）
- [x] T1.5.3 FXAA → ACES(Hill 拟合)+曝光 → 色差（§4.6 链序）
      - FXAA 的 HDR 适配：判据用 Reinhard 压缩后的 luma（阈值按 LDR 调过，直接吃 HDR 会过模糊）
      - core/tonemap.{h,cpp} 提供 ACES/sRGB/box/Catmull-Rom/色差的 CPU 同式实现 + 6 个单测
        （含手算锚点 ACES(1.0)=0.61912 → sRGB 0.8100）
      - **新增 post 一致性对照**：GPU 后处理成品 vs CPU 参考，三条分辨率路径实测 最大差 1/255
      - capture_hdr 语义修正：PFM = "分辨率变换后、色调映射前"的输出分辨率 HDR（res_scale=1 时逐位不变）

### T1.6 Vulkan 后端对齐 ⛓T1.4, T1.5
- [ ] T1.6.1 VK 侧管线/描述符/同步（§5.4.1 对照表逐项）
- [ ] T1.6.2 glslang 库接入：启动时 GLSL→SPIR-V（含 include 展开后源码）
- [ ] T1.6.3 面板后端切换：窗口句柄销毁重建 + Config 保留（§5.2）
- [ ] T1.6.4 🏁 目标机双后端截图对比（§12 风险缓解）

### T1.7 UI 完整化 ⛓T1.3
- [ ] T1.7.1 面板 7 组全参数（§8 表逐项）+ 自旋 a 灰显锁定
- [ ] T1.7.2 监控 overlay（左上角独立）：FPS/帧时间/分辨率/后端/GPU 计时/平均步数
- [ ] T1.7.3 自由飞行相机（§4.7）+ 双模式切换
- [ ] T1.7.4 Config 持久化（启动加载、退出/切后端保存）

### T1.8 粒子 + 音频 ⛓T1.1（与渲染解耦，可并行）
- [ ] T1.8.1 粒子 compute（初始化/leapfrog/回收，§5.6）+ 点精灵加色混合
- [ ] T1.8.2 UI 明示"牛顿近似"标注
- [ ] T1.8.3 miniaudio 接入 + 棕噪声低通合成（§5.7 参数映射）

### T1.9 冒烟与发布 ⛓T1.6, T1.7, T1.8
- [ ] T1.9.1 `--smoke` 模式（§6.3 CLI、退出码、PFM+PNG 输出）
- [ ] T1.9.2 目标机冒烟 NMSE 差分通过（阈值校准记录原因，§6.2）
- [ ] T1.9.3 🏁 **v1 发布**：git tag `v1.0-m1` + DESIGN.md 版本行补"已实现"标注

---

## M2 — Kerr 解锁 + 后置视觉项（DESIGN §10/M2，粗拆，开工前细化）

- [ ] 自旋 a 解锁 + 极端 Kerr 钳制验证（a→0.998 扫描稳定性）
- [ ] Page–Thorne 完整通量修正因子评估（§4.5）
- [ ] 喷流体积渲染（⚠ 先读 §13.1：径向分量必须过 Jacobian，高危静默错误）
- [ ] 星空 cubemap 背景 + 引力透镜（逃逸光线采样）
- [ ] L1 基准扩展：a>0 阴影偏心轮廓（文献比对）；静态 tetrad → ZAMO 评估

## M3 — 高级视觉（粗拆）
- [ ] TSR/TAAU（角坐标重投影方案，§10/M3）
- [ ] 偏振 EHT 视图（并行传输 + 线段 overlay）

---

## 执行纪律

1. 每条任务做完立即勾选 + 日期；验收不通过不得勾。
2. 实现中发现 DESIGN.md 需改 → 走 §13 变更流程升版本，TASKS.md 同步升版本。
3. 🏁 门禁不过，不进入下一任务组。
4. 开发机零 GPU：所有"跑起来看"的步骤标注在目标机执行，其余在开发机完成。
