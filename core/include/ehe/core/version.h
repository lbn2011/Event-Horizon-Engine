#pragma once

// EHE core —— 版本与构建信息
// 说明：版本与构建信息查询。core 的其余模块（Config / Camera / units / metric /
//       integrator / blackbody / golden / sha256）已按 DESIGN §5.1 落地。

namespace ehe::core {

/// 引擎版本字符串，形如 "0.1.0"（来自 CMake project VERSION）
const char* version_string();

/// 构建期编译开关摘要，形如 "fp64_metric=1"（用于运行期日志与 issue 报告）
const char* build_flags();

}  // namespace ehe::core
