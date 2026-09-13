#pragma once

#include "ehe/render/backend.h"

// render/vk —— Vulkan 后端占位接口
// 脚手架：T0.4 起实现空窗口 + ImGui 面板初始化；T1.6 完成后端对齐与运行时切换。

namespace ehe::render::vk {

/// 后端自述（编译期可用性；真实初始化在 T0.4）
Backend backend();

}  // namespace ehe::render::vk
