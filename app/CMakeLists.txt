cmake_minimum_required(VERSION 3.21)

project(Nextboard VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

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

find_package(Qt6 REQUIRED COMPONENTS Widgets Quick QuickControls2 QuickWidgets)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

add_executable(nextboard WIN32
    src/main.cpp
    src/host/single_instance.cpp
    src/host/auto_start.cpp
    src/host/tray.cpp
    src/host/hotkeys.cpp
    src/host/core_bridge.cpp
    src/panel/panel_window.cpp
    src/panel/qml_bootstrap.cpp
    src/panel/core_facade.cpp
    src/panel/clip_entry_model.cpp
    src/capture/capture_session.cpp
    src/editor/editor_window.cpp
    src/pin/pin_window.cpp
    src/translate/translate_window.cpp
)

# ---- 主面板 QML(qt_add_qml_module → qrc,无运行期文件依赖) ----
# qt_standard_project_setup 默认 RESOURCE_PREFIX /qt/qml;
# QML 内 import Nextboard.panel 即可用本模块类型(含 Theme 单例)。
set_source_files_properties(panel/qml/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)
qt_add_qml_module(nextboard
    URI Nextboard.panel
    VERSION 1.0
    QML_FILES
        panel/qml/Main.qml
        panel/qml/EntryCard.qml
        panel/qml/KeyCap.qml
        panel/qml/ReadableComboBox.qml
        panel/qml/ThemedButton.qml
        panel/qml/AppDrawer.qml
        panel/qml/SettingsDrawer.qml
        panel/qml/TranslateWindow.qml
        panel/qml/Theme.qml
)

target_include_directories(nextboard PRIVATE
    src
)

target_link_libraries(nextboard PRIVATE
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickWidgets
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
    COMMENT "Deploy nbcore.dll"
)

# QML 模块资源的 rcc 输出随 nextboard 目标自动重建;Sciter 时代的 panel HTML
# 目录拷贝已移除(qt_add_qml_module 直接打进 qrc)。

# Qt DLL 依赖由 windeployqt 处理(发布脚本负责;开发期用 PATH 上的 Qt bin 运行)
