#pragma once

// EHE core —— 控制台编码（Windows）
//
// 为什么需要：本项目的源码与字符串字面量都是 **UTF-8**（clang 编译，见 .clang-format / 编译选项），
//   程序输出到控制台的字节就是 UTF-8。而中文 Windows 的控制台默认代码页是 **GBK(936)**，
//   于是所有中文输出都变成乱码（"输出全是乱码"即由此而来）。
//
// 修法：程序启动时**自己**把控制台输出代码页设为 UTF-8（`SetConsoleOutputCP(CP_UTF8)`），
//   而不是指望用户手动 `chcp 65001`——这样无论从资源管理器双击、还是被脚本调起，输出都正常。
//
// 说明：
//   - 仅 Windows 有实际动作；其它平台为空实现（便于将来移植）。
//   - 若控制台**字体**不支持中文，仍会显示为方块/问号——那是字体问题，与本函数无关（换 Consolas→等距更纱等）。

namespace ehe::core {

/// 把当前控制台切到 UTF-8（Windows）；失败或非 Windows 时无副作用
void enable_utf8_console();

}  // namespace ehe::core
