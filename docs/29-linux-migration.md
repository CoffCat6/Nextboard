# 29 · Linux 迁移:盘点、方案与路线

| 项 | 内容 |
| --- | --- |
| 版本 | v1.4(2026-11-07:Spike 2 屏幕采集完成——WSLg 拒 GetImage,结论 §12.2;Spike 1 剪贴板 §12.1;2026-11-04:编译普查 §4.3) |
| 日期 | 2026-11-07 |
| 状态 | 盘点+普查+Spike 1/2 完成;**下一步 = 搭 KDE VM(M2 前置,提前于原计划)→ M0**;遗留复测项集中在 VM(§12.1/§12.2) |
| 关联 | [01-architecture.md](./01-architecture.md)(其 §"非目标"声明需随本迁移修订)· [04-build-package-release.md](./04-build-package-release.md)(打包链重建)· [08-rapidocr-engine.md](./08-rapidocr-engine.md)(OCR 引擎平移)· [19-scrolling-capture-plan.md](./19-scrolling-capture-plan.md)(长截图受 §5.3 影响) |
| 一句话 | 把"Windows 优先、全量 Win32 无条件编译"的现状梳理成:**两个 Rust 编译硬阻断 + 一个 C++ 平台抽象层 + 一条全新构建分发链**,按 M0–M4 五步在 Linux 复刻 X11 全功能版,Wayland 逐项降级。 |

---

## 1. 目的与边界

- **目的**:评估 Nextboard 迁移到 Linux 的全部改动面,给出分级清单、策略决策与实施顺序,作为后续立项与排期的依据。
- **边界**:仅盘点与分析,不含代码改动;具体实现方案在开工时按里程碑补设计。
- **方法**:三路并行扫描(C++ 宿主 71 文件 / Rust core 全源码 / 构建打包资源),逐文件核对 file:line,结论均在 §10 checklist 落表。

---

## 2. 总体结论(先读这个)

1. **分层架构对迁移友好**:"有窗口的归 C++、有逻辑的归 Rust、有界面的归 QML"意味着编辑器、贴图、便签贴边吸附、翻译、待办、RapidOCR(纯 Rust ONNX)几乎免费平移;数据层(rusqlite bundled、blob 相对路径、zip 备份)已按可移植格式设计。
2. **但现状是"零条件编译"**:全部 71 个 C++ 源文件里**只有 `stitch_hud.cpp` 一处用了 `Q_OS_WIN`**(5 处),其余 Win32 代码全部无条件编译直接 `#include <windows.h>`(约 25 个文件)。迁移第一步必然是新建平台目录(`platform_win/` / `platform_linux/`)收口,而不是"补几个 `#ifdef`"。
3. **两个 Rust 编译硬阻断**:① `clipboard-win` 无条件依赖 + 整个剪贴板子系统基于 Win32 消息窗口(最大单项工程);② 翻译传输层 WinHTTP 全文无门控。
4. **架构级决策前置**:截图工具的核心能力(屏幕采集/窗口枚举/全局热键/模拟按键/排除捕获)在 Wayland 下要么走 portal 要么无解,X11 下全部有成熟等价物。**建议 Linux v1 按 X11(含 XWayland)做全功能版,Wayland 会话按能力探测逐项降级**(详见 §8,待拍板)。
5. **已有降级契约是迁移的本钱**:智能框选枚举失败 → 空表 → 纯手动框选(`smart_capture.hpp` 头注释);长截图 Esc 热键失败 → 按钮收尾文案;OCR 后端失效有 `repair_removed_ocr_backend` 式归一先例。Wayland 降级路径可以复用同一套契约。
6. **工作量排序**:剪贴板子系统重写 > FFI 通知协议重设计(跨 C++/Rust 联动)> UIA→AT-SPI2(或降级)> 构建分发链重建 > 其余(多数是"直接替代,低难度")。

---

## 3. 现状盘点:平台耦合面全景

### 3.1 C++ 宿主(app/src)

| 事实 | 依据 |
| --- | --- |
| 约 25 个文件含 Windows 专属代码,全部无条件编译 | 清单见 §10 |
| 全项目仅 `stitch_hud.cpp` 用了 `Q_OS_WIN`(L50/95/249/270/285) | 全 src 无 `Q_OS_LINUX/MACOS`、无 `_WIN32` 宏 |
| 无 QSettings NativeFormat 注册表用法;注册表仅 `auto_start.cpp` 直连 | 设置全在 nbcore 的 sqlite/json |
| 宿主侧剪贴板零 Win32:位图/文本写入全部经 nbcore FFI | `copyRgbaToClipboard` / `copyTextToClipboard` |
| editor/、ui/、pin/、note_registry、qml_bootstrap、clip_entry_model、ui_theme、app_font、image_save、log_rotate 均无 Win32 依赖 | 纯 Qt |
| tray.cpp / pin_window.cpp 纯 Qt,仅需平台验证 | QSystemTrayIcon + DPR 变化处理已跨平台 |

### 3.2 Rust 核心(core/nbcore)

| 事实 | 依据 |
| --- | --- |
| FFI 导出全部 `#[no_mangle] pub extern "C"`,**无 stdcall**,Win-x64 与 Linux ABI 一致 | `ffi.rs:38` 起全部入口 |
| panic 边界(`catch_unwind`)、rusqlite bundled(含 FTS5)、zip/image/rqrr 平台无关 | `ffi.rs:59,139` 等 |
| `windows = "0.62"` 已正确放在 `[target.'cfg(windows)'.dependencies]`,Linux 构建自动跳过 | `Cargo.toml:50` |
| **`clipboard-win = "5.4"` 无条件依赖(未进 cfg 块)** | `Cargo.toml:28` |
| ORT 动态加载,运行时显式 `ort::init_from(路径)`,不依赖 PATH | `rapid.rs:108-133` |
| **`ORT_DYLIB` 常量已预留** `libonnxruntime.so` 命名 | `rapid.rs:33-36` |
| 模型定位 = exe 同级 `models/ocr`(正斜杠),平台无关 | `rapid.rs:29-30,223-229` |
| 系统 OCR(Windows.Media.Ocr)全部封在 `#[cfg(windows)] mod platform` | `system.rs:67,71,86,90,99,103,146` |
| 通知 `PostMessageW` 已有 `not(windows)` 空实现门控 | `core.rs:16-22,37`、`ffi.rs:91-95,211` |
| 无外部进程调用、无注册表访问;`Command::new` 零处 | quick payload 只是存储给宿主执行 |

### 3.3 构建 / 打包

| 事实 | 依据 |
| --- | --- |
| CMake MSVC 专属:RC 语言、.rc 图标、WIN32 子系统、5 个 Windows 系统库、/MANIFEST:EMBED | `app/CMakeLists.txt:6-8,32,140-145,147-160,170` |
| nbcore 显式 `LoadLibrary` 决策(避免 CRT/静态初始化冲突),Linux 同构保留 → QLibrary/dlopen | `core_bridge.cpp:46-53`、`CMakeLists.txt:161-163` |
| build.ps1 硬编码 Qt/VC 路径 + icacls 完整性修复 | `build.ps1:25-28,135-144` |
| NSIS 安装器、app-local CRT、体积预算 90MB | `tools/installer.nsi`、`tools/package.ps1:31,171` |
| `thirdparty/models/ocr/` 四件 ONNX 模型**跨平台直接复用**;ORT 仅 win-x64 dll | `thirdparty/ort/`、`thirdparty/models/ocr/` |
| 文档明确"Linux/macOS 为非目标"(本迁移需修订该决策) | `docs/01-architecture.md:23` |
| **docs/ 无任何 Linux 迁移文档**(本文档为第一份) | grep 全 docs |

---

## 4. 硬阻断(不改过不了编译)

### 4.1 Rust 侧

| # | 位置 | 问题 | 方案 | 量级 |
| --- | --- | --- | --- | --- |
| R1 | `Cargo.toml:28` + `clip.rs:13,39-135,196,282-297,952-997` + `error.rs:16` | `clipboard-win` 无条件依赖;剪贴板监听(Win32 消息窗口 + `AddClipboardFormatListener`)、CF_DIBV5/CF_HDROP 读写、`AppError::ClipboardWin(#[from])` 全是 Win32 | **选型重写**(推荐 `arboard`:X11/XCB + Wayland data-control 双协议);监听线程现有 `not(windows)` 分支只是空转(`clip.rs:137-146`),Linux 需真实现(Wayland 下"监听"语义本就是 data-control 协商,需按协议重设计);`error.rs` 变体随之重构 | **高,最大单项工程** |
| R2 | `translate/http.rs` 全文 | 翻译传输层用 `windows::Win32::Networking::WinHttp`,零 cfg 门控 | `send()` 已按代理类型分派(`http.rs:243-252`),SOCKS5 已走 ureq(`socks.rs:15-51`);把直连/HTTP 代理分支也切 ureq。连带:`Cargo.toml:32` `native-tls` 在 Linux 落 `openssl-sys`,需系统 OpenSSL 开发头或改 `rustls` feature | 中 |
| R3 | `error.rs:16,23,45` | `AppError::Windows` 已门控;`ClipboardWin` 随 R1 重构 | 随 R1 | 低(随 R1) |

