# 开发进度日志

> 按里程碑记录关键决策与实测数据,保持第一代 phase 文档的习惯。

## 2026-10-05 S1(nbcore.dll)完成

### 环境(S0)

- Rust 1.98.1(x86_64-pc-windows-msvc)/ VS2022 MSVC 14.44 / Qt 6.9.0 msvc2022_64 / CMake 4.0.1。
- clipboard-plus 定版 tag:`nextboard-baseline-20261005`(工作区尚有 7 行未提交改动,以 tag 为准)。
- Sciter 许可:本机网络受限无法直连 sciter.com 获取原文;检索结果显示其许可为
  "binary 免费使用 + 特定条件商业授权"模式。**风险 R2 仍开放**,S2 嵌入 PoC 前须由
  用户到官网确认(见 03-roadmap.md S0.1)。

### 里程碑

| 步骤 | 结果 |
| --- | --- |
| M1.1 error+store 平移 | 26 单测全绿;error.rs 去 Tauri 化(去 Store/Clipboard/Hotkey/Tauri 变体,增 Json/Panicked) |
| M1.2 translate+ocr+settings+capture+screen | 141 单测全绿;translate/window.rs 的窗口逻辑不迁移,历史改为模块级内存队列;capture 只迁数据层(枚举/捕获/裁剪),会话归 C++ |
| M1.3 clip 事件驱动化 + schema v5 | 160 单测全绿;监听改为 AddClipboardFormatListener 消息窗口;v5 迁移加 source/tags |
| M2 FFI 门面 | nb_init/nb_call/nb_version/nb_abi_version/nb_free_string/nb_free_buffer/nb_shutdown/nb_capture_monitor/nb_capture_crop;panic 全部 catch_unwind |
| M3 C++ 冒烟宿主 | 11/11 PASS(含真机剪贴板中文写读、错误隔离、shutdown 后拒调) |

### 实测数据

- `cargo build --release`:**50.6s**,nbcore.dll **3.65MB**(x64,strip+lto+opt3)。
- 单测:160 passed / 5 ignored(原第一代 OCR 探测类真机用例在 CI 环境跳过)。
- 冒烟宿主 C++ 编译即通过 MSVC 链接 nbcore.dll.lib(自动生成导入库,无需 .def)。

### 踩坑记录

1. **clipboard-win 的 ErrorCode 需要 `std` feature**:第一代由 tauri 传递启用,
   剥离后必须显式声明(错误表现为 thiserror 的 `#[from]` 报 AsDynError 不满足)。
2. **windows 0.62 API 位置与签名**:AddClipboardFormatListener/RemoveClipboardFormatListener
   在 `Win32::System::DataExchange`(不是 WindowsAndMessaging);PostMessageW 的 hwnd
   参数是 `Option<HWND>`;clipboard_win 的 `raw::size` 返回 `Option<NonZeroUsize>`。
3. **release profile 的 panic 必须 unwind**:已在 core/Cargo.toml 注释固定;
   全部导出函数 catch_unwind,冒烟验证非法入参返回 `{"ok":false,"error":{…}}` 而非崩溃。
4. **serde_json 字段序**:to_value 的 Map 按字母序输出,宿主断言不要写死字段顺序。
5. **MSVC 编译含中文的 C++ 源码**:加 `/utf-8`(默认代码页 936 报 C4819 且可能错编译)。

### FFI 方法表现状(ABI v1)

已实现:list_entries / entry_counts / get_entry / delete_entry / clear_history /
set_pinned / copy_text_to_clipboard / read_clipboard_text / get_settings /
save_settings / translate_text / translate_history / clear_translate_history /
translate_probe / proxy_probe / ocr_probe / ocr_entry / ingest_capture_png;
另加 nb_capture_monitor / nb_capture_crop(RGBA 通道)。

第一代 command 未纳入 v1(按 02 §4.2 移交 C++ 或后续迭代):overlay_*/editor_*/
commit_capture/capture_*/create_pin 系列/save_entry_*/reveal_entry/
save_text_file/open_translate_window/translate_init/suspend_hotkeys 等。

### 下一步(S2)

Qt 宿主骨架:进程壳(托盘/热键/单实例)→ Sciter 嵌入 PoC → 遮罩 PoC(QImage 直绘
nb_capture_monitor 的像素)→ 贴图 PoC。

---

