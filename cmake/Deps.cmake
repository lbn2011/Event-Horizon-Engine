# ============================================================================
# EHE 第三方依赖声明与版本 pin（DESIGN §9）
# 全部源码构建，不使用任何包管理器；版本集中在本文件，升级即改动此处。
#
# 网络受限环境（本机 git 直连 github.com 被代理阻断时）可启用仓库级镜像改写：
#   git config url."https://v4.gh-proxy.org/https://github.com/".insteadOf "https://github.com/"
# 该改写只写入本地 .git/config，不随仓库分发（见 CONTRIBUTING「网络受限环境」）。
#
# ⚠ 重要：FetchContent 的克隆发生在 build/_deps/*-subbuild 的独立 git 上下文中，
#   仓库级 .git/config 对它**不生效**。配置时必须用环境变量注入 git 配置（对所有子进程生效）：
#     GIT_CONFIG_COUNT=2
#     GIT_CONFIG_KEY_0=url.https://v4.gh-proxy.org/https://github.com/.insteadOf
#     GIT_CONFIG_VALUE_0=https://github.com/
#     GIT_CONFIG_KEY_1=http.sslBackend
#     GIT_CONFIG_VALUE_1=openssl
#   另外可用 -DFETCHCONTENT_BASE_DIR=<持久目录> 让依赖跨多次 build 复用，避免重复拉取。
# ============================================================================

include(FetchContent)

# 统一关闭 FetchContent 的更新检查，避免每次配置都访问网络
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# 依赖均按发行 tag pin；SDK 系组件（Vulkan/glslang）统一使用同一 SDK 版本号。
set(EHE_VULKAN_SDK_TAG "vulkan-sdk-1.4.357.0" CACHE STRING "Vulkan-Headers/volk/glslang 统一 pin 的 SDK tag")
set(EHE_STB_COMMIT    "2c980bb59875b0d32144a71867fbdebb2f77cd20" CACHE STRING "stb 无 tag，pin 提交")

# ---------------------------------------------------------------------------
# 统一禁用子模块更新（GIT_SUBMODULES ""）
# 原因有二：
#   1) 本机 Git 的 shell 辅助脚本不可用（git-submodule 依赖 basename/sed 等缺失），
#      任何 submodule 操作都会失败，导致 FetchContent 配置阶段中断；
#   2) 当前依赖集实际不需要子模块：glslang（vulkan-sdk-1.4.357.0）已自带
#      SPIRV/spirv.hpp11，且本项目 ENABLE_OPT=OFF 不需 SPIRV-Tools。
# 若将来引入确需子模块的依赖，改为按需指定 GIT_SUBMODULES 并先修复 Git shell 环境。
# ---------------------------------------------------------------------------
set(EHE_NO_SUBMODULES "" CACHE STRING "传给 GIT_SUBMODULES 的值（空串=不更新任何子模块）")

# ---------------------------------------------------------------------------
# 1. GLFW —— 窗口与输入（DESIGN §9）
# ---------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.5.1
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(glfw)

# ---------------------------------------------------------------------------
# 2. GLM —— 数学（header-only，CPU 侧）
# ---------------------------------------------------------------------------
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.3
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(glm)

# ---------------------------------------------------------------------------
# 3. Vulkan-Headers + volk —— 免 SDK 方案（运行时动态加载 vulkan-1.dll）
#    注意：必须早于 ImGui 声明——imgui_impl_vulkan 需要 vulkan/vulkan.h
# ---------------------------------------------------------------------------
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        ${EHE_VULKAN_SDK_TAG}
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(vulkan_headers)

FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG        ${EHE_VULKAN_SDK_TAG}
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(volk)

if(TARGET volk)
  target_link_libraries(volk PUBLIC Vulkan::Headers)
endif()

# ---------------------------------------------------------------------------
# 4. ImGui —— 面板（无 CMake，自行组织 target）
#    需要三个后端文件：glfw + opengl3 + vulkan（DESIGN §9 注记）
#    Vulkan 后端走 volk（无 vulkan-1.lib，故启用 IMGUI_IMPL_VULKAN_USE_VOLK）
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.9
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(imgui)