### 4.2 C++ 侧(汇总;逐文件见 §10)

| # | 域 | 涉及文件 | 阻断形式 |
| --- | --- | --- | --- |
| C1 | 入口/崩溃诊断 | `main.cpp` | `windows.h` + RtlCaptureStackBackTrace/VirtualQuery/SetUnhandledExceptionFilter 无条件编译 |
| C2 | 热键 | `host/hotkeys.cpp/.hpp`、`hotkey_lifecycle_flow.cpp` | 类接口被 Win32 类型污染(`UINT*`),WM_HOTKEY 过滤器 |
| C3 | 自动粘贴 | `host/auto_paste.cpp/.hpp` | SendInput/GetForegroundWindow |
| C4 | 单实例 | `host/single_instance.cpp/.hpp` | CreateMutexW/共享节/PostMessageW |
| C5 | 自启动 | `host/auto_start.cpp` | HKCU Run 注册表 |
| C6 | FFI 桥 | `host/core_bridge.cpp/.hpp` | LoadLibraryW/HWND 通知协议 |
| C7 | 窗口效果 | `host/window_effects.hpp`、`panel/panel_window.cpp`、`note/note_window.cpp` | DWM/SetWindowPos/WM_NCHITTEST/WM_NCLBUTTONDOWN |
| C8 | 采集 | `capture/capture_session.cpp`、`smart_capture.cpp` | hwndToExeName/EnumWindows/DWM/UIA COM |
| C9 | HUD | `capture/stitch_hud.cpp` | WS_EX_* / WDA / RegisterHotKey(唯一 Q_OS_WIN 文件) |
| C10 | 快捷指令 | `host/quick_commands.cpp` | ShellExecuteW/IShellLink/HICON/ms-settings 表 |

---

### 4.3 编译普查实测(2026-11-04,`tools/linux_census.sh` @ WSL2 Fedora 41)

> 环境:Fedora Remix for WSL 41 · glibc 2.40 · rustc/cargo 1.91.1 · Qt 6.8.3 · cmake 3.30.8 · ninja 1.12.1;WSLg 1.0.65 下 Qt Widgets 窗口冒烟通过(X11 链路可用)。

**C++(app):configure 全过(Qt 6.8.3 兼容,QML 模块注册/AUTOMOC 正常),ninja 首波 fatal error 11 个全部是缺 `windows.h`**

| 首爆位置 | 次数 | 对应 checklist 档位 |
| --- | --- | --- |
| `host/core_bridge.hpp:6` | ×4(多 TU 包含,扇出最大的收口点) | B(FFI 桥) |
| `host/hotkeys.hpp:7`(含 moc 产物) | ×2 | B(热键) |
| `main.cpp:26` | ×1 | A/B(入口) |
| `host/app_paths.cpp:6`、`host/auto_start.cpp:8`、`host/single_instance.cpp:6` | 各 ×1 | A |

→ 与 §10 checklist 完全吻合,**无漏报**;A 档三件(路径/自启/单实例)先行替换,大部分 TU 即可走到下一层报错。

**Rust(core):先撞三关环境依赖(即 M0 的 Linux 环境清单),再进源码级错误**

1. 环境依赖三关(全部来自 xcap 的 Linux 链:wayland-sys → libspa-sys/pipewire → bindgen):`wayland-client`(wayland-devel)→ `libpipewire-0.3`(pipewire-devel)→ `libclang`(clang-devel)。
   **M0 Linux 环境清单(Fedora 系)**:`dnf install cargo cmake ninja-build qt6-qtbase-devel qt6-qtdeclarative-devel openssl-devel wayland-devel libxkbcommon-devel libX11-devel pipewire-devel clang-devel`(Ubuntu 系对应:libwayland-dev libpipewire-0.3-dev libclang-dev libssl-dev qt6-base-dev qt6-declarative-dev,需 Qt≥6.5)。
2. 过三关后 **nbcore 源码级错误 20 个**,与静态预测吻合,并抓到 3 个静态扫描未覆盖的 cfg 门控缺陷:

| 实测错误 | 数量 | 结论 |
| --- | --- | --- |
| `clip.rs` clipboard-win API(§4.1 R1) | 14 | 预测命中;且发现 clipboard-win 在 Linux 能编译(空壳),错误全在我方引用——R1 重写面聚焦 |
| `translate/http.rs` WinHTTP(§4.1 R2) | 4 | 预测命中 |
| `error.rs:16` ErrorCode | 1 | 预测命中 |
| **`clip.rs:141` 引用不存在的 `crate::core::ready_flag`** | 1 | **新增**:not(windows) 监听分支 cfg 漏改 |
| **`ffi.rs:99` CoreConfig 缺 `notify_hwnd` 字段仍被赋值** | 1 | **新增**:notify 门控不对称 |
| **`ocr/system.rs:74,93` `AppError` 未导入** | 2 | **新增**:not(windows) 降级路径缺陷 |

**对排期的影响**:§9 M0–M4 无需修正;M2 风险下调——xcap 0.9 的 Linux 栈完整(wayland + pipewire + x11 全带);M1 的 R1/R2 各并入 3 个小时级 cfg 修复。

> 原始产物:`census/census-summary.md`、`census-rust-check.txt`、`census-cpp-build.txt`(.gitignore 内,不入库);ubuntu:22.04 基线 glibc 普查待 Docker Desktop 集成后补测。

---

## 5. 功能域分级清单

### 5.1 A 档:Qt 跨层直接替代(低难度,先行建立平台层骨架)

| 功能 | 现状(file:line) | Linux 方案 | 说明 |
| --- | --- | --- | --- |
| 单实例门禁 | CreateMutexW + ERROR_ALREADY_EXISTS(`single_instance.cpp:56-61`,互斥量名=数据目录 FNV-1a `:20-29`) | `QLockFile`(数据目录内) | 门禁语义 1:1 |
| 跨进程唤起 | 共享节发布 HWND + PostMessage(`single_instance.cpp:71-115`) | `QLocalServer/QLocalSocket`(socket 名=数据目录哈希),二次启动发 JSON → `showAtCursor()` | 难度低 |
| 开机自启动 | HKCU Run 键(`auto_start.cpp:13-14,62-92`),命令行升级 `:94-118` | XDG `~/.config/autostart/nextboard.desktop`(`Exec=… --hidden`);enabled=文件存在且 `Hidden=false` | 全函数 1:1 语义移植 |
| 应用路径 | `GetModuleFileNameW`(`app_paths.cpp:33-37`) | `/proc/self/exe` readlink | 便携布局决策见 §7.3 |
| 内存探针 | psapi GetProcessMemoryInfo(`memory_probe.cpp:21-30`) | `/proc/self/status`(VmRSS/VmHWM)或 getrusage | |
| 崩溃诊断 | RtlCaptureStackBackTrace/VirtualQuery/SEH(`main.cpp:111-209,340-341`) | `backtrace()/backtrace_symbols` + `sigaction`(SIGSEGV/SIGABRT/SIGBUS)+ `/proc/self/maps` 反查模块偏移 | |
| Win11 圆角 | DwmSetWindowAttribute(33)(`window_effects.hpp:20-28`) | QSS/QML 圆角背景(Wayland 透明窗口成熟;当初选 DWM 是规避 Windows 透明合成风险,Linux 无此顾虑) | 代码注释已写明出处 |
| 日志滚动 | "先删后改名"防御(`lib.rs:74-77`、`clip.rs:740`)是 Windows rename 语义坑 | Linux rename 原子覆盖,**防御代码无害保留** | 零改动 |
| 托盘 | 纯 Qt(`tray.cpp`),`messageClicked` 已承认不可靠(L45-49) | QSystemTrayIcon 走 StatusNotifierItem:KDE 原生,GNOME 需扩展;气泡走 org.freedesktop.Notifications,行为与 Win10/11 一致 | 验证即可 |
| 贴图窗口 | 无 Win32(`pin_window.cpp`),DPR 时序处理跨平台 | — | 验证即可 |

### 5.2 B 档:需要平台抽象层(中难度)