## 2026-10-06 S2(Qt 宿主骨架 + 三窗口 PoC)完成

### 里程碑

| 步骤 | 结果 |
| --- | --- |
| S2.1a Qt 壳 | 单实例(命名互斥量)+ 托盘(菜单含暂停监听,读写 nbcore 设置)+ 文件日志 |
| S2.1b 热键 + nbcore 集成 | RegisterHotKey 管理,6 组合键全部注册成功;CoreBridge 校验 ABI 版本 |
| S2.3 Sciter 嵌入 | **mixin 模式**:Qt 顶层 HWND 上经 SciterProcND 驱动引擎,主面板 HTML 加载成功 |
| S2.4 遮罩 PoC | OverlayWidget 冻结底图 + 半透明罩 + 框选亮框 + 尺寸提示 + Esc 取消 |
| S2.5 贴图 PoC | PinWindow 置顶/拖动/滚轮缩放/Alt+滚轮透明度/双击关闭 |

### 关键决策与踩坑

1. **Sciter v6 嵌入采用 mixin 模式(SciterProcND)而非 SW_CHILD 子窗口**——SDK 明确
   子窗口模式"not anymore supported"。Qt 对接:PanelWindow::nativeEvent 把顶层窗口
   消息先转发 SciterProcND,handled 则拦截;SC_LOAD_DATA 回调兜底 this://app/ 资源。
2. **互斥量名禁用自定义 "目录\名称" 前缀**:本环境下 `Nextboard\xxx` 形式的名字
   CreateMutexW 返回 NULL + ERROR_PATH_NOT_FOUND(同名调用在 PowerShell 中正常,
   怀疑与沙箱/作业对象命名空间限制相关)。改用 plain name 解决。
3. **GetLastError 时序**:CreateMutexW 与 GetLastError 之间任何分配/释放(含临时
   std::wstring 析构)都会覆盖 last-error——先拼好名字、立即取错误码。
4. **windeployqt 部署 Qt 运行时**;sciter.dll(18.4MB,D2D 版)与 nbcore.dll 由
   CMake POST_BUILD 自动拷贝;panel/ HTML 目录同样部署。
5. **EULA 合规**:主面板 footer 已含 "Uses Sciter Engine (https://sciter.com)…" 署名。

### 运行验证(真机)

- 进程稳定存活(6s+ 多轮);nextboard.log 无错误;
- nbcore 初始化 OK(数据目录 %LOCALAPPDATA%/Nextboard);
- 6 个全局热键注册成功(未与其他程序冲突);
- Sciter 引擎附着主面板窗口并加载 panel/main.htm。

### 遗留(S3 处理)

- 遮罩框选确认后的 nb_capture_crop 裁剪 + ingest 入库接线(坐标已回传);
- Sciter ↔ C++ 桥(sciter::om::asset)打通 list_entries 数据驱动列表;
- 贴图接 pinLatest 数据查询;编辑器窗口(QGraphicsScene,S4);
- 无边框主面板的拖拽区域(HTML 上 data-role="window-drag" 或 native 拦截);
- 滚轮事件未转发引擎(S3 列表滚动时补)。

---

## 2026-10-06(二)S2.3 界面渲染修复 + 构建脚本

### "运行后没有正常界面"排障实录

现象:exe 启动、进程存活、日志全绿,但屏幕上看不到界面。用 PrintWindow/
CopyFromScreen 截图逐步定位,先后修掉四个叠加的问题:

| # | 问题 | 修复 |
| --- | --- | --- |
| 1 | **启动即退出**(偶发):主面板从未获得焦点时失焦隐藏 200ms 触发 hide(),唯一窗口关闭 → 应用退出 | blur-hide 仅在 `m_everFocused` 后启用;`setQuitOnLastWindowClosed(false)` 常驻托盘(第一代语义) |
| 2 | **窗口完全透明**:mixin 模式(SciterProcND)下 Qt 拥有 WM_PAINT/backing store,引擎 GDI 绘制被 Qt 合成覆盖,两套绘制管线打架 | **弃用 mixin,改 ProcX 位图模式**(官方 demos.lite/lite-bitmap 的 Qt 适配):引擎渲染进共享 DIB 表面,Qt paintEvent 里 drawImage —— 渲染路径唯一 |
| 3 | **ProcX 表面接错时序**:resizeEvent 早于 attach 发生,`SXM_SIZE`(交像素指针)被跳过,引擎渲染进空表面 → 画面恒为初始底色 | attach() 内补发 `SXM_SIZE`;paintEvent 先发 `SXM_PAINT`(引擎按需渲染)再 drawImage |
| 4 | **DPI 混乱**(1.75x 放大渲染/文字忽大忽小)与中文乱码:工程无 DPI 声明,坐标被虚拟化;HTML 无 charset | 嵌入 per-monitor V2 manifest(CMake `/MANIFESTINPUT`);`<meta charset="utf-8">` |

