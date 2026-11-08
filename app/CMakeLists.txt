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

# ---- nbcore(Rust 核心库)产物存在性检查(仅警告;显式加载在运行期解析)----
set(NBCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/target/release" CACHE PATH "nbcore release output")
if(WIN32)
    set(NBCORE_LIB "${NBCORE_DIR}/nbcore.dll.lib")
else()
    set(NBCORE_LIB "${NBCORE_DIR}/libnbcore.so")
endif()
if(NOT EXISTS "${NBCORE_LIB}")
    message(WARNING "nbcore library not found at ${NBCORE_LIB} — build core first: cd core && cargo build --release")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Quick QuickControls2 QuickWidgets)

# REQUIRES 6.5(QML 侧已做跨版本兼容,见 TranslateView.qml 2026-11-07 的
# ContextMenu.menu → TapHandler 改写 —— 该附加属性是 Qt 6.9-only,曾致 6.8 下
# 整个 Main.qml 加载失败,docs/29 §9.1 K3)。上调下限前先把 QML 回到 6.5 口径。
qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

add_executable(nextboard WIN32
    src/main.cpp
    src/app_font.cpp
    src/host/single_instance.cpp
    src/host/app_paths.cpp
    src/host/log_rotate.cpp
    src/host/log_rotate_flow.cpp
    src/host/auto_start.cpp
    src/host/auto_paste.cpp
    src/host/image_save.cpp
    src/host/tray.cpp
    src/host/hotkeys.cpp
    src/host/quick_commands.cpp
    src/host/quick_commands_flow.cpp
    src/host/acceptance.cpp
    src/host/note_flow.cpp
    src/host/capture_toolbar_flow.cpp
    src/host/smart_capture_flow.cpp
    src/host/hotkey_lifecycle_flow.cpp
    src/host/editor_flow.cpp
    src/host/stitch_flow.cpp
    src/host/stitch_live_flow.cpp
    src/host/text_edit_flow.cpp
    src/host/core_bridge.cpp
    src/host/memory_probe.cpp
    src/panel/panel_window.cpp
    src/panel/qml_bootstrap.cpp
    src/panel/core_facade.cpp
    src/panel/clip_entry_model.cpp
    src/ui/ocr_result_panel.cpp
    src/capture/capture_session.cpp
    src/capture/overlay_geometry.cpp
    src/capture/smart_capture.cpp
    src/capture/stitch_hud.cpp
    src/editor/annotation_engine.cpp
    src/editor/annotation_toolbar.cpp
    src/editor/tool_icons.cpp
    src/editor/editor_window.cpp
    src/pin/pin_window.cpp
    src/note/note_window.cpp
    src/note/note_registry.cpp
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
        panel/qml/QuickRail.qml
        panel/qml/ReadableComboBox.qml
        panel/qml/ReadableCheckBox.qml
        panel/qml/ReadableSpinBox.qml
        panel/qml/ReadableTextField.qml
        panel/qml/TextEditMenu.qml
        panel/qml/ThemedButton.qml
        panel/qml/ThemedMenu.qml
        panel/qml/ThemedMenuItem.qml
        panel/qml/ThemedMenuSeparator.qml
        panel/qml/HotkeyField.qml
        panel/qml/ColorSwatchGrid.qml
        panel/qml/SectionDivider.qml
        panel/qml/SectionTitle.qml
        panel/qml/FieldLabel.qml
        panel/qml/HintLabel.qml
        panel/qml/SettingsView.qml
        panel/qml/SettingsPageGeneral.qml
        panel/qml/SettingsPageCapture.qml
        panel/qml/SettingsPagePin.qml
        panel/qml/SettingsPageHotkeys.qml
        panel/qml/SettingsPageQuick.qml
        panel/qml/SettingsPageOcr.qml
        panel/qml/SettingsPageTranslate.qml
        panel/qml/SettingsPageNetwork.qml
        panel/qml/SettingsPageData.qml
        panel/qml/SettingsPageAbout.qml
        panel/qml/TranslateView.qml
        panel/qml/Theme.qml
        panel/qml/TodoDatePicker.qml
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
)
# Windows 系统库(Linux 无对应物,各能力在源码层按平台拆分,docs/29 §5):
#   dwmapi               → 窗口圆角(window_effects)
#   ole32/oleaut32/uiautomationcore → 智能框选元素级探测(UIA;窗口级 S1 不依赖,
#                          但同一文件里编译,链接期必须解析 COM 符号)
#   psapi                → GetProcessMemoryInfo 兜底依赖(PSAPI_VERSION>=2 时由
#                          kernel32 提供,写上无副作用)
if(WIN32)
    target_link_libraries(nextboard PRIVATE
        dwmapi.lib
        ole32.lib
        oleaut32.lib
        uiautomationcore.lib
        psapi.lib
    )