| 功能 | 现状 | Linux 方案 | 要点 |
| --- | --- | --- | --- |
| 全局热键 | RegisterHotKey + WM_HOTKEY 过滤器(`hotkeys.cpp:117-139,231-245`);VkKeyScanW `:51-53`;冲突检测 GetLastError `:231-245` | 抽象 `IPlatformHotkeys`:X11 `XGrabKey`(BadAccess=冲突)经 QAbstractNativeEventFilter 处理 XCB;Wayland 走 XDG portal `GlobalShortcuts`(KDE 已实现,GNOME 部分),无则降级应用内快捷键 | 键码换 Qt::Key/Modifiers;`hotkey_lifecycle_flow.cpp:28,51-80` 手工构造 MSG 的验收流需按平台重写 |
| 长截图 Esc | RegisterHotKey(VK_ESCAPE)+ 过滤器(`stitch_hud.cpp:249-290`,唯一 Q_OS_WIN 文件) | 同上热键抽象;已有降级文案(L257-260 点按钮收尾) | 保留 `#else` 分支即可 |
| FFI 加载 | LoadLibraryW/GetProcAddress(`core_bridge.cpp:46-53`) | QLibrary + `libnbcore.so`;显式加载决策保留 | 接口不变 |
| **宿主通知协议** | `nb_set_notify_hwnd(HWND)` + WM_APP_CLIP_CHANGED/SETTINGS_CHANGED/TODO_DUE/ACTIVATE(`core_bridge.hpp:99-109` ↔ `core.rs:16-22,37`) | **重设计**:`nb_set_notify_fd`(eventfd/Unix socket)或回调+事件转发;枚举值经 fd 传送,宿主 PanelWindow 四个 nativeEvent 分支(`panel_window.cpp:600-633`)改 Qt 信号 | **跨 C++/Rust 联动,与 R1 并列的最大改动项之一**;Rust 侧 `core.rs/ffi.rs` 同步 |
| 置顶 | SetWindowPos(HWND_TOPMOST)(`panel_window.cpp:101-114`、`note_window.cpp:348-358`;不用 setWindowFlag 的原因:重建窗口) | X11:直接改 `_NET_WM_STATE_ABOVE` property 免重建;Wayland:无全局置顶 → 降级 | |
| 边缘缩放 | WM_NCHITTEST 命中码(`window_effects.hpp:41-70`,panel `:592-598`) | 自算 hit-test 保留,确认后调 `QWindow::startSystemResize(Qt::Edges)`(Qt 6,X11/Wayland 都支持) | |
| 系统拖动/缩放收尾 | WM_NCLBUTTONDOWN 模态循环 + WM_EXITSIZEMOVE 收尾(`panel_window.cpp:361-374`、`note_window.cpp:852-866,1241-1261`) | `startSystemMove/Resize` + `QEvent::Move/Resize` 收尾替代 WM_EXITSIZEMOVE;便签吸附要"松手时刻"(`note_window.cpp:4-8` 头注释)→ 手动 resize 方案备选 | 贴边/半隐藏/多屏夹回逻辑本身纯 Qt,零改动 |
| 失焦隐藏 | WM_ACTIVATE 分支(`panel_window.cpp:635-641`) | `QEvent::WindowDeactivate` 跨平台 | 搬进 event() 即可 |
| 来源应用名 | hwndToExeName(OpenProcess + QueryFullProcessImageNameW,`capture_session.cpp:60-75,1741-1747`) | X11:`_NET_WM_PID` + `readlink /proc/<pid>/exe`;Wayland:无 → 来源为空(卡片隐藏"来自 x") | |

### 5.3 C 档:无等价物,降级或换技术(高难度)

| 功能 | 现状 | Linux 对应 | 降级契约 |
| --- | --- | --- | --- |
| **智能框选元素级(UIA)** | CUIAutomation + TreeWalker DFS 全套(`smart_capture.cpp:344-812`);ComGuard `:153-166` | AT-SPI2(org.a11y.atspi,Chromium/GTK/Qt 均导出树);若不做 AT-SPI → 退化窗口级 | **已有**:枚举失败 → 空候选 → 纯手动框选(`smart_capture.hpp` 头注释)。v1 可先窗口级,AT-SPI 二期 |
| 窗口枚举/可见矩形 | EnumWindows + DWMWA_EXTENDED_FRAME_BOUNDS/CLOAKED + 类名黑名单(`smart_capture.cpp:43-145,170-178`) | X11: `_NET_CLIENT_LIST`(EWMH)+ `_NET_WM_STATE_HIDDEN` + `_GTK_FRAME_EXTENTS`;类名黑名单换 WM_CLASS;**Wayland 无法枚举他窗 → 空候选** | 同上,契约已备 |
| 屏幕采集 | xcap 0.9(Windows GDI BitBlt;`capture.rs:3` 自述) | xcap 名义支持 Linux X11(需实测质量/性能);**Wayland 需 PipeWire portal,xcap 支持有限** | `ScreenCapturer` 抽象已留替换口(`capture.rs:40-48`) |
| 自动粘贴 | SendInput(KEYEVENTF_SCANCODE)+ UIPI 约束 + 前台判断(`auto_paste.cpp:24-93`;hpp:9-12 设计约束) | X11:`XTestFakeKeyEvent`(同样先确认前台非本进程);**Wayland 无法全局注入键盘** → ydotool(uinput,需权限/常驻)/ portal RemoteDesktop / **降级"仅复制"**(开关语义保留,`CoreFacade::autoPasteIfEnabled` 入口不变) | 设置项在 nbcore,语义可保留 |
| 排除捕获/鼠标穿透(HUD) | WS_EX_TRANSPARENT\|NOACTIVATE + WDA_EXCLUDEFROMCAPTURE(`stitch_hud.cpp:50-105`) | X11:shape/xcb input_region 可做穿透;WDA 无等价 → 靠摆位规避(候选表已优先把 HUD 放选区外,`stitch_hud.cpp:183-198` fallback) | 已有 fallback |
| 快捷指令 | ShellExecuteW / IShellLink .lnk / SHGetFileInfoW+GetDIBits 提图标 / ms-settings·msc·cpl 页目录表(`quick_commands.cpp:30-50,166-203,339-370`) | **整文件重写**(接口保留):启动=QDesktopServices/QProcess;图标=QFileIconProvider + icon theme;.lnk→.desktop 解析 Exec/Path;系统页表全新内容(gnome-control-center / systemsettings 模块 / xdg-open) | 无降级,纯重写 |
| Chromium 判定 | `Chrome_WidgetWin_1` 类名(`smart_capture_flow.cpp:1031`) | 换 WM_CLASS / AT-SPI role 判定 | 验收代码 |

### 5.4 行为级问题(编译可过,功能会不对)

| # | 问题 | 位置 | 处置 |
| --- | --- | --- | --- |
| B1 | 系统内置 OCR 不可用(Windows.Media.Ocr),**默认后端是 `Backend::System` → Linux 开箱即坏** | `ocr/system.rs:67-146`、默认值 `ocr/mod.rs:93-97,114` | Linux 默认改 RapidOCR;复用 `repair_removed_ocr_backend` 式归一清洗存量设置(`settings.rs:1232` 先例) |
| B2 | 显示器 name 契约 `\\.\DISPLAYn`(`MONITORINFOEXW.szDevice`),与宿主 `QScreen::name()` 配对 | `capture.rs:14,22,348`、`ffi.rs:254,283` | xcap Linux 侧 name 语义不同,多屏配对契约重验;文档同步改 |
| B3 | 存量 `data_dir` 绝对路径校验 `is_absolute()`——Windows 的 `D:\...` 在 Linux 不是绝对路径,校验失败 | `settings.rs:782,1130-1135` | 迁移工具/导入逻辑需清洗;Linux 存量配置不存在,主要影响"搬数据"场景 |
| B4 | DB 中 legacy blob 绝对路径原样采用,Windows 盘符会被误当相对路径 join | `clip.rs:562-569` | 同 B3;相对路径 blob 与 zip 内路径已是可移植格式 |
| B5 | `ProxyConfig::System`(跟随系统代理)语义未定 | `http.rs`(R2 连带) | Linux 语义:读环境变量 http_proxy/all_proxy |
| B6 | 残留 include 无实际调用 | `capture_toolbar_flow.cpp:46` | 顺手删除 |
| B7 | 验收探针 | `stitch_live_flow.cpp:235-238` CreateRectRgn/GetWindowRgn | 平台化或删除 |

---

## 6. 与 Windows 版的功能对照表(Linux v1 目标形态)

