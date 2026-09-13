// EHE 主程序入口（脚手架）
//
// 现状（M0/T0.2）：仅验证「core + render + 双后端静态库 → 单一可执行文件」的链接链路，
// 打印版本与构建开关后正常退出。
//
// 后续（T0.3 / T0.4）：
//   - T0.3 FetchContent 接入 GLFW / ImGui / glad / volk 等依赖
//   - T0.4 创建 GLFW 窗口 + ImGui 空面板，GL 与 Vulkan 两后端各初始化一次，
//          并在面板中实现运行时后端切换（销毁/重建窗口句柄，进程不退）
//
// 开发机限制：本机无 Vulkan、GL ≤ 4.1，渲染验证一律在目标机执行（DESIGN §2.1）。

#include <iostream>

#include "ehe/core/version.h"
#include "ehe/render/backend.h"
#include "ehe/render/gl/gl_backend.h"
#include "ehe/render/vk/vk_backend.h"

int main() {
    std::cout << "Event Horizon Engine " << ehe::core::version_string()
              << " [" << ehe::core::build_flags() << "]\n";
    std::cout << "backends compiled: " << ehe::render::compiled_backend_count()
              << " (" << ehe::render::backend_name(ehe::render::vk::backend()) << ", "
              << ehe::render::backend_name(ehe::render::gl::backend()) << ")\n";
    std::cout << "scaffold only: window/UI not implemented yet (see TASKS.md T0.4)\n";
    return 0;
}
