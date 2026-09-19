#pragma once

// EHE render/vk —— Vulkan 渲染链（T1.6.1，DESIGN §5.4.1 对照表）
//
// 与 GL 后端的链序**逐项对齐**（§4.6）：
//   raymarch（内部分辨率 FP16）→ 分辨率变换（SSAA 面积加权降采样 / Catmull-Rom 升频）
//   → FXAA → final（曝光 → ACES → 色差 → sRGB，画进**离屏 LDR 目标**，之后 ImGui 叠加）
//
// 设计取舍（首版，正确性优先）：
//   1. **离屏图像统一用 VK_IMAGE_LAYOUT_GENERAL**：该布局对"既作附件又作采样源"都合法，
//      且**不需要布局过渡**，只需在 pass 之间插内存屏障。专用布局更省带宽，但过渡时机容易出错，
//      首版刻意避开；后续按实测再优化（DESIGN §7.1 的优化优先级不依赖此改动）。
//   2. **final pass 画进离屏 LDR 图**（请求分辨率，格式 = 交换链格式），呈现由调用方经
//      record_present() 用 vkCmdBlitImage 线性拉伸到交换链——与 GL 的 final_fbo +
//      glBlitFramebuffer 完全同构。渲染尺寸与窗口尺寸解耦：离屏冒烟时窗口会被系统拉大
//      （实测 32×32 → 180×32），若直接画进交换链，成品与读回都会是窗口尺寸而非请求分辨率。
//      交换链图像只作呈现（无需 TRANSFER_SRC），读回一律从离屏 LDR 图走。
//   3. 每个 pass 的 UBO 按 frames-in-flight 各一份，避免 CPU 写入与 GPU 读取竞争。
//
// 共享的 GLSL 源码经 render/spirv.cpp 编译为 SPIR-V（注入 `EHE_VULKAN`；fp64 不可用时加 `EHE_FP32_ONLY`）。

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "ehe/render/sim_params.h"

namespace ehe::render::vk {

/// 渲染链初始化参数
struct ChainInitInfo {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;           ///< 图形队列（LUT 上传与读回的同步用）
    std::uint32_t queue_family = 0;           ///< 队列族索引（命令池用）
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;  ///< 交换链颜色格式（离屏 LDR 图同格式，
                                                      ///< blit 免转换、读回走同一通道序）
    VkExtent2D output_extent{};               ///< 输出分辨率（**请求分辨率**，与窗口尺寸解耦，
                                              ///< 与 GL 的 render_width/render_height 同语义）
    VkExtent2D internal_extent{};             ///< 内部分辨率（= output × res_scale）
    std::string shader_root;
    std::vector<std::string> shader_defines;  ///< 已由调用方规整（含 EHE_VULKAN / 可能的 EHE_FP32_ONLY）
    std::string lut_path;                     ///< 黑体 LUT（256×1 float32，二进制）
};

/// Vulkan 渲染链：拥有 4 条管线、描述符、离屏目标与读回缓冲
class RaymarchChain {
public:
    RaymarchChain() = default;
    ~RaymarchChain() = default;
    RaymarchChain(const RaymarchChain&) = delete;
    RaymarchChain& operator=(const RaymarchChain&) = delete;

    /// 创建全部资源；失败返回 false 且 `last_error()` 给出原因
    bool init(const ChainInitInfo& info);

    /// 资源是否可用（管线/描述符/目标齐备）
    bool ready() const { return ready_; }

    const std::string& last_error() const { return last_error_; }

    /// 是否做了分辨率变换（res_scale != 1）——影响 FXAA 的输入与读回目标
    bool resolved() const { return resolved_; }

    /// 录制 3 个**离屏** pass（raymarch → resolve → fxaa）。
    /// @param frame_slot 0..frames_in_flight-1（选择对应那份 UBO）
    /// @param res_scale  当前内部分辨率档（决定是否做分辨率变换）
    /// @param fxaa       是否启用 FXAA
    void record_offscreen(VkCommandBuffer cmd, std::uint32_t frame_slot, const SimParams& sim,
                          float res_scale, bool fxaa);

    /// 上传本帧 UBO（在录制之前调用；内部按 frame_slot 选择缓冲）
    void upload_uniforms(std::uint32_t frame_slot, const SimParams& sim,
                         const FinalParams& final_params, float res_scale);

    /// 录制 final pass（ACES/曝光/色差 → sRGB）到**离屏 LDR 目标**（输出分辨率）。
    /// 在 record_offscreen 之后、record_present 之前调用。
    void record_final(VkCommandBuffer cmd, std::uint32_t frame_slot, bool aces);

    /// 把离屏 LDR 成品线性拉伸 blit 到交换链图像，并把交换链图像过渡到
    /// COLOR_ATTACHMENT_OPTIMAL（调用方随后在交换链 render pass 内画 ImGui）。
    /// @param dst_image  本帧 acquire 到的交换链图像
    /// @param dst_extent 交换链尺寸（窗口尺寸，可与输出分辨率不同）
    void record_present(VkCommandBuffer cmd, VkImage dst_image, VkExtent2D dst_extent);

    /// 读回**输出分辨率的 HDR**（分辨率变换之后、色调映射之前）——smoke 的 PFM 基准
    bool read_hdr(std::vector<float>& out, int& width, int& height);

    /// 读回 **LDR 成品**（final pass 之后，离屏 LDR 图，输出分辨率）。
    /// 不再从交换链回读——交换链图像无 TRANSFER_SRC 用途，且呈现布局不可作拷贝源。
    bool read_ldr(std::vector<unsigned char>& out, int& width, int& height);

    /// 释放全部资源
    void destroy();

    /// 本链占用的帧槽数（与后端 frames-in-flight 一致）
    static constexpr std::uint32_t kFramesInFlight = 2;

private:
    // 内部实现细节放在 .cpp（避免头文件暴露大量 Vulkan 结构）
    struct Impl;
    Impl* impl_ = nullptr;

    bool ready_ = false;
    bool resolved_ = false;
    std::string last_error_;
};

}  // namespace ehe::render::vk
