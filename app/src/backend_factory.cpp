#include "backend_factory.h"

#include <cstring>
#include <string>

#include "ehe/render/gl/gl_backend.h"
#include "ehe/render/vk/vk_backend.h"

namespace ehe::app {

std::unique_ptr<render::IRenderer> create_renderer(render::Backend backend) {
    switch (backend) {
        case render::Backend::Vulkan:
            return render::vk::create_renderer();
        case render::Backend::OpenGL:
            return render::gl::create_renderer();
    }
    return nullptr;
}

bool parse_backend(const char* text, render::Backend& out) {
    if (text == nullptr) {
        return false;
    }
    // 复用 core 的解析（Config 与 CLI 必须认同一套取值：gl/opengl、vk/vulkan）
    return core::parse_backend(std::string(text), out);
}

}  // namespace ehe::app