| 功能 | Linux v1 | 备注 |
| --- | --- | --- |
| 剪贴板历史/搜索/固定/分类 | ✅ 全量 | R1 完成后 |
| 截图框选/全屏/编辑/贴图 | ✅ 全量(X11) | Qt 主体零改动 |
| 智能框选 | ⚠️ 窗口级 | UIA→AT-SPI 二期;Wayland 手动框选 |
| 滚动长截图 | ⚠️ X11 可做 | HUD 穿透/排除捕获/来源 exe 均按 §5.2-5.3 |
| 自动粘贴 | ⚠️ X11 全量 / Wayland 仅复制 | |
| 全局热键 | ⚠️ X11 全量 / Wayland portal 或应用内 | |
| 待办/便签(贴边/半隐藏/提醒) | ✅ 全量 | 吸附逻辑纯 Qt |
| OCR(RapidOCR) | ✅ 全量(默认档) | 系统后端隐藏 |
| 翻译 | ✅ 全量 | R2 完成后;System 代理语义按 B5 |
| 快捷指令 | ✅ 重写后 | 系统页表换 Linux 内容 |
| 托盘/开机自启/单实例唤起 | ✅ 全量 | A 档 |
| 数据备份/恢复/导入导出 | ✅ 全量 | 平台无关 |
| 记忆点 | — | X11 置顶/圆角/缩放齐;Wayland 部分缺 |

---

## 7. 构建 / 打包 / 分发重建

### 7.1 CMake(`app/CMakeLists.txt`)

| 位置 | 改动 |
| --- | --- |
| :6-8 RC 语言、:140-145 ico .rc 生成、:32 WIN32 子系统 | `if(WIN32)` 条件化;Linux 普通可执行,图标走 qrc + .desktop |
| :16-18 `Qt6_DIR` 硬编码 msvc2022_64 | Linux 用系统 Qt6(≥6.5)find_package 或 aqtinstall 路径 |
| :21-25 `nbcore.dll.lib` 存在性检查 | 改 `libnbcore.so`;显式 dlopen 机制保留(`:161-163` 注释决策不变) |
| :147-160 dwmapi/ole32/oleaut32/uiautomationcore/psapi | 按平台分组;Linux 视 UIA/AT-SPI 方案决定是否链接 atspi/glib |
| :170 `/MANIFEST:EMBED` | Linux 无 manifest,删;PerMonitorV2(`app.manifest:7`)由 Qt/Wayland fractional scaling 替代 |
| :184-187 拷 nbcore.dll、:196-212 nb_deploy_ocr_assets | 拷 `libnbcore.so` + `libonnxruntime.so` + `models/ocr/`(RPATH 或同目录) |

### 7.2 构建与打包脚本

| 项 | Windows 现状 | Linux 方案 |
| --- | --- | --- |
| 构建脚本 | build.ps1(硬编码 vcvars64/Qt `:25-28`,icacls `:135-144`) | build.sh;icacls 整段删除 |
| 部署 | windeployqt(`build.ps1:103-126`,校验 qwindows.dll 等) | `linuxdeploy`(+Qt 插件)或 linuxdeployqt;校验项换 `platforms/libqxcb.so`(+ Wayland 插件)、imageformats(qico 不必要) |
| 安装器 | NSIS `installer.nsi`(免 UAC `:61-63`、HKCU 卸载/Run `:37-38,103-114,130-132`、卸载保留 data `:166-184`) | **AppImage(便携)与/或 deb**;卸载保留 data → postrm 逻辑;自启 → XDG autostart |
| CRT | app-local msvcp140/vcruntime140(`package.ps1:31,129-142`) | 不需要(发行版 glibc/libstdc++;AppImage 注意 baseline glibc) |
| 体积预算 | 90MB(`package.ps1:171`) | Linux 重算(ORT so 体积不同) |
| 签名 | 无(现状) | 可延用;后续可 GPG/sigstore |

### 7.3 分发布局决策(已拍板 2026-11-03:**AppImage 先行**)

"数据在 exe 同级 `data/`" 的便携式布局(`app_paths.hpp:9`、`installer.nsi:40`、`build.ps1:61-75`)与 FHS 冲突:

- **AppImage(先行)**:保持便携布局(与现状同构,零数据迁移成本,单文件分发);
- **deb(二期跟进)**:迁 `$XDG_DATA_HOME/Nextboard`(`QStandardPaths::AppLocalDataLocation`),因此 `app_paths` 抽象从 M0 起就要按"安装形态选布局"设计,而非硬编码一种;
- auto-update 本就未实装(`NB_UPDATE_CHECK_URL` 空,`CMakeLists.txt:180-181`),正好此时定 Linux 更新渠道。

### 7.4 第三方二进制与工具

| 项 | 现状 | 改动 |
| --- | --- | --- |
| onnxruntime | `thirdparty/ort/onnxruntime.dll`(16MB,win-x64) | 换/加 `libonnxruntime.so`(1.28+,与 ort 2.0.0-rc.13 api-28 配对;Rust 侧 `ORT_DYLIB` 常量已备,改动≈0) |
| OCR 模型 | `thirdparty/models/ocr/` 四件 | **直接复用** |
| 图标 | 仅 `nextboard.ico`(PE 资源 + qrc 双用) | 补 hicolor PNG/SVG + `nextboard.desktop` |
| tools/verify_todo*.py、update_fixture.py | 硬编码 nbcore.dll + nextboard.exe(`verify_todo_groups.py:24-25`),ctypes CDLL | 改 `libnbcore.so` 路径与 exe 名 |
| tools/smoke_host | windows.h + LoadLibraryA(`smoke_host.cpp:12,70`),提交了 exe/dll 产物 | dlopen 重写;产物从 git 清理 |
| tools/make_icon.py | 输出 ico | 增加 PNG/SVG 输出 |
| 根 build/ | 遗留 MinGW CMake 树(`build/CMakeCache.txt` mingw1310_64) | 杂物,清理 |
| check_db_integrity.py | 基本跨平台 | 复用 |

---

## 8. 策略决策:X11 先行(已拍板 2026-11-03)

### 8.1 背景

截图工具五项核心能力在两个窗口系统下的支持度:

| 能力 | X11(含 XWayland) | Wayland 原生 |
| --- | --- | --- |
| 屏幕采集 | ✅ XShm/xcap | ⚠️ PipeWire portal(需用户授权、每会话) |
| 窗口枚举/智能框选 | ✅ EWMH / AT-SPI2 | ❌ 无法枚举他窗 |
| 全局热键 | ✅ XGrabKey | ⚠️ portal GlobalShortcuts(KDE 有,GNOME 部分) |
| 模拟按键(自动粘贴) | ✅ XTest | ❌(ydotool 需 uinput 权限/常驻) |
| 排除捕获/鼠标穿透 | ✅ shape | ❌ |
| 置顶 | ✅ property | ❌(层 shell 仅 layer-surfaces) |

### 8.2 建议

**Linux v1 按 X11/窗口化 XWayland 做全功能版;Wayland 会话启动时做能力探测,逐项降级**:

- 智能框选 → 空候选 → 手动框选(契约已备);
- 自动粘贴 → 关闭开关,提示"仅复制";
- 全局热键 → portal(可用则用)→ 应用内快捷键;
- HUD 穿透/排除捕获 → 摆位规避 fallback(已备);
- 置顶 → 隐藏该设置项。

Wayland 原生截图/PipeWire 作为二期评估项(取决于 xcap 上游或自实现 portal 截图)。

> **已拍板(2026-11-03)**:
> ① v1 平台基线 = **X11 全功能 + Wayland 能力探测降级**(五项核心能力 X11 全解,XWayland 兜底;PipeWire 原生截图二期评估);
> ② 分发 = **AppImage 先行**(便携布局同构,零数据迁移),deb 二期;
> ③ 智能框选元素级 = **二期**(AT-SPI2),一期只做窗口级 + 手动框选兜底。
> 三项均直接约束 §5.2/§5.3 的接口形态:热键接口必须是回调式(portal 参与降级链),`app_paths` 必须支持双布局。

### 8.3 第一步执行计划(2026-11-03 定:**先消不确定性,零代码改动**)

第一步不是拆平台目录——接口形态被 §8 三决策点锁死(回调式热键接口、双布局路径),先拆会返工;静态扫描也有盲区,编译器才是裁判。第一步共三件事,1~2 天完成:

**① 环境基线(零采购,利用现有条件):**

| 环境 | 定位 | 能验证 | 不能验证 |
| --- | --- | --- | --- |
| WSL2 fedoraremix(已有) | 日常普查 + CLI spike | WSLg 自带 XWayland:剪贴板/热键 demo、Qt 窗口渲染;**采集不可测**(WSLg 的 RDP 型 XWayland 拒绝 GetImage,见 §12.2) | SNI 托盘、portal、真 Wayland 会话、DE 差异、**屏幕采集** |
| Docker Desktop(已有) | Rust 普查基线容器(ubuntu:22.04) | **AppImage 基线 glibc(2.35)下 libnbcore.so 编译实测** | GUI 全部 |
| 真机/VM:KDE + GNOME | DE 级验证(每里程碑一次) | 托盘、portal、Wayland 降级清单逐项验收 | — |

