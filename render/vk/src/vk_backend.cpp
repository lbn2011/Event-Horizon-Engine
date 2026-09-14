// EHE —— Vulkan 1.2 后端实现（T0.4）
//
// 职责：volk 初始化 → instance（1.2）→ 物理设备/逻辑设备/队列 → swapchain + imageview
//       → render pass + framebuffer → 命令缓冲与同步 → ImGui（ImGui_ImplVulkan）空面板。
//
// 规格依据：DESIGN §5.4.1（资源约定）、§5.2（生命周期与后端切换）
//
// 设计取舍：
//   - 自行管理 swapchain（不使用 ImGui 示例的 ImGui_ImplVulkanH_Window 辅助层），
//     因为 T1.3/T1.5 需要 HDR 离屏 attachment 与多 pass 后处理，自管更易扩展；
//   - 本阶段无 shader：render pass 只做 clear + ImGui 绘制，Vulkan 1.2 核心特性范围内；
//   - 开发机无 Vulkan 设备，本文件仅做编译期验证，运行验证在目标机（DESIGN §2.1）。

#include "ehe/render/vk/vk_backend.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <volk.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "ehe/render/backend.h"

namespace ehe::render::vk {
namespace {

constexpr int kFramesInFlight = 2;
constexpr VkFormat kPreferredFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
constexpr VkColorSpaceKHR kPreferredColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

struct FrameSync {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

void check_vk(VkResult result) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] Vulkan 调用返回错误码 %d\n", static_cast<int>(result));
    }
}

class VKBackend final : public IRenderer {
public:
    Backend backend() const override { return Backend::Vulkan; }

    bool init(const RendererConfig& cfg) override {
        // Vulkan 不需要 GL 上下文：窗口必须以 GLFW_NO_API 创建
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, cfg.visible ? GLFW_TRUE : GLFW_FALSE);
        window_ = glfwCreateWindow(cfg.width, cfg.height, cfg.title, nullptr, nullptr);
        if (window_ == nullptr) {
            std::fprintf(stderr, "[vk] 创建窗口失败\n");
            return false;
        }
        vsync_ = cfg.vsync;

