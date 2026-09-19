#pragma once

// EHE render/vk —— Vulkan 渲染链（T1.6.1，DESIGN §5.4.1 对照表）
//
// 与 GL 后端的链序**逐项对齐**（§4.6）：
//   raymarch（内部分辨率 FP16）→ 分辨率变换（SSAA 面积加权降采样 / Catmull-Rom 升频）
//   → FXAA → final（曝光 → ACES → 色差 → sRGB，画进交换链帧缓冲，之后 ImGui 叠加）
//
// 设计取舍（首版，正确性优先）：
//   1. **离屏图像统一用 VK_IMAGE_LAYOUT_GENERAL**：该布局对"既作附件又作采样源"都合法，
//      且**不需要布局过渡**，只需在 pass 之间插内存屏障。专用布局更省带宽，但过渡时机容易出错，
//      首版刻意避开；后续按实测再优化（DESIGN §7.1 的优化优先级不依赖此改动）。
//   2. **final pass 直接画进交换链**（复用后端已有的 render pass），因此不需要额外的 blit pass；
//      ImGui 在其后于同一 render pass 内绘制（与 T0.4 的行为一致）。
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
    VkRenderPass swapchain_render_pass = VK_NULL_HANDLE;  ///< 交换链 render pass（final pass 的目标）
    VkExtent2D output_extent{};                           ///< 输出分辨率（= 交换链分辨率）
    VkExtent2D internal_extent{};                         ///< 内部分辨率（= output × res_scale）
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

    /// 在"已处于交换链 render pass 内"时录制 final pass（ACES/曝光/色差 → sRGB）
    void record_final_in_pass(VkCommandBuffer cmd, std::uint32_t frame_slot, bool aces);

    /// 读回**输出分辨率的 HDR**（分辨率变换之后、色调映射之前）——smoke 的 PFM 基准
    bool read_hdr(std::vector<float>& out, int& width, int& height);

    /// 读回 **LDR 成品**（final pass 之后）。需要交换链图像作为拷源（调用方传入）。
    bool read_ldr(VkImage swapchain_image, std::vector<unsigned char>& out, int& width, int& height);

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
