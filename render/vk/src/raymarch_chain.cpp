// EHE render/vk —— Vulkan 渲染链实现（T1.6.1）
//
// 与 GL 后端（render/gl/src/gl_backend.cpp 的 post 链）**逐项对齐**，差异只在 API 机械层面：
//   GL                                    Vulkan
//   FP16 FBO（GL_RGBA16F）                R16G16B16A16_SFLOAT 图像 + render pass + framebuffer
//   GL_TEXTURE_2D + GL_LINEAR 采样         VkImage + VkImageView + VkSampler（combined image sampler）
//   UBO binding 0/3/4                      descriptor set 的 binding 0/3/4（同一套编号，便于 shader 共用）
//   全屏三角形（gl_VertexID）              全屏三角形（gl_VertexIndex，由 EHE_VULKAN 宏抹平）
//   final_fbo + glBlitFramebuffer          离屏 LDR 图 + vkCmdBlitImage（record_present）
//
// 首版取舍：离屏图像统一 GENERAL 布局（免布局过渡，只需内存屏障）；每个 pass 的 UBO 按
// frames-in-flight 各一份，避免 CPU 写/GPU 读竞争。

#include "ehe/render/vk/raymarch_chain.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "ehe/core/blackbody.h"
#include "ehe/render/spirv.h"

namespace ehe::render::vk {
namespace {

/// 4 个 pass 的编号（与 sets_/pipelines_ 的索引一致）
enum PassIndex : int { kPassRaymarch = 0, kPassResolve = 1, kPassFxaa = 2, kPassFinal = 3 };
constexpr int kPassCount = 4;

const char* const kPassNames[kPassCount] = {"raymarch.frag", "post_resolve.frag", "post_fxaa.frag",
                                            "post_final.frag"};

/// half → float（读回 RGBA16F 时用；核心只做 1-5-10 位拆解，与本项目用途一致）
float half_to_float(std::uint16_t value) {
    const std::uint32_t sign = (value >> 15) & 0x1U;
    const std::uint32_t exponent = (value >> 10) & 0x1FU;
    const std::uint32_t mantissa = value & 0x3FFU;
    float result = 0.0F;
    if (exponent == 0) {
        result = std::ldexp(static_cast<float>(mantissa), -24);  // 非规格化
    } else if (exponent == 31) {
        result = (mantissa == 0) ? INFINITY : NAN;
    } else {
        result = std::ldexp(static_cast<float>(mantissa + 1024), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -result : result;
}

/// 把 VkResult 拼成可读后缀（目标机日志定位用：错误串直接携带结果码，
/// 否则"分配描述符集失败"这类文案无法区分 OUT_OF_POOL / DEVICE_LOST）
std::string vk_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "（SUCCESS）";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "（OUT_OF_HOST_MEMORY）";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "（OUT_OF_DEVICE_MEMORY）";
        case VK_ERROR_DEVICE_LOST: return "（DEVICE_LOST）";
        case VK_ERROR_MEMORY_MAP_FAILED: return "（MEMORY_MAP_FAILED）";
        case VK_ERROR_FRAGMENTED_POOL: return "（FRAGMENTED_POOL）";
        default: return "（VkResult=" + std::to_string(static_cast<int>(result)) + "）";
    }
}

}  // namespace

struct RaymarchChain::Impl {
    ChainInitInfo info{};

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    std::array<VkShaderModule, kPassCount> fragment_modules{};
    std::array<VkPipeline, kPassCount> pipelines{};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    /// [pass][frame_slot]
    std::array<std::array<VkDescriptorSet, kFramesInFlight>, kPassCount> sets{};

    VkRenderPass hdr_pass = VK_NULL_HANDLE;
    std::array<VkFramebuffer, 3> hdr_framebuffers{};  // 0 = internal, 1 = out0, 2 = out1

    // HDR 图像（GENERAL 布局，兼具附件与采样源）
    VkImage internal_image = VK_NULL_HANDLE;
    VkImageView internal_view = VK_NULL_HANDLE;
    VkDeviceMemory internal_memory = VK_NULL_HANDLE;
    std::array<VkImage, 2> out_images{};
    std::array<VkImageView, 2> out_views{};
    std::array<VkDeviceMemory, 2> out_memories{};

    // 离屏 LDR 目标（final 的画布，输出分辨率 = 请求分辨率；交换链格式）。
    // 与 GL 的 final_fbo_ 同构：渲染尺寸与窗口解耦，呈现经 blit 拉伸，读回从这里走。
    VkImage ldr_image = VK_NULL_HANDLE;
    VkImageView ldr_view = VK_NULL_HANDLE;
    VkDeviceMemory ldr_memory = VK_NULL_HANDLE;
    VkRenderPass ldr_pass = VK_NULL_HANDLE;
    VkFramebuffer ldr_framebuffer = VK_NULL_HANDLE;

    // 黑体 LUT + 采样器
    VkImage lut_image = VK_NULL_HANDLE;
    VkImageView lut_view = VK_NULL_HANDLE;
    VkDeviceMemory lut_memory = VK_NULL_HANDLE;
    VkSampler lut_sampler = VK_NULL_HANDLE;
    VkSampler post_sampler = VK_NULL_HANDLE;