        if (volkInitialize() != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] volk 初始化失败：系统缺少 vulkan-1.dll（未安装显卡驱动？）\n");
            destroy_window();
            return false;
        }
        if (!create_instance() || !create_surface() || !pick_physical_device() || !create_device()) {
            return false;
        }
        if (!create_swapchain() || !create_render_pass() || !create_framebuffers()) {
            return false;
        }
        if (!create_commands() || !create_sync_objects()) {
            return false;
        }
        if (!init_imgui()) {
            return false;
        }

        const auto* name = physical_device_properties_.deviceName;
        std::printf("[vk] 初始化完成：%s / %s / swapchain %ux%u images=%u\n", name,
                    backend_name(Backend::Vulkan), swapchain_extent_.width, swapchain_extent_.height,
                    static_cast<unsigned>(swapchain_images_.size()));
        return true;
    }

    void shutdown() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        if (imgui_ready_) {
            ImGui_ImplVulkan_Shutdown();
            imgui_ready_ = false;
        }
        if (imgui_context_created_ && ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
            imgui_context_created_ = false;
        }
        if (device_ != VK_NULL_HANDLE && imgui_descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, imgui_descriptor_pool_, nullptr);
            imgui_descriptor_pool_ = VK_NULL_HANDLE;
        }
        destroy_sync_objects();
        destroy_command_pool();
        destroy_swapchain_resources();

        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        destroy_window();
    }

    void resize(int width, int height) override {
        resize_pending_ = true;
        pending_width_ = width;
        pending_height_ = height;
        glfwGetFramebufferSize(window_, &width_, &height_);
    }

    void begin_frame() override {
        if (device_ == VK_NULL_HANDLE || window_ == nullptr) {
            return;
        }
        // 尺寸检测放在后端内部（app 层不介入）；变化即重建交换链
        glfwGetFramebufferSize(window_, &width_, &height_);
        if (width_ <= 0 || height_ <= 0) {  // 最小化：跳过渲染但保持消息循环
            skip_frame_ = true;
            return;
        }
        if (static_cast<uint32_t>(width_) != swapchain_extent_.width ||
            static_cast<uint32_t>(height_) != swapchain_extent_.height) {
            recreate_swapchain();
            skip_frame_ = true;  // 本帧不渲染，下一帧用新交换链
            return;
        }
        skip_frame_ = false;

        FrameSync& frame = frames_[current_frame_];
        vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

        VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                 frame.image_available, VK_NULL_HANDLE, &image_index_);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            skip_frame_ = true;
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            std::fprintf(stderr, "[vk] 获取交换链图像失败（%d）\n", static_cast<int>(acquire));
            skip_frame_ = true;
            return;
        }
        vkResetFences(device_, 1, &frame.in_flight);
        vkResetCommandBuffer(frame.cmd, 0);
        record_commands(frame.cmd, image_index_);
        image_acquired_ = true;

        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
    }

    void end_frame() override {
        if (skip_frame_) {
            return;  // 本帧未调用 ImGui::NewFrame，无需收尾
        }
        FrameSync& frame = frames_[current_frame_];

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.cmd);
        vkCmdEndRenderPass(frame.cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.image_available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &frame.render_finished;
        check_vk(vkQueueSubmit(queue_, 1, &submit, frame.in_flight));

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &frame.render_finished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index_;
        const VkResult present_result = vkQueuePresentKHR(queue_, &present);

        image_acquired_ = false;
        current_frame_ = (current_frame_ + 1) % kFramesInFlight;

        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR ||
            resize_pending_) {
            resize_pending_ = false;
            recreate_swapchain();
        }
    }

    bool should_close() const override {
        return window_ != nullptr && (glfwWindowShouldClose(window_) != 0);
    }

    void framebuffer_size(int& width, int& height) const override {
        width = width_;
        height = height_;
    }

    // ---------------------------------------------------------------- raymarch 管线（T1.6 实现）
    // 规格见 DESIGN §5.4.1 的 Vulkan 列：R16G16B16A16_SFLOAT 附件 + descriptor set 0
    // （binding 0 = UBO，binding 1 = 黑体 LUT）+ glslang 运行时编译 SPIR-V。
    // T1.3 阶段先提供接口骨架（Vulkan 路径尚未接渲染管线，M0 门禁也仍待目标机验收）。

    void set_params(const SimParams& params) override { params_ = params; }

    void set_shader_root(const std::string& root) override { shader_root_ = root; }

    bool pipeline_ready() const override { return false; }

    const std::string& last_error() const override { return last_error_; }

    bool capture_hdr(std::vector<float>& rgb, int& width, int& height) override {
        (void)rgb;
        (void)width;
        (void)height;
        last_error_ = "Vulkan 后端的 raymarch 管线与 HDR 回读将在 T1.6 实现（当前仅 GL 可用）";
        return false;
    }

