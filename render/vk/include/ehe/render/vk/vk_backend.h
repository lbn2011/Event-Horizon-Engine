#pragma once

#include <memory>

#include "ehe/render/IRenderer.h"

// render/vk —— Vulkan 1.2 后端
// T0.4：volk 初始化 + instance/device + swapchain + ImGui 空面板
// T1.6：与 GL 后端对齐（HDR attachment、FSR1/SSAA、双后端画面一致性）
//
// 关键约束（DESIGN §5.4.1）：
//   - 免 SDK：Vulkan-Headers 提供头文件，volk 运行时加载 vulkan-1.dll，不链接 vulkan-1.lib
//   - shader 由 glslang 在启动时编译为 SPIR-V（T1.6 接入；本阶段无 shader）
//   - 开发机无 Vulkan，本后端只能在目标机运行验证

namespace ehe::render::vk {

/// 创建 Vulkan 后端实例（失败时返回 nullptr，错误信息见 stderr）
std::unique_ptr<IRenderer> create_renderer();

}  // namespace ehe::render::vk