    // UBO（每 pass 每帧一份，host-visible 持久映射）
    std::array<VkBuffer, kFramesInFlight> sim_buffers{};
    std::array<VkDeviceMemory, kFramesInFlight> sim_memories{};
    std::array<void*, kFramesInFlight> sim_mapped{};
    std::array<VkBuffer, kFramesInFlight> post_buffers{};
    std::array<VkDeviceMemory, kFramesInFlight> post_memories{};
    std::array<void*, kFramesInFlight> post_mapped{};
    std::array<VkBuffer, kFramesInFlight> final_buffers{};
    std::array<VkDeviceMemory, kFramesInFlight> final_memories{};
    std::array<void*, kFramesInFlight> final_mapped{};

    // 本帧 final pass 的输入（在 record_offscreen 末尾确定，供 record_final 使用）
    VkImageView last_final_source_ = VK_NULL_HANDLE;
    VkImage last_final_source_image_ = VK_NULL_HANDLE;

    // 读回
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* readback_mapped = nullptr;
    VkDeviceSize readback_size = 0;

    VkCommandPool upload_pool = VK_NULL_HANDLE;

    // ---------------------------------------------------------------- 工具

    std::uint32_t find_memory_type(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(info.physical_device, &properties);
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((allowed & (1U << i)) != 0U &&
                (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool create_image(std::uint32_t width, std::uint32_t height, VkFormat format,
                      VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) {
        VkImageCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = format;
        create.extent = {width, height, 1};
        create.mipLevels = 1;
        create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // 规范要求 initialLayout ∈ {UNDEFINED, PREINITIALIZED, ZERO_INITIALIZED}
        //（VUID-VkImageCreateInfo-initialLayout-00993，真机验证层抓出）；
        // 统一在 LUT 上传提交里做 UNDEFINED → GENERAL 过渡，之后的约定仍是 GENERAL 免过渡
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(info.device, &create, nullptr, &image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(info.device, image, &requirements);
        const std::uint32_t type = find_memory_type(requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(info.device, &allocate, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }
        // 绑定图像内存——分配 ≠ 绑定。缺这一步图像就没有后备存储，所有创建类调用都
        // "成功"，首次 GPU 访问即 page fault → DEVICE_LOST（真机 Iris Xe 2026-09-19：
        // LUT copy 是链内第一条访问图像的提交，init 与两次 rebuild 全部同样挂）。
        const VkResult bound = vkBindImageMemory(info.device, image, memory, 0);
        if (bound != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 绑定图像内存失败%s\n", vk_result_name(bound).c_str());
            return false;
        }
        return true;
    }

    bool create_view(VkImage image, VkFormat format, VkImageView& view) {
        VkImageViewCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        create.image = image;
        create.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create.format = format;
        create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(info.device, &create, nullptr, &view) == VK_SUCCESS;
    }

    bool create_host_buffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped) {
        VkBufferCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create.size = size;
        create.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(info.device, &create, nullptr, &buffer) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(info.device, buffer, &requirements);
        const std::uint32_t type =
            find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(info.device, &allocate, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }
        vkBindBufferMemory(info.device, buffer, memory, 0);
        return vkMapMemory(info.device, memory, 0, size, 0, &mapped) == VK_SUCCESS;
    }

    /// 一次性命令缓冲（LUT 上传等）。err 非空时带回带 VkResult 的失败原因。
    /// 为什么全路径检查：目标机（Iris Xe）实测出现过"首建成功、重建全挂"，若返回值被吞
    /// 就无法区分是提交参数问题还是 device lost 级联——每个返回值都必须可见。
    bool submit_one_shot(const std::function<void(VkCommandBuffer)>& record,
                         std::string* err = nullptr) {
        auto fail = [err](const char* what, VkResult r) {
            if (err != nullptr) {
                *err = std::string(what) + vk_result_name(r);
            }
            return false;
        };
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = upload_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkResult r = vkAllocateCommandBuffers(info.device, &allocate, &cmd);
        if (r != VK_SUCCESS) {
            return fail("分配命令缓冲失败", r);
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vkBeginCommandBuffer(cmd, &begin);
        if (r != VK_SUCCESS) {
            vkFreeCommandBuffers(info.device, upload_pool, 1, &cmd);
            return fail("begin 命令缓冲失败", r);
        }
        record(cmd);
        r = vkEndCommandBuffer(cmd);
        if (r != VK_SUCCESS) {
            vkFreeCommandBuffers(info.device, upload_pool, 1, &cmd);
            return fail("end 命令缓冲失败", r);
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        r = vkQueueSubmit(info.queue, 1, &submit, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) {
            r = vkQueueWaitIdle(info.queue);  // 等待结果不再吞掉（device lost 会在这里显形）
        }
        vkFreeCommandBuffers(info.device, upload_pool, 1, &cmd);
        if (r != VK_SUCCESS) {
            return fail("提交/等待队列失败", r);
        }
        return true;
    }

    /// GENERAL → GENERAL 的写后读屏障（离屏图像既作附件又作采样源）
    void barrier_attachment_to_shader(VkCommandBuffer cmd, VkImage image) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    }
};

// ---------------------------------------------------------------- 生命周期

bool RaymarchChain::init(const ChainInitInfo& info) {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->info = info;
    ready_ = false;
    last_error_.clear();

    const std::vector<std::string> roots = {info.shader_root, "shaders"};
    const VkExtent2D output = info.output_extent;
    const VkExtent2D internal_extent = (info.internal_extent.width > 0)
                                           ? info.internal_extent
                                           : info.output_extent;
    resolved_ = (internal_extent.width != output.width || internal_extent.height != output.height);

    // ---- 命令池（一次性上传/读回用）----
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = info.queue_family;
    if (vkCreateCommandPool(info.device, &pool_info, nullptr, &impl_->upload_pool) != VK_SUCCESS) {
        last_error_ = "创建命令池失败";
        return false;
    }

    // ---- shader 模块（VS 一份 + 4 个 FS）----
    const SpirvResult vertex = compile_shader_file(info.shader_root + "/fullscreen.vert", roots,
                                                   /*fragment_stage=*/false, info.shader_defines);
    if (!vertex.ok) {
        last_error_ = "fullscreen.vert 编译失败：" + vertex.log;
        return false;
    }
    auto make_module = [&](const std::vector<std::uint32_t>& words) {
        VkShaderModuleCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create.codeSize = words.size() * sizeof(std::uint32_t);
        create.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        return (vkCreateShaderModule(info.device, &create, nullptr, &module) == VK_SUCCESS) ? module
                                                                                          : VK_NULL_HANDLE;
    };
    impl_->vertex_module = make_module(vertex.words);
    for (int pass = 0; pass < kPassCount; ++pass) {
        const SpirvResult fragment = compile_shader_file(
            info.shader_root + "/" + kPassNames[pass], roots, true, info.shader_defines);
        if (!fragment.ok) {
            last_error_ = std::string(kPassNames[pass]) + " 编译失败：" + fragment.log;
            return false;
        }
        impl_->fragment_modules[static_cast<std::size_t>(pass)] = make_module(fragment.words);
    }
    std::printf("[vk] 5 个 shader 模块就绪（SPIR-V；精度宏：");
    for (const std::string& define : info.shader_defines) {
        std::printf("%s ", define.c_str());
    }
    std::printf("）\n");

    // ---- 离屏 HDR 图像（GENERAL 布局，兼具附件与采样源；用途含 TRANSFER_SRC 供读回）----
    const VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags hdr_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!impl_->create_image(internal_extent.width, internal_extent.height, hdr_format, hdr_usage,
                             impl_->internal_image, impl_->internal_memory) ||
        !impl_->create_view(impl_->internal_image, hdr_format, impl_->internal_view)) {
        last_error_ = "创建内部 HDR 图像失败";
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (!impl_->create_image(output.width, output.height, hdr_format, hdr_usage,
                                 impl_->out_images[static_cast<std::size_t>(i)],
                                 impl_->out_memories[static_cast<std::size_t>(i)]) ||
            !impl_->create_view(impl_->out_images[static_cast<std::size_t>(i)], hdr_format,
                                impl_->out_views[static_cast<std::size_t>(i)])) {
            last_error_ = "创建输出 HDR 图像失败";
            return false;
        }
    }

    // ---- HDR render pass（1 个颜色附件；loadOp=DONT_CARE，每帧全覆盖）----
    {
        VkAttachmentDescription color{};
        color.format = hdr_format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;

        // 允许后续 pass 采样本 pass 的输出（真正的同步由显式内存屏障完成）
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        create.attachmentCount = 1;
        create.pAttachments = &color;
        create.subpassCount = 1;
        create.pSubpasses = &subpass;
        create.dependencyCount = 1;
        create.pDependencies = &dependency;
        if (vkCreateRenderPass(info.device, &create, nullptr, &impl_->hdr_pass) != VK_SUCCESS) {
            last_error_ = "创建 HDR render pass 失败";
            return false;
        }

        const VkImageView views[3] = {impl_->internal_view, impl_->out_views[0], impl_->out_views[1]};
        for (int i = 0; i < 3; ++i) {
            VkFramebufferCreateInfo framebuffer{};
            framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer.renderPass = impl_->hdr_pass;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &views[i];
            framebuffer.width = (i == 0) ? internal_extent.width : output.width;
            framebuffer.height = (i == 0) ? internal_extent.height : output.height;
            framebuffer.layers = 1;
            if (vkCreateFramebuffer(info.device, &framebuffer, nullptr,
                                    &impl_->hdr_framebuffers[static_cast<std::size_t>(i)]) !=
                VK_SUCCESS) {
                last_error_ = "创建 HDR framebuffer 失败";
                return false;
            }
        }
    }

    // ---- 离屏 LDR 目标（输出分辨率 = 请求分辨率；格式 = 交换链格式，blit 免转换）----
    {
        const VkImageUsageFlags ldr_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!impl_->create_image(output.width, output.height, info.swapchain_format, ldr_usage,
                                 impl_->ldr_image, impl_->ldr_memory) ||
            !impl_->create_view(impl_->ldr_image, info.swapchain_format, impl_->ldr_view)) {
            last_error_ = "创建离屏 LDR 图像失败";
            return false;
        }

        // final 专用的 render pass：布局约定与 HDR 图一致（GENERAL 进出，免过渡）；
        // 每帧全覆盖，loadOp=DONT_CARE。与交换链 render pass 彻底解耦——交换链重建不再牵连链。
        VkAttachmentDescription color{};
        color.format = info.swapchain_format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo pass_create{};
        pass_create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass_create.attachmentCount = 1;
        pass_create.pAttachments = &color;
        pass_create.subpassCount = 1;
        pass_create.pSubpasses = &subpass;
        pass_create.dependencyCount = 1;
        pass_create.pDependencies = &dependency;
        if (vkCreateRenderPass(info.device, &pass_create, nullptr, &impl_->ldr_pass) != VK_SUCCESS) {
            last_error_ = "创建 LDR render pass 失败";
            return false;
        }

        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = impl_->ldr_pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &impl_->ldr_view;
        framebuffer.width = output.width;
        framebuffer.height = output.height;
        framebuffer.layers = 1;
        if (vkCreateFramebuffer(info.device, &framebuffer, nullptr, &impl_->ldr_framebuffer) !=
            VK_SUCCESS) {
            last_error_ = "创建 LDR framebuffer 失败";
            return false;
        }
    }

    // ---- 采样器 ----
    {
        // LUT：线性过滤（与 core 的插值一致）；若设备不支持 32 位浮点线性过滤则退化为最近邻并告警
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(info.physical_device, VK_FORMAT_R32G32B32A32_SFLOAT,
                                           &format_properties);
        const bool linear_ok =
            (format_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U;
        if (!linear_ok) {
            std::fprintf(stderr, "[vk] 警告：该设备不支持 R32G32B32A32_SFLOAT 线性过滤，LUT 退化为最近邻\n");
        }
        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.minFilter = linear_ok ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0.0F;
        if (vkCreateSampler(info.device, &sampler, nullptr, &impl_->lut_sampler) != VK_SUCCESS) {
            last_error_ = "创建 LUT 采样器失败";
            return false;
        }
        // 后处理输入：线性过滤（分辨率变换后仍需要采样）
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        if (vkCreateSampler(info.device, &sampler, nullptr, &impl_->post_sampler) != VK_SUCCESS) {
            last_error_ = "创建后处理采样器失败";
            return false;
        }
    }

    // ---- 黑体 LUT（256×1，从 core 的二进制读取后转 RGBA32F 上传）----
    {
        std::vector<float> lut_rgb;
        std::size_t count = 0;
        if (!core::read_blackbody_lut(info.lut_path, lut_rgb, count) || count == 0) {
            last_error_ = "读取黑体 LUT 失败：" + info.lut_path;
            return false;
        }
        std::vector<float> lut_rgba(count * 4, 1.0F);
        for (std::size_t i = 0; i < count; ++i) {
            lut_rgba[i * 4 + 0] = lut_rgb[i * 3 + 0];
            lut_rgba[i * 4 + 1] = lut_rgb[i * 3 + 1];
            lut_rgba[i * 4 + 2] = lut_rgb[i * 3 + 2];
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(lut_rgba.size() * sizeof(float));
        if (!impl_->create_image(static_cast<std::uint32_t>(count), 1,
                                 VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 impl_->lut_image, impl_->lut_memory) ||
            !impl_->create_view(impl_->lut_image, VK_FORMAT_R32G32B32A32_SFLOAT, impl_->lut_view)) {
            last_error_ = "创建 LUT 图像失败";
            return false;
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void* staging_mapped = nullptr;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(info.device, &buffer_info, nullptr, &staging) != VK_SUCCESS) {
            last_error_ = "创建 LUT staging 缓冲失败";
            return false;
        }
        // 失败路径统一清理（staging 缓冲/内存不泄漏）
        auto staging_cleanup = [&]() {
            vkDestroyBuffer(info.device, staging, nullptr);
            if (staging_memory != VK_NULL_HANDLE) {
                vkFreeMemory(info.device, staging_memory, nullptr);
            }
        };
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(info.device, staging, &requirements);
        const std::uint32_t type =
            impl_->find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) {
            last_error_ = "LUT staging：找不到 HOST_VISIBLE 内存类型";
            staging_cleanup();
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        VkResult r = vkAllocateMemory(info.device, &allocate, nullptr, &staging_memory);
        if (r != VK_SUCCESS) {
            last_error_ = "分配 LUT staging 内存失败" + vk_result_name(r);
            staging_cleanup();
            return false;
        }
        r = vkBindBufferMemory(info.device, staging, staging_memory, 0);
        if (r != VK_SUCCESS) {
            last_error_ = "绑定 LUT staging 内存失败" + vk_result_name(r);
            staging_cleanup();
            return false;
        }
        r = vkMapMemory(info.device, staging_memory, 0, bytes, 0, &staging_mapped);
        if (r != VK_SUCCESS) {
            last_error_ = "映射 LUT staging 内存失败" + vk_result_name(r);
            staging_cleanup();
            return false;
        }
        std::memcpy(staging_mapped, lut_rgba.data(), static_cast<std::size_t>(bytes));
        vkUnmapMemory(info.device, staging_memory);

        std::string upload_err;
        const bool uploaded = impl_->submit_one_shot(
            [&](VkCommandBuffer cmd) {
                // 首次使用前过渡：5 张图（HDR×3 + LDR + LUT）从 UNDEFINED → GENERAL。
                // 之后离屏链沿用"GENERAL 免过渡"约定，只需 pass 间写后读屏障。
                VkImage images[5] = {impl_->internal_image, impl_->out_images[0],
                                     impl_->out_images[1], impl_->ldr_image, impl_->lut_image};
                VkImageMemoryBarrier barriers[5] = {};
                for (int i = 0; i < 5; ++i) {
                    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barriers[i].image = images[i];
                    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                }
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                     nullptr, 5, barriers);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {static_cast<std::uint32_t>(count), 1, 1};
                vkCmdCopyBufferToImage(cmd, staging, impl_->lut_image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                       &region);
            },
            &upload_err);
        staging_cleanup();
        if (!uploaded) {
            last_error_ = "LUT 上传失败：" + upload_err;
            return false;
        }
        std::printf("[vk] 黑体 LUT 已上传：%zu 点（RGBA32F）\n", count);
    }

    // ---- UBO（每 pass 每帧一份）----
    for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        if (!impl_->create_host_buffer(sizeof(SimParams), impl_->sim_buffers[slot],
                                       impl_->sim_memories[slot], impl_->sim_mapped[slot]) ||
            !impl_->create_host_buffer(sizeof(PostParams), impl_->post_buffers[slot],
                                       impl_->post_memories[slot], impl_->post_mapped[slot])) {
            last_error_ = "创建 UBO 失败";
            return false;
        }
        // FinalParams 用 UNIFORM usage 的缓冲（复用 create_host_buffer 的 usage）
        if (!impl_->create_host_buffer(sizeof(FinalParams), impl_->final_buffers[slot],
                                       impl_->final_memories[slot], impl_->final_mapped[slot])) {
            last_error_ = "创建 final UBO 失败";
            return false;
        }
    }

    // ---- descriptor set layout（binding 与 GL 的 texture unit / UBO binding 一致）----
    {
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        // 0 = SimParams（raymarch）
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // 1 = 黑体 LUT（raymarch）
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // 2 = 后处理输入纹理（post_* 共用）
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // 3 = PostParams（resolve）
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // 4 = FinalParams（final）
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(info.device, &layout_info, nullptr, &impl_->set_layout) !=
            VK_SUCCESS) {
            last_error_ = "创建描述符布局失败";
            return false;
        }

        // 池容量按"集合数 × 每集合描述符数"算：每集合 3 个 UBO（binding 0/3/4）+ 2 个 CIS（1/2）。
        // 首版 UBO 只按 1 个/集合算 → 真机 vkAllocateDescriptorSets 返回 OUT_OF_POOL
        //（表现为"分配描述符集失败"，Iris Xe 实测 2026-09-19）
        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPassCount * kFramesInFlight * 3};
        sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPassCount * kFramesInFlight * 2};
        VkDescriptorPoolCreateInfo pool_create{};
        pool_create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_create.maxSets = kPassCount * kFramesInFlight;
        pool_create.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool_create.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(info.device, &pool_create, nullptr, &impl_->descriptor_pool) !=
            VK_SUCCESS) {
            last_error_ = "创建描述符池失败";
            return false;
        }

