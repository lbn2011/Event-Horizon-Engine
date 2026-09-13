#include "ehe/render/gl/gl_backend.h"

namespace ehe::render::gl {

// 脚手架实现：仅返回后端标识，用于验证「两后端同时链接进同一 exe」的骨架链路。
// 真实内容（GLFW 窗口、GL 4.5 context、glad 加载、FP16 FBO、后处理链）见 T0.4 / T1.3。
Backend backend() { return Backend::OpenGL; }

}  // namespace ehe::render::gl
