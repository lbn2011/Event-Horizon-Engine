#pragma once

#include "ehe/render/backend.h"

// render/gl —— OpenGL 后端占位接口
// 脚手架：T0.4 起实现空窗口 + ImGui 面板初始化；T1.3 起承担 raymarch 管线主开发
// （开发机 GL ≤ 4.1，实际渲染验证在目标机执行）。

namespace ehe::render::gl {

/// 后端自述（编译期可用性；真实初始化在 T0.4）
Backend backend();

}  // namespace ehe::render::gl