else()
    # Linux(M3):全局热键 XGrabKey(Xlib)+ 自动粘贴 XTest(libXtst 扩展)。
    find_package(X11 REQUIRED)
    target_link_libraries(nextboard PRIVATE X11::X11)
    if(X11_Xtst_FOUND)
        target_link_libraries(nextboard PRIVATE X11::Xtst)
        target_compile_definitions(nextboard PRIVATE NB_HAVE_XTEST)
    else()
        message(WARNING "libXtst 未找到:自动粘贴功能将不可用")
    endif()
endif()
# nbcore 走纯显式加载(Windows LoadLibrary / Linux dlopen,见 core_bridge.cpp):
# 隐式链接 nbcore 导入库时,Rust cdylib 的 CRT/线程局部状态与 Qt 静态初始化
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

# Linux(M0b):libnbcore.so 部署到可执行文件同目录,dlopen 以运行目录解析 ——
# $ORIGIN RPATH 兜住"从任意工作目录启动"的场景(dlopen 的依赖搜索不含 CWD)。
# 另:Qt 运行库也要能找到 —— aqtinstall 自装 Qt 不在 ld 搜索路径,不写 RPATH
# 的话运行期会解析到系统发行版 Qt(版本不一致 → QML 模块错位,实测 ContextMenu
# 附加类型在 6.8 缺失致 TranslateView 加载失败;docs/29 §9.1 K3)。
if(UNIX AND NOT APPLE)
    get_filename_component(NB_QT_PREFIX "${Qt6Core_DIR}/../../.." ABSOLUTE)
    set_target_properties(nextboard PROPERTIES
        BUILD_RPATH "\$ORIGIN;${NB_QT_PREFIX}/lib")
endif()

# 更新检查地址(docs/07-auto-update.md §5):S6.4 v1 的**唯一配置点**,留空 = 功能禁用
# (设置页「检查更新」可见但禁用并说明原因)。发布渠道定了填 GitHub releases/latest 或
# 自建 manifest 的 https 地址,重编即生效 —— 不做成用户设置项,免得多个入口对不上。
set(NB_UPDATE_CHECK_URL "" CACHE STRING "更新检查地址(留空 = 检查更新功能禁用)")
target_compile_definitions(nextboard PRIVATE NB_UPDATE_CHECK_URL="${NB_UPDATE_CHECK_URL}")

# ---- 部署:运行所需核心库拷贝到输出目录(平台各取其库)----
# ⚠️ 必须用 add_custom_target(ALL) 而非 POST_BUILD(2026-11-07 实测踩到,
# 与下方 OCR 资产同一条经验):POST_BUILD 只在目标**重链接**时执行 ——
# 只改 Rust 侧(core/libnbcore)不触发 C++ 重链接,旧核心库会一直留在输出目录,
# 表现为"改了 Rust 代码但行为纹丝不动"。
if(WIN32)
    add_custom_target(nb_deploy_core ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NBCORE_DIR}/nbcore.dll" "$<TARGET_FILE_DIR:nextboard>"
        COMMENT "Deploy nbcore.dll"
        VERBATIM
    )
else()
    # Linux(M0b):libnbcore.so 由 M0a 的 cargo build --release 产出;
    # 运行期 core_bridge 以 dlopen("libnbcore.so") 加载,须与可执行文件同目录
    # ($ORIGIN RPATH 见下方 target 的 BUILD_RPATH 设置)。
    add_custom_target(nb_deploy_core ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NBCORE_DIR}/libnbcore.so" "$<TARGET_FILE_DIR:nextboard>"
        COMMENT "Deploy libnbcore.so"
        VERBATIM
    )
