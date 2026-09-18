# 目标机验证指南（T1.3.7 / M0 门禁）

> 适用：**Intel Iris Xe 核显**（i5-1135G7，Tiger Lake，Gen12）一类无独立显卡的机器。
> 规格依据：DESIGN §6（四层验证）、§6.2（NMSE）、§6.3（smoke CLI）、§4.3/§7（精度模式）。

## 0. 先看结论：这台机器能验什么、不能验什么

| 项目 | Iris Xe 上的预期 | 依据 |
|---|---|---|
| OpenGL 版本 | **4.6** ✓ 可用 | Intel 官方 API 支持矩阵 |
| Vulkan | **1.3** ✓ 可用（需装最新显卡驱动） | 同上 |
| **fp64（双精度）** | ❌ **无原生硬件** | Intel 工程师原话：「Ice Lake / Tiger Lake 不原生支持双精度，默认不暴露该扩展」；第三方基准亦确认 "Emulated FP64 through FP32" |
| GL 侧 double | 能编译（软件模拟），**很慢** | 本机对照实验（AMD 老卡）：fp64 约为 fp32 的 2.5 倍单步成本，但绝对值让 512² 不可行 |
| Vulkan 侧 fp64 | **多半不可用**（`shaderFloat64 = false`） | 同上；`--caps` 会直接给出该特性的真假 |
| 512² 抹烟（fp32） | **可行**，单帧预计数十秒量级 | 推算自本机 HD 7400M 的实测（64²/60 步 ≈ 2.0 s） |

**因此：目标机上请用 `"precision": "fp32"`**（无 fp64 硬件时的唯一实用精度）。
`mixed(fp64)` 只用于「能不能编译」的判定，不做性能验收。

## 1. 拿到可执行文件（免编译）

exe 是**静态链接**的（只依赖 Windows 系统 DLL），且 `--caps`/`--smoke` 都是离屏运行，**不需要装编译器或运行时**：

1. 从 CI 的 artifact 下载 **`ehe-target-bundle.zip`**（每次推送都会生成，见 `.github/workflows/ci.yml` 的打包步骤）；
2. 解压到任意目录（例：`C:\ehe-target\`）——包内应有：
   ```
   ehe.exe                      主程序（含 GL / VK 两后端）
   ehe_reftool.exe              参照工具（烘焙 LUT / 渲染 golden / PFM 差分）
   shaders/                     GLSL 源码 + blackbody_lut.f32
   tests/golden/                golden 参数与基线（params*.json / golden*.pfm）
   run_target_tests.ps1         一键测试脚本
   TARGET_MACHINE_TESTS.md      本文件
   ```
3. **先装/更新 Intel 显卡驱动**（Vulkan 需要系统存在 `vulkan-1.dll`；最新驱动才能拿到 GL 4.6 / VK 1.3）。

## 2. 一键跑完（推荐）

在包根目录打开 PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File .\run_target_tests.ps1
```

它会依次做：

1. 采集环境信息（OS / CPU / GPU / 驱动版本）
2. `--caps`：能力清单 + 两种精度的可编译性与帧耗时
3. 小尺寸抹烟（32²、64²，fp32）→ 与仓库基线做 NMSE 差分
4. **512² 抹烟（fp32）** → 与 `golden.pfm` 差分（DESIGN §6.1 的固化参数）
5. `--try-backends`：两后端探测

**只需回传一个文件**：`target_report\target_report.txt`（内含每一步的完整输出、耗时与退出码），外加 `target_report\*.pfm/*.png` 便于核对。

常用开关：

```powershell
# 先快速拿数据，不跑 512²
.\run_target_tests.ps1 -SkipLarge

# 512² 留足时间；并额外尝试 fp64（可能极慢/无响应，默认关闭）
.\run_target_tests.ps1 -TimeoutSec 1800 -TryMixed

# 额外跑一次窗口模式（会短暂弹出窗口，验证起窗与面板）
.\run_target_tests.ps1 -Windowed
```

## 3. 手工分步（想看清楚每一步时）

