#pragma once

#include "ehe/render/backend.h"

namespace ehe::render {

/// 渲染器创建参数（DESIGN §5.2 / §7）
struct RendererConfig {
    Backend backend = Backend::OpenGL;
    int width = 1280;
    int height = 720;
    const char* title = "Event Horizon Engine";
    bool vsync = true;
};

/// 后端无关的渲染器接口。
///
/// 生命周期约定（DESIGN §5.2）：
///   - 实例由 app 层工厂按 RendererConfig.backend 创建；
///   - 切换后端 = shutdown 旧实例 → **销毁并重建 GLFW 窗口句柄**
///     （CLIENT_API hint 在窗口创建时固定，GL 窗口不能原地变 VK 窗口）→ init 新实例；
///   - 进程不退，Config/窗口几何由 app 层保留并回填。
class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// 后端标识
    virtual Backend backend() const = 0;

    /// 创建窗口与图形上下文，完成 API 初始化；失败返回 false
    virtual bool init(const RendererConfig& cfg) = 0;

    /// 释放全部图形资源（含窗口）
    virtual void shutdown() = 0;

    /// 窗口尺寸变化；尺寸为 0（最小化）时应跳过渲染
    virtual void resize(int width, int height) = 0;

    /// 每帧开始：清屏 + ImGui 新帧
    virtual void begin_frame() = 0;

    /// 每帧结束：ImGui 绘制 + 提交 + 交换缓冲
    virtual void end_frame() = 0;

    /// 用户是否请求关闭窗口
    virtual bool should_close() const = 0;

    /// 当前帧缓冲尺寸（已考虑 HiDPI/最小化）
    virtual void framebuffer_size(int& width, int& height) const = 0;
};

}  // namespace ehe::render