最终验收截图:主面板 800×600 完整渲染(标题/搜索框/提示文案/Sciter 署名页脚,
中文正常)。

### ProcX 模式的 Qt 适配要点(工程范式)

- 表面:`QImage(Format_RGB32)`,把 `bits()/bytesPerLine()` 经 `SXM_SIZE` 交给引擎;
- 渲染:paintEvent 里 `SXM_PAINT(rc)` → `QPainter::drawImage`(同进程共享像素,零拷贝);
- 心跳:QTimer 16ms 发 `SXM_HEARTBIT`(CSS 动画/计时器驱动);
- 输入:Qt 事件 → `SXM_MOUSE`/`SXM_KEY`(按钮/修饰键按位掩码组装);
- 失效:SC_INVALIDATE_RECT 回调 → QWidget::update(rect) 触发重绘;
- 注意:`SciterProcND(WM_CREATE)` 在 Qt 宿主下**不创建引擎**(实测 version=0 空转),
  必须走 ProcX 的 `SXM_CREATE(SL_TARGET_BITMAP, FALSE)`。

### build.ps1 部署选项

- 自动判定:`platforms\qwindows.dll` 缺失即自动 windeployqt(此前只查
  Qt6Widgets.dll,平台插件缺失时 exe 弹窗报错或无窗口);`-Deploy` 强制重跑;
- 坑 1:PowerShell 5.1 要求脚本**带 BOM UTF-8**(编辑工具会剥 BOM,中文注释崩解析);
- 坑 2:链接期 LNK1104 = 旧 nextboard.exe 未退占文件,先杀进程再构建;
- 坑 3:自动化/无人值守会话里 activateWindow 常失败,依赖焦点初态的逻辑必须有
  "从未聚焦"降级路径。

### 下一步(S3)

主面板功能:sciter::om 桥(list_entries 驱动列表)→ 分页/分类/缩略图 → 搜索 →
设置视图 → 翻译抽屉。

---

## 2026-10-06(三)S3 主面板数据驱动完成

### 里程碑

| 步骤 | 结果 |
| --- | --- |
| S3.1 Sciter 桥 | `NextboardAsset`(sciter::om::asset)+ SciterSetGlobalAsset → JS 全局 `Nextboard.*`;nbcore JSON 响应 ↔ sciter::value 经 CVT_JSON_LITERAL 整体互转 |
| S3.2 面板五视图 | main.htm 重写:分类标签(计数徽标)/卡片列表(时间/大小/类型/固定/来源徽标)/设置抽屉/翻译抽屉/toast |
| S3.3 图片通路 | `Nextboard.imageUrl(id, thumb)` → blobs 绝对路径 file:// URL(缩略图 .thumb.png 命名对齐 core 缓存) |
| S3.4 设置/翻译联动 | 设置读改写 save_settings;翻译抽屉走 translate_text(三类 Provider 就绪) |
| S3.5 验收 | 自截图确认:分类计数、3 条真实条目卡片、中文渲染、时间/大小格式化全部正常 |

### nbcore 增强

- 新增 `copy_entry` FFI 方法:按条目类型写剪贴板(文本 Unicode / 图片 PNG→CF_DIBV5 /
  文件 CF_HDROP),与第一代 commands.rs 的 copy_entry 语义对齐;
- 新增 `write_files_to_clipboard`(clipboard-win 的 `set(formats::FileList,…)` 泛型
  推导有缺陷,改用 `raw::set_file_list` + Clipboard 重试 guard);
- 160 单测保持全绿。

### 关键踩坑(S3)

1. **CoreBridge 双实例**:main.cpp 栈实例 `CoreBridge core` 与桥的
   `CoreBridge::instance()` 是两个对象,桥侧永远"未初始化"→ 列表加载失败且无错误
   消息。修复:main 改用 `CoreBridge::instance()`,全进程唯一。
