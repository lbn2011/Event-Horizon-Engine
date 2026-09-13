// EHE 测试入口（脚手架）
//
// 现状：doctest 于 T0.3 经 FetchContent 接入；本文件当前仅做最小自检并返回 0，
//       以保证 ehe_tests 目标在骨架阶段即可编译运行。
//
// 后续：T0.3 引入 doctest 后，本文件改为 DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN，
//       用例按 DESIGN §6.5 清单逐个加入（r 求根 a=0 退化、RK4 收敛阶、b_crit 捕获阈值…）。

#include <iostream>

#include "ehe/core/version.h"

int main() {
    std::cout << "[ehe_tests] scaffold, no cases yet (doctest 接入见 T0.3)\n";
    std::cout << "[ehe_tests] core version=" << ehe::core::version_string() << "\n";
    return 0;
}
