#pragma once

// EHE app —— 能力探测（--caps）
//
// 目的：把「目标机能不能跑、该用哪种精度、各后端能到多少帧」一次性量化成可回传的报告。
//   设计约束（DESIGN §6 L4）：GPU 渲染不进 CI，所以 GPU 相关的结论必须**在目标机上采集数据**，
//   再由开发侧核对——本探针就是采集器，配合 tools/run_target_tests.ps1 生成整份报告。

#include <string>

namespace ehe::app {

struct CapOptions {
    std::string config_path = "tests/golden/params_tiny.json";
    int probe_frames = 3;      ///< 每个精度模式测时的帧数
    bool time_mixed = false;   ///< 是否对 mixed(fp64) 模式实测帧耗时（默认关闭，避免软件模拟 fp64 挂死驱动）
    int probe_size = 64;       ///< 测时用的渲染尺寸（避开受限 GPU 的大尺寸回读问题）
};

/// 输出 GL / Vulkan 能力清单 + 各精度模式的可编译性与帧耗时；返回 0 表示无致命问题
int run_caps(const CapOptions& options);

}  // namespace ehe::app
