#pragma once

#include <memory>

#include "ehe/render/IRenderer.h"

// render/gl —— OpenGL 4.5 后端
// T0.4：GLFW 窗口 + GL 4.5 core context + glad 加载 + ImGui 空面板
// T1.3：raymarch 管线主开发（全屏三角形 + 后处理链）
//
// 开发机限制：本机 GL ≤ 4.1 且无 Vulkan，本后端**只能在目标机运行验证**；
// 开发机以「编译通过 + 接口一致性」作为验收（DESIGN §2.1 / §6 L3）。

namespace ehe::render::gl {

/// 创建 GL 后端实例（失败时返回 nullptr，错误信息见 stderr）
std::unique_ptr<IRenderer> create_renderer();

}  // namespace ehe::render::gl
