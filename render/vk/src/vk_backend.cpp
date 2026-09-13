#include "ehe/render/vk/vk_backend.h"

namespace ehe::render::vk {

// 脚手架实现：仅返回后端标识，用于验证「两后端同时链接进同一 exe」的骨架链路。
// 真实内容（volk 初始化、instance/device/swapchain、glslang 运行时编译）见 T0.4 / T1.6。
Backend backend() { return Backend::Vulkan; }

}  // namespace ehe::render::vk
