cmake_minimum_required(VERSION 3.21)

project(Nextboard VERSION 0.2.0 LANGUAGES CXX)

# RC:把应用图标编进 exe 的 PE 资源(资源管理器/任务栏显示的那个图标)
if(WIN32)
    enable_language(RC)
endif()

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
    src/host/memory_probe.cpp
    src/panel/panel_window.cpp
    src/panel/qml_bootstrap.cpp
    src/panel/core_facade.cpp
    src/panel/clip_entry_model.cpp
    src/capture/capture_session.cpp
    src/editor/editor_window.cpp
    src/pin/pin_window.cpp
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
        panel/qml/AppMark.qml
        panel/qml/HeaderIcon.qml
        panel/qml/ReadableComboBox.qml
        panel/qml/ThemedButton.qml
        panel/qml/ThemedMenu.qml
        panel/qml/ThemedMenuItem.qml
        panel/qml/ThemedMenuSeparator.qml
        panel/qml/AppDrawer.qml
        panel/qml/SettingsDrawer.qml
        panel/qml/TranslateView.qml
        panel/qml/Theme.qml
)

target_include_directories(nextboard PRIVATE
    src
)

# ---- 应用图标 ----------------------------------------------------------
# 设计稿来自 app/panel/qml/AppMark.qml(圆角板 + 三条横线),由 tools/make_icon.py
# 生成多尺寸 ico(16/20/24/32/40/48/64/128/256);改设计后重跑该脚本即可。
#
# 两处用途,缺一不可:
#  1) 运行期窗口/托盘图标 → 打进 Qt 资源,按路径 ":/resources/nextboard.ico" 加载;
#  2) 资源管理器/任务栏显示的 exe 图标 → 只能由 rc.exe 编入 PE 资源。
set(NB_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/nextboard.ico")
# ⚠️ FILES 必须用相对路径:qt_add_resources 拒绝绝对路径(要求另设 QT_RESOURCE_ALIAS),
# 相对路径生成的资源地址即 ":/resources/nextboard.ico"(与 main.cpp 里一致)。
qt_add_resources(nextboard "app_icon"
    PREFIX "/"
    FILES "resources/nextboard.ico"
)
if(WIN32)
    # 路径用正斜杠即可,rc.exe 接受;写成构建目录避免污染源码树
    set(NB_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/nextboard_app_icon.rc")
    file(WRITE "${NB_ICON_RC}" "IDI_ICON1 ICON \"${NB_ICON}\"\n")
    target_sources(nextboard PRIVATE "${NB_ICON_RC}")
endif()

target_link_libraries(nextboard PRIVATE
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickWidgets
    dwmapi.lib
    # GetProcessMemoryInfo 的兜底依赖(PSAPI_VERSION>=2 时由 kernel32 提供,写上无副作用)
    psapi.lib
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

# 版本号:注入编译期宏,供设置页「关于」显示 —— 与 project(... VERSION) 单一来源,
# 避免版本号在 CMake 与 C++ 里各写一份而对不上。
target_compile_definitions(nextboard PRIVATE NB_APP_VERSION="${PROJECT_VERSION}")

# ---- 部署:运行所需 DLL 拷贝到输出目录 ----
add_custom_command(TARGET nextboard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NBCORE_DIR}/nbcore.dll" "$<TARGET_FILE_DIR:nextboard>"
    COMMENT "Deploy nbcore.dll"
)

# QML 模块资源的 rcc 输出随 nextboard 目标自动重建;Sciter 时代的 panel HTML
# 目录拷贝已移除(qt_add_qml_module 直接打进 qrc)。

# Qt DLL 依赖由 windeployqt 处理(发布脚本负责;开发期用 PATH 上的 Qt bin 运行)
