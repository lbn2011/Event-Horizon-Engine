#pragma once

// EHE render —— GLSL → SPIR-V 运行时编译（DESIGN §5.2 / §5.4.1）
//
// 为什么在运行时编译（而不是构建期离线编译）：
//   - shader 需要按**精度宏**（`EHE_FP32_ONLY`）出多份变体，且面板可运行时切换精度（§4.3/§7）；
//   - include 展开由 render/ShaderSource 在 C++ 侧完成（§5.8），与 GL 路径共用同一份源码；
//   - 依赖已在构建期 pin 好（glslang），运行时不需要外部工具链。
//
// 编译目标：**Vulkan 1.2 + SPIR-V 1.5**（§5.2：Vulkan 1.2 核心特性范围内）。
// 注意：`glslang::glslang` 主库已含 SPIRV 源码（本项目 `ENABLE_OPT=OFF`，无需 SPIRV-Tools）。

#include <cstdint>
#include <string>
#include <vector>

namespace ehe::render {

struct SpirvResult {
    bool ok = false;
    std::string log;                  ///< 编译日志（含警告）
    std::vector<std::uint32_t> words; ///< SPIR-V 字流（首字为魔数 0x07230203）
};

/// 把 GLSL 源码编译为 SPIR-V。
/// @param source        已是**展开过 include** 的源码（ShaderSource 的输出）
/// @param fragment_stage true = 片元阶段，false = 顶点阶段
/// @param defines       额外宏（在 `#version` 之后插入，如 {"EHE_FP32_ONLY"}）
SpirvResult compile_glsl_to_spirv(const std::string& source, bool fragment_stage,
                                  const std::vector<std::string>& defines = {});

/// 便捷入口：从文件加载（含 include 展开 + 宏注入）并编译为 SPIR-V。
/// @param path   入口 shader 路径
/// @param roots  include 搜索根（除入口文件所在目录外）
SpirvResult compile_shader_file(const std::string& path, const std::vector<std::string>& roots,
                                bool fragment_stage,
                                const std::vector<std::string>& defines = {});

}  // namespace ehe::render
