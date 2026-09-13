#pragma once

// EHE render —— 后端无关层信息
// 脚手架占位：T0.4 起引入 IRenderer 抽象（生命周期/resize/render/参数注入），
// 由 render/vk 与 render/gl 各自实现，运行时按 Config.backend 切换（DESIGN §5.2）。

namespace ehe::render {

/// 后端枚举（与 DESIGN §8 渲染组「后端(Vulkan/OpenGL)」对应）
enum class Backend { Vulkan, OpenGL };

/// 后端名称（用于日志与监控 overlay）
const char* backend_name(Backend backend);

/// 已编译进本程序的后端数量（切换前提：两个后端都静态链接进同一可执行文件）
int compiled_backend_count();

}  // namespace ehe::render