add_library(ehe_imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(ehe_imgui SYSTEM PUBLIC
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends"
)
target_compile_definitions(ehe_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(ehe_imgui PUBLIC glfw volk Vulkan::Headers)
target_link_libraries(ehe_imgui PRIVATE ehe_warnings)


# ---------------------------------------------------------------------------
# 5. glslang —— GLSL → SPIR-V（库形式链入 render-vk，启动时编译，DESIGN §5.2）
#    选项尽量精简：关 HLSL/优化器/测试/命令行工具，只保留核心编译库
# ---------------------------------------------------------------------------
set(ENABLE_OPT           OFF CACHE BOOL "" FORCE)   # 不需 SPIRV-Tools 优化器
set(ENABLE_HLSL          OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS        OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)  # 运行时用库编译，不需要 glslang/glslangValidator 工具
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
  GIT_TAG        ${EHE_VULKAN_SDK_TAG}
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(glslang)

# ---------------------------------------------------------------------------
# 6. doctest —— 单元测试（单头文件，DESIGN §6 L2）
# ---------------------------------------------------------------------------
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.5.3
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(doctest)

# ---------------------------------------------------------------------------
# 7. nlohmann/json —— Config 序列化（header-only，DESIGN §8.1）
# ---------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# 8. miniaudio —— 程序化音效（单头文件，DESIGN §5.7）
# ---------------------------------------------------------------------------
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG        0.11.25
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(miniaudio)

add_library(ehe_miniaudio INTERFACE)
target_include_directories(ehe_miniaudio SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# 9. stb —— PNG 写出（stb_image_write，smoke 预览用，DESIGN §6.3）
#    无发行 tag，pin 提交
# ---------------------------------------------------------------------------
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        ${EHE_STB_COMMIT}
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
  # 注意：按提交 pin 时必须全量克隆（shallow 克隆无法定位任意提交）
)
FetchContent_MakeAvailable(stb)

add_library(ehe_stb INTERFACE)
target_include_directories(ehe_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# 10. FidelityFX FSR1 —— 空间升频（EASU + RCAS，DESIGN §4.6）
# ---------------------------------------------------------------------------
FetchContent_Declare(fsr1
  GIT_REPOSITORY https://github.com/GPUOpen-Effects/FidelityFX-FSR.git
  GIT_TAG        v1.0.2
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES "${EHE_NO_SUBMODULES}"
)
FetchContent_MakeAvailable(fsr1)

add_library(ehe_fsr1 INTERFACE)
target_include_directories(ehe_fsr1 SYSTEM INTERFACE "${fsr1_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# 11. glad —— GL 4.5 核心加载器（生成文件入库，见 third_party/glad）
#    生成方式（复现用）：glad --profile core --api gl=4.5 --generator c
# ---------------------------------------------------------------------------
set(EHE_GLAD_DIR "${CMAKE_SOURCE_DIR}/third_party/glad")
if(NOT EXISTS "${EHE_GLAD_DIR}/src/glad.c")
  message(FATAL_ERROR "缺少 third_party/glad/src/glad.c；见本文件注释中的生成命令")
endif()
add_library(ehe_glad STATIC "${EHE_GLAD_DIR}/src/glad.c")
target_include_directories(ehe_glad SYSTEM PUBLIC "${EHE_GLAD_DIR}/include")
target_link_libraries(ehe_glad PRIVATE ehe_warnings)
# 第三方生成代码不参与本项目的告警策略（不修改上游产物，整体抑制告警）
target_compile_options(ehe_glad PRIVATE -w)

# ---------------------------------------------------------------------------
# 依赖摘要
# ---------------------------------------------------------------------------
message(STATUS "EHE 依赖: glfw=${glfw_VERSION} glm=1.0.3 imgui=v1.92.9 "
               "vulkan=${EHE_VULKAN_SDK_TAG} glslang/volk=同上 doctest=v2.5.3 "
               "json=v3.12.0 miniaudio=0.11.25 fsr1=v1.0.2 glad=生成入库")
