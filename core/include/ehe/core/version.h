#pragma once

// EHE core —— 版本与构建信息
// 说明：本文件属 M0/T0.2 脚手架，用于验证「core 零图形依赖 + 可编译」链路。
//       Config / Camera / BlackHoleParams / ReferenceIntegrator / BlackbodyLut 等
//       真实模块在 T1.1 / T1.2 落地（DESIGN §5.1）。

namespace ehe::core {

/// 引擎版本字符串，形如 "0.1.0"（来自 CMake project VERSION）
const char* version_string();

/// 构建期编译开关摘要，形如 "fp64_metric=1"（用于运行期日志与 issue 报告）
const char* build_flags();

}  // namespace ehe::core
