cmake_minimum_required(VERSION 3.21)

project(Nextboard VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ---- 工具链定位(Qt 安装自带 CMake/Ninja;命令行构建请先运行 vcvars64) ----
# Qt 路径:本机 D:\Qt\6.9.0\msvc2022_64;可用 -DQt6_DIR=... 覆盖
if(NOT DEFINED Qt6_DIR)
    set(Qt6_DIR "D:/Qt/6.9.0/msvc2022_64/lib/cmake/Qt6")
endif()

# ---- nbcore(Rust DLL)导入库 ----
set(NBCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/target/release" CACHE PATH "nbcore release output")
set(NBCORE_LIB "${NBCORE_DIR}/nbcore.dll.lib")
if(NOT EXISTS "${NBCORE_LIB}")
    message(WARNING "nbcore.dll.lib not found at ${NBCORE_LIB} — build core first: cd core && cargo build --release")
endif()

# ---- Sciter SDK ----
set(SCITER_SDK "D:/4Lib/sciter-js-sdk-main" CACHE PATH "Sciter SDK root")
set(SCITER_INCLUDE "${SCITER_SDK}/include")
set(SCITER_DLL_DIR "${SCITER_SDK}/bin/windows/x64")
# EULA:应用须在 About 界面注明 "uses Sciter Engine (https://sciter.com), copyright Terra Informatica Software, Inc."

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(nextboard WIN32
    src/main.cpp
    src/host/single_instance.cpp
    src/host/tray.cpp
    src/host/hotkeys.cpp
    src/host/core_bridge.cpp
    src/panel/panel_window.cpp
    src/panel/sciter_view.cpp
    src/panel/bridge.cpp
    src/capture/capture_session.cpp
    src/pin/pin_window.cpp
)

target_include_directories(nextboard PRIVATE
    src
    ${SCITER_INCLUDE}
)

target_link_libraries(nextboard PRIVATE
    Qt6::Widgets
    dwmapi.lib
)
# nbcore 走纯显式加载(LoadLibrary + GetProcAddress,见 core_bridge.cpp):
# 隐式链接 nbcore.dll.lib 时,Rust cdylib 的 CRT/线程局部状态与 Qt 静态初始化
# 顺序存在冲突(实测 nb_init 内 0xC0000005),显式加载则一切正常。

# MSVC /utf-8:源码为 UTF-8(含中文字面量)
if(MSVC)
    target_compile_options(nextboard PRIVATE /utf-8 /W3)
    # per-monitor DPI(截图/贴图/多屏缩放的前提);manifest 由 mt.exe 在链接期嵌入
    set_target_properties(nextboard PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()

# ---- 部署:运行所需 DLL 拷贝到输出目录 ----
add_custom_command(TARGET nextboard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NBCORE_DIR}/nbcore.dll" "$<TARGET_FILE_DIR:nextboard>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCITER_DLL_DIR}/sciter.dll" "$<TARGET_FILE_DIR:nextboard>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/panel" "$<TARGET_FILE_DIR:nextboard>/panel"
    COMMENT "Deploy nbcore.dll + sciter.dll + panel html"
)

# ---- panel HTML 作为构建依赖:修改 main.htm 后必须重新拷贝+触发重链 ----
# (否则 POST_BUILD 不执行,build/panel 里是旧 UI,排障时被过期帧误导)
set(PANEL_HTML "${CMAKE_CURRENT_SOURCE_DIR}/panel/main.htm")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/panel/main.htm"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PANEL_HTML}" "${CMAKE_CURRENT_BINARY_DIR}/panel/main.htm"
    DEPENDS "${PANEL_HTML}"
    COMMENT "Deploy panel/main.htm")
add_custom_target(panel_assets ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/panel/main.htm")
add_dependencies(nextboard panel_assets)

# Qt DLL 依赖由 windeployqt 处理(发布脚本负责;开发期用 PATH 上的 Qt bin 运行)
