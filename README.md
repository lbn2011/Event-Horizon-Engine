# Event Horizon Engine (EHE)

实时黑洞渲染模拟器：在 GPU 上对 Kerr 时空做光线测地线积分（raymarching），渲染体积吸积盘、
多普勒聚束、引力红移与标志性黑洞阴影。

## 技术栈

| 项 | 选型 |
|---|---|
| 语言 | C++20（全 C++，无 FFI） |
| 构建 | CMake + Ninja |
| 编译器 | LLVM-MinGW（Clang + lld），备选回退 WinLibs GCC |
| 图形后端 | Vulkan 1.2（volk 动态加载，免 SDK）+ OpenGL 4.5（glad） |
| 着色器 | GLSL 450，双后端共享；Vulkan 侧经 glslang 运行时编 SPIR-V |
| 依赖 | 全部 FetchContent 源码构建 |

## 文档（唯一权威来源）

- **[DESIGN.md](DESIGN.md)** —— 开发文档：15 项决策、方程级物理规格、模块与接口、验证体系、里程碑。
  凡与本文档冲突的需求，以本文档为准。
- **[TASKS.md](TASKS.md)** —— 任务拆解：M0–M3 可勾选执行清单，含验收标准与依赖。
- **[CONTRIBUTING.md](CONTRIBUTING.md)** —— 分支策略与协作约定。

## 当前状态

**M0 进行中**（工具链与仓库骨架）。

- [x] 设计文档 V5.1 / 任务拆解 / 许可 / 协作模板 / GitHub 仓库与分支策略
- [ ] LLVM-MinGW + Ninja 工具链部署（T0.1）
- [ ] 目录骨架与 CMake 工程（T0.2 余项）
- [ ] 依赖拉通、空窗口双后端（T0.3 / T0.4）

开发机无 Vulkan 支持，**GPU 渲染需在目标现代独显机器上验证**；物理正确性由 CPU 参考实现
（L1）与 golden image 差分在开发机把关。

## 许可

MIT，见 [LICENSE](LICENSE)。