        std::array<VkDescriptorSetLayout, kPassCount * kFramesInFlight> layouts{};
        layouts.fill(impl_->set_layout);
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = impl_->descriptor_pool;
        allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, kPassCount * kFramesInFlight> flat{};
        if (vkAllocateDescriptorSets(info.device, &allocate, flat.data()) != VK_SUCCESS) {
            last_error_ = "分配描述符集失败";
            return false;
        }
        for (int pass = 0; pass < kPassCount; ++pass) {
            for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
                impl_->sets[static_cast<std::size_t>(pass)][slot] =
                    flat[static_cast<std::size_t>(pass) * kFramesInFlight + slot];
            }
        }
    }

    // ---- 管线布局 ----
    {
        VkPipelineLayoutCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        create.setLayoutCount = 1;
        create.pSetLayouts = &impl_->set_layout;
        if (vkCreatePipelineLayout(info.device, &create, nullptr, &impl_->pipeline_layout) !=
            VK_SUCCESS) {
            last_error_ = "创建管线布局失败";
            return false;
        }
    }

    // ---- 4 条 graphics pipeline ----
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = impl_->vertex_module;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend_attachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        for (int pass = 0; pass < kPassCount; ++pass) {
            stages[1].module = impl_->fragment_modules[static_cast<std::size_t>(pass)];

            VkGraphicsPipelineCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            create.stageCount = 2;
            create.pStages = stages;
            create.pVertexInputState = &vertex_input;
            create.pInputAssemblyState = &assembly;
            create.pViewportState = &viewport;
            create.pRasterizationState = &raster;
            create.pMultisampleState = &multisample;
            create.pColorBlendState = &blend;
            create.pDynamicState = &dynamic;
            create.layout = impl_->pipeline_layout;
            // final 画进离屏 LDR 目标；其余 3 个画进 HDR pass（呈现经 blit，见 record_present）
            create.renderPass = (pass == kPassFinal) ? impl_->ldr_pass : impl_->hdr_pass;
            if (vkCreateGraphicsPipelines(info.device, VK_NULL_HANDLE, 1, &create, nullptr,
                                           &impl_->pipelines[static_cast<std::size_t>(pass)]) !=
                VK_SUCCESS) {
                last_error_ = std::string("创建管线失败：") + kPassNames[pass];
                return false;
            }
        }
    }

    // ---- 读回缓冲（输出分辨率 × 4 通道 × 4 字节，够放半精度或 8 位数据）----
    {
        const VkDeviceSize needed =
            static_cast<VkDeviceSize>(output.width) * output.height * 4 * sizeof(float);
        VkBufferCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create.size = needed;
        create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(info.device, &create, nullptr, &impl_->readback_buffer) != VK_SUCCESS) {
            last_error_ = "创建读回缓冲失败";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(info.device, impl_->readback_buffer, &requirements);
        const std::uint32_t type =
            impl_->find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) {
            last_error_ = "读回缓冲：找不到 HOST_VISIBLE 内存类型";
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        VkResult r = vkAllocateMemory(info.device, &allocate, nullptr, &impl_->readback_memory);
        if (r != VK_SUCCESS) {
            last_error_ = "分配读回内存失败" + vk_result_name(r);
            return false;
        }
        r = vkBindBufferMemory(info.device, impl_->readback_buffer, impl_->readback_memory, 0);
        if (r != VK_SUCCESS) {
            last_error_ = "绑定读回内存失败" + vk_result_name(r);
            return false;
        }
        r = vkMapMemory(info.device, impl_->readback_memory, 0, needed, 0, &impl_->readback_mapped);
        if (r != VK_SUCCESS) {
            last_error_ = "映射读回内存失败" + vk_result_name(r);
            return false;
        }
        impl_->readback_size = needed;
    }

    // ---- 初始描述符绑定 ----
    // raymarch / resolve 的绑定固定；fxaa 与 final 的输入纹理随模式变化，在录制时更新。
    {
        const VkDescriptorBufferInfo sim_info{impl_->sim_buffers[0], 0, sizeof(SimParams)};
        const VkDescriptorBufferInfo post_info{impl_->post_buffers[0], 0, sizeof(PostParams)};
        const VkDescriptorBufferInfo final_info{impl_->final_buffers[0], 0, sizeof(FinalParams)};
        const VkDescriptorImageInfo lut_info{impl_->lut_sampler, impl_->lut_view,
                                             VK_IMAGE_LAYOUT_GENERAL};

        for (int pass = 0; pass < kPassCount; ++pass) {
            for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
                const VkDescriptorBufferInfo sim_slot{impl_->sim_buffers[slot], 0, sizeof(SimParams)};
                const VkDescriptorBufferInfo post_slot{impl_->post_buffers[slot], 0,
                                                       sizeof(PostParams)};
                const VkDescriptorBufferInfo final_slot{impl_->final_buffers[slot], 0,
                                                        sizeof(FinalParams)};
                std::array<VkWriteDescriptorSet, 5> writes{};
                for (int i = 0; i < 5; ++i) {
                    writes[static_cast<std::size_t>(i)].sType =
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[static_cast<std::size_t>(i)].dstSet =
                        impl_->sets[static_cast<std::size_t>(pass)][slot];
                    writes[static_cast<std::size_t>(i)].dstBinding = static_cast<std::uint32_t>(i);
                    writes[static_cast<std::size_t>(i)].descriptorCount = 1;
                }
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &sim_slot;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &lut_info;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[2].pImageInfo = &lut_info;  // 占位：post pass 的实际输入在录制时更新
                writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[3].pBufferInfo = &post_slot;
                writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[4].pBufferInfo = &final_slot;
                vkUpdateDescriptorSets(info.device, static_cast<std::uint32_t>(writes.size()),
                                       writes.data(), 0, nullptr);
            }
        }
        (void)sim_info;
        (void)post_info;
        (void)final_info;
    }

    ready_ = true;
    std::printf("[vk] 渲染链就绪：%ux%u（内部 %ux%u）%s\n", output.width, output.height,
                internal_extent.width, internal_extent.height,
                resolved_ ? "，启用分辨率变换" : "");
    return true;
}

