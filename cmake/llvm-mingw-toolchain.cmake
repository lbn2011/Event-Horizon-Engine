# ============================================================================
# EHE 工具链文件：LLVM-MinGW（Clang + lld）
# 背景：开发机无 MSVC，C++20 工程需走 GNU 风格工具链（DESIGN §2.2 / §9.1）
# 版本：llvm-mingw-20260908-ucrt-x86_64
#       （升级版本需走 DESIGN §13 变更流程，并同步 CI 上的同一压缩包）
# 用法：cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-toolchain.cmake -B build
# ============================================================================

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 解压根目录。默认按 DESIGN §9.1 约定；其他机器可用
#   -DEHE_LLVM_MINGW_ROOT=D:/path/to/llvm-mingw
# 覆盖，避免把绝对路径写死。
set(EHE_LLVM_MINGW_ROOT "C:/tools/llvm-mingw" CACHE PATH "llvm-mingw 解压根目录")

set(_ehe_bin "${EHE_LLVM_MINGW_ROOT}/bin")

set(CMAKE_C_COMPILER   "${_ehe_bin}/clang.exe"        CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${_ehe_bin}/clang++.exe"      CACHE FILEPATH "")
set(CMAKE_AR           "${_ehe_bin}/llvm-ar.exe"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_ehe_bin}/llvm-ranlib.exe"  CACHE FILEPATH "")
set(CMAKE_STRIP        "${_ehe_bin}/llvm-strip.exe"   CACHE FILEPATH "")
set(CMAKE_RC_COMPILER  "${_ehe_bin}/windres.exe"      CACHE FILEPATH "")

# llvm-mingw 的 clang 默认目标即 x86_64-w64-windows-gnu（ucrt 包；cmake -dumpmachine
# 实测输出该三元组，与 x86_64-w64-mingw32 等价）。
set(CMAKE_C_COMPILER_TARGET   x86_64-w64-mingw32)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-mingw32)

# 查找规则：构建期程序用宿主工具，库与头文件只在工具链根内查找
set(CMAKE_FIND_ROOT_PATH "${EHE_LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# ----------------------------------------------------------------------------
# 运行时链接方式
# llvm-mingw 默认动态链接 libc++/libunwind，直接运行产物会报 0xC0000135（缺 DLL），
# 需要把 ${EHE_LLVM_MINGW_ROOT}/bin 放进 PATH。为便于把产物拷到目标机直接运行
# （免安装部署），默认改为静态链接运行时。
# 关闭方式：-DEHE_STATIC_RUNTIME=OFF
# ----------------------------------------------------------------------------
option(EHE_STATIC_RUNTIME "静态链接 C++ 运行时（libc++/libunwind/winpthread），产物免 DLL 依赖" ON)
if(EHE_STATIC_RUNTIME)
  set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
endif()

# 说明：
# 1) 语言标准（C++20）与警告集在顶层 CMakeLists.txt 设置，不在此文件。
# 2) Vulkan 加载器经 volk 运行时动态加载 vulkan-1.dll（系统驱动自带），不受静态链接影响。
