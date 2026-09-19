#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ehe/render/backend.h"
#include "ehe/render/sim_params.h"

namespace ehe::render {

/// 渲染器创建参数（DESIGN §5.2 / §7）
struct RendererConfig {
    Backend backend = Backend::OpenGL;
    int width = 1280;
    int height = 720;
    const char* title = "Event Horizon Engine";
    bool vsync = true;
    bool visible = true;   ///< smoke/离屏模式置 false（不显示窗口）

    /// 显式渲染尺寸（0 = 跟随窗口帧缓冲 × res_scale）。
    /// smoke/离屏模式必须显式指定：Windows 对可见窗口有最小尺寸限制，32×32 之类的窗口
    /// 会被系统放大（实测被拉到 120×32），若渲染尺寸跟随窗口就会与 golden 尺寸不一致。
    int render_width = 0;
    int render_height = 0;

    /// shader 搜索根目录（含 common/ 的 shaders 目录）。
    /// **必须在 init 前设置**：GL 后端在 init 里就构建管线，init 之后设置已来不及
    /// （踩坑：bundle 解压到任意目录后 CWD 没有 shaders/，--caps 因此报"无法打开 shader 文件"）。
    std::string shader_root;

    /// 编译 shader 时注入的宏（在 `#version` 之后插入 `#define`）。
    /// 主要用途：`EHE_FP32_ONLY` —— 受限 GPU（无 fp64 硬件）上启用 fp32 精度路径（§4.3/§7）。
    std::vector<std::string> shader_defines;
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

    // ---------------------------------------------------------------- raymarch 管线（T1.3 起）

    /// 每帧上传的参数（SimParams UBO，§5.3）
    virtual void set_params(const SimParams& params) = 0;

    /// shader 搜索根目录（含 common/ 的 shaders 目录）；需在 init 前设置或由后端自动探测
    virtual void set_shader_root(const std::string& root) = 0;

    /// shader / 管线是否就绪（shader 编译失败时为 false，窗口仍可用）
    virtual bool pipeline_ready() const = 0;

    /// 最近一次 shader 编译/链接错误（无错误时为空）
    virtual const std::string& last_error() const = 0;

    /// 读取 HDR 结果（内部分辨率，线性 RGB，**行序自下而上** —— 与 PFM 的存储约定一致）。
    /// 用途：smoke 模式的 PFM 输出与差分（DESIGN §6.3）。
    /// @param rgb   输出缓冲（3 × width × height）
    /// @param width 内部分辨率宽
    /// @param height 内部分辨率高
    virtual bool capture_hdr(std::vector<float>& rgb, int& width, int& height) = 0;

    /// 用新的 shader 宏重建管线（**不重建上下文**）。
    /// 用途：--caps 的精度模式对比、T1.7 面板的运行时精度切换。
    /// 注意：同一进程内反复创建 GL 上下文会失败（实测 glad 第二次加载返回 0），故必须走此路径。
    virtual bool rebuild_pipeline(const std::vector<std::string>& shader_defines) = 0;

    /// 后端能力清单（人类可读，供 --caps 采集与目标机报告）。
    /// 内容：API 版本/设备名/关键扩展或设备特性/上限值；不含任何与本次参数相关的状态。
    virtual std::string capability_report() const = 0;

    /// 等待 GPU 完成（GL: glFinish；VK: queue wait idle）。
    /// 用途：**仅冒烟/能力探测**需要真实帧耗时（DESIGN §5.4.1：正常路径靠 vsync，不主动同步）。
    virtual void finish() = 0;

    /// 读取**后处理链之后**的 8 位输出（默认帧缓冲，不含 ImGui 合成）。
    /// 用途：验证后处理（ACES/曝光/色差/编码）的 GPU 结果与 `core/tonemap.cpp` 的 CPU 参考一致——
    /// 这是后处理链唯一的数值验证途径（它作用在 golden 之前，不进 NMSE 差分，§6.1）。
    /// @param rgb 输出缓冲（3 × width × height，8 位，**第 0 行 = 图像顶部**）
    virtual bool capture_ldr(std::vector<unsigned char>& rgb, int& width, int& height) = 0;
};

}  // namespace ehe::render
