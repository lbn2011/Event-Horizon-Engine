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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "ehe/core/blackbody.h"
#include "ehe/core/particles.h"
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

    // ---------------------------------------------------------------- 粒子（T1.8.1，§5.6）
    // SSBO 按 frames-in-flight 双缓冲（与 UBO 同一惯例）：frame N 的 compute 写 slot N 的
    // buffer、point draw 读同一份（同提交内 barrier 保证）；frame N-1 读 slot 1 —— 两个
    // frame slot 的 SSBO 互不相交，杜绝跨提交读写竞争。
    VkShaderModule compute_module = VK_NULL_HANDLE;
    VkShaderModule point_vertex_module = VK_NULL_HANDLE;
    VkShaderModule point_fragment_module = VK_NULL_HANDLE;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    VkPipeline point_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout particle_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout particle_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool particle_descriptor_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> particle_sets{};
    std::array<VkBuffer, kFramesInFlight> particle_buffers{};
    std::array<VkDeviceMemory, kFramesInFlight> particle_memories{};
    std::uint32_t particle_capacity = 0;  ///< 已上传的粒子数（0 = 未上传；变化即重建）
    bool particle_ready = false;          ///< 粒子链是否可用（shader/管线创建失败不拖垮主链）

    // 粒子专用 render pass：attachment 描述与 hdr_pass 一致（格式/samples 相同 → 与
    // hdr_framebuffers[0] 兼容，可复用），仅 loadOp=LOAD —— 保留 raymarch 输出做加色叠加。
    VkRenderPass point_pass = VK_NULL_HANDLE;

    // ---------------------------------------------------------------- GPU 统计（T1.7.2）
    // GPU 计时：timestamp query，每 frame slot 一对（帧首/帧尾），避免跨 slot 复用未完成结果
    VkQueryPool timer_pool = VK_NULL_HANDLE;
    double timer_period_ms = -1.0;  ///< timestampPeriod 换算；<0 = 设备不支持计时

    // 平均步数：alpha 通道逐级 blit 归约到 1×1（与 GL 的 mipmap 归约同语义），末端拷到
    // host-visible 缓冲延迟读（拷贝是异步的，读到的可能是上一轮的值——overlay 用途足够）
    VkImage reduction_image = VK_NULL_HANDLE;
    VkDeviceMemory reduction_memory = VK_NULL_HANDLE;
    std::uint32_t reduction_mips = 0;  ///< = floor(log2(max(w,h))) + 1（与 GL 公式一致）
    bool reduction_ready = false;      ///< 设备不支持 16F blit 时禁用（显示 n/a）
    VkBuffer steps_buffer = VK_NULL_HANDLE;
    VkDeviceMemory steps_memory = VK_NULL_HANDLE;
    void* steps_mapped = nullptr;
    int steps_frame_counter = 0;
    mutable float avg_steps_cache = -1.0F;  ///< gpu_frame_ms/last_avg_steps 由 const 方法更新
    mutable double gpu_ms_cache = -1.0;

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
                      VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory,
                      std::uint32_t mip_levels = 1) {
        VkImageCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = format;
        create.extent = {width, height, 1};
        create.mipLevels = mip_levels;
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

    /// 任意用途的缓冲创建（device local 或 host visible 均可；粒子 SSBO 与 steps 读回用）。
    /// 与 create_host_buffer 不同：不绑定固定 usage、不映射，由调用方按需处理。
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags wanted,
                       VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create.size = size;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(info.device, &create, nullptr, &buffer) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(info.device, buffer, &requirements);
        const std::uint32_t type = find_memory_type(requirements.memoryTypeBits, wanted);
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
        // 绑定内存同图像同理：缺绑定 = 无后备存储，首次 GPU 访问即 page fault
        const VkResult bound = vkBindBufferMemory(info.device, buffer, memory, 0);
        return bound == VK_SUCCESS;
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

    /// 粒子数变化时重建双缓冲 SSBO 并上传初始分布（core/particles.cpp 生成，§5.6 规则）。
    /// 与 GL 的 ensure_particles 同语义：count 变化即重建；固定种子 → 初始分布可复现。
    /// 成功后补写各 frame slot 描述符的 binding 2（SSBO），返回 false 时 particle_ready 置 false。
    bool upload_particles(std::uint32_t count, float r_in, float r_out) {
        const std::uint32_t clamped =
            count < 1000000U ? count : 1000000U;  // 与 GL 的 clamp(0, 1'000'000) 对齐
        if (clamped == particle_capacity) {
            return true;
        }
        const std::vector<float> initial = core::generate_particle_buffer(
            static_cast<std::size_t>(clamped), 1.0, r_in, r_out,
            0.5, 20260920ULL);  // σ=0.5M【§5.6 建议默认】；种子与 GL 一致
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(initial.size() * sizeof(float));

        // 销毁旧 buffer（容量变化 → 尺寸变化 → 一律重建两份）
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (particle_buffers[slot] != VK_NULL_HANDLE) {
                vkDestroyBuffer(info.device, particle_buffers[slot], nullptr);
                particle_buffers[slot] = VK_NULL_HANDLE;
            }
            if (particle_memories[slot] != VK_NULL_HANDLE) {
                vkFreeMemory(info.device, particle_memories[slot], nullptr);
                particle_memories[slot] = VK_NULL_HANDLE;
            }
        }
        if (bytes == 0) {
            particle_capacity = clamped;  // count=0：无需资源，描述符留空（不 bind 即可）
            return true;
        }

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void* staging_mapped = nullptr;
        if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging, staging_memory)) {
            return false;
        }
        if (vkMapMemory(info.device, staging_memory, 0, bytes, 0, &staging_mapped) != VK_SUCCESS) {
            vkDestroyBuffer(info.device, staging, nullptr);
            vkFreeMemory(info.device, staging_memory, nullptr);
            return false;
        }
        std::memcpy(staging_mapped, initial.data(), static_cast<std::size_t>(bytes));
        vkUnmapMemory(info.device, staging_memory);

        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, particle_buffers[slot],
                               particle_memories[slot])) {
                vkDestroyBuffer(info.device, staging, nullptr);
                vkFreeMemory(info.device, staging_memory, nullptr);
                return false;
            }
        }

        // 一条 one-shot 提交把 staging 拷进两份 device-local SSBO（拷完 QueueWaitIdle）
        std::string upload_err;
        bool copied = true;
        if (!submit_one_shot(
                [&](VkCommandBuffer cmd) {
                    VkBufferCopy region{};
                    region.size = bytes;
                    vkCmdCopyBuffer(cmd, staging, particle_buffers[0], 1, &region);
                    vkCmdCopyBuffer(cmd, staging, particle_buffers[1], 1, &region);
                },
                &upload_err)) {
            std::fprintf(stderr, "[vk] 粒子 SSBO 上传失败：%s\n", upload_err.c_str());
            copied = false;
        }
        vkDestroyBuffer(info.device, staging, nullptr);
        vkFreeMemory(info.device, staging_memory, nullptr);
        if (!copied) {
            return false;
        }

        // 补写各 slot 描述符的 binding 2（init 时 SSBO 尚不存在，只写了 binding 0/1）
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            const VkDescriptorBufferInfo ssbo_info{particle_buffers[slot], 0, bytes};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = particle_sets[slot];
            write.dstBinding = 2;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &ssbo_info;
            vkUpdateDescriptorSets(info.device, 1, &write, 0, nullptr);
        }

        particle_capacity = clamped;
        std::printf("[vk] 粒子 SSBO 就绪：%u 粒子 ×2 份（%llu 字节/份）\n", clamped,
                    static_cast<unsigned long long>(bytes));
        return true;
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

    // ---- 粒子 shader（T1.8.1）：编译/创建失败**不拖垮主链**（与 GL 的 build_pipeline 同策略）----
    {
        const SpirvResult comp = compile_shader_file_stage(
            info.shader_root + "/particle_update.comp", roots, ShaderStage::Compute,
            info.shader_defines);
        const SpirvResult point_vert = compile_shader_file_stage(
            info.shader_root + "/particle_draw.vert", roots, ShaderStage::Vertex,
            info.shader_defines);
        const SpirvResult point_frag = compile_shader_file_stage(
            info.shader_root + "/particle_draw.frag", roots, ShaderStage::Fragment,
            info.shader_defines);
        if (comp.ok && point_vert.ok && point_frag.ok) {
            impl_->compute_module = make_module(comp.words);
            impl_->point_vertex_module = make_module(point_vert.words);
            impl_->point_fragment_module = make_module(point_frag.words);
            impl_->particle_ready = (impl_->compute_module != VK_NULL_HANDLE &&
                                     impl_->point_vertex_module != VK_NULL_HANDLE &&
                                     impl_->point_fragment_module != VK_NULL_HANDLE);
        } else {
            const std::string& bad =
                !comp.ok ? comp.log : (!point_vert.ok ? point_vert.log : point_frag.log);
            std::fprintf(stderr, "[vk] 粒子 shader 编译失败（主链不受影响）：%s\n", bad.c_str());
        }
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

    // ---- 粒子 point pass（loadOp=LOAD：保留 raymarch 输出，粒子加色叠加）----
    // attachment 描述与 hdr_pass 相同（格式/samples 一致 → 与 hdr_framebuffers 兼容，
    // 复用 framebuffer 无需成对创建——与 vk_backend 的 clear/load 双 pass 同一手法）。
    // 注意 dependency 的 srcAccess 须含 COLOR_WRITE、dstAccess 须含 COLOR_READ：
    // loadOp=LOAD 的"读"需要外部依赖保证先前写入完成（hdr_pass 的 DONT_CARE 无此要求）。
    if (impl_->particle_ready) {
        VkAttachmentDescription color{};
        color.format = hdr_format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
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
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        create.attachmentCount = 1;
        create.pAttachments = &color;
        create.subpassCount = 1;
        create.pSubpasses = &subpass;
        create.dependencyCount = 1;
        create.pDependencies = &dependency;
        if (vkCreateRenderPass(info.device, &create, nullptr, &impl_->point_pass) != VK_SUCCESS) {
            last_error_ = "创建粒子 point render pass 失败";
            return false;
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

    // ---- 粒子描述符资源（T1.8.1）----
    // 独立于主链 set_layout：主链 binding 2 已是 post 输入纹理，粒子 SSBO 无法共用一套布局。
    // 编号与 GL 对齐（§5.4.1）：0 = SimParams UBO（vert+compute 读）、1 = LUT（frag）、
    // 2 = 粒子 SSBO（compute 读写 + vert pulling）。binding 2 在 upload_particles 时补写
    //（init 时 SSBO 尚未创建——容量由首帧参数决定）。
    if (impl_->particle_ready) {
        std::array<VkDescriptorSetLayoutBinding, 3> particle_bindings{};
        particle_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        particle_bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        particle_bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(particle_bindings.size());
        layout_info.pBindings = particle_bindings.data();
        if (vkCreateDescriptorSetLayout(info.device, &layout_info, nullptr,
                                        &impl_->particle_set_layout) != VK_SUCCESS) {
            last_error_ = "创建粒子描述符布局失败";
            return false;
        }

        // 池容量精确到需求（真机 OUT_OF_POOL 的教训）：2 set ×（1 UBO + 1 CIS + 1 SSBO）
        std::array<VkDescriptorPoolSize, 3> sizes{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight};
        sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
        sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight};
        VkDescriptorPoolCreateInfo pool_create{};
        pool_create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_create.maxSets = kFramesInFlight;
        pool_create.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool_create.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(info.device, &pool_create, nullptr,
                                   &impl_->particle_descriptor_pool) != VK_SUCCESS) {
            last_error_ = "创建粒子描述符池失败";
            return false;
        }

        std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
        layouts.fill(impl_->particle_set_layout);
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = impl_->particle_descriptor_pool;
        allocate.descriptorSetCount = kFramesInFlight;
        allocate.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(info.device, &allocate, impl_->particle_sets.data()) !=
            VK_SUCCESS) {
            last_error_ = "分配粒子描述符集失败";
            return false;
        }

        // binding 0（UBO，按 slot 各一份）与 binding 1（LUT）现在写定；binding 2 待上传后补
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            const VkDescriptorBufferInfo sim_slot{impl_->sim_buffers[slot], 0, sizeof(SimParams)};
            const VkDescriptorImageInfo lut_info{impl_->lut_sampler, impl_->lut_view,
                                                 VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = impl_->particle_sets[slot];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &sim_slot;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = impl_->particle_sets[slot];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &lut_info;
            vkUpdateDescriptorSets(info.device, 2, writes, 0, nullptr);
        }

        // 粒子管线布局（compute 与 point 两种 bind point 共用）
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &impl_->particle_set_layout;
        if (vkCreatePipelineLayout(info.device, &pipeline_layout_info, nullptr,
                                   &impl_->particle_pipeline_layout) != VK_SUCCESS) {
            last_error_ = "创建粒子管线布局失败";
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

    // ---- 粒子 compute + point 管线（T1.8.1）----
    if (impl_->particle_ready) {
        // compute（leapfrog KDK 积分一步）
        VkComputePipelineCreateInfo compute_create{};
        compute_create.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute_create.stage = {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, impl_->compute_module, "main", nullptr};
        compute_create.layout = impl_->particle_pipeline_layout;
        if (vkCreateComputePipelines(info.device, VK_NULL_HANDLE, 1, &compute_create, nullptr,
                                     &impl_->compute_pipeline) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 粒子 compute 管线创建失败（主链不受影响）\n");
            impl_->particle_ready = false;
        }

        // point（点精灵 vertex pulling + 加色混合进 HDR；与 GL 的 GL_ONE/GL_ONE 对齐）
        if (impl_->particle_ready) {
            VkPipelineShaderStageCreateInfo point_stages[2]{};
            point_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            point_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            point_stages[0].module = impl_->point_vertex_module;
            point_stages[0].pName = "main";
            point_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            point_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            point_stages[1].module = impl_->point_fragment_module;
            point_stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vertex_input{};
            vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

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

            // 加色混合（GL_ONE/GL_ONE），在色调映射前进 HDR（§5.6）
            VkPipelineColorBlendAttachmentState blend_attachment{};
            blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

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

            VkGraphicsPipelineCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            create.stageCount = 2;
            create.pStages = point_stages;
            create.pVertexInputState = &vertex_input;
            create.pInputAssemblyState = &assembly;
            create.pViewportState = &viewport;
            create.pRasterizationState = &raster;
            create.pMultisampleState = &multisample;
            create.pColorBlendState = &blend;
            create.pDynamicState = &dynamic;
            create.layout = impl_->particle_pipeline_layout;
            create.renderPass = impl_->point_pass;
            if (vkCreateGraphicsPipelines(info.device, VK_NULL_HANDLE, 1, &create, nullptr,
                                          &impl_->point_pipeline) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] 粒子 point 管线创建失败（主链不受影响）\n");
                impl_->particle_ready = false;
            }
        }

        if (!impl_->particle_ready) {
            // 任一管线失败即整条粒子链禁用（资源随 destroy 统一清理）
            impl_->particle_ready = false;
        } else {
            std::printf("[vk] 粒子管线就绪（compute + point）\n");
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

    // ---- GPU 计时（timestamp query，T1.7.2）----
    {
        // 队列族 timestampValidBits > 0 才能打时间戳（规范不保证图形队列支持）
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(info.physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        if (family_count > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(info.physical_device, &family_count,
                                                     families.data());
        }
        VkQueueFamilyProperties family{};
        if (info.queue_family < family_count) {
            family = families[info.queue_family];
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(info.physical_device, &properties);
        if (family.timestampValidBits > 0 && properties.limits.timestampPeriod > 0.0F) {
            VkQueryPoolCreateInfo pool_create{};
            pool_create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            pool_create.queryType = VK_QUERY_TYPE_TIMESTAMP;
            pool_create.queryCount = 2 * kFramesInFlight;  // 每帧首/尾一对，按 slot 分开
            if (vkCreateQueryPool(info.device, &pool_create, nullptr, &impl_->timer_pool) ==
                VK_SUCCESS) {
                impl_->timer_period_ms =
                    static_cast<double>(properties.limits.timestampPeriod) / 1e6;
            } else {
                std::fprintf(stderr, "[vk] 创建 timestamp query 池失败，GPU 计时不可用\n");
            }
        } else {
            std::fprintf(stderr, "[vk] 该队列不支持 timestamp，GPU 计时不可用（overlay 显示 n/a）\n");
        }
    }

    // ---- 平均步数归约资源（T1.7.2：alpha 通道逐级 blit 到 1×1，与 GL 的 mipmap 归约同语义）----
    {
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(info.physical_device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                            &format_properties);
        const bool blit_ok =
            (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0U &&
            (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0U;
        if (blit_ok && internal_extent.width > 0 && internal_extent.height > 0) {
            impl_->reduction_mips = static_cast<std::uint32_t>(
                                        std::floor(std::log2(static_cast<double>(std::max(
                                            internal_extent.width, internal_extent.height))))) +
                                    1;
            if (impl_->create_image(
                    internal_extent.width, internal_extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    impl_->reduction_image, impl_->reduction_memory, impl_->reduction_mips)) {
                impl_->reduction_ready = true;
                // 首用前过渡 UNDEFINED → GENERAL（全 mip 链）。LUT 上传的 one-shot 在本块
                // 之前执行（那里它还不存在），故单独提交一次；之后维持 GENERAL 免过渡约定。
                impl_->submit_one_shot([&](VkCommandBuffer cmd) {
                    VkImageMemoryBarrier barrier{};
                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = impl_->reduction_image;
                    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                impl_->reduction_mips, 0, 1};
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &barrier);
                });
            } else {
                std::fprintf(stderr, "[vk] 创建步数归约图像失败，平均步数不可用\n");
                impl_->reduction_image = VK_NULL_HANDLE;
            }
        } else {
            std::fprintf(stderr, "[vk] 该设备不支持 R16G16B16A16_SFLOAT blit，平均步数不可用\n");
        }
        if (impl_->reduction_ready) {
            // 1×1 RGBA16F 的读回落点（host-visible，永久映射；末级拷出后 host 侧延迟读）
            if (!impl_->create_buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      impl_->steps_buffer, impl_->steps_memory) ||
                vkMapMemory(info.device, impl_->steps_memory, 0, 16, 0, &impl_->steps_mapped) !=
                    VK_SUCCESS) {
                std::fprintf(stderr, "[vk] 创建步数读回缓冲失败，平均步数不可用\n");
                impl_->steps_mapped = nullptr;
                impl_->reduction_ready = false;
            } else {
                std::memset(impl_->steps_mapped, 0, 16);
            }
        }
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
        // ---- 粒子（T1.8.1）----
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (impl_->particle_buffers[slot] != VK_NULL_HANDLE) {
                vkDestroyBuffer(device, impl_->particle_buffers[slot], nullptr);
            }
            if (impl_->particle_memories[slot] != VK_NULL_HANDLE) {
                vkFreeMemory(device, impl_->particle_memories[slot], nullptr);
            }
        }
        if (impl_->point_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, impl_->point_pipeline, nullptr);
        }
        if (impl_->compute_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, impl_->compute_pipeline, nullptr);
        }
        if (impl_->particle_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, impl_->particle_pipeline_layout, nullptr);
        }
        if (impl_->particle_descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, impl_->particle_descriptor_pool, nullptr);
        }
        if (impl_->particle_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, impl_->particle_set_layout, nullptr);
        }
        if (impl_->point_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, impl_->point_pass, nullptr);
        }
        if (impl_->point_fragment_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, impl_->point_fragment_module, nullptr);
        }
        if (impl_->point_vertex_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, impl_->point_vertex_module, nullptr);
        }
        if (impl_->compute_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, impl_->compute_module, nullptr);
        }
        // ---- GPU 统计（T1.7.2）----
        if (impl_->timer_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, impl_->timer_pool, nullptr);
        }
        if (impl_->steps_mapped != nullptr) {
            vkUnmapMemory(device, impl_->steps_memory);
        }
        if (impl_->steps_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, impl_->steps_buffer, nullptr);
        }
        if (impl_->steps_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, impl_->steps_memory, nullptr);
        }
        if (impl_->reduction_image != VK_NULL_HANDLE) {
            vkDestroyImage(device, impl_->reduction_image, nullptr);
        }
        if (impl_->reduction_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, impl_->reduction_memory, nullptr);
        }
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