void RaymarchChain::destroy() {
    if (impl_ == nullptr) {
        return;
    }
    const VkDevice device = impl_->info.device;
    if (device != VK_NULL_HANDLE) {
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (impl_->sim_mapped[slot] != nullptr) {
                vkUnmapMemory(device, impl_->sim_memories[slot]);
            }
            if (impl_->post_mapped[slot] != nullptr) {
                vkUnmapMemory(device, impl_->post_memories[slot]);
            }
            if (impl_->final_mapped[slot] != nullptr) {
                vkUnmapMemory(device, impl_->final_memories[slot]);
            }
            vkDestroyBuffer(device, impl_->sim_buffers[slot], nullptr);
            vkFreeMemory(device, impl_->sim_memories[slot], nullptr);
            vkDestroyBuffer(device, impl_->post_buffers[slot], nullptr);
            vkFreeMemory(device, impl_->post_memories[slot], nullptr);
            vkDestroyBuffer(device, impl_->final_buffers[slot], nullptr);
            vkFreeMemory(device, impl_->final_memories[slot], nullptr);
        }
        if (impl_->readback_mapped != nullptr) {
            vkUnmapMemory(device, impl_->readback_memory);
        }
        vkDestroyBuffer(device, impl_->readback_buffer, nullptr);
        vkFreeMemory(device, impl_->readback_memory, nullptr);
        vkDestroySampler(device, impl_->lut_sampler, nullptr);
        vkDestroySampler(device, impl_->post_sampler, nullptr);
        vkDestroyImageView(device, impl_->lut_view, nullptr);
        vkDestroyImage(device, impl_->lut_image, nullptr);
        vkFreeMemory(device, impl_->lut_memory, nullptr);
        for (int i = 0; i < 2; ++i) {
            vkDestroyImageView(device, impl_->out_views[static_cast<std::size_t>(i)], nullptr);
            vkDestroyImage(device, impl_->out_images[static_cast<std::size_t>(i)], nullptr);
            vkFreeMemory(device, impl_->out_memories[static_cast<std::size_t>(i)], nullptr);
        }
        vkDestroyImageView(device, impl_->internal_view, nullptr);
        vkDestroyImage(device, impl_->internal_image, nullptr);
        vkFreeMemory(device, impl_->internal_memory, nullptr);
        vkDestroyFramebuffer(device, impl_->ldr_framebuffer, nullptr);
        vkDestroyRenderPass(device, impl_->ldr_pass, nullptr);
        vkDestroyImageView(device, impl_->ldr_view, nullptr);
        vkDestroyImage(device, impl_->ldr_image, nullptr);
        vkFreeMemory(device, impl_->ldr_memory, nullptr);
        for (VkFramebuffer framebuffer : impl_->hdr_framebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        vkDestroyRenderPass(device, impl_->hdr_pass, nullptr);
        for (VkPipeline pipeline : impl_->pipelines) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        vkDestroyPipelineLayout(device, impl_->pipeline_layout, nullptr);
        vkDestroyDescriptorPool(device, impl_->descriptor_pool, nullptr);
        vkDestroyDescriptorSetLayout(device, impl_->set_layout, nullptr);
        for (VkShaderModule module : impl_->fragment_modules) {
            vkDestroyShaderModule(device, module, nullptr);
        }
        vkDestroyShaderModule(device, impl_->vertex_module, nullptr);
        vkDestroyCommandPool(device, impl_->upload_pool, nullptr);
    }
    delete impl_;
    impl_ = nullptr;
    ready_ = false;
}

