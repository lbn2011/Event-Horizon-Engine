#include "ehe/core/console.h"

#if defined(_WIN32)
// 只用到控制台代码页设置，故不整体引入 windows.h（避免污染大量宏）
extern "C" {
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int code_page);
__declspec(dllimport) int __stdcall SetConsoleCP(unsigned int code_page);
}
#define EHE_CP_UTF8 65001U
#endif

namespace ehe::core {

void enable_utf8_console() {
#if defined(_WIN32)
    // 输出代码页：程序用 printf/fprintf 写中文（UTF-8 字节）时由控制台按 UTF-8 解释
    SetConsoleOutputCP(EHE_CP_UTF8);
    // 输入代码页：读取非 ASCII 输入（将来如有）也不会乱
    SetConsoleCP(EHE_CP_UTF8);
#endif
}

}  // namespace ehe::core