void RaymarchChain::record_offscreen(VkCommandBuffer cmd, std::uint32_t frame_slot,
                                     const SimParams& sim, float res_scale, bool fxaa) {
    if (!ready_) {
        return;
    }
    const bool want_resolve = (res_scale > 1.001F) || (res_scale < 0.999F);
    const VkExtent2D output = impl_->info.output_extent;
    const VkExtent2D internal_extent = impl_->info.internal_extent;

    // ---- GPU 计时帧首（T1.7.2）：覆盖 compute → 全部离屏 pass → final（帧尾在 record_final）----
    if (impl_->timer_period_ms > 0.0) {
        // 同 slot 上一帧已完成（fence 保证），重置本 slot 的一对 query 合法
        vkCmdResetQueryPool(cmd, impl_->timer_pool, 2 * frame_slot, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl_->timer_pool,
                            2 * frame_slot);
    }

    // ---- 粒子 compute（pass 外，T1.8.1）：ensure → （动画开）leapfrog 积分一步 ----
    const bool particle_enabled = (sim.flags[0] & kFlagParticleEnabled) != 0U;
    const bool particle_mode = (sim.flags[0] & kFlagParticle) != 0U;
    if (particle_enabled && impl_->particle_ready) {
        // count 变化即重建（GL 同语义）；SSBO 双缓冲按 slot 分份，跨提交无读写竞争
        impl_->upload_particles(
            static_cast<std::uint32_t>(unpack_float(sim.flags[2])), sim.hole[1], sim.hole[2]);
        if ((sim.flags[0] & kFlagAnimate) != 0U && impl_->particle_capacity > 0 &&
            impl_->particle_buffers[frame_slot] != VK_NULL_HANDLE) {
            // 上传（TRANSFER）或同 slot 上一帧 compute 写 → 本次 compute 读写的内存依赖
            VkBufferMemoryBarrier ready_barrier{};
            ready_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            ready_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            ready_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            ready_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ready_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ready_barrier.buffer = impl_->particle_buffers[frame_slot];
            ready_barrier.offset = 0;
            ready_barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                 &ready_barrier, 0, nullptr);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->compute_pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    impl_->particle_pipeline_layout, 0, 1,
                                    &impl_->particle_sets[frame_slot], 0, nullptr);
            const std::uint32_t groups = (impl_->particle_capacity + 63U) / 64U;
            vkCmdDispatch(cmd, groups, 1, 1);

            // SSBO 写 → 后续同提交的 compute 读写 + 顶点拉取读
            VkBufferMemoryBarrier after_barrier{};
            after_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            after_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            after_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            after_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            after_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            after_barrier.buffer = impl_->particle_buffers[frame_slot];
            after_barrier.offset = 0;
            after_barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                                 0, 0, nullptr, 1, &after_barrier, 0, nullptr);
        }
    }

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

    // ---- pass 1：raymarch → internal（粒子模式 = 清黑跳过，§5.6「独立显示」）----
    begin_pass(impl_->hdr_framebuffers[0], internal_extent);
    if (particle_mode && particle_enabled && impl_->particle_ready) {
        // 独立粒子模式：黑背景（与 GL 的 glClear(0,0,0,0) 对齐），alpha 也清零
        VkClearAttachment clear_color{};
        clear_color.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear_color.colorAttachment = 0;
        clear_color.clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
        VkClearRect clear_rect{{{0, 0}, internal_extent}, 0, 1};
        vkCmdClearAttachments(cmd, 1, &clear_color, 1, &clear_rect);
    } else {
        draw_fullscreen(impl_->pipelines[kPassRaymarch],
                        impl_->sets[kPassRaymarch][frame_slot]);
    }
    vkCmdEndRenderPass(cmd);
    impl_->barrier_attachment_to_shader(cmd, impl_->internal_image);

    // ---- 平均步数归约（T1.7.2；raymarch 之后、粒子叠加之前——alpha 未被污染）----
    // alpha = step_used（raymarch.frag V5.18）；逐级 blit 到 1×1 = 全图平均（GL 的
    // mipmap 归约同语义），末级拷到 host buffer 延迟读。低频更新（每 15 帧）。
    if (!particle_mode && impl_->reduction_ready && impl_->steps_mapped != nullptr) {
        ++impl_->steps_frame_counter;
        if (impl_->steps_frame_counter >= 15) {
            impl_->steps_frame_counter = 0;
            const std::uint32_t w = internal_extent.width;
            const std::uint32_t h = internal_extent.height;

            // internal 的 attachment 写 → TRANSFER 读
            VkImageMemoryBarrier to_transfer{};
            to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.image = impl_->internal_image;
            to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            to_transfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_transfer);

            // mip0：同尺寸 copy（internal → reduction）
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.extent = {w, h, 1};
            vkCmdCopyImage(cmd, impl_->internal_image, VK_IMAGE_LAYOUT_GENERAL,
                           impl_->reduction_image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);

            // 逐级 blit（LINEAR 缩半 ≈ box 平均）。同 image 不同 mip level 的 region 不重叠，
            // 每级之间用 TRANSFER 写后读屏障隔离。
            for (std::uint32_t level = 1; level < impl_->reduction_mips; ++level) {
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                blit.srcOffsets[1] = {static_cast<std::int32_t>(std::max(1U, w >> (level - 1))),
                                      static_cast<std::int32_t>(std::max(1U, h >> (level - 1))),
                                      1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.dstOffsets[1] = {static_cast<std::int32_t>(std::max(1U, w >> level)),
                                      static_cast<std::int32_t>(std::max(1U, h >> level)), 1};
                vkCmdBlitImage(cmd, impl_->reduction_image, VK_IMAGE_LAYOUT_GENERAL,
                               impl_->reduction_image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                               VK_FILTER_LINEAR);
                VkImageMemoryBarrier level_barrier{};
                level_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                level_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                level_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                level_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                level_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                level_barrier.image = impl_->reduction_image;
                level_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                level_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                level_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &level_barrier);
            }

            // 末级 1×1 → host buffer（RGBA16F 的第 4 通道 = 平均 alpha = 平均步数）
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, impl_->reduction_mips - 1, 0, 1};
            region.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, impl_->reduction_image, VK_IMAGE_LAYOUT_GENERAL,
                                   impl_->steps_buffer, 1, &region);
        }
    }

    // ---- pass 1.5：粒子点精灵（T1.8.1）——画进 internal HDR，与 GL 的 draw_particles 同位----
    if (particle_enabled && impl_->particle_ready && impl_->particle_capacity > 0) {
        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = impl_->point_pass;  // loadOp=LOAD：保留 raymarch 输出做加色
        begin.framebuffer = impl_->hdr_framebuffers[0];
        begin.renderArea = {{0, 0}, internal_extent};
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(internal_extent.width),
                            static_cast<float>(internal_extent.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, internal_extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->point_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                impl_->particle_pipeline_layout, 0, 1,
                                &impl_->particle_sets[frame_slot], 0, nullptr);
        vkCmdDraw(cmd, impl_->particle_capacity, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        impl_->barrier_attachment_to_shader(cmd, impl_->internal_image);
    }

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

    // GPU 计时帧尾（与 record_offscreen 帧首配对；呈现 blit 不计入——与 GL 的计时口径一致）
    if (impl_->timer_period_ms > 0.0) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl_->timer_pool,
                            2 * frame_slot + 1);
    }
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

    // 线性拉伸 blit：请求分辨率（LDR）→ 窗口尺寸（交换链）。
    // 垂直镜像（dst y 反向）：LDR 图 row 0 = 世界下方（与 GL 的 FBO row 域同语义），
    // 交换链 row 0 显示在屏幕顶部，故需镜像才能让屏幕顶部 = 世界上方——
    // 与 GL 交互显示（FBO row 0 落在屏幕底部）方向一致（真机五轮：读回翻转 + 呈现镜像）。
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(output.width),
                          static_cast<std::int32_t>(output.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, static_cast<std::int32_t>(dst_extent.height), 0};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(dst_extent.width), 0, 1};
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

    // 图像格式 RGBA16F → 转 float RGB。
    // 行序：拷贝从图像 row 0 开始（= v_uv.y=0 = 世界下方，与 GL 的 FBO row 0 同语义），
    // 而 core 约定"第 0 行 = 顶部" → 逐行翻转（与 gl_backend capture_hdr 完全同构；
    // 旧注释"原点在左上与 core 一致无需翻转"混淆了显示方向与数据行序，真机
    // NMSE=1.83 的根因之一，2026-09-20）
    std::vector<std::uint16_t> flipped(static_cast<std::size_t>(width) * height * 4);
    const std::size_t row_pixels = static_cast<std::size_t>(width);
    for (int y = 0; y < height; ++y) {
        const auto* src = static_cast<const std::uint16_t*>(impl_->readback_mapped) +
                          static_cast<std::size_t>(height - 1 - y) * row_pixels * 4;
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * row_pixels * 4, src,
                    row_pixels * 4 * sizeof(std::uint16_t));
    }
    out.assign(static_cast<std::size_t>(3) * width * height, 0.0F);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        out[i * 3 + 0] = half_to_float(flipped[i * 4 + 0]);
        out[i * 3 + 1] = half_to_float(flipped[i * 4 + 1]);
        out[i * 3 + 2] = half_to_float(flipped[i * 4 + 2]);
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

    // 离屏 LDR 图格式 = 交换链格式（多数设备 B8G8R8A8，少数 R8G8B8A8）→ 按实际通道序转 RGB。
    // 行序：拷贝从图像 row 0 开始（= 世界下方）→ 逐行翻转对齐 core"第 0 行 = 顶部"
    //（与 read_hdr 同理，与 gl_backend capture_ldr 同构）
    const VkFormat format = impl_->info.swapchain_format;
    const auto* source_pixels = static_cast<const unsigned char*>(impl_->readback_mapped);
    std::vector<unsigned char> flipped(static_cast<std::size_t>(width) * height * 4);
    const std::size_t row_pixels = static_cast<std::size_t>(width);
    for (int y = 0; y < height; ++y) {
        const auto* src = source_pixels +
                          static_cast<std::size_t>(height - 1 - y) * row_pixels * 4;
        std::memcpy(flipped.data() + static_cast<std::size_t>(y) * row_pixels * 4, src,
                    row_pixels * 4);
    }
    const bool bgra = (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB);
    out.assign(static_cast<std::size_t>(3) * width * height, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        if (bgra) {
            out[i * 3 + 0] = flipped[i * 4 + 2];  // R ← B 通道
            out[i * 3 + 1] = flipped[i * 4 + 1];
            out[i * 3 + 2] = flipped[i * 4 + 0];  // B ← R 通道
        } else {
            out[i * 3 + 0] = flipped[i * 4 + 0];
            out[i * 3 + 1] = flipped[i * 4 + 1];
            out[i * 3 + 2] = flipped[i * 4 + 2];
        }
    }
    return true;
}

