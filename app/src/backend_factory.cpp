#include "backend_factory.h"

#include <cstring>

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
    if (std::strcmp(text, "gl") == 0 || std::strcmp(text, "opengl") == 0) {
        out = render::Backend::OpenGL;
        return true;
    }
    if (std::strcmp(text, "vk") == 0 || std::strcmp(text, "vulkan") == 0) {
        out = render::Backend::Vulkan;
        return true;
    }
    return false;
}

}  // namespace ehe::app