2. **Sciter `document.ready` 是赋值式回调**(`document.ready = function(){}),
   不是 Promise(`.then()` 静默无效)—— 列表卡"加载中"的直接原因。
3. **Sciter CSS 弹性单位是 `*`**(`width: 1*`),不是 CSS3 的 `flex: 1` —— header
   搜索框塌陷的原因。
4. **HTML 不在 CMake 依赖里**:改 main.htm 后 POST_BUILD 不重拷,build/panel 里是
   旧 UI。已加 `panel_assets` 自定义目标把 main.htm 纳入依赖图。
5. **PrintWindow 不可信**:DWM 缓存旧帧,RedrawWindow 强刷也无效;最终用
   `--screenshot` 参数让应用自己 `QWidget::grab()`(触发 paintEvent → SXM_PAINT →
   引擎重渲),成为自动化验收的可靠手段。
6. **SciterEval 在 ProcX(SL_TARGET_BITMAP) 引擎实例上不可用**(实测 `1+1` 返回
   FALSE、无主 VM)—— 注入式验收路线放弃,`setSearchText` 接口保留待
   sciter::host::call_function(需主 VM)再启用;搜索链路的正确性由 nbcore 的
   FTS/中文兜底单测保障。

### 自动化验收设施

`nextboard.exe --screenshot <path> [--query <text>]`:启动 → 延时 5s 自截 → 退出。
截图经 widget grab(含 Sciter 表面),是唯一可信的画面取证方式。

### 遗留(S3 收尾/S4)

- ~~搜索框 UI 注入验收~~ → QML 版由 `PanelWindow::applySearchText` + `--query` 覆盖;
- ~~滚轮事件转发引擎(列表滚动)~~ → QML 化后由场景图直接派发,无需宿主转发
  (EntryCard 的 MouseArea 显式 `wheel.accepted = false` 转交列表,防回归);
- ~~分页加载更多(loadMore)~~ / ~~条目右键菜单~~ → 已补齐:菜单含
  复制 / 翻译 / 识别文字 / 固定 / 删除;loadMore 的 append 分支改为以服务端 total 收敛;
- ~~翻译抽屉的语言选择持久化~~ → 落 `settings.translateTargetLang`(打开载入、切换回写);
- 设置页自定义数据目录(需 nbcore 以新数据目录重载,未做);
- S4:编辑器(QGraphicsScene)、commit_capture 闭环、按条目贴图 FFI、定位文件(reveal)。

---

## 2026-10-06(四)剪贴板"不可用"排障 + 通知链路补全

### 现象与定位

用户报告"剪贴板无法使用"。分层排查:

| 检查项 | 结果 |
| --- | --- |
| nbcore 监听线程注册 | ✅ `nbcore.log`:"剪贴板监听注册: true / 监听线程就绪"(新加的文件日志) |
| 进程内剪贴板读写 | ✅ 冒烟宿主 copy_text/copy_entry 全过 |
| 数据库内容 | ✅ 历史测试条目全部在库 |
| **UI 实时刷新** | ❌ **`notify_hwnd=0`,核心的 WM_APP 通知被丢弃,面板只在启动时拉一次数据** |

**根因**:通知链路断在最后一环。监听/入库一直正常,但用户复制的新内容永远不会
出现在列表里 —— 表现为"剪贴板无法使用"。

### 修复(通知链路全通)

```
nbcore 监听线程 → PostMessage(面板HWND, WM_APP_CLIP_CHANGED)
  → PanelWindow::nativeEvent → emit clipChanged()
  → pushNativeNotification("clip_changed") → 桥队列
  → JS setInterval(1s) takeNotifications() → refresh()
```

- nbcore 新增 `nb_set_notify_hwnd` 导出 + `core::set_notify_hwnd`(运行期更新);
- CoreBridge 新增 `setNotifyHwnd`;main.cpp 在面板创建后接线;
- 桥新增通知队列(`takeNotifications`,互斥锁保护)+ JS 1s 轮询。

### 顺带修复

- **nbcore 日志缺失**:log crate 无 subscriber,监听/入库日志全丢。
  新增 `init_logging`(极简文件日志器 → 数据目录 nbcore.log),nb_init 首行调用;
- `--screenshot` 增加 `--query` 参数(搜索注入接口预留);
- 清理全部排障临时脚本。

### 环境限制(重要)

本自动化会话(沙箱)**禁止跨进程剪贴板访问**:纯 PowerShell 连写两次都失败、
python OpenClipboard 全败、GetOpenClipboardWindow 返回 0 —— 与 nbcore 无关。
因此"外部复制 → 面板出现条目"的端到端验证**需要用户在真实桌面手动执行**:
启动 nextboard.exe → 随便复制一段文字 → 1 秒内面板列表应自动出现该条目。
(进程内读写已被冒烟宿主与单测覆盖,监听注册已被 nbcore.log 确认。)

---

## 2026-10-06(五)主界面按钮不可点击修复

### 现象

主面板上的按钮(设置/翻译/分类标签)点击无反应。

### 根因(两个)

1. **Sciter.JS 的选择器委托事件必须带 `^` 前缀**:`document.on('^click', selector, fn)`
   ——我们写的 `document.on('click', selector, fn)` 静默无效(SDK gsciter/sys-info.htm
   官方示例确认此语法)。面板里全部按钮/卡片/标签绑定因此失效。
2. **MOUSE_UP 的按钮位错误**:release 事件里 `e->buttons()` 已不含该键,
   UP 消息的 buttons=0,引擎判定按钮不匹配可能丢弃点击完成序列。
   修复:UP/DCLICK 用 `e->button()`(单键),DOWN/MOVE 用 `e->buttons()`。

### 修复内容

- main.htm:全部委托事件加 `^` 前缀(卡片/标签/设置/翻译/保存/关闭/复制译文);
- sciter_view.cpp:sendMouse 按 UP/DCLICK vs DOWN/MOVE 区分按钮位来源;
- 双击序列注释明确(DOWN→UP→DCLICK 替代第二次 DOWN)。

### 验收方式说明

沙箱环境的截图(PrintWindow 的 DWM 缓存帧、CopyFromScreen 被前台锁遮挡)与
UI 点击(mouse_event 受前台锁/焦点限制)都不可靠,自动化验收在该环境只能覆盖
"进程内可达"的部分(构建/运行/日志/自截图/DB)。交互类功能请手动验证:
点击设置按钮应打开抽屉;点分类标签应过滤;点卡片应复制并 toast;
列表滚动;复制新内容 1s 内自动出现。

### 下一步

S3 收尾(滚动转发/右键菜单/loadMore)→ S4 编辑器与截图闭环。

---

## 2026-10-06(四)面板迁移 Qt Quick/QML + S3 收尾

主面板由 Sciter 迁移到 Qt Quick/QML(`QQuickWidget` 承载,提交 `a71275d`)。迁移
只搬了 UI,事件接线未同步,导致面板交互闭环断裂;本轮先补闭环,再收尾 S3。

### 一、交互闭环缺陷(同一类:迁移遗留的接线缺失)

| # | 缺陷 | 证据 | 修复 |
| --- | --- | --- | --- |
| 1 | ESC 无法收起面板 | 真机日志 `Main.qml:26 ReferenceError: panelWindow is not defined`——`PanelWindow` 从未注册进 QML | `setupUi()` 在 `setSource` **之前** `engine()->rootContext()->setContextProperty("panelWindow", this)` |
| 2 | 全部 toast 静默失效 | 全项目 `grep Connections` 为空,`Core.toastMessage` 无人订阅 | Main.qml 增 `Connections` 订阅 toastMessage |
| 3 | 译文永不显示(翻译实际不可用) | 同上,`Core.translateFinished` 无人订阅 | TranslateDrawer 增 `Connections` 写入译文区 |
| 4 | 引用未定义的 `app` | `TranslateDrawer.qml:105 app.showToast(...)` | 改 `Core.showToast` |
| 5 | 复制条目后不收面板 | 第一代 `use-entries.ts` copy() 有 `getCurrentWindow().hide()` | 复制成功后 `panelWindow.hidePanel()` |
| 6 | `Ctrl+1..9` 序号复制缺失 | 第一代 `App.tsx` F2 | 新增 `ClipEntryModel::copyByIndex(int)` |
| 7 | 分类计数永不刷新 | `Core.entryCount(viewId)` 是无依赖的一次性绑定,且每次求值触发 5 次同步 DB 查询 | `CoreFacade` 新增 `counts(QVariantMap)` 属性 + `countsChanged`,模型在 `applyPage` 后 `refreshCounts()` |
| 8 | 来源徽标永不显示 | `store/entry.rs` 的 `get/list/list_via_fts/latest_pinnable` SELECT 漏取 `source`/`tags`,`from_row` 回退为 `"clipboard"` | 四处 SELECT 补齐列(`list()` 因此依赖 v5 schema,`migration_v4_indexes_existing_rows` 测试补跑 `MIGRATION_V5` 再断言) |

### 二、S3 收尾

- **滚动转发**:原为 Sciter 时代遗留(宿主需手动把滚轮转发进引擎)。QML 化后由
  场景图直接派发,问题在架构上消失;EntryCard 的 MouseArea 显式 `wheel.accepted = false`
  固定"卡片不吞滚轮"语义,防后续加 `onWheel` 时误吞滚动。
- **右键菜单**:对齐第一代 `App.tsx` ctx-menu —— 复制 / 翻译 / 识别文字 / 固定 / 删除
  (「贴到屏幕」「编辑」留 S4:缺按条目贴图 FFI 与编辑器)。翻译抽屉在 Main.qml 作用域,
  EntryCard 经 `signal translateRequested(string)` 上抛;文本条目取全文直填,图片提示
  先「识别文字」。
- **loadMore**:`applyPage` 的 append 分支原先不更新 `m_total`(仅首屏更新),翻页期间
  条目增删会让 `hasMore` 失真;改为两分支统一以服务端 `total` 收敛。
- **OCR 结果**:core 的 `ocr_entry` 只返回 `{text}`(不写条目、不写剪贴板),而门面提示
  写的是"已写入条目"——错误信息。对齐第一代:识别成功后把文本写入剪贴板并提示字符数;
  未识别到文字给出明确提示。
- **翻译语言持久化**:目标语言落 `settings.translateTargetLang`(打开载入、切换回写)。

### 三、验收与限制

- core 单测 142 通过(18 项 `translate::*` 环回测试因**沙箱禁止绑定 socket**失败,与改动无关);
- app 编译链接通过;`qmllint` 0 error(96 项 warning 均因 C++ 注册模块不在静态导入路径);
- **沙箱(含放开沙箱)无法启动 GUI**:`nextboard.exe` 卡在 `QApplication` 平台插件初始化,
  日志不增长,offscreen 平台亦无效。**运行时行为须在真实桌面手动验证**。
- 构建 app 需手动注入 MSVC + SDK 环境:本机 Windows SDK 装在**非默认路径**
  `D:\Windows Kits\10`,而 `vcvars64.bat` 依赖 `reg.exe` 读注册表 `KitsRoot10` 定位它;
  沙箱硬黑名单拦截 `reg.exe`(不可绕过),故 INCLUDE 缺 UCRT 路径报 `C1083 stddef.h`。
  绕过办法:手动设 INCLUDE/LIB/PATH 指向 MSVC 14.44.35207 与 `D:\Windows Kits\10`,再跑 ninja。

### 四、仓库卫生

`app/build/` 与 `core/target/` 的构建产物曾被 git 跟踪(`.gitignore` 对已跟踪文件无效),
每次构建 diff 噪声达数千行。已执行 `git rm -r --cached app/build core/target` 并从索引移除
5665 条(工作区文件保留),`.gitignore` 补 `core/target`。**删除已暂存,待提交。**

### 下一步

S4:编辑器(QGraphicsScene)+ 截图/贴图闭环(4.1–4.5)。

---

## 2026-10-06(四)S4 起步:截图与贴图闭环

### 起点诊断

| 环节 | 状态 |
| --- | --- |
| 框选确认 | ❌ **空壳**:`OverlayWidget::regionConfirmed` 只 `qInfo()` 打印选区矩形,未裁剪、未入库、未写剪贴板 |
| 全屏截图 | ⚠️ 能入库(`ingest_capture_png`),但**不写剪贴板**——用户截完无法直接粘贴 |
| 一键贴图 | ❌ `hotkeys.cpp` 的 `PinLatest` 是 `qInfo() << "pinLatest (PoC stub)"` |
| `nb_capture_crop` | ⚠️ `CoreBridge` 已解析该导出却**从未调用**(成员 `m_captureCrop` 悬空) |
| `latest_pinnable` | ⚠️ core 的 `store::entry::latest_pinnable` 有实现与单测,但 **FFI 未暴露** |

### 本轮实现(4.2 直通路径 + 4.3 全屏与一键贴图)

1. **core**:`ffi.rs` 增加 `latest_pinnable` 方法(无参,返回条目 JSON;空库为 `null`)。
2. **CoreBridge**:新增 `captureCrop(screen, rect)`(接通 `nb_capture_crop`,RGBA8888 紧凑排布)
   与 `loadEntryImage(blobPath)`(兼容相对/历史绝对路径)。
3. **框选闭环**(`capture_session.cpp`):
   - `regionConfirmed` 语义由"全局逻辑坐标"改为**本屏物理像素坐标**——冻结底图是物理像素,
     而选区是窗口局部逻辑坐标,2x 屏上不换算会裁错区域;换算在遮罩侧按 `devicePixelRatioF()` 完成。
   - 确认后:`captureCrop` 裁剪 → `ingest_capture_png(source=capture)` → `copy_entry` 写剪贴板。
   - 顺带删除已无用的 `toGlobal` / `screenForWidget`。
4. **全屏直通**:捕获 → 入库 → 写剪贴板(补齐原先缺失的剪贴板环节)。
5. **一键贴图**:`hotkeys.cpp` 实现 `pinLatest()` —— `latest_pinnable` 取条目,图片读 blob、
   文本渲染成图(`PinWindow::renderText`,圆角底板 + 自动换行),建置顶贴图窗口。
6. **截图前收面板**:热键触发先 `panel.hide()`,延时 150ms 再截,否则面板会被拍进画面。

### 关键约定

- **入库先于写剪贴板**(第一代 `docs/phase5.md` §3.6):写剪贴板后监听线程会读到同一张图,
  按 blake3 hash 命中去重只 `touch` 不新建,因此不会产生重复条目。
- 裁剪仍走**核心 RGBA 通道**(架构 §4.1 契约),坐标换算在宿主侧——核心只认"物理像素 + 相对屏左上角"。
- 图片超过 `maxImageMb` 时 `ingest_capture_png` 返回 `entry=null`,此时**不写剪贴板**(与入库一致)。

### 验证与限制

- core 单测:`capture::` 8 项、`store::` 26 项全绿;`nbcore.dll` 重建。
- app:编译链接通过(`[1/1] Linking`),无编译警告。
- ⚠️ **沙箱无法启动 GUI**,框选/贴图/剪贴板的运行时行为**须在真实桌面验证**。
- ⚠️ 构建时若 `nextboard.exe` 被残留进程占用会报 `LNK1104 无法打开文件 nextboard.exe`;
  沙箱内 `taskkill` 被拒,用 PowerShell 的 `Stop-Process -Force` 可清掉。

### 下一步

S4.1 编辑器(QGraphicsScene:画笔/箭头/矩形/马赛克/文字 + QUndoStack + Ctrl+S)、
S4.4 截图翻译/取字闭环(`captureTranslate` 热键目前落到 `default` 分支,未实现)。

---

## 2026-10-06(四)截图质量修复:多屏遮罩错位 + 高 DPI 模糊

### 现象

用户反馈:遮罩只在一个屏幕加载;屏1、屏0 依次截图;分辨率不高、图片不清晰。

### 定位

新增诊断工具 `core/nbcore/examples/monitor_probe.rs`(`cargo run --example monitor_probe`)
实测本机:

| 显示器 | 物理尺寸 | scale |
| --- | --- | --- |
| 0(主) | 3072×1920 | 1.7504 |
| 1 | 1080×1920 | 1 |

**捕获尺寸与显示器物理尺寸完全一致** → core 采集是原生分辨率,模糊**不在采集层**。

两个现象同一根因:**`showFullScreen()` 只把窗口放到"它当前所在的屏",而新建窗口默认
落在主屏**。多屏时循环创建的 N 块遮罩**全部叠在主屏**上,最后创建的那块在最上层——
用户看到的其实是**屏1 的 1080×1920 底图被拉伸铺满屏0 的 3072×1920**,所以又糊又错位;
点一次只关掉最上面那块并露出下一块 → 表现为"逐屏依次确认"(日志里 屏1/屏0 交替
出现正是这个)。

### 修复

1. **每块遮罩显式落到目标屏**:`setScreen(screen)` + `setGeometry(screen->geometry())`
   + `showFullScreen()`。
2. **底图按 DPR 反向缩放绘制**(`painter.scale(1/dpr)`),保证 1 图像像素 = 1 设备像素;
   原先 `drawImage(rect(), frozen)` 会把物理底图重采样到逻辑矩形,高分屏预览发虚。
3. **选区"恢复原图"的源矩形改为物理像素**——原先拿逻辑矩形当源,会从底图上取到错位区域。
4. 尺寸提示改显示**物理像素**,即最终截出的真实分辨率。
5. **会话级收敛**:任一屏确认或取消即收起**全部**遮罩(原先只关一块);Esc 同理。
6. 忙碌判定改为"是否还有遮罩",替代布尔标记——遮罩被外部销毁(Alt+F4/WM)时自动解除,
   不会卡死截图功能。
7. DPR 取**所属屏幕**的 `screen->devicePixelRatio()`,而非窗口的(窗口 `show()` 之前
   可能还是主屏的值,与底图采集口径不一致)。

### 验证

- 探针确认采集为原生分辨率;app 编译链接通过、无编译警告。
- ⚠️ GUI 仍无法在沙箱启动,**多屏与高 DPI 行为须在真实桌面验证**(重点看:
  两块遮罩是否各占一屏、预览是否清晰、尺寸提示是否等于实际截出分辨率)。

---

## 2026-10-06(四)S4.4 截图翻译 / 截图取字闭环

### 起点

`captureTranslate`(Ctrl+Shift+E)与 `openTranslate`(Ctrl+Shift+T)虽已在设置里注册热键,
但 `hotkeys.cpp` 的 switch 没有对应分支,落到 `default` 只打一行日志;截图会话也只有
「入库 + 写剪贴板」一条出口,无法接续 OCR / 翻译。

### 实现

1. **截图会话外置后处理**:`startCaptureSession(intent, onCaptured)` 增加**入库后回调**
   (参数为条目 id);不传则维持原行为。裁剪/全屏两条路径统一走 `commitCapture()`。
2. **`CoreFacade::processCapture(id, mode)`**:
   - `mode="ocr"` → 识别文本写入剪贴板(截图取字);
   - `mode="translate"` → 发 `translatePrefill(text)`。
   原 `ocrEntry` 改为 `processCapture(id,"ocr")`,消除重复实现。
3. **`CoreFacade::captureOcr()`** —— 翻译抽屉「截图取字」入口(延时 150ms 等面板从屏幕上
   消失后再框选,避免面板入镜)。
4. **`CoreFacade::openTranslate()` + `translatePrefill` 信号**统一"打开翻译抽屉"的两条链路:
   热键打开(空文本=手动输入)、截图翻译完成后回填。
5. **热键补分支**:`OpenTranslate` → 显示面板 + 打开翻译抽屉;`CaptureTranslate` →
   收面板 → 框选 → 入库 → OCR → 抽屉预填并自动翻译。
6. **`PanelWindow::showPanel()`**(Q_INVOKABLE)—— 截图翻译完成后把面板带回前台。
7. **QML**:Main.qml 订阅 `translatePrefill`(showPanel + `translateDrawer.openWith`);
   TranslateDrawer 增「截图取字」按钮。

### 完整链路

```
Ctrl+Shift+E
  → 收起面板(150ms 等窗口消失)
  → 各屏遮罩框选 → nb_capture_crop 裁剪
  → ingest_capture_png(source=capture) → copy_entry 写剪贴板
  → OCR(nbcore ocr_entry)
  → translatePrefill → 面板回前台 + 翻译抽屉预填原文并自动翻译
```

取字分支(抽屉「截图取字」按钮)走同一路径,OCR 结果写入剪贴板并提示字符数。

### 验证与限制

- `qmllint` 0 error;app 编译链接通过(`[25/25] Linking`)、无编译警告。
- ⚠️ GUI 无法在沙箱启动,OCR 与翻译链路须在真实桌面验证(翻译需已配置 provider)。

### 下一步

**S4.1 编辑器**是 S4 唯一剩余的大项(QGraphicsScene:画笔/箭头/矩形/马赛克/文字 +
QUndoStack + Ctrl+S 另存),完成后才能收尾 **4.2** 的 `regionAutoConfirm` /
`regionSkipEditor` 两模式确认。
