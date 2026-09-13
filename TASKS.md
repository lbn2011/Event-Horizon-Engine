# EHE 任务拆解 V1.1

> 执行清单，与 `DESIGN.md`（规格权威）配套使用。冲突时以 DESIGN.md 为准。
> 每条任务：可勾选状态、验收标准、依赖、产出物。完成后勾 `[x]` 并注明日期。
> 版本：V1.0（2026-09-13，基于 DESIGN.md V5.1 拆出）；V1.1（2026-09-13，补 T0.2 仓库/协作设施与远端任务，勾选已完成项）。

图例：🏁 = 里程碑验收门；⛓ = 有前置依赖；产出物用 `代码格式` 标注。

---

## M0 — 工具链与骨架（DESIGN §10/M0，验收：空窗口双后端可编译）

### T0.1 工具链部署（DESIGN §9.1）
- [ ] T0.1.1 下载 `llvm-mingw-<ver>-ucrt-x86_64.zip`（mstorsjo/llvm-mingw Releases），解压 `C:\tools\llvm-mingw`
  - 验收：`C:\tools\llvm-mingw\bin\clang++ --version` 输出含 `x86_64-w64-mingw32`
- [ ] T0.1.2 下载 `ninja-win.zip`，解压 `C:\tools\ninja`
  - 验收：`C:\tools\ninja\ninja.exe --version` 有输出
- [ ] T0.1.3 编写 `cmake/llvm-mingw-toolchain.cmake`（绝对路径引用编译器，不污染 PATH；版本号写入注释）
  - 产出：`cmake/llvm-mingw-toolchain.cmake`
- [ ] T0.1.4 空 CMake 项目用 `-G Ninja -DCMAKE_TOOLCHAIN_FILE=...` 配置+编译通过
  - 验收：hello world 可执行文件运行成功

### T0.2 仓库骨架 ⛓T0.1
- [x] T0.2.1 `git init` + `LICENSE`（MIT）+ `.gitignore`(build/、.cache/、ehe.config.json、*.pfm) —— 2026-09-13
- [ ] T0.2.2 按 DESIGN §5.1 建目录骨架（core/render/render/vk/render/gl/app/shaders/tests/cmake）
- [ ] T0.2.3 顶层 `CMakeLists.txt`：C++20、警告集、选项（EHE_FP64_METRIC 默认 ON）、子目录挂接
  - 产出：可配置的空工程
- [x] T0.2.4 协作设施：issue 模板（bug_report/task）+ PR 模板 + CONTRIBUTING.md 分支策略（dev→PR→main，squash 合并）—— 2026-09-13
- [x] T0.2.5 本地 `main` 初始提交（设计文档 V5.1 + 任务拆解 + 许可）—— 2026-09-13
- [x] T0.2.6 建 `dev` 分支 —— 2026-09-13（远端已建并同步至 main）
- [x] T0.2.7 GitHub 远程仓库：`lbn2011/Event-Horizon-Engine`（Public）
  - 旧仓库（2026-03 旧 C++/Rust 架构，8 commits）已按用户决定删除；完整克隆备份在
    `C:\Users\lbn\Desktop\code\Event-Horizon-Engine-legacy-backup`，快照 zip 同目录
  - 远端内容经 GitHub Git Data API 建立（本机 git push 被代理阻断：sandbox 出口代理对 github.com
    返回 502，直连超时）。本地已对齐：main = dev = `6a1937a`
  - 首个 PR：#1（dev → main，squash 合并）—— 2026-09-13 ✅
  - ⚠ 遗留：网络恢复后验证 `git push origin dev` 可直连；备用 remote `mirror` 指向 gh-proxy 镜像
    （读通道验证可用，写通道未验证）

### T0.3 依赖拉通 ⛓T0.2
- [ ] T0.3.1 `cmake/Deps.cmake`：11 个仓库 FetchContent 声明 + 版本 pin（DESIGN §9）
- [ ] T0.3.2 逐个验证编译：GLFW → GLM → ImGui(含3后端文件) → glad → Vulkan-Headers+volk →
  glslang → miniaudio → doctest → nlohmann/json → FSR1 头文件 → stb
  - 验收：全部 target 编译通过；遇 clang-mingw 摩擦记录现象（DESIGN §12 风险行）
- [ ] T0.3.3 FSR1 与 stb 头文件入库路径确定（非 FetchContent 的手动 vendored 项写进 Deps.cmake 注释）