> **Qt 版本约束(普查环境的硬前提)**:`app/CMakeLists.txt:29` `qt_standard_project_setup(REQUIRES 6.5)` + `Qt6_DIR` 回退写法 → Ubuntu 22.04(系统 Qt 6.2)/24.04(Qt 6.4)的**系统 Qt 连 configure 都过不了**;C++ 普查须用 Qt≥6.6 的环境(Fedora/Arch 系统仓或 aqtinstall 自装 6.9)。基线 glibc(2.35)只约束 M4 打包(AppImage 自带 Qt 6.9,构建机用 22.04),不约束普查。

支持矩阵建议:**最低 Ubuntu 22.04 LTS / glibc 2.35,DE 首选 KDE Plasma、GNOME 次之**。

**② 编译普查(半天,零改动,`tools/linux_census.sh`):**

```bash
# Rust(ubuntu:22.04 容器,顺带实测基线 glibc;或 fedoraremix)
./tools/linux_census.sh rust

# C++(fedoraremix: dnf install cargo cmake ninja-build qt6-qtbase-devel qt6-qtdeclarative-devel)
./tools/linux_census.sh cpp
```

产出 `census/census-summary.md`(环境指纹 + 错误码频次 + 首层文件清单)→ **回填本文 §4,静态结论换实测**。已知事实:configure 阶段 `nbcore.dll.lib` 缺失仅 WARNING(`app/CMakeLists.txt:23-25`),能拿到完整编译错误级联。

> **已执行(2026-11-04)**:结果回填 §4.3,零漏报;过程中沉淀的 Linux 环境依赖清单见 §4.3(wayland/pipewire/clang-devel 三关)。ubuntu:22.04 基线 glibc 补测待 Docker Desktop WSL 集成开启。

**③ 普查之后的第一批代码动作(预览):**

1. spike 1:arboard 双协议(独立小 crate,不碰主工程)→ 决定 R1 排期;
2. spike 2:xcap Linux 采集 + `QScreen::name()` 配对实测 → 决定 M2 风险;
3. 两个 spike 结论落地 → 开 M0 平台目录拆分(此时接口形态已定,不返工)。

---

## 9. 迁移里程碑 M0–M4

> **2026-11-07 调整**:M0 拆为 M0a(core 编译门)与 M0b(宿主平台化)。原 M0 验收"骨架可运行"依赖 `libnbcore.so` 可加载,而 core 离编译通过只差 20 个错误点(普查实测),故把"最小编译门"提前——剪贴板/HTTP 以可读错误桩降级(M1 再接 arboard/ureq),**Windows 行为零影响**(全部改动在 cfg(not(windows)) 分支或无条件无行为差异处)。

| 里程碑 | 内容 | 验收门 | 前置 |
| --- | --- | --- | --- |
| **M0a core 编译门(2026-11-07 ✅)** | 修复普查 20 错误点:3 个 cfg 缺陷(`clip.rs:141` 改用 `core::SHUTDOWN`、`ffi.rs:99`/`core.rs` CoreConfig 字段无条件化、`ocr/system.rs` AppError 导入)+ clip.rs 读写 5 函数 not(windows) 桩(可读错误)+ http.rs WinHTTP 整模块门控(send 非 Windows 降级错误)+ clipboard-win 移入 target 门控依赖 + **B3/B4 路径语义加固**(`safe_blob_rel` 显式拒绝反斜杠;盘符路径测试平台化)+ 17 个 Windows 专属行为用例随平台门控 | Linux:`cargo check` 零错误、`libnbcore.so`(8.7MB)、ctypes 冒烟 abi=1/version=100、**单测 376 passed / 0 failed**;Windows:`cargo check` 零回归、单测 407 passed + 1 个**既有**环境性失败(`send_dispatches_socks5_through_ureq`,经 git stash 基线复证实证非本次回归,与本机 DNS/代理环境相关) | Spike 1/2 |
| **M0b 宿主平台化(2026-11-07 ✅)** | **17 个 C++ 文件平台化**(模式统一:`#if defined(Q_OS_WIN)` 原样保留 + `#else` Linux 实现/桩):core_bridge(dlopen/dlsym)、app_paths、memory_probe、single_instance(QLockFile+QLocalServer+信号)、auto_start(XDG)、main 崩溃诊断(backtrace+dladdr+sigaction)、hotkeys(接口去 Win32 + 降级通道)、auto_paste(桩)、quick_commands(Linux 重写)、smart_capture(段式拆分,UIA 降级空候选)、capture_session、window_effects(no-op)、stitch_hud/stitch_live_flow/smart_capture_flow/hotkey_lifecycle_flow(验收门控)、panel_window(nativeEvent 拆分 + WindowDeactivate 失焦)、note_window(startSystemResize 缩放);CMake 条件化(五系统库/部署/OCR 资产/RPATH)+ **build.sh**(仿 build.ps1) | **验收达成:WSL 里 `nextboard` 完整启动**——libnbcore.so 加载成功、nb_init=0、QML 面板显示(软件渲染)、单实例锁/数据目录就位、build.sh 全新构建 130/130 全绿。已知项(非阻塞):① WSLg 无 SNI 宿主,托盘注册失败(KDE VM 验证);② TranslateView 的 `ContextMenu.menu` 附加属性 Qt 6.8 不可用(6.9 特性,待兼容小修);③ Windows MSVC 全量回归**已通过**(2026-11-07:build.ps1 完整链路 cargo+MSVC+链接+部署全绿;期间 nextboard.exe 被运行中实例占用致 LNK1104,属环境占用非代码问题) | M0a |
| **M1 core 接入(2026-11-07 ✅ R1/R2/R3 全部完成)** | **R2 ✅**:ureq_http 模块(直连/HTTP 代理/环境变量系统代理)+ 非 2xx 语义对齐 + **socks5h 隐私修复(↑K1)**。**R1 ✅**:clip.rs 五函数 arboard 真实现(文本/位图读写;文件条目降级二期)+ **轮询监听线程**(300ms/单例驻线程/内容哈希防抖/入库管线与 Windows 共用尾部);E2E 实证:spike 写入 → 轮询命中 → DB 入库。**R3 ✅**:通知协议 `nb_set_notify_fd`(核心写 4 字节消息 id 到管道)+ 宿主 QSocketNotifier 读出 → emit 与 Windows nativeEvent 同组信号;E2E 实证:通道就绪日志 + 入库 + stderr 干净。**部署坑**:libnbcore 拷贝 POST_BUILD→ALL(重链接才触发,纯 Rust 改动不生效,与 OCR 资产同教训) | Linux `cargo test` **402 passed / 0 failed** + 运行期双 E2E;Windows **408 passed / 0 failed**(R1/R3 全在 cfg 分支,零回归) | M0b;VM 复测项可并行 |
| **M2 截图闭环(进行中 2026-11-08)** | **VM(Ubuntu 22.04/Xfce/Xorg)实证**:全屏采集热键 → `截图已入库并写入剪贴板(RGBA 直入):1303x768` → 剪贴板 TARGETS 出现 `image/png` → 面板历史出条目(缩略图/250.2KB/图片+capture 标签);框选 → 遮罩+橡皮筋+尺寸角标(301×201)+8 手柄 + **10 项浮条**全部渲染正确;**编辑器标注链路 `--editor-flow` RESULT failures = 0**(裁剪/旋转/端到端)。**修一条流**:capture-toolbar-flow 的 Qt 即时读回 arboard 写入的剪贴板为空——X11 选区异步应答协议 + Qt 同栈读回时序兼容性问题,**外部粘贴方(xclip/GTK)全部正常=产品无缺陷**,flow 读回加重试自旋(每拍 processEvents+50ms)后 RESULT failures = 0 | **已过**:截图→剪贴板+入库;框选+浮条;编辑器流。**待做**:贴图手动冒烟、多屏、DPR 变化夹回 | M1 |
| **M3 集成件(进行中 2026-11-08)** | **热键 X11 ✅**、**自动粘贴 ✅**(↑K11)、**智能框选窗口级 ✅**(↑K5)、**SNI ✅**(↑K2)——均 VM 实证。**长截图 ✅(2026-11-08)**:产品代码本就跨平台,零新代码;`--stitch-live-flow` **RESULT failures = 0**——十项断言全绿,含三铁证:直抓选区 400x300 与请求完全一致、选区像素 `#af2080` 与页面自报逐位一致(遮罩下抓到真实页面)、滚动 80px 后长图 380>300(拼接真的长高)。HUD:Esc 走 Windows「注册失败」同款降级(面板常显「点结束收尾」提示,采集期页面持焦点);穿透依赖 Qt xcb 对 `WindowTransparentForInput` 的 XShape 实现(待手动冒烟)。**待做**:元素级 AT-SPI2(二期)、Wayland 降级清单 | **已过**:热键/自动粘贴/智能框选窗口级/长截图 live flow(S11 十项)+ stitch-flow(30 ok)+ stitch-settings-flow(16 ok)+ editor-flow(RESULT 0)+ capture-toolbar-flow(RESULT 0)+ note-flow(53 PASS)+ settings-flow(全程 0 FAIL)——**全部 VM 实证**。**待做**:Wayland 会话降级清单逐项验收 | M2 |
| **M4 分发** | B1(默认 OCR 档)+ libonnxruntime.so 打包 + AppImage(/deb)+ .desktop/图标 + tools 重写 + B3/B4 数据清洗工具 | AppImage 冒烟(全新装/升级/卸载);OCR 三入口;体积预算重算留痕 | M3 |


