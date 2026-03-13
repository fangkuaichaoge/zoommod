cmake_minimum_required(VERSION 3.18)
project(ZoomMod LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译优化与链接配置（适配安卓NDK）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

# 屏蔽CMake弃用警告，适配GitHub Actions高版本CMake
cmake_policy(SET CMP0169 OLD)
cmake_policy(SET CMP0135 NEW)

include(FetchContent)

# ===================== 1. fmt库（固定稳定版） =====================
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(fmt)

# ===================== 2. preloader-android（固定稳定版） =====================
FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.1.14
    GIT_SHALLOW ON
)
FetchContent_GetProperties(preloader_android)
if(NOT preloader_android_POPULATED)
    FetchContent_Populate(preloader_android)
    
    # 修复preloader的format依赖，替换为fmt库
    file(READ "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" LOGGER_CONTENT)
    string(REPLACE "#include <format>" "#include <fmt/format.h>" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::vformat" "fmt::vformat" LOGGER_CONTENT "${LOGGER_CONTENT}")
    string(REPLACE "std::make_format_args" "fmt::make_format_args" LOGGER_CONTENT "${LOGGER_CONTENT}")
    file(WRITE "${preloader_android_SOURCE_DIR}/src/pl/Logger.h" "${LOGGER_CONTENT}")
    
    add_subdirectory(${preloader_android_SOURCE_DIR} ${preloader_android_BINARY_DIR})
endif()
target_link_libraries(preloader PUBLIC fmt::fmt)

# ===================== 项目头文件与源文件配置 =====================
target_include_directories(preloader PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ImGui
    ${CMAKE_SOURCE_DIR}/src/ImGui/backends
)

set(SOURCES
    src/main.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_opengl3.cpp
    src/ImGui/backends/imgui_impl_android.cpp
)

# 生成最终的so库
add_library(ZoomMod SHARED ${SOURCES})

# 链接所有依赖库
target_link_libraries(ZoomMod
    preloader
    fmt::fmt
    log
    android
    EGL
    GLESv3
    GLESv2
)

# ===================== 输出目录配置（修复GitHub Actions产物路径问题） =====================

# 强制设置所有构建类型的输出目录到统一的 output 文件夹
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/output)

set_target_properties(ZoomMod PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
    LIBRARY_OUTPUT_NAME ZoomMod
)

# 构建完成后自动复制到 output 目录（保险措施）
add_custom_command(TARGET ZoomMod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/output
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ZoomMod> ${CMAKE_BINARY_DIR}/output/
    COMMENT "Copying library to output directory"
)
