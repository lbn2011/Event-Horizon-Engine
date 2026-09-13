#include "ehe/render/backend.h"

namespace ehe::render {

const char* backend_name(Backend backend) {
    switch (backend) {
        case Backend::Vulkan: return "vulkan";
        case Backend::OpenGL: return "opengl";
    }
    return "unknown";
}

int compiled_backend_count() {
    // 两个后端均为静态链接库（DESIGN §5.2「双后端静态链接单一 exe」），
    // 故只要两个 target 都参与链接，数量恒为 2；运行期重建实例完成切换。
    return 2;
}

}  // namespace ehe::render
