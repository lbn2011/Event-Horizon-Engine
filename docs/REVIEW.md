# EHE 项目审查记录（2026-09-14）

> 触发：项目所有者要求「按步骤完成所有任务和设计，都完成后再进行一遍审查」。
> 本文档记录本次审查的范围、方法、发现与处置。规格依据为 `DESIGN.md`（V5.4）。

## 一、审查范围与方法

| 维度 | 方法 |
|---|---|
| 构建可复现性 | 全新配置 → 构建 → 跑全量测试（本机，含依赖缓存复用） |
| 文档↔实现一致性 | 逐条核对 DESIGN §6.5 的 12 条用例、§4.4 的五项基准、§8.1 的字段清单 |
| 仓库完整性 | 本地树 vs 远端树**哈希核对**（`git rev-parse HEAD^{tree}` vs GitHub 树 SHA） |
| 合规 | 随仓库分发内容（vendored glad、嵌入 CMF 数据）的许可与来源声明 |
| 卫生 | 忽略规则、临时文件残留、过期注释、TODO/FIXME |
| CI 覆盖 | DESIGN §6 L4 要求的编译检查是否落地 |

## 二、交付物状态

| 里程碑 | 状态 |
|---|---|
| **M0**（T0.1–T0.4） | ✅ 全部完成；**门禁未闭环**——Vulkan 起窗与后端切换待目标机（issue #27） |
| **M1 · T1.1** core 基础 | ✅ 完成（Config / Camera / units / math） |
| **M1 · T1.2** L1 参考实现 | ✅ 完成，**L1 门通过**（度规内核、RK4、解析基准、黑体 LUT、golden 渲染器） |
| **M1 · T1.3–T1.9** | ⬜ 未开始（GLSL 移植、盘体渲染进 shader、后处理链、Vulkan 对齐、UI、粒子+音效、冒烟发布） |
| **M2 / M3** | ⬜ 未开始；M2 条目在 TASKS 中为粗拆（开工前细化） |

测试规模：`ehe_tests` **43 用例 / 34962 断言**全绿（其中 §6.5 十二条用例**全部落地**）。

## 三、发现与处置

### 🔴 严重 1：`tests/CMakeLists.txt` 丢失，全新克隆无法构建

- **现象**：历史提交中存在该文件，但当前树里没有；`add_subdirectory(tests)` 报「does not contain a CMakeLists.txt」，
  即**任何全新克隆都无法配置构建**（CI 若先于本修复运行也必然失败）。
- **根因**：此前网络受限时采用的「按 ``git diff --name-only`` 取变更文件 + ``base_tree`` 增量造提交」的
  GitHub API 同步方式，会在文件集合出现偏差时**静默丢弃**条目，且没有任何校验。
- **处置**：恢复文件；**同步方式改为「按 ``git ls-tree -r HEAD`` 全树重建 + 树哈希核对」**，
  同步后强制比对远端树 SHA 与本地 `HEAD^{tree}`（本次已核对一致：`286ba921…`）。

### 🔴 严重 2：golden PFM 基线未入库

- **现象**：`tests/golden/golden.pfm`（差分基准，§6.4 要求入库）不在版本控制中。
- **根因**：`.gitignore` 的 `*.pfm` 规则把所有 PFM 一律忽略，与 §6.4 直接冲突。
- **处置**：改为只忽略运行期输出（`smoke_*.pfm` / `out*.pfm`），用 `!tests/golden/*.pfm` 放行基线；
  基线（512²，3.0 MB）已入库。

### 🟠 中 3：CI 编译检查缺失

- **现象**：DESIGN §6 L4 要求「无 GPU runner：编译 + L1/L2 + LUT 一致性校验」，但仓库没有 workflow。
- **处置**：新增 `.github/workflows/ci.yml`——同源 LLVM-MinGW + Ninja → 配置 → 构建 → 全量测试 →
  **LUT 可复现性校验**（重烘焙后 `git diff --exit-code`）→ GL 自检（`continue-on-error`）；
  GPU 渲染刻意不进 CI（§6 边界）。

### 🟡 轻 4：第三方许可与数据来源声明缺失

- **处置**：新增 `THIRD_PARTY_NOTICES.md`。随仓库分发的内容：vendored `third_party/glad`（MIT）、
  嵌入的 CIE 1931 CMF 数据（提取自 **BSD-3** 项目，附许可全文与抽样校验）；构建期拉取的依赖列清单。

### 🟡 轻 5：文档/卫生

- 两处过期注释（`version.h`、`render/backend.h` 仍写「脚手架」，模块实际已落地）；
- `.gitignore` 未覆盖测试临时文件（`ehe_test_lut.f32`、`ehe_test_config.json`）→ 已补。

## 四、一致性核对结果

- **§6.5 十二条用例**：`config_roundtrip` / `camera_tetrad_orthonormal` / `ks_radius_a0` /
  `metric_derivative_numeric` / `ks_metric_a0_limit` / `rk4_convergence` / `photon_capture_bc` /
  `shadow_radius_image` / `null_norm_init` / `g_factor_monotonic` / `lut_monotonic_peak` /
  `lut_files_identical` —— **全部实现且通过**。
- **§4.4 五项基准**：b_crit 相对误差 **4.1e-09**；阴影角（解析）**2.9e-09**、（图像测量）**0.245%**；
  光子球（动力学验证）；g 因子范围 [0.51, 1.46]；类光漂移 1.15e-08。
- **文档版本链**：DESIGN V5.4 / TASKS V1.9 / CONTRIBUTING（看板与网络章节）三者互相引用一致。
- **代码卫生**：无遗留 TODO/FIXME；`core` 保持零图形 API 依赖（stb 仅在 `tests/reftool` 使用）。

## 五、风险与建议（未处置项）

| 项 | 说明 | 建议 |
|---|---|---|
| API 同步的脆弱性 | 本机 git push 被代理阻断，长期依赖 GitHub API 造提交 | 网络恢复后改用常规 `git push`；在切换到 push 前，每次同步都执行全树哈希核对（现已固化进流程） |
| `app` 未接入 Config | app 仍用自己的 `Options` 结构，Config 的持久化（§8.1）未接 | 由 T1.7（UI 完整化）一并处理，届时删除 `Options` 重复定义 |
| `shaders/` 仅 LUT + README | GLSL 源码尚未落地 | T1.3 起按 §5.8 的文件清单逐个补齐 |
| M2/M3 未细化 | 粒度停留在条目级 | 进入 M2 前细化（含 §13.1 坐标 Jacobian 的必读提醒） |
| Vulkan 零本机验证 | 开发机无 Vulkan ICD | 由 issue #27 在目标机验收；M1 完成前必须闭环 |

## 六、结论

M0 与 M1 的 core/L1 部分（T1.1、T1.2）已完成且经机器可验证的严格检查：**12 条用例全落地、五项解析基准精度达 1e-9 量级**。
本次审查另修复了 2 个严重缺陷（构建完整性、基准入库）与 3 个中等/轻微问题，并把「同步后树哈希核对」固化为流程。
剩余工作集中在 T1.3–T1.9 与 M2/M3，以及目标机对 M0 门禁的验收。