// ---------------------------------------------------------------- 监控数据（T1.7.2）

bool RaymarchChain::internal_extent(std::uint32_t& width, std::uint32_t& height) const {
    if (impl_ == nullptr || !ready_) {
        return false;
    }
    width = impl_->info.internal_extent.width;
    height = impl_->info.internal_extent.height;
    return true;
}

double RaymarchChain::gpu_frame_ms() const {
    if (impl_ == nullptr || !ready_ || impl_->timer_period_ms <= 0.0 ||
        impl_->timer_pool == VK_NULL_HANDLE) {
        return -1.0;
    }
    // 非阻塞轮询两个 slot 的（首，尾）时间戳对：fence 已等待完成的 slot 结果必然就绪，
    // 另一个 slot 保持 NOT_READY 就跳过。缓存最近一次成功读到的帧耗时。
    double latest = -1.0;
    for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        std::uint64_t stamps[2] = {0, 0};
        const VkResult r = vkGetQueryPoolResults(impl_->info.device, impl_->timer_pool,
                                                 2 * slot, 2, sizeof(stamps), stamps,
                                                 sizeof(std::uint64_t),
                                                 VK_QUERY_RESULT_64_BIT);
        if (r == VK_SUCCESS && stamps[1] >= stamps[0]) {
            latest = static_cast<double>(stamps[1] - stamps[0]) * impl_->timer_period_ms;
        }
    }
    if (latest > 0.0) {
        impl_->gpu_ms_cache = latest;
    }
    return impl_->gpu_ms_cache;
}

double RaymarchChain::last_avg_steps() const {
    if (impl_ == nullptr || !ready_ || !impl_->reduction_ready ||
        impl_->steps_mapped == nullptr) {
        return -1.0;
    }
    // 读 host buffer 的 alpha（RGBA16F 第 4 通道，byte offset 6）。拷贝是异步的——读到的
    // 可能是上一轮归约的结果；步数 ≥ 1 恒为正 half，读到 0 = 尚无数据。
    std::uint16_t raw = 0;
    std::memcpy(&raw, static_cast<const std::uint8_t*>(impl_->steps_mapped) + 6, sizeof(raw));
    const float value = half_to_float(raw);
    if (value > 0.0F) {
        impl_->avg_steps_cache = value;
    }
    return impl_->avg_steps_cache;
}

}  // namespace ehe::render::vk
