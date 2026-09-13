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
- [x] LLVM-MinGW 20260908 + Ninja 1.13.2 工具链部署（T0.1，产物静态链接运行时）
- [x] 目录骨架与 CMake 工程（T0.2）
- [x] 依赖拉通：11 个 FetchContent 仓库全部编译通过（T0.3）
- [x] 空窗口双后端：GL 4.5 与 Vulkan 1.2 实现 + 运行时切换（T0.4；GL 路径已在开发机验证，Vulkan 待目标机）

**环境实测更正**：开发机显卡（AMD Radeon HD 7400M）**支持 OpenGL 4.5 核心**，
因此 GL 路径可在本机直接运行验证；Vulkan 因系统无 `vulkan-1.dll` 仍需目标机（详见 DESIGN §2.1）。

运行方式：

```bash
build/bin/ehe.exe                    # 默认 GL 后端
build/bin/ehe.exe --backend=vk       # Vulkan 后端
build/bin/ehe.exe --try-backends     # 探测各后端可用性（自检）
build/bin/ehe.exe --frames=120       # 渲染 120 帧后退出（自动化验证）
```

构建与运行：

```bash
# 配置（网络受限环境见 CONTRIBUTING「网络受限环境」的环境变量注入方式）
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-toolchain.cmake \
  -DCMAKE_MAKE_PROGRAM=C:/tools/ninja/ninja.exe \
  -DFETCHCONTENT_BASE_DIR=<依赖缓存目录，可复用>

cmake --build build        # 产物在 build/bin 与 build/lib
build/bin/ehe.exe          # 主程序
build/bin/ehe_tests.exe    # doctest 单元测试
```

工具链解压根目录默认 `C:/tools/llvm-mingw`，其他机器用 `-DEHE_LLVM_MINGW_ROOT=...` 覆盖；
构建类型默认 `RelWithDebInfo`，可用 `-DCMAKE_BUILD_TYPE=...` 指定。

开发机无 Vulkan 支持，**GPU 渲染需在目标现代独显机器上验证**；物理正确性由 CPU 参考实现
（L1）与 golden image 差分在开发机把关。

## 许可

MIT，见 [LICENSE](LICENSE)。
