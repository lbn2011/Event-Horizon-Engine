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
#include "ehe/render/vk/raymarch_chain.h"

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

/// 验证层回调（T1.6.4 真机诊断）：违规详情直接进 stderr，替代"猜 DEVICE_LOST 根因"。
VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user_data*/) {
    std::fprintf(stderr, "[vk] 验证层（%s%s%s）：%s\n",
                 (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "错误" : "",
                 (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "警告" : "",
                 (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
                     ? ""
                     : "提示",
                 data->pMessage != nullptr ? data->pMessage : "(无消息内容)");
    if (data->pMessageIdName != nullptr) {
        std::fprintf(stderr, "    id=%s\n", data->pMessageIdName);
    }
    return VK_FALSE;
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
        // 与 GL 后端同语义：config 里的 shader 根目录直接落地（app 后续的 set_shader_root 会覆盖）
        if (!cfg.shader_root.empty()) {
            shader_root_ = cfg.shader_root;
        }
        // 显式渲染尺寸优先（离屏 smoke 用）；0 = 跟随窗口（与 GL 的 render_width_/render_height_ 一致）
        render_width_ = cfg.render_width;
        render_height_ = cfg.render_height;

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
        // 建渲染链（raymarch + 后处理，T1.6.1）。与 GL 的 build_pipeline 同语义：
        // 失败不致命（窗口与面板仍可用），错误经 last_error() 暴露给 UI。
        // 宏规整在此处做（create_device 之后，shaderFloat64 已知）：
        //   命令行/Config 的 defines（如 EHE_FP32_ONLY）必须参与规整，不能丢。
        requested_defines_ = effective_shader_defines(cfg.shader_defines);
        if (!create_chain()) {
            std::fprintf(stderr, "[vk] 渲染链未就绪：%s\n", last_error_.c_str());
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
        // 链依赖 device 与 render pass，须在两者之前销毁
        chain_.destroy();
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
            // messenger 须在销毁实例前销毁，否则验证层报 VUID-vkDestroyInstance-instance-00629 泄漏
            if (debug_messenger_ != VK_NULL_HANDLE) {
                vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
                debug_messenger_ = VK_NULL_HANDLE;
            }
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
        // res_scale 变化 → 离屏目标尺寸随之变化，必须重建整条链（§5.4.1）。
        // 窗口/交换链尺寸**不再**触发链重建：渲染分辨率与窗口解耦后，链的离屏目标
        // 只跟请求分辨率走，窗口变化由呈现期的 blit 拉伸吸收（与 GL 的 ensure_targets 同语义）。
        {
            const float res_scale = unpack_float(params_.flags[1]);
            const bool scale_changed = (res_scale != chain_res_scale_);
            if (chain_.ready() && scale_changed && device_ != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device_);
                chain_.destroy();
                create_chain();
            }
        }
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
        // 修复：此前从未调用 vkEndCommandBuffer 就提交（真机验证层抓出
        // VUID-vkQueueSubmit-pCommandBuffers-00070，Iris Xe 2026-09-19）
        check_vk(vkEndCommandBuffer(frame.cmd));

        // fence 重置放在提交前：begin 失败路径不提交时 fence 仍 signaled，下一帧等待立即通过不会挂死
        vkResetFences(device_, 1, &frame.in_flight);

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

    bool pipeline_ready() const override { return chain_.ready(); }

    const std::string& last_error() const override { return last_error_; }

    void finish() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
    }

    bool capture_ldr(std::vector<unsigned char>& rgb, int& width, int& height) override {
        if (!chain_.ready()) {
            last_error_ = "Vulkan 渲染链未就绪，无法回读 LDR";
            return false;
        }
        // 从离屏 LDR 图回读（请求分辨率成品）；交换链图像只作呈现，不参与回读
        return chain_.read_ldr(rgb, width, height);
    }

    /// 物理设备能力清单：T1.6 的实现选型依据（尤其 shaderFloat64 —— Intel 核显不原生支持，
    /// 直接决定 Vulkan 侧能否走 mixed/fp64，见 DESIGN §7 精度模式）。
    bool rebuild_pipeline(const std::vector<std::string>& shader_defines) override {
        // 规整宏（恒含 EHE_VULKAN；fp64 不可用时加 EHE_FP32_ONLY），随后重建整条链
        requested_defines_ = effective_shader_defines(shader_defines);
        std::string list;
        for (const std::string& define : requested_defines_) {
            list += define;
            list += " ";
        }
        std::printf("[vk] 重建管线，精度宏：%s\n", list.c_str());
        if (device_ == VK_NULL_HANDLE) {
            last_error_ = "设备未就绪，无法重建管线";
            return false;
        }
        vkDeviceWaitIdle(device_);
        chain_.destroy();
        return create_chain();
    }

    std::string capability_report() const override {
        std::string report;
        if (instance_ == VK_NULL_HANDLE) {
            return "[VK] instance 未创建（volk 初始化或驱动加载失败）\n";
        }
        std::uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
        char line[512] = {};
        std::snprintf(line, sizeof(line), "[VK] physical_devices=%u\n", device_count);
        report += line;
        if (device_count == 0) {
            report += "[VK] 无可用物理设备（未安装 Vulkan 运行时/驱动？）\n";
            return report;
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
        for (std::uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(devices[i], &features);

            const auto major = VK_VERSION_MAJOR(properties.apiVersion);
            const auto minor = VK_VERSION_MINOR(properties.apiVersion);
            const auto patch = VK_VERSION_PATCH(properties.apiVersion);
            std::snprintf(line, sizeof(line), "[VK] [%u] name=%s\n[VK] [%u] apiVersion=%u.%u.%u\n",
                          i, properties.deviceName, i, major, minor, patch);
            report += line;
            std::snprintf(line, sizeof(line),
                          "[VK] [%u] driverVersion=0x%08X deviceType=%d vendorID=0x%04X deviceID=0x%04X\n",
                          i, properties.driverVersion, static_cast<int>(properties.deviceType),
                          properties.vendorID, properties.deviceID);
            report += line;
            std::snprintf(line, sizeof(line),
                          "[VK] [%u] features.shaderFloat64=%d shaderInt64=%d shaderInt16=%d "
                          "fragmentStoresAndAtomics=%d shaderStorageImageExtendedFormats=%d "
                          "vertexPipelineStoresAndAtomics=%d\n",
                          i, features.shaderFloat64 ? 1 : 0, features.shaderInt64 ? 1 : 0,
                          features.shaderInt16 ? 1 : 0, features.fragmentStoresAndAtomics ? 1 : 0,
                          features.shaderStorageImageExtendedFormats ? 1 : 0,
                          features.vertexPipelineStoresAndAtomics ? 1 : 0);
            report += line;
            std::snprintf(line, sizeof(line),
                          "[VK] [%u] limits maxComputeWorkGroupInvocations=%u "
                          "maxComputeSharedMemorySize=%u maxStorageBufferRange=%u maxImageDimension2D=%u\n",
                          i, properties.limits.maxComputeWorkGroupInvocations,
                          properties.limits.maxComputeSharedMemorySize,
                          properties.limits.maxStorageBufferRange,
                          properties.limits.maxImageDimension2D);
            report += line;
        }
        report += shader_float64_supported_
                      ? "[VK] 精度策略：mixed(fp64) 可用（已启用 shaderFloat64）\n"
                      : "[VK] 精度策略：**强制 fp32**（该设备 shaderFloat64=0，fp64 流水线无法创建）\n";
        report += "[VK] 说明：raymarch/后处理管线已实现（T1.6.1-3 已合并），当前处于目标机运行验证期（T1.6.4）\n";
        return report;
    }

    bool capture_hdr(std::vector<float>& rgb, int& width, int& height) override {
        if (!chain_.ready()) {
            last_error_ = "Vulkan 渲染链未就绪，无法回读 HDR";
            return false;
        }
        // 与 GL 后端同语义：输出分辨率、分辨率变换之后、色调映射之前的 HDR（§6.1）
        return chain_.read_hdr(rgb, width, height);
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

        // ---- 验证层 + debug messenger（T1.6.4 真机诊断，2026-09-19）----
        // 目标机装了 Vulkan SDK（vulkaninfo 可见 VK_LAYER_KHRONOS_validation），默认启用：
        // 任何违规（如未绑定图像内存）都会在 DEVICE_LOST 之前打出可读的违规详情。
        // EHE_VK_VALIDATE=0 可关闭；T1.9 正式发布前把默认值改为关闭。
        const char* validate_env = std::getenv("EHE_VK_VALIDATE");
        const bool want_validation = (validate_env == nullptr || std::strcmp(validate_env, "0") != 0);

        std::vector<const char*> layer_names;
        if (want_validation) {
            uint32_t layer_count = 0;
            vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            std::vector<VkLayerProperties> layers(layer_count);
            if (layer_count > 0) {
                vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
            }
            for (const VkLayerProperties& layer : layers) {
                if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                    layer_names.push_back("VK_LAYER_KHRONOS_validation");
                    std::printf("[vk] 验证层已启用（EHE_VK_VALIDATE=0 可关闭）\n");
                    break;
                }
            }
        }

        std::vector<const char*> all_extensions(extensions, extensions + extension_count);
        bool debug_utils_available = false;
        {
            uint32_t prop_count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &prop_count, nullptr);
            std::vector<VkExtensionProperties> props(prop_count);
            if (prop_count > 0) {
                vkEnumerateInstanceExtensionProperties(nullptr, &prop_count, props.data());
            }
            for (const VkExtensionProperties& prop : props) {
                if (std::strcmp(prop.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                    debug_utils_available = true;
                    break;
                }
            }
        }
        if (debug_utils_available) {
            all_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Event Horizon Engine";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "EHE";
        app.apiVersion = VK_API_VERSION_1_2;  // 底线 Vulkan 1.2（DESIGN §2.1）

        // messenger 挂到 instance pNext：instance 创建/销毁期间的违规也能被捕获
        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        if (debug_utils_available) {
            messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger.pfnUserCallback = debug_utils_callback;
        }

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pNext = debug_utils_available ? &messenger : nullptr;
        info.pApplicationInfo = &app;
        info.enabledLayerCount = static_cast<uint32_t>(layer_names.size());
        info.ppEnabledLayerNames = layer_names.empty() ? nullptr : layer_names.data();
        info.enabledExtensionCount = static_cast<uint32_t>(all_extensions.size());
        info.ppEnabledExtensionNames = all_extensions.data();

        const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建实例失败（%d）：驱动可能不支持 Vulkan 1.2%s\n",
                         static_cast<int>(result),
                         layer_names.empty() ? ""
                                             : "（已请求验证层，罕见不兼容；可设 EHE_VK_VALIDATE=0 重试）");
            return false;
        }
        volkLoadInstance(instance_);
        if (debug_utils_available) {
            vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &debug_messenger_);
        }
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

        // ---- 设备特性：fp64 是"要么启用、要么绕开"的硬约束（DESIGN V5.10 约定③）----
        // 查询 shaderFloat64：
        //   - 支持 → 显式启用（**不启用的话 fp64 流水线在创建时就会失败**）；
        //   - 不支持（如 Intel 核显实测 0）→ 记录并在编译 shader 时强制 EHE_FP32_ONLY，
        //     否则 SPIR-V 里的 Float64 能力会让 vkCreateGraphicsPipelines 直接失败。
        VkPhysicalDeviceFeatures supported_features{};
        vkGetPhysicalDeviceFeatures(physical_device_, &supported_features);
        shader_float64_supported_ = (supported_features.shaderFloat64 == VK_TRUE);

        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.shaderFloat64 = shader_float64_supported_ ? VK_TRUE : VK_FALSE;
        // 采样器各向异性在本项目未使用；其余特性按需最小化，避免在不支持的设备上创建失败

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue_info;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = device_extensions;
        info.pEnabledFeatures = &enabled_features;

        const VkResult result = vkCreateDevice(physical_device_, &info, nullptr, &device_);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[vk] 创建逻辑设备失败（%d）\n", static_cast<int>(result));
            return false;
        }
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        std::printf("[vk] 设备就绪：shaderFloat64=%s → 精度策略：%s\n",
                    shader_float64_supported_ ? "YES" : "no",
                    shader_float64_supported_ ? "可用 mixed(fp64)" : "强制 fp32（EHE_FP32_ONLY）");
        return true;
    }

    /// 把调用方给的宏规整为 VK 路径实际使用的集合：
    ///   - 恒注入 `EHE_VULKAN`（抹平 gl_VertexIndex / gl_VertexID 差异，DESIGN V5.10 约定①）；
    ///   - 设备不支持 fp64 时强制 `EHE_FP32_ONLY`（V5.10 约定③）。
    std::vector<std::string> effective_shader_defines(const std::vector<std::string>& requested) const {
        std::vector<std::string> defines = requested;
        const auto ensure = [&defines](const char* name) {
            for (const std::string& existing : defines) {
                if (existing == name) {
                    return;
                }
            }
            defines.emplace_back(name);
        };
        ensure("EHE_VULKAN");
        if (!shader_float64_supported_) {
            ensure("EHE_FP32_ONLY");
        }
        return defines;
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
        // usage：COLOR_ATTACHMENT（render pass 必需）+ TRANSFER_DST（record_present 的 blit 目标，
        // VUID-vkCmdBlitImage-dstImage-00224，真机验证层抓出：#47 漏改此处，blit 写 COLOR_ATTACHMENT-only
        // 的交换链行为未定义 → smoke NMSE=1.83）。TRANSFER_DST 非规范保证项，先查 surface 能力。
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U) {
            info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        } else {
            std::fprintf(stderr, "[vk] 警告：交换链不支持 TRANSFER_DST 用途，blit 呈现将不可用\n");
        }
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
        // 双 render pass：附件格式/samples 相同 → 与同一组 framebuffer 兼容（loadOp 与
        // initialLayout 不参与兼容性判定），framebuffer 无需成对创建。
        //   clear 版：链未就绪的降级路径（清屏 + ImGui），图像 acquire 后从未写过 → initial=UNDEFINED
        //   load 版：正常路径（blit 已把成品画上，只叠 ImGui）→ initial=COLOR_ATTACHMENT_OPTIMAL
        auto make_pass = [this](VkImageLayout initial_layout, VkAttachmentLoadOp load_op,
                                VkRenderPass& out) {
            VkAttachmentDescription color{};
            color.format = swapchain_format_;
            color.samples = VK_SAMPLE_COUNT_1_BIT;
            color.loadOp = load_op;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            color.initialLayout = initial_layout;
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
            return vkCreateRenderPass(device_, &info, nullptr, &out) == VK_SUCCESS;
        };
        if (!make_pass(VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR, render_pass_)) {
            std::fprintf(stderr, "[vk] 创建 render pass（clear 版）失败\n");
            return false;
        }
        if (!make_pass(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                       render_pass_load_)) {
            std::fprintf(stderr, "[vk] 创建 render pass（load 版）失败\n");
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

    /// 创建渲染链（raymarch + 后处理，T1.6.1）。要求交换链与 render pass 已就绪。
    bool create_chain() {
        if (device_ == VK_NULL_HANDLE || swapchain_format_ == VK_FORMAT_UNDEFINED) {
            last_error_ = "设备未就绪，无法创建渲染链";
            return false;
        }
        // 请求分辨率优先（离屏 smoke 用）；0 = 跟随交换链尺寸（与 GL 的 ensure_targets 同语义）
        const std::uint32_t base_w =
            (render_width_ > 0) ? static_cast<std::uint32_t>(render_width_) : swapchain_extent_.width;
        const std::uint32_t base_h = (render_height_ > 0)
                                         ? static_cast<std::uint32_t>(render_height_)
                                         : swapchain_extent_.height;
        if (base_w == 0 || base_h == 0) {
            last_error_ = "请求分辨率与交换链尺寸均为 0，无法创建渲染链";
            return false;
        }
        const float res_scale = unpack_float(params_.flags[1]);
        const double scale = (res_scale > 0.0F) ? static_cast<double>(res_scale) : 1.0;

        ChainInitInfo info{};
        info.physical_device = physical_device_;
        info.device = device_;
        info.queue = queue_;
        info.queue_family = queue_family_;
        info.swapchain_format = swapchain_format_;  // 离屏 LDR 图同格式：blit 免转换
        info.output_extent = {base_w, base_h};      // 请求分辨率（与窗口尺寸解耦）
        info.internal_extent = {
            std::max(1U, static_cast<std::uint32_t>(std::lround(base_w * scale))),
            std::max(1U, static_cast<std::uint32_t>(std::lround(base_h * scale)))};
        info.shader_root = shader_root_.empty() ? std::string("shaders") : shader_root_;
        // 宏规整：恒含 EHE_VULKAN；设备不支持 fp64 时含 EHE_FP32_ONLY
        info.shader_defines = requested_defines_.empty() ? effective_shader_defines({})
                                                         : requested_defines_;
        info.lut_path = info.shader_root + "/blackbody_lut.f32";

        if (!chain_.init(info)) {
            last_error_ = chain_.last_error();
            std::fprintf(stderr, "[vk] 渲染链初始化失败：%s\n", last_error_.c_str());
            return false;
        }
        chain_res_scale_ = res_scale;
        std::printf("[vk] 渲染链输出 %ux%u（请求分辨率），交换链 %ux%u（窗口）\n", base_w, base_h,
                    swapchain_extent_.width, swapchain_extent_.height);
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

        // ImGui 1.92+ 新纹理系统：外部提供的池必须含 SAMPLED_IMAGE + SAMPLER 两类描述符
        //（IMGUI_IMPL_VULKAN_MINIMUM_*_POOL_SIZE，imgui_impl_vulkan.h:81）。
        // 旧代码只给 COMBINED_IMAGE_SAMPLER → 真机验证层报
        // WARNING-CoreValidation-AllocateDescriptorSets-WrongType（Iris Xe 2026-09-19）
        const VkDescriptorPoolSize pool_sizes[2] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},
            {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
        };

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // ImGui 后端要求池带 FREE_DESCRIPTOR_SET_BIT
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 64;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = pool_sizes;
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
        init.PipelineInfoMain.RenderPass = render_pass_load_;  // ImGui 画在 load 版（blit 之后）
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
        if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
            // begin 失败时本帧不提交：fence 重置已移至 end_frame 提交前，此处返回后
            // fence 仍 signaled，下一帧等待立即通过，不会挂死（实际几乎不发生）
            std::fprintf(stderr, "[vk] begin 命令缓冲失败\n");
            skip_frame_ = true;
            return;
        }

        const float res_scale = unpack_float(params_.flags[1]);
        const bool fxaa_enabled = (params_.flags[0] & kFlagFxaa) != 0U;
        const bool aces_enabled = (params_.flags[0] & kFlagAces) != 0U;

        // ---- 离屏链（raymarch → 分辨率变换 → FXAA → final）全部在交换链 pass 之前录制 ----
        // final 画进离屏 LDR 图（请求分辨率），经 blit 线性拉伸呈现到交换链（窗口尺寸）——
        // 渲染尺寸与窗口解耦，与 GL 的 final_fbo + glBlitFramebuffer 同构（§4.6）。
        bool chain_drew = false;
        if (chain_.ready()) {
            FinalParams final_params{};
            final_params.exposure_and_chroma[0] = params_.disk[2];  // exposure
            final_params.exposure_and_chroma[1] = params_.disk[3];  // chroma_ab
            final_params.flags[0] = aces_enabled ? 1.0F : 0.0F;
            chain_.upload_uniforms(current_frame_, params_, final_params, res_scale);
            chain_.record_offscreen(cmd, current_frame_, params_, res_scale, fxaa_enabled);
            chain_.record_final(cmd, current_frame_, aces_enabled);
            chain_.record_present(cmd, swapchain_images_[image_index], swapchain_extent_);
            chain_drew = true;
        }

        // 链就绪 → load 版（blit 成果保留，只叠 ImGui）；链未就绪 → clear 版（清屏 + ImGui）
        VkClearValue clear{};
        clear.color = {{0.02F, 0.02F, 0.03F, 1.0F}};

        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = chain_drew ? render_pass_load_ : render_pass_;
        pass.framebuffer = framebuffers_[image_index];
        pass.renderArea.offset = {0, 0};
        pass.renderArea.extent = swapchain_extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        // ImGui 随后经 ImGui_ImplVulkan_RenderDrawData 在同一 render pass 内叠加（end_frame）
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
        if (render_pass_load_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, render_pass_load_, nullptr);
            render_pass_load_ = VK_NULL_HANDLE;
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
    std::vector<std::string> requested_defines_;  ///< 规整后的精度宏（含 EHE_VULKAN / 可能的 EHE_FP32_ONLY）
    std::string last_error_;

    GLFWwindow* window_ = nullptr;
    bool vsync_ = true;
    bool shader_float64_supported_ = false;  ///< VK 设备是否支持 fp64（不支持则强制 fp32，V5.10 约定③）

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
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
    VkRenderPass render_pass_ = VK_NULL_HANDLE;       ///< clear 版（链未就绪的降级路径）
    VkRenderPass render_pass_load_ = VK_NULL_HANDLE;  ///< load 版（blit 成果保留，只叠 ImGui）
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    RaymarchChain chain_;            ///< raymarch + 后处理链（T1.6.1）
    float chain_res_scale_ = 1.0F;   ///< 建链时的 res_scale（变了要重建）
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
    int render_width_ = 0;   ///< 请求分辨率（RendererConfig；0 = 跟随窗口，与 GL 同语义）
    int render_height_ = 0;
};

}  // namespace

std::unique_ptr<IRenderer> create_renderer() { return std::make_unique<VKBackend>(); }

}  // namespace ehe::render::vk
