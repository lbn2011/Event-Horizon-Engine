#pragma once

#include <memory>

#include "ehe/render/IRenderer.h"

// app —— 后端工厂
//
// 位置说明（DESIGN §5.2）：工厂放在 app 层，因为它需要同时看到 render_gl 与 render_vk 两个实现，
// 而 render/ 本体是后端无关的（不能反向依赖具体后端）。

namespace ehe::app {

/// 按后端枚举创建渲染器实例（不做初始化；init 由调用方触发）
std::unique_ptr<render::IRenderer> create_renderer(render::Backend backend);

/// 后端命令行参数解析（"gl"/"vk"/"opengl"/"vulkan"）
bool parse_backend(const char* text, render::Backend& out);

}  // namespace ehe::app