```powershell
# 3.1 能力探测：先看 GL/VK 版本、设备名、rgba16f 附件、fp32/mixed 能否编译
.\ehe.exe --caps --config=tests\golden\params_tiny.json --shader-root=shaders

# 3.2 小尺寸抹烟（fp32）——把参数文件里的 precision 改成 fp32
#     （脚本会自动生成 fp32 版参数；手工跑时可直接编辑 params_tiny.json）
.\ehe.exe --smoke --frames=1 --shot=out_tiny --config=tests\golden\params_tiny.json `
          --golden=tests\golden\golden_tiny.pfm --shader-root=shaders

# 3.3 512² 抹烟（fp32，DESIGN §6.1 固化参数）
.\ehe.exe --smoke --frames=1 --shot=out_512 --config=params_fp32_512.json `
          --golden=tests\golden\golden.pfm --shader-root=shaders

# 3.4 窗口模式（肉眼对齐 golden.png、切换调试视图）
.\ehe.exe --width=1280 --height=720 --config=tests\golden\params_tiny.json --shader-root=shaders
```

退出码：`0` 通过 / `1` 超阈值（NMSE > 1e-3）/ `2` 运行错误。窗口模式里可用面板：
调试视图 `shaded / steps / classify / g_factor / null_drift`、相机拖拽与滚轮、后端下拉切换。

## 4. 结果怎么判读

| 观察 | 含义 |
|---|---|
| `--caps` 里 `rgba16f_color_attachment=COMPLETE` | HDR 目标格式可用（我们所有渲染都依赖它） |
| `--caps` 里 `fp32 管线就绪` + 一个毫秒数 | fp32 可用；该毫秒数**已含 GPU 同步**，是真实单帧成本 |
| `--caps` 里 Vulkan `shaderFloat64=0` | 该 GPU 无原生 fp64 → Vulkan 侧必须走 fp32（T1.6 起） |
| 抹烟输出 `NMSE ≤ 1e-3` | shader 实现与 fp64 CPU 参考一致（本机 fp32 实测 **1.6e-4**） |
| `NMSE > 1e-3` | 需要进一步定位：对比 `classify` 视图的阴影轮廓、检查是否尺寸/行序不一致 |
| 某步骤 `TIMEOUT` | 该尺寸在该机算不动（可 `-SkipLarge`，或调大 `-TimeoutSec`） |

## 5. 回传内容清单

1. `target_report.txt`（必须）
2. `target_report\shot_512_fp32.pfm` / `.png`（若有）
3. 若出现超时或崩溃：该步骤对应的时间点、是否弹过"显示驱动已停止响应"（TDR）提示

> 为什么要回传文件而不是口头描述：DESIGN §6 L4 明确 GPU 渲染不进 CI，
> 目标机的结论只能靠**采集到的数据**来核对；PFM 是 float 全精度基准，比截图更可靠（§6.4）。

## 6. 已知问题 / 待确认

- ~~开发机 ≥128² 时 `capture_hdr` 崩溃~~ —— **已定性为老驱动特例**：目标机（Intel Iris Xe）**512² 回读正常**
  （2026-09-19 实测：512² 抹烟完成、NMSE=5.654e-04、PFM 正常写出）。开发机的 AMD HD 7400M 为 2015 年驱动，
  可视为该卡的孤例，不再作为阻塞项。
- Vulkan 的 raymarch 管线属 **T1.6** 范围，当前 `--caps` 只能给出设备能力，不能渲染。
- **后端运行时切换**（面板下拉，§5.2）尚未在目标机验证：需 `-Windowed` 起窗后在面板里切换一次
  （会销毁并重建窗口句柄），确认不崩、画面正常——这是 issue #27 的剩余项。
- **参考实测（Intel Iris Xe，2026-09-19）**：64²/N_max=60/fp32 = 28.3 ms/帧；512²/N_max=1000/fp32 = 2516.6 ms/帧；
  512² 抹烟 NMSE=5.654e-04（阈值 1e-3）。`--caps` 与 `--smoke` 均一次跑通。
- 若 `--caps` 显示 `mixed` 与 `fp32` 都可编译但耗时接近，说明该机 fp64 未真正走硬件路径，
  仍应使用 fp32（除 §6.1 基线对比外）。