### T0.4 空窗口双后端 ⛓T0.3
- [ ] T0.4.1 GLFW 窗口 + GL 4.5 context + ImGui 空面板（GL 路径）
- [ ] T0.4.2 volk 初始化 + Vulkan 1.2 instance/device + swapchain + ImGui 空面板（VK 路径）
- [ ] T0.4.3 `IRenderer` 空实现 ×2 + 工厂 + 窗口句柄销毁重建切换（DESIGN §5.2）
- [ ] T0.4.4 🏁 **M0 验收**：目标机上 GL/VK 各起一次空窗口、面板后端切换不崩（人工确认）

---

## M1 — v1（DESIGN §10/M1，🏁 验收 = §6 黄金标准 + §8 面板全参数生效）

### T1.1 core 基础 ⛓T0.4
- [ ] T1.1.1 `Config`：§8.1 schema 全字段 + nlohmann/json 序列化 + 容错规则（未知键告警/缺失默认）
  - 测试：`config_roundtrip`
- [ ] T1.1.2 `units/math`：几何化单位常量、vec4/mat4（GLM 包装，对齐 GLSL 语义）
- [ ] T1.1.3 `Camera` 轨道模式：§4.7 数学 + tetrad 输出（先静态 tetrad 的 a=0 路径）
  - 测试：`camera_tetrad_orthonormal`
- [ ] T1.1.4 doctest 骨架接入 CI 本地脚本
  - 产出：`ehe_core` 静态库 + `ehe_tests`

### T1.2 L1 CPU 参考实现 ⛓T1.1（本阶段最重要，DESIGN §6/L1）
- [ ] T1.2.1 Kerr-Schild r 求根 + 度规分量（§4.1，fp64）
  - 测试：`ks_radius_a0`、`ks_metric_a0_limit`
- [ ] T1.2.2 度规导数链式三层（§4.1.1）+ Christoffel 组装
  - 测试：`metric_derivative_numeric`（中心差分回归 < 1e-6）
- [ ] T1.2.3 RK4 积分器 + 自适应步长 + 终止条件（§4.3）
  - 测试：`rk4_convergence`（4 阶）、`null_norm_init`
- [ ] T1.2.4 解析基准验证：光子球、b_crit 捕获扫描、阴影半径（§4.4）
  - 测试：`photon_capture_bc`（3√3 ± 0.5%）
- [ ] T1.2.5 黑体 LUT 生成器（§4.5 生成规格）→ `blackbody_lut.f32` + PNG 预览条
  - 测试：`lut_monotonic_peak`、`lut_files_identical`
- [ ] T1.2.6 golden 渲染器（§6.4）：盘模型 + g 因子 + 体合成（§4.5）→ PFM + PNG
  - 测试：`shadow_radius_image`、`g_factor_monotonic`
- [ ] T1.2.7 🏁 **L1 门**：§4.4 五项基准全过 + golden PFM 入库 `tests/golden/`

### T1.3 GLSL 内核移植（GL 先行）⛓T1.2
- [ ] T1.3.1 `shaders/common/` 四文件（simparams/metric/disk/noise），对照 T1.2 逐行移植
- [ ] T1.3.2 ShaderSource 文本 include 展开器（§5.8 约定）
- [ ] T1.3.3 fullscreen triangle + raymarch.frag 主循环 + UBO 上传（§5.3 布局）
- [ ] T1.3.4 调试视图 5 种（§5.5）——先用 `classify` 和 `steps` 自查积分器行为
- [ ] T1.3.5 🏁 GL 后端阴影轮廓肉眼对齐 golden PNG（开发机不可跑 → 目标机执行）

### T1.4 盘体渲染 + 相对论效应 ⛓T1.3
- [ ] T1.4.1 通量剖面 + 高斯厚度 + 采样裁剪（§4.5）
- [ ] T1.4.2 黑体 LUT 纹理绑定（binding 1）+ log(T) UV 映射
- [ ] T1.4.3 g 因子（每采样点现场计算）→ 聚束 g³ + T_eff 色温偏移
- [ ] T1.4.4 噪声 fbm + 差速旋转动画（UBO time 驱动）

### T1.5 后处理链 ⛓T1.3
- [ ] T1.5.1 FP16 HDR 缓冲 + 内部分辨率档（0.5x–2.0x）
- [ ] T1.5.2 FSR1（EASU+RCAS）/ SSAA box 降采样互斥逻辑
- [ ] T1.5.3 FXAA → ACES(Hill 拟合)+曝光 → 色差（§4.6 链序）

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