// ---------------------------------------------------------------- 录制

void RaymarchChain::upload_uniforms(std::uint32_t frame_slot, const SimParams& sim,
                                    const FinalParams& final_params, float res_scale) {
    if (!ready_ || frame_slot >= kFramesInFlight) {
        return;
    }
    std::memcpy(impl_->sim_mapped[frame_slot], &sim, sizeof(SimParams));
    std::memcpy(impl_->final_mapped[frame_slot], &final_params, sizeof(FinalParams));
    // resolve 的参数：模式由 res_scale 决定（>1 SSAA 降采样 / <1 升频），尺寸取自本次创建的目标
    PostParams post{};
    post.mode_and_src[0] = (res_scale > 1.001F) ? 0 : 1;
    post.mode_and_src[1] = static_cast<std::int32_t>(impl_->info.internal_extent.width);
    post.mode_and_src[2] = static_cast<std::int32_t>(impl_->info.internal_extent.height);
    post.mode_and_src[3] = 1;
    post.dst_size[0] = static_cast<std::int32_t>(impl_->info.output_extent.width);
    post.dst_size[1] = static_cast<std::int32_t>(impl_->info.output_extent.height);
    std::memcpy(impl_->post_mapped[frame_slot], &post, sizeof(PostParams));
}

void RaymarchChain::record_offscreen(VkCommandBuffer cmd, std::uint32_t frame_slot, const SimParams&,
                                     float res_scale, bool fxaa) {
    if (!ready_) {
        return;
    }
    const bool want_resolve = (res_scale > 1.001F) || (res_scale < 0.999F);
    const VkExtent2D output = impl_->info.output_extent;
    const VkExtent2D internal_extent = impl_->info.internal_extent;

    // 让"上一帧对本图像的写入"对本次使用可见（跨帧的读写依赖由后端提交侧保证）
    auto begin_pass = [&](VkFramebuffer framebuffer, VkExtent2D extent) {
        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = impl_->hdr_pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{0, 0}, extent};
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width),
                            static_cast<float>(extent.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    };
    auto draw_fullscreen = [&](VkPipeline pipeline, VkDescriptorSet set) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline_layout, 0, 1,
                                &set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };

    // ---- pass 1：raymarch → internal ----
    begin_pass(impl_->hdr_framebuffers[0], internal_extent);
    draw_fullscreen(impl_->pipelines[kPassRaymarch],
                    impl_->sets[kPassRaymarch][frame_slot]);
    vkCmdEndRenderPass(cmd);
    impl_->barrier_attachment_to_shader(cmd, impl_->internal_image);

    // 后续 pass 的输入纹理
    VkImage source_image = impl_->internal_image;
    VkImageView source_view = impl_->internal_view;

    // ---- pass 2：分辨率变换（仅在 res_scale != 1 时；互斥二选一）----
    if (want_resolve) {
        // resolve 的输入固定为 internal；输出 out0
        VkDescriptorImageInfo input{impl_->post_sampler, impl_->internal_view,
                                    VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = impl_->sets[kPassResolve][frame_slot];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &input;
        vkUpdateDescriptorSets(impl_->info.device, 1, &write, 0, nullptr);

        begin_pass(impl_->hdr_framebuffers[1], output);
        draw_fullscreen(impl_->pipelines[kPassResolve], impl_->sets[kPassResolve][frame_slot]);
        vkCmdEndRenderPass(cmd);
        impl_->barrier_attachment_to_shader(cmd, impl_->out_images[0]);
        source_image = impl_->out_images[0];
        source_view = impl_->out_views[0];
    }

    // ---- pass 3：FXAA（可选）----
    if (fxaa) {
        // 目标必须与源不同：源是 out0（做过变换）时写 out1，否则写 out0
        const int target = (source_image == impl_->out_images[0]) ? 1 : 0;
        VkDescriptorImageInfo input{impl_->post_sampler, source_view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = impl_->sets[kPassFxaa][frame_slot];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &input;
        vkUpdateDescriptorSets(impl_->info.device, 1, &write, 0, nullptr);

        begin_pass(impl_->hdr_framebuffers[static_cast<std::size_t>(1 + target)], output);
        draw_fullscreen(impl_->pipelines[kPassFxaa], impl_->sets[kPassFxaa][frame_slot]);
        vkCmdEndRenderPass(cmd);
        impl_->barrier_attachment_to_shader(cmd, impl_->out_images[static_cast<std::size_t>(target)]);
        source_image = impl_->out_images[static_cast<std::size_t>(target)];
        source_view = impl_->out_views[static_cast<std::size_t>(target)];
    }

    // final pass 的输入在这里确定（final 画进离屏 LDR 目标，见 record_final）
    impl_->last_final_source_ = source_view;
    impl_->last_final_source_image_ = source_image;
}

void RaymarchChain::record_final(VkCommandBuffer cmd, std::uint32_t frame_slot, bool aces) {
    if (!ready_) {
        return;
    }
    (void)aces;  // ACES 开关经 FinalParams.flags 传入（upload_uniforms）
    VkDescriptorImageInfo input{impl_->post_sampler, impl_->last_final_source_,
                                VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = impl_->sets[kPassFinal][frame_slot];
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &input;
    vkUpdateDescriptorSets(impl_->info.device, 1, &write, 0, nullptr);

    // final 画进自己的离屏 LDR pass（ GENERAL → GENERAL，免布局过渡）
    const VkExtent2D output = impl_->info.output_extent;
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = impl_->ldr_pass;
    begin.framebuffer = impl_->ldr_framebuffer;
    begin.renderArea.offset = {0, 0};
    begin.renderArea.extent = output;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(output.width),
                        static_cast<float>(output.height), 0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, output};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipelines[kPassFinal]);
    const VkDescriptorSet set = impl_->sets[kPassFinal][frame_slot];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline_layout, 0, 1, &set,
                            0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // LDR 图的写入对后续 blit（TRANSFER 读）可见：写后读屏障（GENERAL → GENERAL）
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = impl_->ldr_image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void RaymarchChain::record_present(VkCommandBuffer cmd, VkImage dst_image, VkExtent2D dst_extent) {
    if (!ready_ || dst_image == VK_NULL_HANDLE) {
        return;
    }
    const VkExtent2D output = impl_->info.output_extent;

    // 交换链图像 acquire 后布局未定义：先过渡到 TRANSFER_DST_OPTIMAL。
    // srcStage 取 COLOR_ATTACHMENT_OUTPUT，与呈现信号量的等待阶段（pWaitDstStageMask）对齐，
    // 保证图像真正可用之后才执行过渡。
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = dst_image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    // 线性拉伸 blit：请求分辨率（LDR）→ 窗口尺寸（交换链）。与 GL 的 glBlitFramebuffer 同构。
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(output.width),
                          static_cast<std::int32_t>(output.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(dst_extent.width),
                          static_cast<std::int32_t>(dst_extent.height), 1};
    vkCmdBlitImage(cmd, impl_->ldr_image, VK_IMAGE_LAYOUT_GENERAL, dst_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // 交换链图像过渡到 COLOR_ATTACHMENT_OPTIMAL，供随后的交换链 render pass（ImGui）使用；
    // pass 的 finalLayout=PRESENT_SRC_KHR 会接手最后一次过渡。
    VkImageMemoryBarrier to_color{};
    to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = dst_image;
    to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_color.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_color);
}

// ---------------------------------------------------------------- 读回

bool RaymarchChain::read_hdr(std::vector<float>& out, int& width, int& height) {
    if (!ready_) {
        return false;
    }
    // 输出分辨率的 HDR：做过变换时取 out0，否则内部图像（此时 internal == output）
    const VkImage source = resolved_ ? impl_->out_images[0] : impl_->internal_image;
    width = static_cast<int>(impl_->info.output_extent.width);
    height = static_cast<int>(impl_->info.output_extent.height);

    const bool copied = impl_->submit_one_shot([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_GENERAL, impl_->readback_buffer, 1,
                               &region);
    });
    if (!copied) {
        return false;
    }

    // 图像格式 RGBA16F → 转 float RGB（Vulkan 图像原点在左上，与 core 的"第 0 行 = 顶部"一致，无需翻转）
    out.assign(static_cast<std::size_t>(3) * width * height, 0.0F);
    const auto* source_pixels = static_cast<const std::uint16_t*>(impl_->readback_mapped);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        out[i * 3 + 0] = half_to_float(source_pixels[i * 4 + 0]);
        out[i * 3 + 1] = half_to_float(source_pixels[i * 4 + 1]);
        out[i * 3 + 2] = half_to_float(source_pixels[i * 4 + 2]);
    }
    return true;
}