### 9.1 已知项与遗留(随里程碑滚动更新)

| # | 项 | 根因/状态 | 处置 |
| --- | --- | --- | --- |
| K1 | ~~Windows `send_dispatches_socks5_through_ureq` 单测失败~~ **已解决(2026-11-07 M1/R2)** | **根因修正:非环境问题**——ureq 对 `socks5://` 把目标主机名做**本地 DNS 解析**(主机名泄漏),`.invalid` 必失败;`socks5h://`(主机名由代理端解析)ureq 原生支持,socks.rs 已切换并补注释 | **隐私契约修复**;Windows 回归 **408 passed / 0 failed**(此前唯一的失败测试转绿) |
| K2 | WSLg 托盘 StatusNotifierItem 注册失败 | WSLg 无 SNI 宿主;**Xfce VM 已实证注册成功**(2026-11-08:RegisteredStatusNotifierItems = app item + nm_applet 两项) | WSLg 场景保持降级;真机已通 |
| K3 | **QML 的 Qt 6.9-only API 系统性兼容问题**(2026-11-07 实测展开):① TranslateView 的 `ContextMenu.menu`(已修:TapHandler+popup,6.5~6.9 全兼容);② SettingsPageTranslate 的 `onTextEdited`(6.8 报 non-existent,待查——同 API 在 6.9 正常,Windows 截图完美);③ 预计还有后续页面逐个暴露。**根因决策点**:WSL 系统 Qt=6.8 vs 项目工具链 Qt 6.9(README/build.ps1)。已装 aqtinstall Qt 6.9.0(`~/Qt/6.9.0/gcc_64`),但运行期有三层解析要一致(lib 二进制层 LD_LIBRARY_PATH ✓、QML 插件层 qt.conf/QML2_IMPORT_PATH ✓、仍段错误——疑混装残余,待 gdb);或走 6.8 全兼容审计(逐页磨)。**当前态**:REQUIRES 回到 6.5 + TranslateView 已修,SettingsView 链式失败 | 二选一:A) 6.8 全兼容审计(逐页修,WSL 系统包即可);B) WSL 全面切 aqt Qt 6.9(qt.conf+LD_LIBRARY_PATH+gdb 查段错误)——**推荐 A**,理由:Windows 6.9 不动零风险,6.8 兼容同时收紧了项目声明下限的真实性 | 进行中 |
| K4 | ~~Linux ORT so 未入仓~~ **已解决(2026-11-07 M2)** | `thirdparty/ort/onnxruntime-linux-x64-1.28.0/` 入仓(含 LICENSE);CMake Linux 部署分支:版本化 .so + `libonnxruntime.so`/`.1.28` 符号链接(Rust ORT_DYLIB 加载名对齐)。引擎级冒烟(ocr_bench @ 面板截图 800x600):**20 行 358 字符识别成功**,run1 827ms | 已闭环;**B1 同步修复**:默认引擎平台化(System 仅 Windows),存量 "system" 设置自动归一 RapidOCR |
| K5 | Linux 智能框选/热键/自动粘贴为降级桩 | ~~窗口级~~ **已解决(2026-11-08)**:smart_capture 的 Linux `probeWindows` = EWMH/Xlib(`_NET_CLIENT_LIST_STACKING` 反向 = z 序顶到底;`_NET_FRAME_EXTENTS` 外扩 = 可见外框;`_NET_WM_PID` 剔本进程;`_NET_WM_STATE_HIDDEN`/`_DESKTOP`/`_DOCK` 排除;碎片过滤同 Windows 常量)。VM 实证:mousepad 开启时 `候选窗口 1 个`,悬停预选亮起+虚线框贴合外框,单击采纳出浮条;`--smart-capture-flow` **RESULT failures = 0**(D1 自过滤/DPR 换算/命中测试/快照查表全过)。热键/自动粘贴此前已实证。**剩余:元素级(AT-SPI2)仍二期降级桩** | 元素级二期;窗口级闭环 |
| K6 | Linux 退出期堆损坏 `malloc_consolidate(): unaligned fastbin chunk` | **2026-11-08 升级:VM 上不只是退出期**——同一晚七个验收流**每个都被命中**(SIGSEGV sig=11 → SIGABRT sig=6),时机随机:smart-capture/stitch-live/editor 在 RESULT 之后、note-flow 首跑在 STEP2b **中途**(重跑未复现)、stitch-flow 在末条 ok 与 RESULT 之间;栈尾帧全在 libQt6Core(+0xbf60b 等静态析构段),note-flow 的栈写入本身再崩导致帧丢失。**命中前断言全部通过**——功能验收不受影响,但真机用户长时间驻留下的稳定性是真风险 | 待查(升优先级):VM 装 gdb + 开 core(`ulimit -c unlimited` + core_pattern)抓首犯;或 ASan 重编单流跑;先怀疑与 Qt 6.9 xcb + Software 渲染拆卸序、arboard 全局 X 连接泄漏策略的相互作用 |
| ~~K8~~ **已解决(2026-11-07)**:热键线程自有 X 连接(XOpenDisplay + XNextEvent 循环 + select 心跳),注册/解抓经命令队列同步等待(保留失败项 toast 语义),KeyPress 线程内匹配 → invokeMethod 队列投递;守护线程 detach 防 static 析构 terminate。**E2E:xdotool 注入 Ctrl+Shift+V → `热键触发: id=45057 action=0`**,5 热键注册全成功 |
| ~~K7~~ **已解决(2026-11-07)**:根因 = `--data-dir` 实例的 ensure() 被跳过(不参与单实例门禁,Windows 同语义)→ 通道名为空 → `listen("")` 报 Name error;**并非残留/权限问题**。修复:publishWindow 空名守卫跳过发布(此类实例本无被唤起入口)。终验:--data-dir 路径 Name error 归零;默认路径 socket `/tmp/nextboard-si-<hash>` 成功创建;第二实例被拒+正常退出 |
| K9 | ~~双平台日志 0 字节 / Info 级丢失~~ **已解决(2026-11-08)** | **根因双侧**:① Qt 6.9 QIODevice 未加 `Unbuffered` 时自带用户态写缓冲——`QFile::write` 不产生 write(2) 系统调用,Info 行要等 Warn+ flush 或析构才落 OS("Info 交给 OS 页缓存"注释的前提不成立);② Rust 侧 8KB `BufWriter` 仅 Warn+ flush。干净启动零 WARN + 强杀退出 = 两份日志全 0 字节(VM 实测;strace 铁证:openat 与时间戳格式化俱在、write 为零)。Windows 未炸属侥幸——验收路径几乎必现 WARN flush 或正常退出析构 | 宿主 `openLogFileAppend` 加 `QIODevice::Unbuffered`(main.cpp 的注释随之真正成立);核心 `BufWriter`→`LineWriter`(逐行 write(2))。strace 复查:fd6 23 次/1882B、fd7 4 次/247B;Windows 408 passed / 0 failed |
| K10 | 测试 VM 注入键全灭(xdotool/XTest 不达、XGrabKey 零事件) | **xfce4-screensaver 未锁屏也用被动抓取吃掉全部键盘注入**(2026-11-08 逐个击破:杀之 KeyPress 立达;xev 每键 `FocusOut NotifyGrab` 舞步消失;xfwm4/xfsettingsd 已排除) | 测试 VM 移除该包(vm_capture.sh 已自动处理);真实用户机锁屏器仅锁屏态抓取,不影响日常 |
| K11 | ~~auto-paste fail-safe 误拒(xfwm4)~~ **已解决(2026-11-08)** | **根因 = 焦点竞态 + 口径差**:面板 hide 后 X 焦点按 RevertToPointerRoot 回退,xfwm4 可整秒不设显式焦点——`XGetInputFocus` 单发(+80ms)必得 PointerRoot → 拒注;而 xdotool 探针会把 PointerRoot 解析到指针下窗口(同一时刻"看得到" mousepad) | `resolveForegroundPid`:① `_NET_ACTIVE_WINDOW` 优先(WM 权威口径)→ ② PointerRoot 时按指针解析 + 沿框架向上爬找 `_NET_WM_PID`;外加 ≤1s 焦点就绪轮询。fail-safe 语义不变(绝不盲注)。E2E 铁证:清空文档后文本凭空重现 + `已投出 Ctrl+V(XTest)` 日志 | |
| K12 | ~~翻译 panic:nb_call 执行异常(panic 已拦截)~~ **已解决(2026-11-08)** | **根因 = ureq 3.4 的 TLS provider 默认永远是 Rustls**(源码 `#[default]` 标注;`NativeTls` 注明 "never picked up automatically")——只开 native-tls feature 不在 Agent 里显式设置时,**任何 https 请求都 panic**。Linux 翻译(microsoft edge)首发 https 才爆出;**Windows 的 SOCKS5+https 同样埋雷**(此前 E2E 全是 http:// fixture)。panic 拦截器只回固定文案,真因靠 stderr 的默认 panic 钩子拿到(RUST_BACKTRACE=1 + ctypes 探针复现) | ① `socks.rs::tls_provider()` 平台助手(if cfg!(windows) NativeTls else Rustls),两条 ureq 传输的 Agent 都显式 `.tls_config(TlsConfig::builder().provider(...).build())`;② Cargo.toml **按平台拆 feature**:Windows=native-tls(schannel,免 NASM——rustls/ring 在 MSVC 上要 NASM)、非 Windows=rustls(免 openssl,M4 打包减负)。探针:hello world→你好,世界(475ms);Windows 408/0 | |


横切:文档同步 —— 修订 `docs/01-architecture.md:23`("Linux/macOS 为非目标" → "Linux = 二类平台,X11 全功能 / Wayland 按能力降级");progress.md 按里程碑记录踩坑。

---

## 10. 逐文件 checklist

> 状态:☐ 待办 / ☑ 平台无关无需改。档位对应 §5;C++ 侧按功能域分组。

### C++ 宿主

| 文件 | Windows 依赖 | Linux 方案 | 档位 |
| --- | --- | --- | --- |
| `main.cpp` | windows.h:26;栈回溯 :111-175;SEH :179-209,340-341;门禁 :297-307;setNotifyHwnd :668 | backtrace/sigaction//proc/self/maps;门禁 A 档;通知改 fd | B/A |
| `host/hotkeys.cpp/.hpp` | RegisterHotKey 全套;VkKeyScanW;WM_HOTKEY 过滤器(hpp:7 接口污染) | IPlatformHotkeys + X11 XGrabKey / portal | B |
| `host/auto_paste.cpp/.hpp` | SendInput :24-70;前台判断 :83-93;UIPI 约束 hpp:9-12 | X11 XTest;Wayland 降级"仅复制" | C |
| `host/single_instance.cpp/.hpp` | CreateMutexW :56-61;共享节 :71-88;PostMessage :100-115 | QLockFile + QLocalServer/Socket | A |
| `host/auto_start.cpp` | HKCU Run 全函数 | XDG autostart .desktop | A |
| `host/core_bridge.cpp/.hpp` | LoadLibraryW :46-53;HWND 通知 hpp:99-109,123,292;__cdecl typedef hpp:20-70 | QLibrary;通知协议重设计;调用约定默认 | B |
| `host/window_effects.hpp` | DWM 圆角 :20-28;NCHITTEST :41-70 | QSS 圆角;startSystemResize | A/B |
| `panel/panel_window.cpp/.hpp` | SetWindowPos 置顶 :101-114;NCLBUTTONDOWN :361-374;nativeEvent 五分支 :584-644;WinIdChange :646-655;HWND 缓存 hpp:179,260 | X11 property 置顶;startSystemMove;WM_APP 分支→信号;WindowDeactivate | A/B |
| `note/note_window.cpp/.hpp` | setTopMost :348-358;NCLBUTTONDOWN 缩放 :852-866;EXITSIZEMOVE :1241-1261;DPI 时序 :1212-1224;贴边/半隐藏/多屏纯 Qt | 手动 resize 或 startSystemResize + Move/Resize 收尾;置顶同上 | B |
| `capture/capture_session.cpp` | hwndToExeName :60-75;WindowFromPoint :1768-1784;UIA 时序注释 :489-512 | /proc/pid/exe + _NET_WM_PID;Wayland 来源为空 | B |
| `capture/smart_capture.cpp/.hpp` | EnumWindows/DWM/UIA 全套(:43-178,344-812) | X11 EWMH(一期窗口级);AT-SPI2(二期);Wayland 空候选 | C |
| `capture/stitch_hud.cpp/.hpp` | WS_EX/WDA :50-105;RegisterHotKey Esc :249-290(**唯一 Q_OS_WIN 文件**) | X11 shape/摆位;热键抽象;保留 #else | B |
| `host/quick_commands.cpp` | ShellExecuteW/IShellLink/HICON/ms-settings 表(:30-50,90-203,339-412) | 整文件重写(接口保留) | C |
| `host/app_paths.cpp/.hpp` | GetModuleFileNameW :33-37 | /proc/self/exe | A |
| `host/memory_probe.cpp` | psapi :21-30 | /proc/self/status | A |
| `host/core_facade.cpp` | explorer /select(`:825-838`) | DBus org.freedesktop.FileManager1 / xdg-open | A |
| `host/hotkey_lifecycle_flow.cpp` | 手工构造 MSG(:28,51-80) | 按平台重写验收流 | B |
| `host/smart_capture_flow.cpp` | Chrome_WidgetWin_1(:1031)等探针 | WM_CLASS 判定/平台化 | C(验收) |
| `host/stitch_live_flow.cpp` | CreateRectRgn 探针 :235-238 | 平台化或删除 | A |
| `host/capture_toolbar_flow.cpp` | 残留 include :46 | 删除 | A |
| `host/tray.cpp` | 无 Win32 | StatusNotifierItem 验证 | A |
| `pin/pin_window.cpp` | 无 Win32 | — | ☑ |
| `editor/*`、`ui/*`、`note/note_registry`、`image_save`、`log_rotate`、`app_font`、`ui_theme`、`overlay_geometry`、`annotation_*`、`clip_entry_model`、`qml_bootstrap` | 无 Win32 | — | ☑ |
| `app/CMakeLists.txt`、`app.manifest`、`resources/nextboard.ico` | §7.1 | 条件化;manifest 弃用;补 hicolor 图标 | A |

### Rust core

| 文件 | 平台依赖 | Linux 方案 | 档位 |
| --- | --- | --- | --- |
| `Cargo.toml` | clipboard-win :28(无条件);ureq native-tls :32 | R1/R2 | 硬阻断 |
| `clip.rs` | Win32 剪贴板全套(:39-135,196,282-297,430-439,952-997);not(windows) 空转 :137-146 | arboard 重写;Wayland data-control 监听重设计 | 硬阻断 |
| `error.rs` | `#[from] clipboard_win` :16 | 随 R1 | 硬阻断(随) |
| `translate/http.rs` | WinHTTP 全文无门控 | 切 ureq(直连/HTTP 代理分支) | 硬阻断 |
| `ocr/system.rs` | Windows.Media.Ocr(cfg 内,安全) | Linux 返回不支持;**默认后端改 RapidOCR**(B1) | 行为级 |
| `ocr/rapid.rs` | ORT_DYLIB 常量已备 :33-36;模型路径平台无关 | libonnxruntime.so 放 exe 同级 | ☑(≈0) |
| `core.rs` / `ffi.rs` | PostMessageW 通知 :16-22,37(空实现门控已有) | nb_set_notify_fd 重设计(与 C++ 联动) | B |
| `capture.rs` | xcap;monitor name `\\.\DISPLAYn` :14,22,348 | Linux 实测 + 配对契约重验(B2) | B |
| `settings.rs` | data_dir is_absolute :782 | B3 清洗 | 行为级 |
| FFI/panic/SQLite/zip/stitch/todo/日志 | 平台无关 | — | ☑ |
| `nbcore-tests` | 无平台假设(真机冒烟 2 处 #[ignore]) | Linux 跑全量 | ☑ |

---

## 11. 风险登记

| # | 风险 | 影响 | 缓解 |
| --- | --- | --- | --- |
| K1 | **arboard 的 Wayland data-control 成熟度**(位图/监听语义与 Win32 差异大) | R1 选型翻车 → 重选型 | spike 先行(§12.1);保底方案 x11rb + wayland-rs 自写 |
| K2 | xcap Linux 采集质量(多屏/DPR/性能)未知 | M2 排期风险 | spike 实测;保底自实现 XShm |
| K3 | portal GlobalShortcuts 的 DE 覆盖(KDE 有,GNOME 43+ 才有) | Wayland 热键体验 | X11 全功能兜底;探测降级 |
| K4 | AT-SPI2 元素树能力与 UIA 不对齐(Chromium 导出程度) | 智能框选元素级质量 | 二期做,一期窗口级 + 手动框选兜底 |
| K5 | AppImage 基线 glibc 与 ORT so 的兼容(需较新 glibc) | 分发覆盖面 | 旧基线系统构建或文档声明最低发行版 |
| K6 | QML + 软渲染(Windows 默认软渲染收内存)在 Linux 的行为差异 | 内存预算失守 | M0 即测 LLVMpipe/软渲染路径,重核 docs/06 预算 |
| K7 | 双平台长期并行的回归负担 | 维护成本 | 平台层接口冻结 + 现有 Windows 验收流作为回归基线 |
| K8 | 软件授权/分发渠道(GNOME 扩展依赖、AppImage 商店) | 触达 | 非技术,随发布定 |

---

## 12. 先行选型验证(spike)清单

| # | Spike | 验证点 | 产出判据 |
| --- | --- | --- | --- |
| 0 | **编译普查**(`tools/linux_census.sh`,零代码改动) | cargo check/test + cmake/ninja 的真实阻塞清单;环境指纹(glibc/Qt 版本) | **已完成(2026-11-04)→ §4.3**,M0 排期确认 |
| 1 | **arboard 双协议** | X11 与 Wayland(KDE)下文本/位图写入读取、剪贴板变更通知可行性与语义 | **X11 腿已完成(2026-11-07)→ §12.1**;Wayland 腿待 KDE VM |
| 2 | xcap Linux | 多屏枚举 name 语义、DPR、与 QScreen::name() 配对、采集性能 | **WSLg 部分完成(2026-11-07)→ §12.2**:枚举/契约✅,像素级验证待 KDE VM |
| 3 | ORT Linux | libonnxruntime.so(1.28/api-28)+ 现有四件模型跑通 det/cls/rec,精度与耗时 vs Windows | 体积、基线 glibc |
| 4 | X11 热键 | XGrabKey 经 Qt native event filter 的最小 demo(修饰符组合、冲突检测) | IPlatformHotkeys 接口定稿 |
| 5 | AT-SPI2 调研 | Chromium/GTK/Qt 的元素树导出程度、与 UIA 能力对照 | 智能框选二期能否立项 |
| 6 | 打包链 | linuxdeploy + Qt 6.9 AppImage 最小可运行包(Qt Quick/QML 插件齐全) | M4 骨架与体积预算 |

### 12.1 Spike 1 结论(2026-11-07,WSLg:X11=XWayland / Wayland=Weston)

**读写层:arboard X11 选型成立;Wayland 受合成器限制不可用(预期内,不阻塞 v1 的 X11 基线)**

| 项 | X11(XWayland) | Wayland(Weston) |
| --- | --- | --- |
| 文本 set/get | ✅ | ❌ |
| 位图 set/get | ✅;X11 target = `image/png`(+SAVE_TARGETS),xclip 读到合法 PNG 魔数 | ❌ |
| 跨进程互访 | ✅(xclip 作裁判) | ❌ wl-copy/wl-paste **静默挂死** → Weston 未实现 zwlr_data_control |
| arboard 后端选择 | ✅ | ❌(wayland 后端失败后**静默回退 X11**,最终报"X11 超时"——错误被掩盖;R1 接口要显式选后端、避免误诊) |

**监听层 = R1 的真正工作量(arboard 无监听 API,需自建)**

| 候选 | 实测 |
| --- | --- |
| X11 XFixes 事件 | ❌ WSLg XWayland 上**零事件**(xclip / arboard 两种所有权来源均试)→ 判为 WSLg 特有,**真 X11 待 KDE VM 复测** |
| Wayland data-control 事件 | 无协议支撑(Weston),VM 复测 |
| **轮询(200ms 步进)** | ✅ **当下唯一可用通道**:t≈1.0s 的变化在下一个 200ms 步进内捕捉 |

**R1 设计结论(选型定稿输入)**:
1. 读写层选 arboard(X11 路径);位图走 PNG 编码(无 CF_DIBV5 直写等价物,编码内存/CPU 开销入 R1 评估);
2. 监听接口按"**事件优先、轮询兜底**"双通道抽象:X11 监听器做成可插拔,探测不到事件(WSLg/XWayland)自动降级轮询,保证任何环境可用;
3. X11 选择所有权由常驻进程持有——与 Nextboard 常驻形态天然匹配;WSLg 的剪贴板缓存行为(源进程退出后文本仍可读、位图不可读)记录备查,真机以 VM 实测为准;
4. spike 代码存 `census/spikes/clip-spike/`(gitignore 内,主工程零改动),报告 `census/spike1-report.md`。

> 待办(随 KDE VM):① XFixes 事件复测 ② arboard Wayland 后端(KDE 实现 data-control,预期可用)③ 多屏与 DPI 下的位图语义。

### 12.2 Spike 2 结论(2026-11-07,WSLg:X11=XWayland / Wayland=Weston)

**核心发现:WSLg 的 XWayland 是 RDP 镜像后端,服务端拒绝 GetImage——root 与普通窗口皆拒,xwd 独立复现(`BadMatch, X_GetImage`),非 xcap 缺陷。像素级采集验证必须 KDE VM。**

| 项 | 结果 |
| --- | --- |
| 显示器枚举 | ✅ name = XRandR 输出名(`XWAYLAND0/1/2`),几何/scale/freq/primary/is_builtin 可得 |
| B2 配对契约 | **方向明确**:Qt X11 的 `QScreen::name()` 与 xcap 同源(都是 XRandR 输出名),配对比 Windows 的 `\\.\DISPLAYn` 更直接;WSLg 虚拟布局下几何重叠失真,真机复测 |
| root/全屏采集 | ❌ 服务端拒绝 GetImage(xwd 独立复现)→ WSLg 平台限制;真 X11 上 GetImage(root) 是 xwd/scrot/import 全生态的基础路径,**预期可用,VM 必测** |
| capture_region / 30 帧性能 / DPR | ⏸ 被同一限制挡住,未测 |
| 窗口枚举(z 序) | ❌ `_NET_CLIENT_LIST_STACKING` 缺失(WSLg 无 WM)→ 智能框选空候选降级契约再次生效;真 KDE 预期可用 |
| 窗口级捕获 | ⏸ 同 root;xcap 源码(0.9.8 `xorg_capture.rs`)确认其 X11 实现是标准 `GetImage ZPixmap` 路径,含 8/16/24/32bpp 解码 |
| Wayland 腿 | ❌ monitors 失败 + shot 挂死(Weston 无 zwlr_screencopy,预期内;xcap 的 Wayland 路径依赖 wlroots 系协议) |

**对里程碑的影响**:
1. M2 风险定性从"xcap 可能不支持 Linux"降为"WSLg 测不了,真 X11 预期可行(标准路径),VM 必测"——**KDE VM 从 M3 前提前为 M2 前必建**(§8.3 环境矩阵已修正);
2. 智能框选在无 WM 环境(WSLg)走空候选降级,与 Wayland 降级策略同构,设计不用改。

**环境清单增量(Fedora 系,M0/M2 构建+链接需要)**:`dnf install mesa-libgbm-devel mesa-libEGL-devel xclip wl-clipboard xrandr xwininfo xwd`(并入 §4.3 的清单)。

> 原始产物:`census/spike2-run.log`、`census/spike2-rootdiag.log`;spike 代码 `census/spikes/capture-spike/`。

---

## 附:术语

| 术语 | 含义 |
| --- | --- |
| EWMH | Extended Window Manager Hints(X11 窗口管理器协议:_NET_CLIENT_LIST 等) |
| AT-SPI2 | Linux 无障碍服务协议(org.a11y.atspi),UIA 的 Linux 对应物 |
| data-control | Wayland 剪贴板协议(wlr-data-control / zwlr),arboard 的 Wayland 后端 |
| portal | XDG 桌面门户(GlobalShortcuts/RemoteDesktop/Screenshot 等) |
| WDA_EXCLUDEFROMCAPTURE | Win32 窗口属性:窗口不进入屏幕捕获 |
| XTest | X11 测试扩展:合成键盘/鼠标输入(自动粘贴用) |