private:
    // ---------------------------------------------------------------- 初始化分步

    bool create_instance() {
        uint32_t extension_count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extension_count);
        if (extensions == nullptr || extension_count == 0) {
            std::fprintf(stderr, "[vk] 无法获取 GLFW 所需的实例扩展\n");
            return false;
        }

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Event Horizon Engine";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "EHE";
        app.apiVersion = VK_API_VERSION_1_2;  // 底线 Vulkan 1.2（DESIGN §2.1）

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = extension_count;
        info.ppEnabledExtensionNames = extensions;

        const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建实例失败（%d）：驱动可能不支持 Vulkan 1.2\n",
                         static_cast<int>(result));
            return false;
        }
        volkLoadInstance(instance_);
        return true;
    }

    bool create_surface() {
        const VkResult result = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建窗口表面失败（%d）\n", static_cast<int>(result));
            return false;
        }
        return true;
    }

    bool pick_physical_device() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) {
            std::fprintf(stderr, "[vk] 未找到支持 Vulkan 的物理设备\n");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());

        int best_score = -1;
        for (const VkPhysicalDevice candidate : devices) {
            const int score = score_device(candidate);
            if (score > best_score) {
                best_score = score;
                physical_device_ = candidate;
            }
        }
        if (physical_device_ == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[vk] 未找到同时支持图形与呈现的物理设备\n");
            return false;
        }
        vkGetPhysicalDeviceProperties(physical_device_, &physical_device_properties_);
        return true;
    }

    /// 评分：必须含「图形 + 呈现」队列族（独显优先由调用方按类型排序即可，含即可用）
    int score_device(VkPhysicalDevice candidate) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        for (uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 present_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present_supported);
            if (present_supported == VK_TRUE) {
                return 1;
            }
        }
        return -1;
    }

    bool create_device() {
        // 选择「图形 + 呈现」队列族（先扫描一次以确定索引）
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());

        queue_family_ = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 present_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present_supported);
            if (present_supported == VK_TRUE) {
                queue_family_ = i;
                break;
            }
        }
        if (queue_family_ == UINT32_MAX) {
            std::fprintf(stderr, "[vk] 未找到可同时用于图形与呈现的队列族\n");
            return false;
        }

        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue_info;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = device_extensions;

        const VkResult result = vkCreateDevice(physical_device_, &info, nullptr, &device_);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建逻辑设备失败（%d）\n", static_cast<int>(result));
            return false;
        }
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        return true;
    }

    bool create_swapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        if (format_count > 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
        }
        VkSurfaceFormatKHR chosen_format = formats.empty() ? VkSurfaceFormatKHR{kPreferredFormats[0], kPreferredColorSpace}
                                                          : formats.front();
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.colorSpace != kPreferredColorSpace) {
                continue;
            }
            for (const VkFormat preferred : kPreferredFormats) {
                if (candidate.format == preferred) {
                    chosen_format = candidate;
                }
            }
        }

        uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        if (mode_count > 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, modes.data());
        }
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;  // FIFO 是规范保证可用的
        if (!vsync_) {
            for (const VkPresentModeKHR mode : modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    present_mode = mode;
                    break;
                }
                if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                    present_mode = mode;
                }
            }
        }

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {  // 尺寸由应用决定
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window_, &width, &height);
            extent.width = std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width,
                                      caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height,
                                       caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            std::fprintf(stderr, "[vk] 交换链尺寸无效（窗口最小化？）\n");
            return false;
        }

        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
            image_count = caps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = image_count;
        info.imageFormat = chosen_format.format;
        info.imageColorSpace = chosen_format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = present_mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;

        const VkResult result = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建交换链失败（%d）\n", static_cast<int>(result));
            return false;
        }
        swapchain_format_ = chosen_format.format;
        swapchain_extent_ = extent;

        uint32_t actual_count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr);
        swapchain_images_.resize(actual_count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, swapchain_images_.data());

        swapchain_views_.resize(actual_count, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < actual_count; ++i) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = swapchain_images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = swapchain_format_;
            view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &view, nullptr, &swapchain_views_[i]) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] 创建图像视图失败\n");
                return false;
            }
        }

        width_ = static_cast<int>(extent.width);
        height_ = static_cast<int>(extent.height);
        return true;
    }

    bool create_render_pass() {
        VkAttachmentDescription color{};
        color.format = swapchain_format_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // T1.3 起改为 LOAD + 场景先渲染
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &color;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;

        if (vkCreateRenderPass(device_, &info, nullptr, &render_pass_) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建 render pass 失败\n");
            return false;
        }
        return true;
    }

    bool create_framebuffers() {
        framebuffers_.resize(swapchain_views_.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < swapchain_views_.size(); ++i) {
            VkFramebufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = render_pass_;
            info.attachmentCount = 1;
            info.pAttachments = &swapchain_views_[i];
            info.width = swapchain_extent_.width;
            info.height = swapchain_extent_.height;
            info.layers = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] 创建 framebuffer 失败\n");
                return false;
            }
        }
        return true;
    }

    bool create_commands() {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建命令池失败\n");
            return false;
        }

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kFramesInFlight;
        VkCommandBuffer buffers[kFramesInFlight] = {};
        if (vkAllocateCommandBuffers(device_, &alloc, buffers) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 分配命令缓冲失败\n");
            return false;
        }
        for (int i = 0; i < kFramesInFlight; ++i) {
            frames_[i].cmd = buffers[i];
        }
        return true;
    }

    bool create_sync_objects() {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (int i = 0; i < kFramesInFlight; ++i) {
            FrameSync& frame = frames_[i];
            if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.render_finished) != VK_SUCCESS ||
                vkCreateFence(device_, &fence_info, nullptr, &frame.in_flight) != VK_SUCCESS) {
                std::fprintf(stderr, "[vk] 创建同步对象失败\n");
                return false;
            }
        }
        return true;
    }

    bool init_imgui() {
        if (ImGui::GetCurrentContext() == nullptr) {
            ImGui::CreateContext();
            imgui_context_created_ = true;
        }
        ImGui::StyleColorsDark();

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 64;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // ImGui 后端要求池带 FREE_DESCRIPTOR_SET_BIT
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 64;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &imgui_descriptor_pool_) != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建 ImGui 描述符池失败\n");
            return false;
        }

        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_2;
        init.Instance = instance_;
        init.PhysicalDevice = physical_device_;
        init.Device = device_;
        init.QueueFamily = queue_family_;
        init.Queue = queue_;
        init.DescriptorPool = imgui_descriptor_pool_;
        init.MinImageCount = std::max(2U, static_cast<uint32_t>(swapchain_images_.size()));
        init.ImageCount = static_cast<uint32_t>(swapchain_images_.size());
        init.PipelineInfoMain.RenderPass = render_pass_;
        init.PipelineInfoMain.Subpass = 0;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.CheckVkResultFn = check_vk;
        init.MinAllocationSize = 1024 * 1024;

        if (!ImGui_ImplVulkan_Init(&init)) {
            std::fprintf(stderr, "[vk] ImGui Vulkan 后端初始化失败\n");
            return false;
        }
        ImGui_ImplVulkan_SetMinImageCount(init.MinImageCount);
        imgui_ready_ = true;
        return true;
    }

    // ---------------------------------------------------------------- 每帧与重建

    void record_commands(VkCommandBuffer cmd, uint32_t image_index) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkClearValue clear{};
        clear.color = {{0.02F, 0.02F, 0.03F, 1.0F}};

        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = render_pass_;
        pass.framebuffer = framebuffers_[image_index];
        pass.renderArea.offset = {0, 0};
        pass.renderArea.extent = swapchain_extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    }

    void recreate_swapchain() {
        if (device_ == VK_NULL_HANDLE || window_ == nullptr) {
            return;
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {  // 最小化：等待恢复（阻塞在此，与窗口消息循环一致）
            glfwWaitEvents();
            glfwGetFramebufferSize(window_, &width, &height);
        }

        vkDeviceWaitIdle(device_);
        destroy_swapchain_resources();
        if (!create_swapchain() || !create_render_pass() || !create_framebuffers()) {
            std::fprintf(stderr, "[vk] 交换链重建失败\n");
            return;
        }
        // render pass 变更后 ImGui 管线需要同步最小图像数
        if (imgui_ready_) {
            ImGui_ImplVulkan_SetMinImageCount(
                std::max(2U, static_cast<uint32_t>(swapchain_images_.size())));
        }
    }

    void destroy_swapchain_resources() {
        for (const VkFramebuffer framebuffer : framebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
        }
        framebuffers_.clear();
        if (render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            render_pass_ = VK_NULL_HANDLE;
        }
        for (const VkImageView view : swapchain_views_) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, view, nullptr);
            }
        }
        swapchain_views_.clear();
        swapchain_images_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    void destroy_command_pool() {
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
        }
    }

    void destroy_sync_objects() {
        for (FrameSync& frame : frames_) {
            if (frame.image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, frame.image_available, nullptr);
                frame.image_available = VK_NULL_HANDLE;
            }
            if (frame.render_finished != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, frame.render_finished, nullptr);
                frame.render_finished = VK_NULL_HANDLE;
            }
            if (frame.in_flight != VK_NULL_HANDLE) {
                vkDestroyFence(device_, frame.in_flight, nullptr);
                frame.in_flight = VK_NULL_HANDLE;
            }
        }
    }

    void destroy_window() {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
    }

    // ---------------------------------------------------------------- 成员

    SimParams params_{};
    std::string shader_root_;
    std::string last_error_;

    GLFWwindow* window_ = nullptr;
    bool vsync_ = true;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physical_device_properties_{};
    uint32_t queue_family_ = UINT32_MAX;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    FrameSync frames_[kFramesInFlight];
    uint32_t current_frame_ = 0;
    uint32_t image_index_ = 0;

    VkDescriptorPool imgui_descriptor_pool_ = VK_NULL_HANDLE;
    bool imgui_ready_ = false;
    bool imgui_context_created_ = false;

    bool skip_frame_ = false;
    bool image_acquired_ = false;
    bool resize_pending_ = false;
    int pending_width_ = 0;
    int pending_height_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace

std::unique_ptr<IRenderer> create_renderer() { return std::make_unique<VKBackend>(); }

}  // namespace ehe::render::vk
