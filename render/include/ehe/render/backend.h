#pragma once

// EHE render —— 后端无关层信息
//
// 后端枚举的**唯一来源是 core::Backend**（因为它是 Config 的字段类型，而 core 不能依赖 render）；
// 这里只做别名，避免两个枚举定义需要来回转换（DESIGN §8.1 渲染组）。

#include "ehe/core/config.h"

namespace ehe::render {

/// 后端枚举（同 core::Backend，见 DESIGN §8 渲染组「后端(Vulkan/OpenGL)」）
using Backend = core::Backend;

/// 后端名称（用于日志与监控 overlay）
const char* backend_name(Backend backend);

/// 已编译进本程序的后端数量（切换前提：两个后端都静态链接进同一可执行文件）
int compiled_backend_count();

}  // namespace ehe::render