bool RaymarchChain::read_ldr(std::vector<unsigned char>& out, int& width, int& height) {
    if (!ready_) {
        return false;
    }
    width = static_cast<int>(impl_->info.output_extent.width);
    height = static_cast<int>(impl_->info.output_extent.height);

    // 从离屏 LDR 图读回（GENERAL 布局，带 TRANSFER_SRC 用途）——
    // 不再从交换链回读：交换链图像无 TRANSFER_SRC 用途、PRESENT_SRC 布局不可作拷贝源
    //（真机验证层两处报错的根源，2026-09-19）
    const bool copied = impl_->submit_one_shot([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        vkCmdCopyImageToBuffer(cmd, impl_->ldr_image, VK_IMAGE_LAYOUT_GENERAL,
                               impl_->readback_buffer, 1, &region);
    });
    if (!copied) {
        return false;
    }

    // 离屏 LDR 图格式 = 交换链格式（多数设备 B8G8R8A8，少数 R8G8B8A8）→ 按实际通道序转 RGB
    const VkFormat format = impl_->info.swapchain_format;
    const bool bgra = (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB);
    out.assign(static_cast<std::size_t>(3) * width * height, 0);
    const auto* source_pixels = static_cast<const unsigned char*>(impl_->readback_mapped);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        if (bgra) {
            out[i * 3 + 0] = source_pixels[i * 4 + 2];  // R ← B 通道
            out[i * 3 + 1] = source_pixels[i * 4 + 1];
            out[i * 3 + 2] = source_pixels[i * 4 + 0];  // B ← R 通道
        } else {
            out[i * 3 + 0] = source_pixels[i * 4 + 0];
            out[i * 3 + 1] = source_pixels[i * 4 + 1];
            out[i * 3 + 2] = source_pixels[i * 4 + 2];
        }
    }
    return true;
}

}  // namespace ehe::render::vk
