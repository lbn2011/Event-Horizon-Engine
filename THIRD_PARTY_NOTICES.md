# 第三方组件与数据来源声明

EHE 自身以 MIT 许可发布（见 `LICENSE`）。本文件记录**随仓库分发**的第三方内容及其许可，
以及构建期拉取（不随仓库分发）的依赖清单。

## 一、随仓库分发的内容

### 1. glad（GL 加载器，`third_party/glad/`）

- 生成产物入库（DESIGN §9「生成文件入库」），生成命令：
  `glad --profile core --api gl=4.5 --generator c`
- 上游：https://github.com/Dav1dde/glad
- 许可：**MIT**（生成产物沿用上游许可；`glad.h` 文件头保留原始许可声明）

### 2. CIE 1931 2° 标准观察者配色函数数据（`core/include/ehe/core/cie1931_2deg.h`）

- 用途：黑体色温 → sRGB 的颜色映射（DESIGN §4.5）
- 原始数据：CIE 1931 2° 标准观察者配色函数（CIE 公开数据集）
- 提取来源：**colour-science/colour** 项目的 `colour/colorimetry/datasets/cmfs.py`
  （https://github.com/colour-science/colour），许可 **BSD-3-Clause**
- 生成脚本：`tests/tools/gen_cie1931_header.py`（可复现；头文件内含来源与抽样校验说明）
- 抽样校验：380 nm (0.001368, 0.000039, 0.006450)、555 nm (0.512050, 1.000000, 0.0057)、
  600 nm (1.0622, 0.631, 0.0008) —— 与 CIE 公布表一致

```
BSD-3-Clause 许可要求（提取来源 colour-science/colour）：
Copyright (c) 2013-2024, Colour Developers
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## 二、构建期拉取（FetchContent，不随仓库分发）

版本 pin 见 `cmake/Deps.cmake`。这些依赖在构建时下载，其许可随各自仓库分发，故此处仅列清单：

| 依赖 | 用途 | 许可 |
|---|---|---|
| GLFW 3.5.1 | 窗口/输入 | zlib/libpng |
| GLM 1.0.3 | 数学 | MIT |
| Dear ImGui v1.92.9 | 面板 | MIT |
| Vulkan-Headers / volk / glslang（vulkan-sdk-1.4.357.0） | Vulkan 头/加载/GLSL→SPIR-V | Apache-2.0 / MIT / BSD-3-Clause（Khronos） |
| doctest v2.5.3 | 单元测试 | MIT |
| nlohmann/json v3.12.0 | Config 序列化 | MIT |
| miniaudio 0.11.25 | 音频 | MIT-0 / public domain |
| stb（提交 pin） | PNG 写出 | public domain / MIT |
| FidelityFX-FSR v1.0.2 | FSR1 升频 | MIT |

> 合规提示：若将来把上述任一依赖**入库分发**（vendored），须同步在本文件补充其许可全文。