endif()
add_dependencies(nb_deploy_core nextboard)

# ---- OCR 运行期资产(RapidOCR 引擎)---------------------------------------------
# 三样必须与 nextboard.exe / nbcore.dll **同目录**,缺一不可:
#   1) onnxruntime.dll —— nbcore 内 ocr/rapid.rs 以 exe 同级路径显式加载,
#      **不走 PATH**(用户机 system32 可能有不兼容的旧版本,实测见过 1.17.1);
#   2) models/ocr/*    —— det/cls: PP-OCRv4 mobile;rec: PP-OCRv6 small;加 keys 兜底;
#   3) 二者版本配对:ORT 需 1.28+(ort 2.0.0-rc.13 的要求)。
# 资产入仓在 thirdparty/(见 docs/10-plan-b-implementation.md §15)。
set(NB_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty")

# ⚠️ 用 add_custom_target(... ALL) 而**不是** POST_BUILD:
#    POST_BUILD 只在目标被**重新链接**时执行 —— 实测删掉 models/ 后跑增量构建,
#    ninja 报 "no work to do",资产**不会被恢复**(构建图中没有这条规则)。
#    而这两个资产是**拷贝进来的输入**(非构建产物),恰恰最容易被误删/过期。
#    挂在 ALL 上可保证每次构建都校验一次,代价仅一次 copy_if_different。
if(WIN32)
    add_custom_target(nb_deploy_ocr_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NB_THIRDPARTY}/ort/onnxruntime.dll" "$<TARGET_FILE_DIR:nextboard>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nextboard>/models/ocr"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${NB_THIRDPARTY}/models/ocr" "$<TARGET_FILE_DIR:nextboard>/models/ocr"
        COMMENT "Deploy OCR assets (onnxruntime.dll + models/ocr)"
        VERBATIM
    )
else()
    # Linux(M2):ORT linux-x64 so 入仓(onnxruntime-linux-x64-1.28.0/lib/)。
    # Rust 侧 ORT_DYLIB 常量加载 exe 同级的 "libonnxruntime.so" → 部署版本化
    # .so + 同目录符号链接(soname 链一并带上,防 dlopen 解析需要)。
    add_custom_target(nb_deploy_ocr_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NB_THIRDPARTY}/ort/onnxruntime-linux-x64-1.28.0/lib/libonnxruntime.so.1.28.0"
                "$<TARGET_FILE_DIR:nextboard>"
        COMMAND ${CMAKE_COMMAND} -E create_symlink libonnxruntime.so.1.28.0
                "$<TARGET_FILE_DIR:nextboard>/libonnxruntime.so.1.28"
        COMMAND ${CMAKE_COMMAND} -E create_symlink libonnxruntime.so.1.28.0
                "$<TARGET_FILE_DIR:nextboard>/libonnxruntime.so.1"
        COMMAND ${CMAKE_COMMAND} -E create_symlink libonnxruntime.so.1.28.0
                "$<TARGET_FILE_DIR:nextboard>/libonnxruntime.so"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nextboard>/models/ocr"
        # ⚠️ copy_directory(非 if_different 变体):后者是 cmake 3.23+,VM 的
        # 3.22 不支持(docs/29 §9.1);模型约 26MB,全量拷贝代价可接受。
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${NB_THIRDPARTY}/models/ocr" "$<TARGET_FILE_DIR:nextboard>/models/ocr"
        COMMENT "Deploy OCR assets (ORT so + models/ocr)"
        VERBATIM
    )
endif()
add_dependencies(nb_deploy_ocr_assets nextboard)

# QML 模块资源的 rcc 输出随 nextboard 目标自动重建;Sciter 时代的 panel HTML
# 目录拷贝已移除(qt_add_qml_module 直接打进 qrc)。

# Qt DLL 依赖由 windeployqt 处理(发布脚本负责;开发期用 PATH 上的 Qt bin 运行)
