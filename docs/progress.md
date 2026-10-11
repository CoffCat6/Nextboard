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

---

## 2026-10-06(四)主面板图片条目溢出修复

### 现象

列表里的截图条目会盖住下方条目(卡片高度与图片实际绘制高度不一致)。

### 根因

`EntryCard.qml` 的 `imageBody`:父项 `implicitHeight` 固定为
`Math.min(image.sourceSize.height, 110)` = **110**,而内层 `Image` **没有设置宽高**,
只给了 `sourceSize.height: 220` —— Image 按自身隐式尺寸画成 **220** 高,于是
**溢出父项 110px** 盖住下方卡片。

附带说明:`sourceSize` 是"请求加载的尺寸"而非加载后的实际尺寸,拿它推算显示高度本身
就是错的(应为 `implicitWidth/implicitHeight`)。

### 修复

1. **卡片高度与图片显示高度同源**:按图片真实宽高比 × 可用宽度算出应占高度
   (`image.implicitHeight / image.implicitWidth`,未就绪时占位 60),并以
   `maxImageCardHeight`(300)封顶 —— 截图多为 16:9,不封顶会把列表撑成"一屏一条"。
2. **`Image` 显式设 `width/height` 到父项尺寸**,配合 `PreserveAspectFit` 居中留白:
   图片在结构上**不可能再超出卡片**。
3. 顺带修同一渲染路径上的性能陷阱:`ClipEntryModel::imageUrlFor(id, thumb)` 原先
   **为每一行发一次同步 `get_entry` FFI 查询**去取 blobPath(列表重绘时放大成 N 次
   DB 查询)。改为静态 `imageUrlForBlob(blobPath, thumb)`,直接使用 `list_entries`
   响应里已有的 blobPath,并兼容历史绝对路径。

### 验证

- `qmllint` 0 error;app 编译链接通过、无编译警告。
- ⚠️ 需真实桌面确认:图片卡片高度随图片自适应、不再遮挡下方条目;不同宽高比的
  截图(竖图/超宽图)高度表现是否合理。

---

## 2026-10-06(四)S4.1 截图编辑器

### 实现

新增 `app/src/editor/editor_window.{hpp,cpp}`,以 QGraphicsScene + QUndoStack 实现
第一代 F12 的编辑器:

- **五类工具**(对齐第一代 `editor-core.ts` 的 `Tool`):画笔 / 箭头 / 矩形 / 马赛克 / 文字;
- **颜色**(第一代 `PALETTE` 七色)/ **粗细**(2/4/8)/ **字号**(24/36/48);
- 快捷键:`Ctrl+S` 另存为 PNG、`Ctrl+Z` / `Ctrl+Shift+Z`(或 `Ctrl+Y`)撤销重做、
  `Ctrl+Enter` 完成、`Esc` 取消;
- 「完成」→ `compose()` 按底图**物理像素**渲染整场景 → 交调用方入库 + 写剪贴板。

### 关键设计

- **场景坐标 = 底图物理像素**(第一代 §3.4「画布以底图自然分辨率渲染」):导出与预览
  同源;视图按窗口等比缩放显示,**小图不放大**以免虚化。
- **马赛克 = 像素化图层按路径揭示**:底图先缩到 1/8 再放大回原尺寸
  (`FastTransformation` 取整不插值)得到像素化版本,作为 `QGraphicsPathItem` 的**纹理刷子**;
  笔画经 `QPainterPathStroker` 描成有宽度的轮廓,该轮廓即 mask。
- **撤销 = 一次标注一个图元**:`AddItemCommand`(redo 入场景 / undo 出场景)。图元
  **先不入场景**就交给命令,`push()` 触发 `redo()` 完成首次添加,避免重复添加。
- **文字确认后即烘焙**(第一代 v1 取舍):放置即进入 `TextEditorInteraction`,点别处或
  切工具即固化为不可编辑;空文字自动撤销,不留不可见的空图元。
- 文字编辑期间画布 `passthrough`,把鼠标事件还给 `QGraphicsView`,光标才能正常交互。

### 接入截图流程

`capture_session` 框选确认后读设置 `regionSkipEditor`(默认 `true`):

- `true` → 直通:入库 + 写剪贴板(原有行为,截图后立即可粘贴);
- `false` → 打开编辑器,「完成」时才入库 + 写剪贴板。

编辑器是**异步窗口**,故回调由调用方显式携带(不再读会话级的 `g_onCaptured`),
使截图翻译 / 取字链路在编辑器路径下同样成立。

### 验证与限制

- 编译链接通过(`[18/22]` 编译 `editor_window.cpp` → `[21/22]` Linking),无编译警告;
  AUTOMOC 正确处理了 `.cpp` 内带 `Q_OBJECT` 的 `EditorCanvas` 与 `editor_window.moc`。
- ⚠️ 编辑器**完全无法在沙箱运行验证**,五类工具的实际手感、马赛克纹理对齐、文字
  烘焙时机、导出清晰度均须在真实桌面确认。

### 剩余

**4.2 的 `regionAutoConfirm=false`**(选区 8 向手柄微调 + Enter/双击确认)未做 ——
当前只有「松开鼠标即确认」一种模式。这是 S4 的最后一项。

---

## 2026-10-06(四)编辑器不可达 + 主题切换失效修复

### 现象与定位(三条,其中两条同源)

1. **编辑器不弹出**:排查 `app_config.json` 发现 `regionSkipEditor: true`(默认直通),
   而**设置界面里根本没有这个开关** —— 用户无从打开;日志亦确认走的是直通路径
   (直接出现 `截图已入库并写入剪贴板`,全程无任何编辑器日志)。
2. **主题无法切换**:`Theme.qml` 是**硬编码暗色单例**,`setupQmlEnvironment()` 也只注入
   一份固定暗色 `QPalette`;设置里的 `theme` 只写进配置、不作用于任何 UI,切换自然无效。
3. **部分文字不显现**:与 2 同源 —— QML 里散落写死的颜色(`#fff` / `#222` / `#7ec2ff` /
   `#c9a7ff`)不随主题变化,一旦主题真正生效就会出现浅底浅字而"看不见"。

### 修复

1. **设置界面补 `regionSkipEditor` 开关**(「截图后直接入库(跳过编辑器)」)——编辑器从此
   可达:勾选 = 直通(默认),取消 = 框选后进编辑器标注,「完成」才入库 + 写剪贴板。
2. **实现真正的主题系统**:
   - `CoreFacade` 新增 `darkTheme` 属性 + `applyTheme(mode)` + `refreshThemeFromSettings()`;
     `system` 由 C++ 侧用 `QStyleHints::colorScheme()` 解析,QML 侧不感知;
   - `applyTheme()` **同时注入亮/暗两套 `QPalette`** —— Basic 风格的
     Button/ComboBox/SpinBox/TextArea 走的是 QPalette,只改 QML 的 Theme 单例是不够的;
   - `Theme.qml` 全部改为随 `Core.darkTheme` 联动,并新增 `onAccent` / `onPin` /
     `sourceCapture` / `sourceTranslate` 语义色;
   - 启动时按设置应用一次;并连接 `QStyleHints::colorSchemeChanged`,跟随系统主题实时变化;
   - 设置里切换主题**即时生效**(不必等"保存"),保存后落盘。
3. **清除 QML 内全部硬编码颜色**,统一走 Theme 语义色。
4. **编辑器健壮性**:`fitCanvas()` 补判空 —— `resizeEvent` 可能在构造早期(视图/场景尚未
   建好)就触发,原实现会解引用空指针。

### 验证与限制

- `qmllint` 0 error;app 编译链接通过(`[25/25] Linking`)、无编译警告。
- ⚠️ 仍需真实桌面确认:设置里取消勾选「截图后直接入库」后框选应弹出编辑器;
  主题切换应即时生效且无低对比文字。

### 顺带记录(未修)

日志中成组出现的 `QPainter::begin: Paint device returned engine == 0, type: 3` 发生在
**两次截图之间**(遮罩显示期间),且含 `setCompositionMode` / `drawPath` —— 而
`OverlayWidget::paintEvent` 并不调用这些,故不是遮罩自身绘制,疑为 Qt 离屏合成路径的噪声。
截图结果正常(298×953 / 2430×1632 均正确入库),暂按观感问题记录,待后续排查。

---

## 2026-10-06(四)面板全白回归修复(QML `on*` 命名禁区)

### 现象

上一轮主题修复后,主面板打开是**整片空白**。

### 根因(上一轮引入的回归)

运行日志直接给出答案:

```
Theme.qml:28:29: Cannot assign a value to a signal (expecting a script to be run)
    readonly property color onPin: "#222222"
```

QML 把 **`on` + 大写字母开头的标识符当作信号处理器语法**,因此 `onAccent` / `onPin`
**不是合法属性名** → Theme 单例加载失败 → `EntryCard` 不可用 → `Main.qml` 整体加载失败
→ 面板空白。

### 修复

`onAccent` → `accentText`、`onPin` → `pinText`,同步 3 处引用;并在 Theme.qml 内写明
命名禁区。全量 grep 确认无其他 `on*` 属性名。

### 重要教训(已实测)

1. **`on*` 属性名是运行时错误,静态工具抓不到**:用最小复现文件
   (`readonly property color onPin: "#222222"`)单独跑 `qmllint` 也**报不出来**。
   只能靠命名纪律 + 运行时日志发现 —— 好消息是 `QML load errors:` 已会落盘
   (见 `PanelWindow::setupUi`),排查时**先看 `app/build/nextboard.log`**。
2. **`qmllint` 对本项目默认几乎无校验力**:`Nextboard.Core` 由 C++ 侧
   `qmlRegisterSingletonInstance` 注册、**没有生成 `.qmltypes`**,qmllint 解析不了该模块
   就**跳过语义检查**并报 0 error —— 上一轮正是因此漏掉。补一个桩模块后可正常解析,
   从而能校验 `Core.*` 成员用法:

   ```
   # 桩:tools/qmlstub 风格,提供 Nextboard/Core/{qmldir,Core.qml}
   #   qmldir:  module Nextboard.Core
   #            singleton Core 1.0 Core.qml
   #   Core.qml: pragma Singleton + QtObject,声明 darkTheme/counts/各方法与信号
   qmllint -I <stub目录> -I app/build -I D:/Qt/6.9.0/msvc2022_64/qml app/panel/qml/*.qml
   ```

---

## 2026-10-06(四)编辑器两个缺陷修复:底图过小 + 完成闪退

### 现象

1. 编辑器里底图**只有一小点**(截图四周大片空白,中间一个几十像素的小白块),无法编辑;
2. 点「完成」后**应用闪退**。

### 根因

**A. 闪退 = 析构顺序导致的双重释放**

`QObject` 子对象按**创建顺序**析构(`QObjectPrivate::deleteChildren` 正序遍历)。构造时
`m_scene` 早于 `m_undo` 创建,于是:

1. `m_scene` 先析构 → 删掉场景内所有图元(含全部标注);
2. `m_undo` 再析构 → 每个 `AddItemCommand` 析构执行 `delete m_item` → **同一指针二次释放 → 崩溃**。

即:只要画过任意标注,点「完成」或「取消」都必崩;没画过则不触发(用户截图时尚未落笔)。
另有一处同源风险:视图 `m_view` 也晚于场景析构,会解引用已释放的场景。

**B. 底图过小 = 用未定型的视口算缩放**

`fitCanvas()` 挂在**窗口** `resizeEvent` 上,而该事件可能在布局把视图撑开**之前**就触发 ——
那时 `m_view->viewport()` 只有几十像素,算出的缩放极小;而视图之后真正拿到尺寸时
**不会再触发窗口 resizeEvent**,于是这个错误缩放被永久保留。

### 修复

1. **析构按正确顺序拆**:`~EditorWindow()` 先 `m_view->setScene(nullptr)` 断开视图,
   再 `delete m_undo`(此时场景仍在,`removeItem` 安全,图元由命令释放),最后才轮到子对象
   按创建顺序析构。
2. **改为按视图尺寸适配**:`EditorCanvas` 增加 `canvasResized` 信号(在自身 `resizeEvent`
   发出),窗口连到 `fitCanvas`;并加**视口定型判据**(`< 2px` 直接跳过),杜绝用早期小视口
   算缩放。
3. **顺带加滚轮缩放**(以光标为锚点,5%–800%):底图适配后仍能放大看细节;一旦手动缩放过,
   就不再随窗口自动适配,免得重置用户选好的比例。

### 验证与限制

- 编译链接通过(`[19/19] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:底图应铺满画布(而非一小点);画几笔后点「完成」应正常入库且**不闪退**;
  滚轮可缩放。

---

## 2026-10-06(四)补齐设置缺口:翻译配置 UI / 贴图设置落地 / 崩溃日志兜底

盘点发现三类「设置项存在、但界面无入口或代码未读取」的缺口,按优先级依次补齐。

### 1. 翻译配置 UI(此前翻译功能**实际不可用**)

core 侧 `settings.translate_provider` 的校验与分派都正常,但**没有任何界面入口** ——
provider / apiKey / model 只能在 `app_config.json` 手改,等于翻译功能不可用。

- `SettingsDrawer` 新增「翻译服务」区:提供方三选一(OpenAI 兼容 / 微软翻译 / 自定义 HTTP),
  按提供方**条件显示**对应字段(openai: baseUrl/apiKey/model;microsoft: endpoint/apiKey/region;
  custom: url/method/apiKey/model/headers/bodyTemplate/responsePath),外加超时。
- 新增「测试连接」:`CoreFacade::probeTranslate()` **异步**执行 `translate_probe` ——
  探测会真发一次网络请求,同步调用会把界面冻住整个超时时长;结果经
  `translateProbeFinished` 回投,展示 `available + message`。
- 抽屉内容整体移入 `Flickable` 可滚动 —— 抽屉宽度只有主面板的 46%,内容变多后不滚动会溢出。

### 2. 贴图设置落地

`pinDefaultOpacity` / `pinDoubleclickClose` 此前**全仓库零引用**:`PinWindow` 里透明度与
双击关闭是写死的,用户改设置完全无效。新增 `PinOptions`,`pinLatest()` 从设置读取后传入,
`PinWindow` 据此设置初始透明度与双击关闭开关。

### 3. 崩溃日志兜底

本次闪退表现为**静默进程死亡**(日志停在最后一条正常记录上),排查全靠推理。在 `main.cpp`
安装 `SetUnhandledExceptionFilter` + `std::set_terminate`,把未处理异常以
`[CRASH] 未处理异常 code=0x… address=…` 写进同一份 `nextboard.log`,至少留下异常码与地址
(常见码已注释在代码里)。安装点必须在 QApplication 之后 —— `applicationDirPath()` 依赖它。

### 验证与限制

- `qmllint` 0 error(带桩);app 编译链接通过(`[22/22] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:设置里能看到「翻译服务」区并能保存;「测试连接」有结果;
  贴图窗口按设置的不透明度出现。

---

## 2026-10-06(四)S4.2 收口:框选两种确认模式

### 起点

`regionSkipEditor`(直通路径)此前已完成,缺的是 **`regionAutoConfirm = false`** 这一种
确认方式 —— 此前只有"松开鼠标即确认"。

### 实现

`OverlayWidget` 增加 `autoConfirm` 参数与**微调模式**(第一代 F10 的两种确认方式):

- **直通模式**(`regionAutoConfirm = true`,默认):行为不变 —— 松开鼠标即确认;
  点空(没拖出有效范围)仍取消整个会话。
- **微调模式**(`false`):松开鼠标后**选区保留**,并:
  - 绘制 **8 个手柄**(四角 + 四边中点);
  - 拖手柄改对应边/角,拖**选区内部**整体平移;
  - 悬停时按命中的手柄切换光标(对角/水平/垂直/移动);
  - 调整结果**夹在窗口内**(遮罩即整屏,拖到屏外无意义);
  - **Enter 或双击**确认,`Esc` 取消;
  - 点空只清除选区,不取消会话(用户可重新框)。
- 设置界面补 **「框选后松开鼠标即确认」** 复选框(`regionAutoConfirm`)—— 此前无入口。

### 验证与限制

- `qmllint` 0 error(带桩);app 编译链接通过(`[11/11] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:关掉「框选后松开鼠标即确认」后,框选应保留选区、出现 8 个手柄,
  可拖动微调,Enter/双击才入库;Esc 取消。

**S4 至此全部条目完成**(4.1–4.5),整体待真机回归。

---

## 2026-10-06(四)主面板右键补「贴到屏幕」与「编辑」

### 起点

`EntryCard` 菜单此前只有 复制 / 翻译 / 识别文字 / 固定 / 删除 ——「贴到屏幕」「编辑」从
S4 起一直缺(当时标注为"待 S4:需按条目贴图 FFI 与 QGraphicsScene 编辑器")。

### 语义确认(关键,避免了过度设计)

查第一代 `capture/session.rs` 的 `commit_capture`:无论来自**截图会话**还是**历史图片再编辑**,
**都走同一条 `ingest_image`**(按 hash 去重)。也就是说"编辑历史图片"的结果是**新条目**
(除非与已有内容完全相同才 touch 到原条目),**并非原地替换**;`edit_source` 只用于
来源校验与日志。

→ 因此 Nextboard 侧**直接复用既有的「入库 + 写剪贴板」通道即可,无需新增 core 接口**
(原本按"替换原条目"设计的 `replace_entry_image` FFI 没有必要,已放弃)。

### 实现

1. `capture_session`:把「入库 + 写剪贴板」提为公共函数 `ingestImageAndCopy()`,
   截图路径与编辑器路径共用同一条通道。
2. `CoreFacade` 新增三个入口,并把贴图逻辑从 `hotkeys.cpp` **收敛过来**(消除重复):
   - `pinLatest()` —— 热键一键贴图(取最近可贴条目,图片优先、无图回退文本);
   - `pinEntry(id)` —— 面板右键「贴到屏幕」(图片读 blob;文本经 `PinWindow::renderText` 渲染);
   - `editEntry(id)` —— 面板右键「编辑」(图片条目 → 编辑器;「完成」→ 入库 + 写剪贴板 + 刷新列表)。
3. `hotkeys.cpp` 的 `PinLatest` 改为委托 `CoreFacade::pinLatest()`,删除本地重复实现(−42 行)。
4. `qml_bootstrap`:把 `CoreFacade::clipChanged` 也接到列表刷新 —— 编辑产物是**新条目**,
   不刷新列表就看不到。
5. `EntryCard` 菜单补「贴到屏幕」(全类型)与「编辑」(仅图片);编辑前先收起面板(第一代同语义)。

### 验证与限制

- `qmllint` 0 error(带桩);app 编译链接通过(`[26/26] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:图片条目右键应出现「贴到屏幕」「编辑」;贴图按设置透明度出现;
  编辑后「完成」应新增条目并写剪贴板,列表随即刷新。

---

## 2026-10-06(四)快捷键自定义 + 条目卡片快捷按钮

### A. 快捷键自定义

设置里的 `hotkeys` 表此前**只能在配置文件改**。现在:

- `SettingsDrawer` 新增「快捷键」区:6 个动作(呼出面板 / 矩形截图 / 全屏截图 /
  一键贴图 / 打开翻译 / 截图翻译)各配一个**录制控件** —— 点击进入录制态,按下组合键即
  捕获(只按修饰键时继续等待主键,`Esc` 取消录制)。
- **不做客户端校验**:合法性(至少一个修饰键、非空、各动作两两不重复)由 core 的
  `settings.validate()` 负责,界面直接展示后端错误 —— 避免两处规则漂移。
- 保存后调 `Core.reloadHotkeys()` → `CoreFacade::hotkeysChanged` →
  `Hotkeys::applyFromSettings()`,**立即重新注册,不必重启**。

**顺带修掉一个既有缺陷**:`Hotkeys::applyFromSettings()` 重新应用时**不撤销旧注册**
(`registerAction` 每次分配新 id),导致改设置后旧组合键仍然生效(改了等于没改,还多占
一个组合键)。已改为先全部 `UnregisterHotKey` 再重新注册。

### B. 条目卡片右下角快捷按钮

`EntryCard` 元信息行右侧新增三个胶囊按钮(对齐右键菜单里的高频动作):

| 按钮 | 可见性 | 行为 |
| --- | --- | --- |
| **贴图** | 图片 / 文本(文件条目隐藏) | 图片直贴;文本经 `PinWindow::renderText` 渲染成位图后贴 |
| **编辑** | 仅图片 | 收起面板后打开编辑器 |
| **删除** | 全类型 | 删除并 toast |

其余动作(复制 / 翻译 / 识别文字 / 固定)保留在右键菜单,避免卡片过于拥挤。

### 验证与限制

- `qmllint` 0 error(带桩);app 编译链接通过(`[24/24] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:录制快捷键后保存应**立即生效**(旧组合键失效、新组合键可用);
  卡片右下角按钮的可见性与点击行为。

---

## 2026-10-06(四)截图翻译不进编辑器 + 文本贴图样式可配置

### A. 截图翻译 / 取字不进编辑器

`startCaptureSession` 增加 `allowEditor` 参数:截图翻译(`Ctrl+Shift+E`)与截图取字
(翻译抽屉按钮)传 `false` —— 这两条是**"取内容"链路**,中间弹一个标注编辑器既多余
又打断流程。标注编辑器只在 `regionSkipEditor=false` **且不是**取内容链路时才出现。

### B. 文本贴图样式可配置(5 项新设置)

文本条目贴图此前是**硬编码**的暗色底 + 15px 字号 + 520px 宽。新增 core 设置字段
(容器级 `#[serde(default)]`,对旧配置文件向后兼容):

| 字段 | 含义 | 默认 | 校验 |
| --- | --- | --- | --- |
| `pinTextBg` | 背景色 | `#26272e` | `#RRGGBB` |
| `pinTextColor` | 文字色 | `#dfe1e8` | `#RRGGBB` |
| `pinTextFontSize` | 字号(像素) | 15 | 8–96 |
| `pinTextMaxWidth` | 最大宽度(像素) | 520 | 120–2000 |
| `pinTextBgOpacity` | 背景不透明度(%) | 92 | 0–100 |

- 颜色格式非法时**保存直接报错**(带具体字段名),不静默退化成黑色。
- `PinWindow::renderText(text, PinTextStyle)` 按样式渲染;`CoreFacade` 从设置读出样式后
  传入。边框色改为由背景色 `lighter(140)` 派生,不再是写死的蓝色。

**顺带补齐**:`pinDefaultOpacity` / `pinDoubleclickClose` 此前**也没有界面入口**,
一并加上 —— 设置抽屉新增「贴图」区,收纳全部贴图相关项。

### 验证与限制

- core `settings::` 单测 25 项 + 新增 1 项(文本贴图样式校验)全绿;
  app 编译链接通过(`[5/5] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:关掉「截图后直接入库」后按 `Ctrl+Shift+E` 应**直接框选并翻译**、
  不弹编辑器;设置里改文本贴图配色/字号后,文本条目「贴图」应按新样式渲染。

---

## 2026-10-07(三)整屏截图"没反应"根因 + 设置交互改进

### A.【真 bug】整屏截图在某些屏毫无反应

现象:同一台机器,**一块屏按全屏截图热键正常,另一块毫无反应**。日志直接给出答案:

```
[INFO] 截图超过大小上限,已忽略
```

根因:`is_oversized()` 拿**原始 RGBA 字节数**跟 `maxImageMb` 比 ——
3072×1920 的屏 RGBA = **23.6MB > 默认 20MB** → 直接丢弃;1080×1920 = 8.3MB → 通过。
**正好解释"一块屏行、另一块不行"**。

第一代是同一口径,但它超限时**仍写剪贴板**(`capture/mod.rs`:「未入库,仅写剪贴板」),
而 Nextboard 直接丢弃。两处一起改:

1. **图片上限改为按编码后的 PNG 体积判断**(那才是真正占磁盘的东西)。前置过滤不再拦图片,
   体积判断移到 `build_entry`(那里才拿得到 PNG),其返回值改为 `Option`。
2. **未入库时仍把图写进剪贴板**(新增 FFI `copy_image_png`)—— 否则用户按了热键却"毫无反应"。

新增回归测试 `image_size_limit_uses_encoded_png_not_raw_rgba` 把这个结论固定下来
(2000×2000 纯色图:RGBA 15.3MB,PNG < 1MB)。

### B. 截图翻译 / 取字不进编辑器

`startCaptureSession` 增加 `allowEditor` 参数;截图翻译(`Ctrl+Shift+E`)与截图取字传
`false` —— 这两条是"取内容"链路,中间弹标注编辑器既多余又打断流程。

### C. 设置交互改进

- **自动保存**:任何改动经 **400ms 去抖**后落盘,取消底部「保存 / 关闭」按钮(关闭走标题栏 ✕)。
  各控件只接**用户操作**信号(`valueModified` / `clicked` / `textEdited` / `activated`),
  因此加载时回填值**不会误触发保存**,也就不需要 loading 守卫。
- **快捷键只在真改过时才重注册**(`hotkeysDirty` 标记)—— 每次自动保存都注销/重注册一遍
  会留下短暂空窗,期间按热键会丢。
- **数值可手动输入**:所有 `SpinBox` 加 `editable: true`。
- **色板选色**:文本贴图背景色 / 文字色改为**点色块打开 `ColorDialog`**,同时保留 hex 输入框。

### D. 新增设置:框选右键取消截图

`regionRightClickCancel`(默认开)+ 设置界面复选框。

### 验证与限制

- core `clip::` 11 项(含新回归测试)+ `settings::` 26 项全绿;
  app 编译链接通过(`[11/11] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:两块屏的全屏截图都应入库;设置改动应**即时落盘**(不再需要保存按钮);
  SpinBox 可直接输数字;色块能打开取色器。

---

## 2026-10-07(三)主面板再次空白:QtQuick.Dialogs 未部署

### 现象与根因

主面板整片空白。日志一行定位:

```
SettingsDrawer.qml:8:1: module "QtQuick.Dialogs" is not installed
```

→ `SettingsDrawer` 加载失败 → `Main.qml` 整体失败 → 面板空白。

上一轮为做"色板选色"引入了 `import QtQuick.Dialogs` + `ColorDialog`。
**`qmllint` 是通过的** —— 它从 Qt 安装目录解析 import;但**运行时解析不到**:
已部署的 `app/build/qml/QtQuick/Dialogs/` 里**只有 `quickimpl/` 子目录**,缺 `qmldir` /
`plugins.qmltypes` / `qtquickdialogsplugin.dll`。

为什么没部署:**windeployqt 检测不到这个 import** —— QML 已编译进 qrc,它扫描的是
二进制 / qmlcachegen 产物,看不见新引入的模块。(本想重跑 windeployqt 验证,但沙箱拦住了
它依赖的 `qtpaths`,无法确认。)

### 修复

**去掉该依赖**:改为自带的 `ColorPalettePopup`(36 色常用色板,`GridLayout` + `Repeater`,
高亮当前值),保留 hex 输入框用于精确取值。**零部署依赖**,不再受 windeployqt 行为影响。

### 教训(已固化为可执行检查)

> **`qmllint` 通过 ≠ 运行时可用。** 它从 Qt 安装解析 import,而运行时要的是**已部署**的
> 模块。每新增一个 Qt 模块 import,就多一份部署风险。

现在有一行检查可以提前发现(已写入项目记忆):

```bash
# 校验 QML 里 import 的 Qt 模块是否都已在 app/build/qml 下部署(存在 qmldir)
for m in $(grep -rhoE "^import Qt[A-Za-z.]*" app/panel/qml/*.qml | sed 's/^import //' | sort -u); do
  p="app/build/qml/$(echo "$m" | tr '.' '/')"; [ -f "$p/qmldir" ] && echo "OK $m" || echo "MISS $m"
done
```

### 验证与限制

- 模块部署校验:`QtQuick` / `QtQuick.Controls.Basic` / `QtQuick.Layouts` 全部 **OK**;
  `qmllint` 0 error;app 编译链接通过(`[6/6] Linking`)、无编译警告。
- ⚠️ 需真实桌面确认:面板恢复正常;设置里点色块应弹出**内置色板**(不再是系统取色器)。

---

## 2026-10-07(三)色板点击无效:改为内联网格

### 现象

设置里点色块后,点击色板中的颜色**不生效**。

### 根因(推断)

上一版把色板做成 `Popup`(`ColorPalettePopup`),而**外层 `SettingsDrawer` 本身就是
`Popup`(Drawer)** —— 嵌套 Popup 的定位与事件派发在 Qt Quick Controls 里不可靠,
色块上的 `MouseArea` 收不到点击(或浮层根本没正常显示)。

### 修复

改为**内联网格** `ColorSwatchGrid`(根节点是 `GridLayout`,36 色),直接展开在对应颜色
字段下方,由 `root.paletteTarget`("" / "bg" / "color")控制哪一块可见:

- 点色块 → 就地展开色板(色块描边高亮表示展开中);
- 点颜色 → 写回输入框 + 收起 + 自动保存。

**不用 `Popup`、不用 `QtQuick.Dialogs`** —— 两者都已踩过坑(见上文"主面板空白"一节)。

### 验证与限制

- `qmllint` 0 error;无残留 `ColorPalettePopup` 引用;app 编译链接通过(`[6/6] Linking`)、
  无编译警告。
- ⚠️ 需真实桌面确认:点色块应就地展开 36 色网格,点其中任一色应立即写入输入框并保存。

### 附:关于沙箱

本轮尝试确认"关闭沙箱后能否运行 GUI"——结论是**仍然不能**:
`build.ps1 -Run` 能执行完成并 `Start-Process` 成功,但进程不留下(日志无新记录);
Bash 里直接启动被拦。且 `reg.exe` 仍在黑名单,`vcvars64` 仍受限。
**因此 GUI 改动依旧只能由用户在真机验证。**

---

## 2026-10-07(三)自定义数据目录落地(设置 UI + FFI 迁移挂接 + 宿主 blobs 定位修正)

### 起点

盘点确认 `dataDir` 设置字段、绝对路径校验、`effective_data_dir` 与待迁移标记机制
(`write_pending_move` / `take_pending_move`,启动时自动搬迁 db + blobs,源目录保留)
在 nbcore 侧**早已存在**,但三处断点使其完全不可用:

1. FFI `save_settings` 没有挂迁移标记 —— 改了 `dataDir` 没人写
   `data-move-pending.txt`,重启不会搬迁;
2. C++ `CoreBridge::blobsDirAbs()` 写死默认目录 —— 迁移后图片全部 404;
3. 设置界面没有数据目录入口。

### 实现

| 层 | 改动 |
| --- | --- |
| nbcore `core.rs` | `Core` 增加 `effective_dir` 字段(当前生效目录,init 时随 `data_dir` 一起记录) |
| nbcore `ffi.rs` | 新增 `data_dir` 方法返回生效目录;`save_settings` 检测 `dataDir` 变更(与现值 trim 后比较)即以**变更前**的生效目录为源写迁移标记。标记写入失败则保存失败,防"改了配置却迁不动数据" |
| app `core_bridge` | nb_init 成功后调 `data_dir` 查询生效目录(失败回退默认目录并落日志);`blobsDirAbs()` 改基于生效目录 |
| QML `SettingsDrawer` | 新增「数据」区:目录输入框(留空 = 默认位置)+ 变更提示。走既有的 400ms 去抖自动保存;`dataDirDirty` 标记使保存成功后提示"下次启动生效(含数据迁移)",避免静默改目录用户无感知 |

语义沿用第一代 phase4:**重启生效**(运行中直接切库有数据一致性风险);迁移是**复制**
而非移动,源目录保留可回退;目标已有数据库时不覆盖(防回迁时用旧数据盖新数据)。

### 验证与限制

- core `cargo check`(含 tests)通过;`cargo build --release` 通过,新 nbcore.dll 已部署到 app/build;
- 新增 settings 单测 3 项:effective_data_dir 的空/空白/带空白自定义值口径、
  迁移标记全链路(建库建 blobs → 写标记 → 搬迁 → 标记清除 → 源保留 → 目标已有数据不覆盖)、
  相对路径拒绝;
- ⚠️ cargo test 测试进程在本工具会话以 `STATUS_DLL_INIT_FAILED` 失败(见上一轮分析,
  会话级环境限制),新增单测由用户真机 `cargo test` 确认;
- qmllint 0 error;app 编译链接通过(`[11/11] Linking`)、无编译警告;
- ⚠️ 需真实桌面确认:设置 → 数据 → 填入新目录 → 自动保存后重启 —— db + blobs 应出现在新目录
  且历史条目(含图片缩略图)完整;改回默认(清空)重启后数据应迁回;原目录数据始终保留。

---

## 2026-10-07(三)文件条目交互补全:复制污染根因 + 左键打开 + 显式复制路径

### 现象

复制文件条目到其他应用(聊天窗口/编辑器),粘贴出来的是**路径文本**而不是文件本身。

### 根因(clipboard-win 的 NoClear 缺省)

`write_files_to_clipboard` 走 `clipboard_win::raw::set_file_list`,其内部是
`NoClear` 语义(**不清空剪贴板**);而文本/图片写入走的高层 `set` 是 `DoClear`
(写前 `EmptyClipboard`)。于是复制文件时,剪贴板上残留的旧 `CF_UNICODETEXT`
(典型如之前复制过的路径文本)叠加在新 `CF_HDROP` 之下,粘贴目标按格式
优先级取文本 —— 粘出来的就是路径。修复:写入前显式 `raw::empty()`,
与文本/图片写入对齐同一语义。

### 顺带的交互补全

1. **文件条目左键 = 用系统默认程序打开**(`CoreFacade::openEntry`,
   `QDesktopServices::openUrl`;此前左键对所有类型一律是复制)。
2. 文件条目右键菜单补「**复制路径**」—— 粘贴出路径从"意外行为"变成"显式可选"
   (与「复制文件」CF_HDROP 语义区分;文本内容即换行连接的路径列表)。
3. 上一轮的「定位文件」保留(explorer /select,)。

### 验证与限制

- core `cargo build --release` 通过;app 编译链接通过(`[23/23] Linking`)、无编译警告;
- ⚠️ 真机确认清单:复制文件 → 在聊天窗口粘贴应出现文件;复制文件 → 记事本粘贴应为
  路径(仅当右键选了「复制路径」);左键文件条目应以默认程序打开;连续
  「复制文本 → 复制文件 → 粘贴」不应再出现文本残留。

---

## 2026-10-08(四)修复一键贴图:贴出"最近复制的内容"而非"最新图片"

### 现象

通过全局快捷键 `Ctrl+Shift+P`(一键贴图)触发时,刚复制的**文字**贴不出来,
反而贴出更早的**图片**条目。

### 根因(`store::entry::latest_pinnable` 的"图片优先"口径)

`latest_pinnable` 此前按 `for kind in ["image", "text"]` 逐类查询、命中即返回:
只要历史里存在**任意**图片,就永远返回最近图片,`text` 分支根本不会执行 ——
`last_used_at`(最近复制时间)被完全忽略。于是"复制文本 → 按热键"贴出的仍是旧图。

第一代 clipboard-plus 沿用同一口径(design.md F17「图片优先,无图片时回退文本」),
本项目的场景需求与之不同:热键应贴出用户**最近复制的内容**,文本/图片平等。

### 修复

| 层 | 改动 |
| --- | --- |
| nbcore `store/entry.rs` | `latest_pinnable` 改为接收 `prefer_image: bool`：**默认(false)走单条 recency 查询** `WHERE kind IN ('text','image') ORDER BY last_used_at DESC, created_at DESC LIMIT 1`，文本与图片平等参与排序，谁最近复制/入库就贴谁；**true 时保留第一代"图片优先、无图回退文本"** 逐类查询。文件条目仍排除（不可贴图）；`pinned` 不影响排序 |
| nbcore `settings.rs` | 新增设置 `pinPreferImage`（默认 **false**），作为"图片优先"开关 |
| nbcore `ffi.rs` / app `core_facade.hpp` | `latest_pinnable` FFI 读取 `pinPreferImage` 后传入；同步修正 `pinLatest()` 文档注释 |
| QML `SettingsDrawer` | 「贴图」区新增勾选「一键贴图 · 图片优先(否则贴最近复制的内容)」+ 说明;接入 `openAndLoad` / `save` 的自动保存链路 |
| 单测 | 拆为两条:`latest_pinnable_default_is_recency_across_text_and_image`(默认口径:文本最新→贴文本;touch 旧图→贴旧图;仅文件→None)与 `latest_pinnable_prefer_image_keeps_first_gen_semantics`(图片优先:文本更晚仍贴图片;无图回退文本);另加 `pin_prefer_image_roundtrips_as_camel_case` |

### 验证与限制

- core `cargo test`:**172 项**通过(167 passed / 5 ignored,含新增用例);
- core `cargo build --release` 通过,新 nbcore.dll(3.7MB)经 build.ps1 部署到 app/build;
- app 编译链接通过、无编译警告;`qmllint` 无新增告警;
- ⚠️ 真机确认清单:
  1. **默认(开关关)**:复制一段文字 → 按 `Ctrl+Shift+P` 应贴出**该文字**;再复制一张图片 →
     按热键应贴出该图片;不复制新内容时连按热键应始终贴同一(最近)条目;
  2. **开启「图片优先」后**:先复制图片、再复制文字 → 按热键应贴**图片**(旧口径);
     删掉全部图片条目后 → 按热键应回退贴最近文字;
  3. 设置里切换开关后无需重启即生效(下次按热键按新口径取数)。

---

## 2026-10-08(四)修复贴图跨屏 DPI:在不同缩放比的屏幕上忽大忽小

### 现象

贴图窗口在多屏(不同缩放比)之间拖动时,图会**忽大忽小**;缩到极小时几乎成一个点。

### 根因(两处,均在 `pin/PinWindow`)

1. **初始尺寸用了主屏的 DPR**:
   `m_scale = 1.0 / QGuiApplication::primaryScreen()->devicePixelRatio()`,
   再 `resize(原图 × m_scale)`。窗口 show 之前 `devicePixelRatioF()` 还是主屏值,
   若贴图落点屏的缩放比 ≠ 主屏(如主屏 100%、副屏 150%),逻辑尺寸就整体偏大/偏小
   —— 这正是"在不同屏幕上变大变小"的直接来源。
2. **`m_scale` 把"DPI 换算"和"用户缩放"揉在一个变量里,且不响应 DPR 变化**:
   查 Qt 源码确认 —— `QWidgetWindow::handleDevicePixelRatioChange()` 只重绘、
   **不调整窗口几何**(qwidgetwindow.cpp:780);窗口逻辑尺寸不变 → 物理尺寸随 DPR 变化。
   而滚轮缩放走 `applyScale()` 时又用陈旧的 `m_scale` 重算,和 Qt 的平台层
   (`QWindowsWindow::handleDpiChanged` 在 WM_DPICHANGED 里 SetWindowPos)互相打架,
   于是拖动/滚轮来回时尺寸反复跳变,缩放下限(0.1)下再叠一次错误换算就小成一个点。

### 修复(`pin/pin_window.cpp` / `.hpp`)

| 改动 | 说明 |
| --- | --- |
| `m_scale` → `m_zoom` | 语义拆开:**`m_zoom` 只表示用户缩放**(1.0 = 1 图像像素对 1 物理像素),与 DPI 换算解耦 |
| 新增 `logicalSizeFor(dpr)` | 窗口逻辑尺寸 = `原图 × m_zoom ÷ dpr` ⇒ **物理尺寸恒为 原图 × 缩放**,与所在屏 DPR 无关 |
| 新增 `currentDpr()` | 取窗口**当前所在屏**的 DPR(`QWidget::screen()->devicePixelRatio()`),不用主屏 |
| 构造函数 | 改用**目标屏**(`screenAt(光标)`)的 DPR 换算初始尺寸 |
| 新增 `event()` 覆写 | 收到 `DevicePixelRatioChange` / `ScreenChangeInternal` 时,延到事件循环末尾重算尺寸 |

> 为何延后:Qt 在 `screenChanged` 里同步发 `DevicePixelRatioChange`(qwindow.cpp 的
> `init()` 连接),**早于**平台层 WM_DPICHANGED 的 `SetWindowPos`;立即 `resize` 会被
> 后者覆盖,故用 `QTimer::singleShot(0, …)` 在几何落定后再定尺寸。

### 验证与限制

- app 编译链接通过(`[37/37] Linking`)、无编译警告;
- ⚠️ **沙箱无法启动 GUI**,多屏 DPI 行为须在真实桌面验证。确认清单:
  1. 主屏 100% + 副屏 150%(或反之):在两个屏上分别按 `Ctrl+Shift+P` 贴同一张图 →
     两处应显示**同样的物理尺寸**(与原图 1:1),不再一大一小;
  2. 把一张贴图从 100% 屏拖到 150% 屏(再拖回)→ 尺寸**保持不变**;
  3. 滚轮缩放:1.0 档位下与原图 1:1;缩放到最小仍清晰可辨,不会变成一个点;
  4. 运行中改系统缩放(如 100%→125%)→ 贴图尺寸按新 DPR 自适应,不残留错位。

---

## 2026-10-08(四)设置页新增「清空历史数据」

### 背景

core 早已实现 FFI `clear_history`(删除全部条目 + 清理 blob 图片文件),但**没有任何
界面入口** —— 用户只能删单条(右键「删除」),无法一键清空。本轮在设置页补上。

### 实现(QML 单点,无 core/C++ 改动)

| 位置 | 改动 |
| --- | --- |
| `SettingsDrawer.qml` | 表单末尾新增「**危险操作**」分组:说明文案 + `danger` 变体按钮「清空历史数据」 |
| 二次确认 | **就地展开**确认块(说明 + [确认清空]/[取消]),不弹 Dialog/Popup —— 外层 SettingsDrawer 本身即 Popup,嵌套弹层有定位/焦点问题(同 ColorSwatchGrid 的既有取舍) |
| 新增 `clearHistory()` | 调 `Core.call("clear_history")` → 成功则 `Entries.refreshNow()`(重拉列表,内部一并刷新分类计数)并在状态行提示「已清空历史数据」;失败显示后端错误 |

细节:

- 说明文案带实时条数(「删除全部 N 条剪贴板历史…」),`Core.counts.all == 0` 时按钮
  **置灰**并提示「当前没有历史数据。」,避免"点了没反应";计数来自 `Core.counts`
  (countsChanged 通知驱动,单一数据源)。
- 清空后不触碰系统剪贴板:监听只在 `WM_CLIPBOARDUPDATE` 时入库,故清空后剪贴板里
  的内容不会立刻回填;再次复制才会重新入库。

### 验证与限制

- app 编译链接通过(`[6/6] Linking`)、无编译警告;`qmllint` 无新增告警类别
  (仅该文件既有的 unqualified / Theme 单例未解析告警);
- ⚠️ 需真实桌面确认:
  1. 设置 → 危险操作:按钮显示当前条数;有数据时可点,无数据时置灰;
  2. 点「清空历史数据」→ 出现就地确认块 → 点「取消」应无任何变化;
  3. 点「确认清空」→ 主面板列表清空、各分类徽标归零、blobs 目录图片文件被删除;
  4. 清空后按 `Ctrl+Shift+V` 面板应为空态;再复制一次内容应正常重新入库。

---

## 2026-10-08(四)补齐第一代能力(按优先级):贴图工具条/软上限 · 翻译历史 · 托盘入口

差距盘点后按"风险最低、体验最直接"的顺序补三项(详见本轮对话的功能盘点)。

### 1. 贴图窗口:ESC 关闭 + 悬停工具条 + 软上限 10 张

**问题**:`PinWindow` 此前只有拖动/滚轮/双击关闭 —— **无 ESC、无工具条**;
一旦设置里关掉「双击关闭贴图窗口」,贴图将**无法关闭**(只能退出程序)。且架构 §3.1
要求的"贴图软上限 10 张 + 淘汰提示"从未实现。

**改动**:

| 文件 | 内容 |
| --- | --- |
| `pin/pin_window.hpp` | 新增 `PinContent{image, text, idPrefix}`(文本贴图保留原文,供复制/另存按文本处理);`m_scale`→`m_zoom` 之外新增工具条/提示控件成员;新增 `showNotice()` 公开接口 |
| `pin/pin_window.cpp` | ① `keyPressEvent` 处理 ESC;② 悬停工具条(缩放值 / 复制 / 另存 / ✕ / 不透明度滑杆),右上角、移出延时 220ms 隐藏(指针移到工具条上不隐藏);③ 复制/另存:文本走原文(复制文本、存 `.txt`),图片走位图(复制图片、存 `.png`);④ 底部瞬时提示(已复制/已另存),**自包含在贴图窗口内**——主面板隐藏时应用级 toast 不可见 |
| `panel/core_facade.cpp` | `imageForEntry`→`contentForEntry` 返回 `PinContent`;`showPinWindow` 增加软上限:`QList<QPointer<PinWindow>>` 按创建顺序记录,超 10 张时关闭最旧的一张并在新贴图上提示 |

> 交互对齐第一代 `PinApp.tsx`:工具条含缩放值 / 复制 / 另存 / 关闭 / 不透明度滑杆,
> ESC 关闭。工具条控件一律 `Qt::NoFocus`,避免点击抢焦点导致 ESC 失效。

### 2. 翻译历史 UI(F26)

core 早已实现(内存 50 条 + `translate_history`/`clear_translate_history` FFI),但
**QML 无入口**。`TranslateWindow.qml` 语言栏新增「历史」按钮 → 覆盖式抽屉(D16):
列表显示原文/译文/语言对/提供方,点选即回填原文、译文**与当时所用的语言**;
可清空;空白处点击或 ESC 关闭(ESC 优先关抽屉,再退到关窗)。

### 3. 托盘补截图 / 贴图入口

`tray.cpp` 菜单补「矩形截图 / 全屏截图 / 贴图(最近内容)」,对齐第一代托盘。
截图前先收起面板 + 延时 150ms(与热键链路 `hidePanelThenCapture` 同一取舍,
避免面板被拍进画面);贴图口径与热键一致(默认"最近复制",设置可切"图片优先")。

### 验证与限制

- app 编译链接通过(`[5/5]` / `[7/7] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ **沙箱无法启动 GUI**,以下须真机确认:
  1. 贴图:悬停出现工具条;✕ 与 **ESC** 均可关闭(含"双击关闭"关掉时);复制/另存
     对文本条目走文本(.txt)、对图片条目走图片(.png);贴满 10 张后再贴,最旧的一张
     自动关闭并出现提示;
  2. 翻译:翻译若干条 → 点「历史」应列出;点某条回填原文/译文且语言栏同步;
     「清空」后列表为空;
  3. 托盘右键:矩形截图/全屏截图能正常唤起遮罩(面板已收起);「贴图(最近内容)」
     贴出最近条目。

---

## 2026-10-08(四)贴图四边加轻微阴影

### 背景

贴图是无边框**透明**窗口(`WA_TranslucentBackground`),系统不会给它画阴影 ——
浅色背景上白底截图容易与背景糊在一起,难以分辨边界。

### 实现(`pin/pin_window.cpp`)

- 窗口四周各留 `kShadowMargin = 12`(逻辑像素)留白:逻辑尺寸 = 原图 ÷ DPR × 缩放
  **+ 2×留白**;图片绘制在去掉留白的区域,`drawImage` 仍为 1:1(不影响既有 DPI 口径)。
- 阴影**自绘**:自外向内叠加 8 层 `alpha=10` 的圆角矩形(近图处层数最多、最深,
  向外逐层变淡 → 柔和渐变,最深约 27% 黑)。选它而非 `QGraphicsBlurEffect`:
  纯 `QPainter`、对透明窗口可靠、无额外模块依赖。
- 圆角随内容:图片条目直角(radius 0),文本贴图跟底板圆角 8。
- 工具条/瞬时提示改按**图片**右上/左下定位(不再按窗口边缘,否则会落进阴影留白)。

### 验证与限制

- app 编译链接通过(`[37/37] Linking`)、无编译警告;
- ⚠️ 需真机确认:浅色与深色背景上贴图四边均应出现柔和阴影、边界清晰;
  跨屏(不同缩放比)阴影粗细观感应一致;缩放/拖动时阴影随窗口同步、无错位;
  文本贴图阴影应贴合底板圆角(不露直角)。

---

## 2026-10-08(四)贴图工具条改为右键唤出(取代悬停)

### 背景

工具条原为**悬停显示**(鼠标移入贴图即弹出):贴图常被放在内容之上,鼠标划过就
弹出工具条会遮挡画面、也容易误触。改为**右键唤出/收起**。

### 实现(`pin/pin_window.{cpp,hpp}`)

| 项 | 变化 |
| --- | --- |
| 唤出方式 | `enterEvent`/`leaveEvent` + 220ms 延时隐藏 → `contextMenuEvent`:**右键切换**(展开时再右键收起),在**右键落点**展开并夹在窗口内 |
| 收起时机 | 左键点图面(开始拖动)、ESC、焦点离开贴图(点到其他应用) |
| ESC 两段式 | 工具条展开时先收起工具条,再按一次才关窗(与翻译窗口的历史抽屉一致) |
| 工具条上的右键 | 不参与开关(避免在滑杆/按钮附近误关) |
| 移除 | `m_hideToolbarTimer` / `kToolbarHideDelayMs` / `kToolbarMargin`(不再需要);`positionToolbar()` → `showToolbarAt()` + `clampToolbar()` |
| 顺带修正 | 另存的文件对话框会夺走焦点,保存后收回焦点,ESC 才能继续生效 |

> 说明:本轮发现阴影参数已被手工调小(`kShadowMargin` 12→6、`kShadowLayers` 8→3、
> `kShadowLayerAlpha` 10→8),**已按现状保留**,未回退。

### 验证与限制

- app 编译链接通过(`[7/7] Linking`)、无编译警告;
- ⚠️ 需真机确认:移入贴图**不再**弹出工具条;右键在落点处展开、再右键收起;
  左键点图面/ESC 可收起;点工具条按钮与拖滑杆时工具条不消失;点到其他应用后收起;
  ESC 两段式(先收工具条、再关窗)符合预期。

---

## 2026-10-08(四)贴图工具条新增「旋转」「编辑」

### 背景

贴图只能看不能改。补两个高频操作:**旋转 90°** 与**打开标注编辑器**。

### 实现

| 层 | 改动 |
| --- | --- |
| `pin/pin_window.cpp` | 工具条新增「↻ 旋转 90°」「✎ 编辑标注」两个按钮(置于缩放值之后);新增 `rotate90cw()` 纯函数、`rotateImage()` / `requestEdit()` / `setImage()` |
| `pin/pin_window.hpp` | 新增信号 `editRequested(const QImage&)` 与公开方法 `setImage(const QImage&)` |
| `panel/core_facade.cpp` | `showPinWindow` 里把 `editRequested` 接到编辑器:用贴图**当前**内容开编辑器;「完成」后走与历史条目编辑同一条通道(入库 + 写剪贴板),再 `setImage()` 回写贴图 |

要点:

- **旋转无损**:`rotate90cw` 做 32 位像素级置换(源 `(x,y)` → 目标 `(h-1-y, x)`),
  **不做插值**,反复旋转不会逐次糊化;旋转后宽高互换,以**画面中心为锚**重摆位,
  避免跳位。
- **旋转/编辑仅对图片贴图可见**:文本贴图是渲染出的位图,旋转与标注都不合适
  (按钮 `setVisible(m_text.isEmpty())`,文本贴图工具条自动收窄)。
- **编辑用贴图当前画面**(含已做的旋转),而不是回头读条目原图 —— 否则"先旋转再编辑"
  会把旋转丢掉;完成后 `setImage()` 把合成结果回写贴图,避免停留在编辑前画面。
- 贴图可能在编辑器开着时被关闭:回写用 `QPointer` 守卫。

### 验证与限制

- app 编译链接通过(`[37/37] Linking`)、无编译警告;
- ⚠️ 需真机确认:
  1. 右键工具条出现 ↻ / ✎(文本贴图不出现);
  2. 连点 ↻ 四次回到原图,画面中心不跳位,清晰度不劣化;
  3. 点 ✎ 打开编辑器,标注「完成」后:贴图内容同步为编辑结果、主面板列表出现新条目、
     剪贴板为编辑结果;
  4. 编辑器「取消」后贴图保持原样;编辑器开着时关掉贴图不应崩溃。

---

## 2026-10-08(四)修复小贴图工具条显示不全:工具条改独立弹层

### 现象

贴图较小时(窄于工具条约 264 逻辑像素),工具条**右侧的滑杆与 ✕ 被裁掉**。

### 根因

工具条原是贴图窗口的**子控件**,而 Qt 会把子控件**裁剪到父窗口范围**。定位代码
`qBound(0, pos.x(), qMax(0, width() - toolbarW))` 在窗口比工具条窄时,
`width() - toolbarW` 为负 → 被夹成 0 → 工具条从 x=0 起向右排,**超出部分被裁**。

关键结论:只要工具条是子控件,就**不可能**超出贴图范围 —— 内容比贴图大时,
只能"减少内容"或"改成独立窗口"。

### 实现(方案 B:独立弹层)

| 项 | 变化 |
| --- | --- |
| 窗口类型 | `new QWidget(this)` → `new QWidget(this, Qt::Popup \| Qt::FramelessWindowHint)` + `WA_TranslucentBackground`(顶层窗口需透明底,圆角才不被黑直角覆盖) |
| 定位 | `showToolbarAt(屏幕坐标)`:夹进**屏幕可用区**(`screen->availableGeometry()`)而非贴图窗口内 —— 贴图再小也完整可见 |
| 关闭 | 交给弹层自身语义:点外部/ESC 自动关闭(与右键菜单一致);删掉 `focusOutEvent` 与 `clampToolbar` |
| 焦点回收 | 新增 `eventFilter`:弹层 `Hide` 后 `activateWindow()+setFocus()`,使 ESC 继续生效 |
| 顺带 | `saveAs()` 先收起弹层再开文件对话框(弹层持鼠标抓取,可能挡住模态对话框) |

> 观感上大贴图不变:工具条本就在右键落点展开,落点在贴图内时弹层与之重合;
> 只有放不下时才"溢出到贴图外",这正是本次要的效果。

### 验证与限制

- app 编译链接通过(`[7/7] Linking`)、无编译警告;
- ⚠️ 需真机确认(弹层行为是本轮主要风险点):
  1. 很小的贴图(如 100×100)右键:工具条应**完整**显示,不被裁剪、不超出屏幕;
  2. 大贴图观感应与改动前一致;
  3. 点外部/ESC 应关闭弹层;关闭后 ESC 仍能关闭贴图(两段式);
  4. 点「复制/旋转/编辑」后弹层行为正常;「另存」能正常弹出文件对话框;
  5. 多屏/不同缩放比下,弹层应出现在光标所在屏且不越界。

---

## 2026-10-08(四)修复工具条发白:底板改为自绘

### 现象

工具条整体呈白色,按钮与文字看不清。

### 根因

工具条是**顶层弹层 + `WA_TranslucentBackground`**(上一轮改弹层的产物)。这种组合下,
QSS 里给控件**自身**的规则 `#pinToolbar { background: …; border: …; border-radius: … }`
**不生效** —— 底板没被画出来,只剩子控件的样式(半透明白按钮 `rgba(255,255,255,22)`、
浅灰文字 `#DFE1E8`)浮在桌面上,看起来就是"整条发白、看不清"。
(子控件规则是生效的,所以按钮/文字/滑杆都还在。)

### 实现(`pin/pin_window.cpp`)

- 新增 `ToolbarPanel : QWidget`(匿名命名空间内):在 `paintEvent` 里**自绘**深色圆角底板
  (填充 `rgb(38,39,46)` α244 + 1px 浅色描边 + 6px 圆角)。自绘与 QSS 无关,
  不受"顶层 + 透明底"组合的影响。
- 工具条改用 `ToolbarPanel`;去掉 `WA_StyledBackground`(不再需要 QSS 画自身背景),
  并**兜底**把 palette 的 `Window` 设为深色 —— 万一该环境透明底不生效,窗口底色也不会是白的。
- QSS 只保留**子控件**规则(`#pinZoom` / `#pinButton` / `#pinClose` / 滑杆)。

### 验证与限制

- app 编译链接通过(`[5/5] Linking`)、无编译警告;
- ⚠️ 需真机确认:工具条应为**深色圆角底板 + 浅色文字**,在任何桌面背景与明/暗主题下
  都清晰可辨;按钮 hover 变亮、✕ 为红色;底部瞬时提示(已复制/已另存)同样为深色底。

---

## 2026-10-08(四)编辑器补齐工具:直线 / 椭圆 / 模糊 / 序号 / 取色

### 背景

编辑器原只有 画笔 / 箭头 / 矩形 / 马赛克 / 文字 五类(第一代 F12)。本轮补上第一代
F16 的可选工具中除"延时截图"外的四项,并新增取色器。

### 实现(`editor/editor_window.{cpp,hpp}`)

`EditorTool` 扩为 10 项:`Pen, Arrow, Line, Rect, Ellipse, Mosaic, Blur, Number, Text, Picker`。

| 工具 | 实现 |
| --- | --- |
| **直线** | 拖拽类,`QGraphicsLineItem`,画笔 `m_color/m_size` + 圆头 |
| **椭圆** | 拖拽类,`QGraphicsEllipseItem`;与矩形共用一条分支(`QAbstractGraphicsShapeItem`) |
| **模糊** | 描边类,复用马赛克的"路径 mask + 纹理刷子"机制:构造时**惰性**生成 `m_blurSource`(缩到 1/8 再平滑放大 = 近似高斯),松手时按笔画轮廓填充 |
| **序号** | 点击即落一个自绘 `NumberBadgeItem`(实心圆 + 居中数字,数字色随底色明暗取反),编号会话内自增;**一个图元 = 一条撤销记录** |
| **取色** | 点击从**底图**取样(`m_base.pixelColor`,不受已画标注影响)设为当前颜色,并切回**上一个绘图工具**;采样后同步色块选中态(采到调色板外的颜色则全部取消选中) |

配套:新增 `syncColorSwatch()` / `syncToolAction()`(程序化切工具/改色后同步工具条选中态)、
`badgeRadius()`(圆点半径随粗细档位 14/18/26);色块 QAction 用 `setData(color)`、
工具 QAction 用 `setData(int(tool))` 以便反向同步。

### 验证与限制

- app 编译链接通过(`[38/38] Linking`)、无编译警告;
- ⚠️ 需真机确认:
  1. 直线/椭圆:拖拽有实时预览,松手成图,颜色/粗细跟随工具条;
  2. 模糊:涂抹区域应呈"磨砂"效果(与马赛克可叠加使用);纹理在首次选模糊工具时生成;
  3. 序号:每点一次落一个递增编号圆点,白/深色底上数字都清晰;可撤销;
  4. 取色:点击底图任意处 → 颜色即刻生效且工具自动切回上一个(如画笔),随后画的线即为该色;
     采到调色板外的颜色时,色块区应无选中项;
  5. 工具条变长(10 个工具 + 7 色块),窄窗口下应由 QToolBar 的扩展箭头兜底。

---

## 2026-10-08(四)重构列表右键菜单:面板级单例 + 外部点击屏障

### 现象

打开条目右键菜单后,左键点击**其他**列表项会与菜单冲突:菜单没能正常关闭,且那一次
左键还会**穿透**到下层卡片(触发"复制并收起面板"),两者互相干扰。

### 根因

菜单原先**每个 EntryCard 各持一个 `Menu`**:

1. `Menu` 是 Popup,委托被 ListView 回收/重建时,菜单与卡片的绑定可能失效;
2. 菜单打开时**没有"外部点击"拦截层** —— 落在菜单外的左键既走菜单的
   `CloseOnPressOutside`,又照常派发给下层卡片的 MouseArea,于是"关菜单"与
   "触发卡片动作"同时发生(点一下既复制又收面板),看起来就是菜单关不干净。

### 实现

| 文件 | 改动 |
| --- | --- |
| `Main.qml` | 新增**面板级唯一菜单** `entryMenu`(内容按目标条目类型在 `onAboutToShow` 重建)+ `menuTarget` 快照状态 + 菜单动作函数(复制/复制路径/编辑/删除/翻译);新增**外部点击屏障** `menuDismissBarrier` |
| `EntryCard.qml` | 删掉自持的 `ThemedMenu` 与仅菜单用的 `requestTranslate`/`copyPathAndHide`/`translateRequested`;改为 `signal contextMenuRequested(var entry)` 上抛**内容快照** + `property bool menuOpen`(父级注入,驱动"当前操作对象"高亮) |
| `panel_window.{hpp,cpp}` | 新增 `panelHidden()` 信号(QEvent::Hide 时发),供 QML 收起残留菜单 |

要点:

- **屏障**是铺满面板的透明 `MouseArea`(`z:900`,仅菜单打开时可见/可用),
  **吃掉**这一次外部点击并关闭菜单 —— 下层卡片收不到事件,彻底消除"点一下既关菜单
  又触发卡片"的干扰。菜单本体在 Popup 层(内容之上),不受屏障影响;Toast 提到 `z:1000`。
- **卡片只上抛快照**(entryId/kind/pinned/text),菜单不再依赖任何委托实例是否存活 ——
  这就是"独立性"的来源;`menuTarget` 为空即视为菜单未打开。
- 兼容性:卡片的左键复制、双击打开文件、hover 高亮、滚轮转发**全部未改**;仅在
  菜单打开期间多一层屏障。

### 验证与限制

- app 编译链接通过(`[44/44] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 右键某条 → 菜单弹出、该卡片呈"当前操作对象"高亮;
  2. 左键点**其他卡片**:菜单关闭、且**不会**触发那条卡片的复制/收起面板;
  3. 左键点**同一张卡片**同样只关菜单、不触发动作(再点一次才执行复制);
  4. 点菜单项正常执行并关闭;Esc 先关菜单(再按才收面板);
  5. 面板被收起(热键/点外部)后再呼出,不应残留菜单或屏障。

### 追加修复(真机反馈):左键仍会触发复制

**现象**:加了屏障后,左键点其他卡片**仍会触发复制**。

**根因(屏障与 Menu 争抢同一次点击)**:`Menu` 默认 `closePolicy` 含
`CloseOnPressOutside`,而 Qt 的"点外部关闭"是在**按下**时由窗口 overlay 先处理、
且**不消费**该事件:

1. 按下 → overlay 关掉菜单 → `onClosed` 把 `menuTarget` 置空;
2. 屏障 `visible: menuTarget !== null` 随之变 false → **屏障消失**;
3. 这次点击继续正常派发 → 落到下层卡片 → 触发复制。

**修复:让「外部点击」只有一个处理者。**

- `entryMenu.closePolicy: Popup.CloseOnEscape` —— 关掉 Menu 自身的外部点击关闭,
  屏障成为唯一处理者(它在菜单关闭前一直可见,自然能吃掉这次点击)。
- 卡片加防御:`menuOpenElsewhere`(父级注入)+ `menuDismissRequested()` 信号 ——
  万一平台事件路由差异让点击漏到卡片,也只关菜单、不执行动作,不会误复制、也不会卡住菜单。
- 点击**面板窗口之外**(其他应用)仍由既有的 `panelHidden` 兜底关闭;面板"固定"时
  该场景下菜单会留到下次点面板才关(可接受)。

---

## 2026-10-08(四)S5 差异化三件:关于/版本 · 统一素材库 · 标签与分组

### 1. 设置页「关于/版本」

- 版本号**单一来源**:`app/CMakeLists.txt` 里 `project(Nextboard VERSION 0.2.0)` →
  `target_compile_definitions(NB_APP_VERSION="${PROJECT_VERSION}")`,不再在 C++ 里另写一份。
- `CoreBridge` 补解析 `nb_version`(此前只解析了 `nb_abi_version`),`CoreBridge::coreVersion()`
  按 `major*10000+minor*100+patch` 还原成 "0.1.0"。
- `CoreFacade` 暴露 `appVersion` / `coreVersion` 两个 CONSTANT 属性;设置页新增「关于」区。

### 2. S5.1 统一素材库:翻译「存为条目」

| 层 | 改动 |
| --- | --- |
| nbcore `clip.rs` | 新增 `ingest_text(core, text, source)`:与剪贴板监听**共用**「大小过滤 → hash 去重 → 入库 → 淘汰」路径,并回写 `source`(与既有 `ingest_image` 同构) |
| nbcore `ffi.rs` | 新增 `save_text_entry(text, source)`,入库后 `notify_host(WM_APP_CLIP_CHANGED)` |
| `TranslateWindow.qml` | 原文/译文各加「存为条目」按钮 |

因为走的是同一条入库路径:与剪贴板里的同内容文本**天然去重**;入库后经 FTS 触发器
自动可被搜索;`source=translate` 让条目卡片显示来源徽标。

### 3. S5.2 标签与分组

| 层 | 改动 |
| --- | --- |
| nbcore `entry.rs` | 新增 `set_tags`(归一化:去空白/丢空串/按序去重,全空写 NULL)与 `list_tags`(标签+条目数);`list()` 增加 `tag` 过滤,LIKE 与 FTS 两条通道都加 |
| nbcore `ffi.rs` | 新增 `list_tags` / `set_entry_tags`;`list_entries` 接受 `tag` |
| `ClipEntryModel` | 新增 `TagsRole`、`setTag()`、`saveTags()`、`tags` 属性(标签统计)与 `refreshTags()`(与 `refreshCounts` 同源刷新) |
| `Main.qml` | 标签筛选条(chips,有标签才出现,再点取消);右键菜单新增「标签…」→ **内联编辑卡片**(按逗号/空白切分,去空去重);屏障沿用右键菜单同一套"外部点击关闭" |
| `EntryCard.qml` | 暴露 `tags` 并在元信息行展示标签 chips;菜单快照带上 tags 供编辑预填 |

**标签筛选用 `json_each`** 而不是 LIKE 子串:`tags` 存 JSON 数组字符串,`EXISTS (SELECT 1
FROM json_each(tags) WHERE value = ?)` 是精确匹配(SQLite ≥3.38 的 JSON 函数已内置,
bundled 版无需额外开关)。踩坑:`json_each(NULL)` 会产出**一行全 NULL**(不是零行),
统计必须加 `WHERE je.value IS NOT NULL`,否则"无标签条目"会变成一个空标签。

### 验证与限制

- core `cargo test`:**168 项**通过(新增 `tags_normalize_filter_and_count`,覆盖归一化/
  LIKE 与 FTS 两条通道的标签过滤/统计/清空/非法 id);
- app 编译链接通过(`[40/40] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 设置 → 关于:显示 `Nextboard 0.2.0` 与 `nbcore 0.1.0`;
  2. 翻译窗口:点「存为条目」→ 面板出现 `translate` 来源的文本条目,搜索其内容可命中;
  3. 右键某条 → 「标签…」→ 输入 `工作, 重要` → 保存:卡片出现两个标签 chip,
     面板顶部出现标签筛选条;
  4. 点筛选条上的标签 → 列表只剩带该标签的条目;再点一次取消;
     标签筛选与分类/搜索可叠加;
  5. 清空某条标签后,若该标签不再被任何条目使用,筛选条上应消失。

### 追加修复(真机反馈):标签清空后卡片内容消失 / 无法取消标签

**现象**:给条目设标签正常;随后在编辑器里把标签清空保存,该列表项**内容不再显示**;
且感觉"无法取消标签"。

**排查过程**(两步都做了实证,不靠猜):

1. **先排除"清空请求没发出去"**:怀疑 `QVariant(QStringList{})` 经
   `QVariantMap → QJsonObject` 会退化成 JSON `null`(那样 FFI 会报"缺少 tags 数组",
   表现为清空无效)。写了临时探针程序实测:**`isNull()=false`、序列化结果是 `Array`** ——
   该假设**不成立**,请求本身没问题。(探针已删除。)
2. **再验证数据层**:给 core 测试补断言 —— 清空标签后条目**仍完整可列**
   (`total` 正确、`text_content` 原样、`tags` 为 None)。数据层**无问题**。

结论:问题在 **QML 层**,定位到两个风险点:

| 风险 | 说明 | 处理 |
| --- | --- | --- |
| **`RowLayout` 里放 `Repeater`** | 委托在 Layout 中动态增删会让布局的隐式尺寸失效;清空标签时 Repeater 从 1 个委托销毁到 0 个,卡片塌陷 → 内容不可见(与现象吻合) | 卡片标签改为**单个 Label 拼接**(`tags.join(" · ")`),彻底不用 Repeater |
| **模型属性与角色同名** | 模型新增的 `Q_PROPERTY(QVariantList tags)` 与角色名 `tags`(条目自身标签)同名,委托里的 `tags` 有取到统计值而非本条目标签的风险 | 属性改名 `tags` → **`tagStats`**(QML 侧 `Entries.tagStats`) |

顺带补齐"取消标签"的可发现性:

- 右键菜单在条目**有标签时**多一项「**清除标签**」(一键清空,不必进编辑器删空);
- 编辑器输入框占位文案补上"留空保存即清除全部标签"。

### 验证与限制

- core `cargo test`:标签用例补了"清空后仍可列"的断言,全绿;
- app 编译链接通过(`[40/40] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 沙箱无法启动 GUI,以下须真机确认(这是本次修复的关键):
  1. 设标签 → 卡片显示标签、顶部出现筛选条;
  2. 进编辑器把标签删空保存 → **卡片内容应正常显示**(不再消失)、标签消失;
  3. 右键「清除标签」→ 同样只清标签、条目内容完好;
  4. 反复"设标签 → 清标签"多次,卡片高度与内容始终正常。

---

## 2026-10-08(四)补完 F16:延时截图(含两项核查结论)

### 1. 延时截图(F16 最后一个未实现项)

第一代 docs/phase5.md 把它列为「S6 首位,成本低(会话开始前倒计时),截菜单/悬浮提示刚需」。

| 层 | 改动 |
| --- | --- |
| `settings.rs` | 新增 `capture_delay_secs`(默认 3),`validate()` 限定 1~10 |
| `capture_session.{hpp,cpp}` | 新增 `startDelayedCapture(seconds, intent, …)`:先弹**倒计时数字**(置顶无边框、`WindowTransparentForInput` 鼠标穿透、`WA_ShowWithoutActivating` 不抢焦点 —— 用户还要去展开菜单),倒计时结束**先收起倒计时再等 80ms** 才采屏(否则数字会被冻进底图) |
| `tray.cpp` | 托盘新增「延时截图」;**点击时**才从设置读秒数(改设置即时生效),菜单展开时把当前秒数写进标题 |
| `SettingsDrawer.qml` | 截图区新增「延时截图等待(秒)」1~10 |

### 2. 核查结论:编辑器跨屏 DPI **不是缺陷**(此前我两次提示过,现关闭该项)

逐层核对后确认行为正确:

- **导出**:`compose()` 用 `m_scene->render(&painter, QRectF(0,0,W,H), m_scene->sceneRect())`
  —— 源矩形是场景矩形(图像像素)、目标矩形是 `W×H` 像素,**恒为 1:1,与屏幕 DPI 无关**;
- **显示**:场景单位 = 图像像素,视图变换把场景映射到**逻辑**像素。2x 屏上 1 图像像素
  = 1 逻辑像素 = 2 设备像素 —— 这是 DPI 感知应用的标准行为(UI 本身也放大 2 倍),
  且 `fitCanvas()` 用逻辑视口尺寸算适配,任何 DPI 下都能完整显示;
- **标注尺寸**:画笔 `m_size=4` 是**图像像素**,在 2x 屏上渲染为 8 设备像素,
  相对图像内容仍是 4 像素宽 —— 与导出结果一致,这才是关键性质。

即:编辑器在 2x 屏上"看起来更大"是正常的 DPI 缩放,不是缺陷,无需修改。

### 3. 明确不做:译文「另存为 .txt」

F26 曾提到译文另存;已实现过一版(`CoreFacade::saveTextAsFile` + 译文区按钮),
**经用户确认不需要,已完整撤销**(相关文件与 HEAD 一致,无残留)。

### 验证与限制

- core `cargo test`:168 项通过(默认值用例补 `capture_delay_secs == 3`);
- app 编译链接通过(`[45/45] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 托盘 →「延时截图(3 秒)」:面板收起 → 屏幕中央出现倒计时数字(鼠标可穿透,
     能正常去点开菜单)→ 倒计时归零后进入框选;
  2. **倒计时数字不得出现在截出的图里**;
  3. 设置里把等待改为 5 秒 → 再展开托盘菜单,标题应显示「延时截图(5 秒)」;
  4. 倒计时期间按 Esc / 再次触发截图的行为符合预期(会话尚未开始,应无副作用)。

---

## 2026-10-08(四)快捷键:允许留空 + 新增「截图取字入列表」「延时截图」

### 1. 快捷键允许留空(不绑定)

| 层 | 改动 |
| --- | --- |
| `settings.rs` | `validate()` 去掉"不能为空"的硬约束:空/纯空白 = **不绑定该动作**;非空项仍校验合法性与两两不重复 |
| `hotkeys.cpp` | 注册侧本就跳过空串(`if (!combo.isEmpty())`),无需改动 —— 现在"存得下、也注册得上"一致了 |
| `SettingsDrawer.qml` | `HotkeyField` 支持**按 Delete/Backspace 清空**绑定(原先只能 Esc 取消录制);提示文案补上说明 |

> **默认值取舍**:新增的两个动作**默认留空**;既有 6 个动作保留原默认键
> (Ctrl+Shift+V/A/Q/P/T/E)—— 它们是应用的主入口,清空会让新装用户无从呼出。
> 若希望既有项也默认留空,改 `HotkeyMap::default()` 即可。

### 2. 新增快捷键动作

`HotkeyAction` 6 → 8,`Hotkeys::Action` 同步扩两个:

| 动作 | 设置键 | 默认 | 行为 |
| --- | --- | --- | --- |
| **截图取字(入列表)** | `captureOcr` | 留空 | 收起面板 → 框选 → 入库 → OCR → 识别文本**直接存为条目**(`source=ocr`);`allowEditor=false` 全程直通 |
| **延时截图** | `captureDelayed` | 留空 | 收起面板 → 倒计时(取 `captureDelaySecs`)→ 框选 |

- `CoreFacade::processCapture` 新增 `"entry"` 模式:OCR 文本走 `save_text_entry`
  —— 与翻译「存为条目」**同一条入库路径**,因此天然去重、入库即可被搜索。
- `captureDelaySeconds()` 提到 `capture_session`(托盘入口与快捷键共用,不再两处各写一份)。
- 新增来源配色 `Theme.colors.sourceOcr`(绿),条目卡片上 `source=ocr` 显示为绿色徽标。

### 验证与限制

- core `cargo test`:**169 项**通过(新增 `hotkeys_may_be_left_unbound`;
  原 `validate_rejects_empty_hotkey` 语义反转,改名 `validate_treats_blank_hotkey_as_unbound`;
  `hotkey_map_get_maps_actions` 改为只对**已绑定**的默认键做去重断言);
- app 编译链接通过(`[43/43] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 设置 → 快捷键:新增两项显示「未设置」;点一下再按 Delete 可把已绑定的项清空,
     保存后该动作不再响应(其他动作不受影响);
  2. 给「截图取字(入列表)」绑一个键 → 触发 → 框选一段有文字的截图 →
     面板列表出现 `ocr` 来源的文本条目,内容可被搜索;不弹编辑器、不弹翻译窗口;
  3. 给「延时截图」绑键 → 触发 → 出现倒计时 → 归零后进入框选;
  4. 清空全部快捷键后保存应成功(不再报"快捷键不能为空")。

---

## 2026-10-08(四)翻译默认通道改为微软免密钥端点 + 自定义设置可读性

### 1. 微软翻译改为**免授权端点**,并作为默认通道

**动机**:原先默认是 `openai`(需用户自备 Key),新装用户不配密钥就**完全无法翻译**。

依据 2026-08 起 Edge 内置翻译的端点变更,契约改为:

```
POST {endpoint}/translatetext?from=<tag>&to=<tag>&isEnterpriseClient=false
Content-Type: application/json
Body: ["待翻译文本"]            ← 字符串数组(支持批量)
→ [{"translations":[{"text":"…","to":"en"}]}]
```

| 层 | 改动 |
| --- | --- |
| `microsoft.rs` | 全量重写:去掉订阅密钥头与区域头、请求体由 `[{"Text":…}]` 改为字符串数组、`from` 自动检测时拼成**空值** `from=`(带 `auto` 会被判非法)、路径改为 `/translatetext` |
| `settings.rs` | `MicrosoftProvider` 精简为只剩 `endpoint`(默认 `https://edge.microsoft.com/translate`);**默认提供方 `openai` → `microsoft`**;校验去掉区域规则 |
| `translate/mod.rs` | 探测文案改为「可用(端点 · 免密钥)」 |
| `SettingsDrawer.qml` | 服务商下拉顺序改为**微软在前**;去掉密钥/区域字段,改为端点 + 说明;补充"非公开端点、无 SLA"的提示 |

**权衡**:免授权端点属 Edge 前端内部接口,**无 SLA**;因此端点保留可配置,失败时给可读错误,
用户可随时切到 `openai` / `custom`。原 Azure 订阅密钥版实现**已按需求移除**。

### 2. 自定义 HTTP 设置更易懂

- 顶部总说明:讲清"用占位符描述发什么/从哪取译文",列出 5 个占位符与 `${name:-默认值}` 写法;
- 逐字段说明:请求地址(可用占位符、按 URL 编码)、API Key(**只替换占位符,不会自动加鉴权头**)、
  模型/部署名、请求头(每行 `Name: Value`)、请求体(原文引号换行会自动转义)、响应取值路径;
- 末尾给一份**可照抄的完整示例**(DeepL 风格:地址/请求头/请求体/响应路径),并提醒
  各服务语言代码写法可能不同(DeepL 用 `ZH`,本项目默认发 `zh-Hans`)。

### 3. 请求头/请求体编辑框可滚动

原先两个 `TextArea` 用固定 `preferredHeight`(54/66)且**不可滚动**,内容一多就被截断看不到。
现改为 `ScrollView` 包裹:高度提到 84/96、`clip: true`、横向滚动条关闭(靠自动换行),
并补上主题化背景与聚焦高亮边框。

### 验证与限制

- core `cargo test`:**166 项**通过。改动涉及的用例:
  `dispatch_reports_missing_key_per_provider` → 收窄为 `..._for_keyed_provider`(microsoft 不再需要密钥);
  `validate_checks_microsoft_region_and_proxy` → `..._endpoint_and_proxy`;
  microsoft 侧用例全部重写,新增"免授权:请求里**不得出现任何鉴权头**"与
  "自动检测时 `from` 必须为空值"两条契约测试;
- app 编译链接通过(`[6/6] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 设置 → 翻译:默认即为「微软翻译(免密钥)」,**不填任何密钥**点「测试连接」应返回可用;
  2. 翻译窗口实际翻译一段中英文,译文正确;断网时应给出可读错误而不是静默失败;
  3. 切到「自定义 HTTP」:请求头/请求体输入多行内容后**可滚动查看**,不再被截断;
     说明文字与示例可读、能照着填通一个自定义服务。

### 追加修复(真机反馈):微软端点测试报 404

**现象**:切到「微软翻译」点「测试连接」→ `microsoft 接口地址可能有误(HTTP 404):
{"error":"Not found","rc":1}`。

**排查**(先实测端点、再查真实配置,没有靠猜):

1. 直接 curl 文档给的地址 —— **HTTP 200 且返回正常译文**(还带 `detectedLanguage`):
   `POST https://edge.microsoft.com/translate/translatetext?from=&to=zh-Hans&isEnterpriseClient=false`
   → 端点与我的实现都是对的,问题在**实际发出的地址**。
2. 读本机真实配置 `%LOCALAPPDATA%/Nextboard/Nextboard/app_config.json`:
   ```json
   "translateMicrosoft": { "endpoint": "" },
   "translateProvider": "openai"
   ```
   **端点是空串** —— 早期 UI 只把默认地址当 `placeholderText`,值本身是空的;
   一旦保存就写进空串,`endpoint("")` 会拼出**相对地址** `/translatetext?…` → 请求必然失败。

**修复(两处,防御 + 可见)**

| 位置 | 改动 |
| --- | --- |
| `translate/microsoft.rs` | 新增 `pub const DEFAULT_ENDPOINT`(单一来源);`endpoint()` 在 `base` 为空时**回退默认端点**,不再拼相对地址 |
| `settings.rs` | `MicrosoftProvider::default()` 改用该常量;新增加载期修复 `repair_blank_microsoft_endpoint()`(与既有 `migrate_legacy_*` 同套路),把空端点补成默认值 —— 这样**设置页输入栏里会直接显示** `https://edge.microsoft.com/translate`,而不是只有灰字提示 |

**说明**:`translateProvider` 仍是 `openai` 属**正常**行为 —— 你此前显式配过 openai(智谱),
新默认值只对**全新配置**生效,不会覆盖用户的既有选择。要用微软通道,在下拉里选「微软翻译」即可。

**验证**:core `cargo test` **168 项**通过(新增 `endpoint_falls_back_to_default_when_base_blank`
与 `load_repairs_blank_microsoft_endpoint`,后者同时断言"已填自定义端点不得被覆盖");
app 构建通过、无警告。真机确认:重启后设置 → 翻译 → 微软翻译,输入栏应**直接显示**
`https://edge.microsoft.com/translate`,点「测试连接」应返回可用。

---

## 2026-10-09(五)翻译语种代码可在主界面自定义

### 背景

接彩云小译(自定义通道)时发现语言代码对不上:彩云简体中文是 `zh`,本项目是 `zh-Hans`,
拼出的 `auto2zh-Hans` 被服务拒绝(**实测 HTTP 409 Unsupported trans_type**)。

### 先做实测,再定方案

| 探针 | 结果 |
| --- | --- |
| 彩云 `trans_type=auto2zh` | HTTP 200 ✓ |
| 彩云 `auto2zh-Hans` | HTTP **409** `Unsupported trans_type` ✗ |
| 微软端点 `to=zh` | HTTP **200** ✓(`to=zh-Hans` 同样 200) |

结论:**两家都接受 `zh`** —— 所以不需要"按服务商维护一张语言代码映射表",
只要语言代码本身可自定义就够了(原计划加的 `langMap` 字段已按此**撤除**,
设置页保持"统一使用 `${from}` / `${to}`"的简单形态)。

### 实现(翻译窗口)

| 位置 | 改动 |
| --- | --- |
| `TranslateWindow.qml` | 语言栏新增**可编辑的「语种代码」输入框**(紧邻目标语言下拉);下拉仍负责常用语言,选中时同步写入代码框;代码框可直接改写为任意代码(回车/失焦即保存) |
| 同上 | 新增 `targetCodeValue()` / `applyTargetLang()`:`doTranslate` 与"历史回填"统一以**代码框为准**(此前只读下拉的平行数组,自定义代码会被忽略) |

这样 `${from}` / `${to}` 携带的就是用户填的代码,彩云、DeepL(大写)这类
"代码写法不同"的服务都能直接用,不必改设置页模板。

### 验证与限制

- core `cargo test`:**168 项**通过(撤除映射表后回到原数量,无残留);
- app 编译链接通过(`[8/8] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 翻译窗口语言栏出现「语种代码」框,默认显示当前目标语言代码(如 `zh-Hans`);
  2. 把它改成 `zh` → 翻译一段英文:彩云通道应正常出中文(不再 409);
  3. 从下拉选「English」→ 代码框自动变 `en` 并持久化;重开窗口应保持;
  4. 点翻译历史回填:代码框应回到该条历史当时使用的语言代码。

---

## 2026-10-09(五)语言下拉显示修复 + 彩云小译内置化

### 1. 修复:语言下拉被省略成「…」

上一轮把目标语言下拉写成固定 `Layout.preferredWidth: 110` 以腾出代码框的位置,
结果「简体中文」放不下、被 `elide` 成「…」(用户截图确认)。

改为**按内容自适应**并保留下限:`Layout.preferredWidth: Math.max(140, implicitWidth)` ——
Basic ComboBox 的 `implicitWidth` 由 `displayText` 宽度推出,语言名长短不一也不会再截断;
代码框相应收窄到 78 以守住这一行不溢出(窗口宽 520)。

### 2. 新增内置服务商:彩云小译(LingoCloud)

先实测确认了接口行为(见上节),再按同一套 provider 结构实现:

| 层 | 改动 |
| --- | --- |
| `translate/caiyun.rs`(新增) | `POST {endpoint}/v1/translator`;请求头 `X-Authorization: token <token>`(**前缀 `token ` 是协议的一部分**);请求体 `{"source":[…],"trans_type":"auto2zh","detect":…,"media":"text"}`;响应取 `target[0]`;200 但 `rc != 0` 时优先报出服务给的原因 |
| 同上 | **语言代码转换**:本项目 `zh-Hans` → 彩云 `zh`(实测直接发 `zh-Hans` 会 **409**);`zh-Hant` 与其余代码一致;空/`auto` 统一成 `auto` 且 `detect=true` |
| 同上 | 从响应 `trans_type`(如 `en2zh`)反推源语言,填进 `detected_source` |
| `settings.rs` | 新增 `CaiyunProvider { endpoint, token }`;**默认 Token 即官方测试 Token `3975l6lr5pcbvidl6jl2`**;provider 白名单加 `caiyun` |
| `translate/mod.rs` | `Provider::Caiyun` 枚举/`as_str`/`parse`/分派/探测/端点探测全部接入 |
| `SettingsDrawer.qml` | 下拉新增「彩云小译(内置测试 Token)」+ Token / 接口地址两个字段 |

**关于测试 Token**:官方文档明确"不保证该 Token 的可用性",故设置页按需求给出提示 ——
*"已预填官方测试 Token,装上即可试;官方不保证其稳定可用性,持续使用请到彩云科技开放平台
申请正式 Token 后替换(新用户有 100 万字 / 1 个月免费额度)"*。

### 验证与限制

- core `cargo test`:**177 项**通过(新增 9 条彩云用例:语言代码转换 / 方向串拼接 /
  请求体形状 / 端点回退 / 响应解析(含 200 带业务错误) / 401 映射 / 缺 Token /
  环回端到端(**钉住 `token ` 前缀**与请求体形状));
- app 编译链接通过(`[8/8] Linking`)、无编译警告;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 翻译窗口语言下拉完整显示「简体中文」等语言名(不再是「…」);
  2. 设置 → 翻译选「彩云小译」→ Token 已预填测试 Token,点「测试连接」返回可用;
  3. 用彩云翻译中英双向:选「简体中文」→ 英文出中文;把语言代码改成 `en` → 中文出英文;
  4. Token 改成乱码 → 报「鉴权失败(HTTP 401): Invalid token」而不是静默失败。

---

## 2026-10-09(五)内存收口 Tier 1:遮罩空闲超时 + 截图零拷贝 + 内存埋点

**动机**:路线图 S6.1「遮罩空闲超时销毁」一直未做;且经代码核对,本应用的大内存
持有者是**贴图(全分辨率原图 ×≤10)**、**翻译窗口的第二个 QML 引擎**、**截图遮罩**
与**编辑器(5 份全尺寸副本)**。本轮先做三项**低风险、可量化**的改动,建立实测基线。
（口径:4K 全屏 RGBA ≈ 33MB/份。）

### 1. 遮罩空闲超时(S6.1)

| 层 | 改动 |
| --- | --- |
| `settings.rs` | 新增 `overlay_idle_secs`(默认 **600 = 10 分钟**;`0` = 不自动取消;校验 0~3600) |
| `capture_session.{hpp,cpp}` | `OverlayWidget` 新增 `activity()` 信号(鼠标按下/移动/松开、按键均上报);会话级单例 `QTimer` 计时,任一遮罩有动作即重置,超时 → `closeAllOverlays()` 释放整屏底图 |
| `SettingsDrawer.qml` | 截图区新增「遮罩空闲超时(分钟,0=不自动取消)」 |

原缺口:触发框选后离开,33MB×屏数 的冻结底图会**无限期驻留**。

### 2. 截图缓冲零拷贝

`CoreBridge::captureMonitor` 原先 `image.copy()` 深拷贝一份整屏(4K 约 33MB),只为
在拷贝后释放核心缓冲。改为用 `QImage` 的 `cleanupFunction` 托管核心缓冲:最后一份
引用析构时回调 `releaseCoreBuffer` → `nb_free_buffer`。**峰值少一份整屏**。
生命周期依赖:`shutdown()` 不 `FreeLibrary`(既有约定),故回调指针始终可调用。

### 3. 内存埋点

新增 `app/src/host/memory_probe.{hpp,cpp}`:`GetProcessMemoryInfo` 取工作集/私有/峰值;
`logMemory(tag)` 在关键事件各写一条 `mem[...]` 日志;托盘菜单新增只读「内存 …」项
(菜单展开时刷新,与日志同一口径)。

埋点位置:启动 / 面板显隐 / 截图会话始末 / 贴图新增(含淘汰) / 编辑器开关。

**读数时机**:`close()` 与编辑器析构都走 `deleteLater`,在关闭处立即读会得到
"尚未回收"的水位,故 `capture-end` 与 `editor-closed` 都**延后一拍**再读。

### 验证与限制

- core `cargo test`:**178 项**通过(新增 `validate_bounds_overlay_idle_timeout`;
  默认值用例补 `overlay_idle_secs == 600`);
- app 编译链接通过、**无编译警告**;`qmllint` 无新增告警类别;
- ⚠️ **本轮必须真机实测**才能定后续档位:
  1. 触发框选后放着不动 → 10 分钟自动取消;`nextboard.log` 出现
     `截图会话空闲超时,自动取消并释放遮罩` 与 `mem[capture-end]`;
  2. 框选期间持续移动鼠标 → **不**超时;
  3. 设置里把超时改为 0 → 不再自动取消;
  4. 托盘菜单出现「内存 工作集 X MB · 私有 Y MB」,展开即刷新;
  5. 日志读基线:`mem[startup]` → `mem[capture-start]`(应 +屏数×33MB)→
     `mem[capture-end]`(应回落);编辑器同理看 `mem[editor-open]` / `mem[editor-closed]`。

**下一步(待基线数据)**:Tier 2 —— 翻译窗口「关闭即销毁」/共享引擎、面板隐藏释放
场景图、贴图淘汰改「数量 + 字节」双上限。

---

## 2026-10-09(五)内存收口 Tier 2:实测定位 + 翻译窗口/面板/贴图三项

### 实测基线(真机日志,4K 屏)

| 时点 | 工作集 | 私有 |
| --- | --- | --- |
| `startup` | **190.2 MB** | 131.5 MB |
| `panel-hide`(用过一轮后) | 258.3 MB | 209.3 MB |
| `panel-show` | **331.6 MB** | 284.9 MB |
| `capture-end`(会话峰值) | **691.7 MB** | 576.2 MB |

**结论**:①启动基线 190MB 已超 140MB 预算;②面板反复显隐把它推到 330MB;
③截图会话峰值 692MB。原先估的三个大头里,**「面板 + 第二个 QML 引擎」才是主因**,
贴图那一项本轮没被触发(日志无 `pin-add`),故暂按预算风险处理。

### 0. 先修埋点:读数时机(否则后面全是错数据)

`capture-end` 原先用 `QTimer::singleShot(0)` 延后一拍,但**仍早于 deleteLater** ——
日志里 597.7MB → 下一次 `capture-start` 立刻回落到 339.3MB,自相矛盾即是证据。
改为等**最后一块遮罩真正销毁**(`destroyed` 信号 + 计数)再读。
不能在信号栈上强制跑 deleteLater:那会在该遮罩自己的事件处理中途析构它。

### 1. 翻译窗口:懒创建 + 关闭即销毁(攻启动基线)

`main.cpp` 原先启动时就构造 `TranslateWindow` 并常驻 —— 它自带一个 QQuickWidget,
即**第二个 QQmlEngine + 场景图**(实测约占 40~60MB)。改为首次打开才创建、
`hideWindow()` 改 `close()`(WA_DeleteOnClose),恢复架构 D12 的取舍。

配套:`~TranslateWindow()` 记下窗口位置,下次重建沿用并夹回屏幕可用区 ——
销毁只该省内存,不该把用户拖到的顺手位置也一起丢掉。

### 2. 面板隐藏时释放列表(攻 190→331 的增长)

增长来自列表条目的 QML 委托与图片纹理;且**隐藏期间剪贴板通知仍在刷新模型**,
所以内存一直不回落。

| 位置 | 改动 |
| --- | --- |
| `ClipEntryModel` | 新增 `release()`(清空全部行 → 委托销毁、纹理下线;打断待执行的防抖刷新)/ `acquire()`(解除拒绝并立即重拉) |
| 同上 | `reload()` / `refresh()` / `refreshNow()` / `fetchMore()` / `canFetchMore()` 在已释放时一律拒绝 |
| `qml_bootstrap.cpp` | `ShowRefresher` 改为 `PanelListLifecycle`:面板 `panelHidden` → `release()`;`QEvent::Show` → `acquire()`(首帧前重拉,不留空白) |

### 3. 贴图改「张数 + 字节」双上限

数量上限 10 张对**全分辨率原图**没有约束力:4K 截图 33MB/张 → 最坏 332MB。
新增 `kPinByteBudget = 96MB`(即架构 §3.1 的「3–10MB/张 × 10 张」),与张数取先触发者;
淘汰提示同步改为「最多 N 张 / 约 96 MB」。

### 验证与限制

- core `cargo test` **178 项**通过;app 编译链接通过、**无编译警告**;
  `qmllint` 无新增告警类别;
- ⚠️ 需真机复测(重点这三条):
  1. **启动基线**:未打开过翻译窗口时 `mem[startup]` 应明显低于 190MB;
     打开翻译 → 关闭后托盘「内存」应回落(窗口已销毁);
  2. **面板显隐**:反复呼出/收起,`mem[panel-hide]` 不应再逐轮爬升;
     收起再呼出时列表正常(不空白、不闪);搜索/标签筛选在重开后仍生效;
  3. **贴图**:连贴多张 4K 截图,应在到 10 张之前先因字节超限淘汰,提示文案正确;
  4. `capture-end` 读数不再与下一次 `capture-start` 自相矛盾。

---

## 2026-10-09(五)基线内存:先纠正一处误判 + 面板隐藏释放场景图

### 纠正:上轮"启动基线含翻译窗口"的说法是错的

`mem[startup]` 的埋点位于 `panel.showAtCursor()` 之后、`TranslateWindow` 构造**之前**
(见 main.cpp),所以 190/202MB **从来没有包含过翻译窗口**。上一轮把它列为
"抬高启动基线"的元凶属于误判 —— 那个改动的真实收益是**打开翻译期间**少一份 QML 运行时,
对启动数字没有影响。两轮实测(190.2 / 202.2MB)基本一致即是佐证。

### 那 190MB 到底是什么

按代码结构看,基线由四块构成,**没有一块是"那个小翻译页"**:

| 段 | 内容 |
| --- | --- |
| Qt 运行时 + 平台插件 | Qt Widgets / Quick / QuickControls2 / QuickWidgets 的静态初始化 |
| Qt Quick 引擎 | QQmlEngine、场景图、渲染线程、RHI(D3D11)设备与着色器 |
| 主面板 QQuickWidget | 整棵 Main.qml 对象树(含 1244 行的 SettingsDrawer —— 它是 Main.qml 的
**直接子项,启动即整棵构建**,哪怕从不打开)+ 列表委托与缩略图 |
| nbcore | Rust 运行时 + SQLite |

即:这是一个 **Qt Quick 应用的运行时地板**,而不是某个页面的开销。故本轮做了两件事:
把可释放的部分放掉,并加上**分段埋点**以便用数据而不是推测来定位。

### 1. 面板隐藏时释放场景图(S6.3)

QQuickWidget 文档 *"Scene Graph and Context Persistency"* 明确说明:它 **honors**
`QQuickWindow::isPersistentSceneGraph()`,默认持久 —— 即面板隐藏后场景节点与
纹理**继续驻留**。而本应用的面板绝大多数时间都是隐藏的(托盘常驻 + 失焦即隐),
这笔驻留纯属浪费。

```cpp
if (QQuickWindow* window = m_quick->quickWindow())
    window->setPersistentSceneGraph(false);
```

代价:每次呼出要重建场景,首次显示略有延迟(需真机确认可接受)。
注意文档的另一条:`QQuickWidget` 的 OpenGL 上下文**始终**持久,该开关只放场景资源。

### 2. 启动分段埋点

`main.cpp` 新增 `mem[app-init]` / `mem[core-ready]` / `mem[qml-env]` / `mem[panel-built]`,
翻译窗口新增 `mem[translate-open]` / `mem[translate-closed]` —— 基线偏高时可直接看出
是哪一段吃掉的,同时能量化"第二个 QML 引擎"到底值多少。

### 待定:尚未做的杠杆

| 杠杆 | 预期收益 | 代价 |
| --- | --- | --- |
| `SettingsDrawer` 改 `Loader` 延迟实例化(1244 行,启动即整棵构建) | 数 MB | 低(需给 4 处引用加空判) |
| 面板长期隐藏后**卸载整棵 QML**(`setSource(QUrl())`,下次呼出重建) | 数十 MB | 中:呼出需重建,且丢失搜索词/滚动位置 |
| 结构性:主面板脱离 Qt Quick | 最大 | 不可行(等于重写 UI 层) |

真机复测:看 `mem[app-init] → core-ready → qml-env → panel-built` 四段各占多少,
以及 `mem[panel-hide]` 是否明显低于改动前的 258MB。

---

## 2026-10-09(五)翻译并入主面板:整窗模式切换(去掉第二个 QML 引擎)

**动机**:上一轮分段实测显示基线 188.6MB 里 **Qt 运行时只占 25.5MB**,其余 160MB
全是主面板自己(QML 树 +92MB、首次渲染 +68MB)。翻译作为**独立顶层窗口**还要再叠
一份同量级开销(第二个 `QQmlEngine` + 它自己的场景图/渲染栈)。而两者本来就互斥
(打开翻译即收起面板),所以"独立窗口"没换来任何功能,只换来内存。

**不采用抽屉**:抽屉只占一侧、滑入滑出动画廉价、还要来回开关。改为**整窗模式切换** ——
同一窗口、同一 `QQuickWidget`、同一 `QQmlEngine`,内容整体换掉(0ms 切换,无位移动画)。
翻译区还因此比原来的 520×660 更大。

### 结构

| 层 | 改动 |
| --- | --- |
| `panel/qml/TranslateWindow.qml` | → **`TranslateView.qml`**:根对象 `Rectangle`→`Item`(窗口壳归面板)、去掉自带圆角/描边、5 处 `translateWindow.*` 改为面板接口、新增 `exitRequested` 信号与 `dismissTopLayer()`(Esc 分层交回面板统一排队) |
| `panel/qml/Main.qml` | 新增 `translatePageBuilt` 与 `openTranslate(text)`;列表页 `visible: mode === "list"`;其后插入 `Loader` 延迟构建翻译页(`active: translatePageBuilt`,之后常驻 → 切走再回来内容还在);Esc 改为四层排队(设置抽屉 → 翻译页浮层 → 退出翻译页 → 收起面板) |
| `panel/panel_window.{hpp,cpp}` | 新增 `Mode{List,Translate}` + `mode` 属性 + `setMode` + `showTranslate(text)` + `exitTranslateMode()`;两套尺寸/下限 + `applyModeSize()`(先设下限再 resize、保持窗口中心、夹回屏幕可用区);新增 `m_holdOpen` —— 翻译页只**抑制失焦隐藏**,不动用户的固定状态、也不置顶 |
| `panel/qml_bootstrap.cpp` | `PanelListLifecycle` 改为"**可见 且 停在列表页**"才算在线:进翻译页 → `Entries.release()`,回列表页 → `acquire()` |
| `main.cpp` | 删掉懒创建 `TranslateWindow` 的 lambda;`translatePrefill` 改接 `panel.showTranslate(text)` |
| `translate/translate_window.{hpp,cpp}` | **整对删除**(目录一并移除) |
| `CMakeLists.txt` / `tray.cpp` / `hotkeys.cpp` / `core_facade.*` | 源列表与 QML 条目更新;文案与注释同步 |

`CoreFacade` 的三个入口(`openTranslate` / `openTranslateWith` / `translatePrefill`)
**未改**,所以面板按钮、条目右键、全局热键三条链路**一处调用点都没动**。

### 两套尺寸(由 C++ 独占,QML 不碰)

| 模式 | 尺寸 | 下限 |
| --- | --- | --- |
| List | 800 × 600 | 620 × 380 |
| Translate | 520 × 660 | 420 × 500 |

⚠️ 切换顺序必须**先设新下限、再 resize**:520 < 620,反过来会被旧下限顶成 620 宽。

### 验证与限制

- app 编译链接通过、**无编译警告**;`qmllint` 无新增告警类别;
- ⚠️ 需真机确认:
  1. 热键/托盘「打开翻译」→ 窗口**原地变成翻译页**(尺寸切到 520×660、位置不跳);
  2. 翻译页里 Esc → 先关历史抽屉、再退回列表页(**不**收起窗口);列表页 Esc 才收起;
  3. 翻译页输入时点其它窗口 → **不**自动隐藏;退出翻译页后恢复"失焦即隐";
  4. 面板「固定」按钮行为不受影响(翻译页不改变用户的固定状态,也不置顶);
  5. 切走再回翻译页 → 原文/译文还在;
  6. 「截图取字」→ 面板先收起再框选,截出的图里不含面板;
  7. 内存:进翻译页看 `mem[mode-translate]`、退出看 `mem[mode-list]` ——
     两者应基本相等(翻译页多一棵 QML 树,列表已 release)。

### 已知取舍

- 面板是 `Qt::Tool`(不占任务栏),原翻译窗口是 `Qt::Window`(占)——
  合并后**翻译不再有独立任务栏按钮**。
- 热键语义定为"呼出剪贴板历史",故 `showAtCursor()` 一律切回列表页;
  停在翻译页时按热键会回到列表(翻译内容仍在,再点「翻译」即可回去)。

---

## 2026-10-09(五)翻译页两处修正:关闭即收起 + 拖动标题栏不再挪动控件

### 1. 关闭翻译页 = 直接收起面板(不再顺带切回列表)

原先 ✕/Esc 是"退回列表页(窗口保持打开)",实际用起来像"关掉翻译又硬塞一个主界面"。
改为与关闭语义一致:**直接收起整个面板**。

| 位置 | 改动 |
| --- | --- |
| `TranslateView.qml` | 标题栏 ✕ → `panelWindow.hidePanel()`;删掉已无用的 `exitRequested` 信号 |
| `Main.qml` | Esc 分层由"退出翻译页"改为"收起面板"(先关翻译页自己的浮层,如历史抽屉) |
| `panel_window.hpp` | 删掉随之变成死代码的 `exitTranslateMode()` |

**回列表的路径仍然通**:按面板热键(Ctrl+Shift+V)会走 `showAtCursor()` → `setMode(List)`,
一步回到列表且窗口保持打开。

### 2. 拖动标题栏时控件被挪走(实测截图可见标题被右移、✕ 被挤出)

**根因**(Qt 源码 + 文档确认,非推测):

> `PointerHandler::target` — The Item which this handler will manipulate.
> **By default, it is the same as the `parent`**, the Item within which the handler is
> declared. … or to `null`, to disable the default behavior and do something else instead.

`DragHandler` 未设 `target` 时,它操作的是**声明它的父项**。翻译页标题栏的
`DragHandler` 声明在标题栏 `Rectangle` 内,于是拖动窗口时它在改那个 Rectangle 的
`x/y` —— 该 Rectangle 归 `ColumnLayout` 管,布局要等下次重排才把它拉回来,
拖动过程中整条标题栏(连同标题与 ✕)就被挪走了。

**修复**:两处 `DragHandler` 都补上 `target: null`(只保留 `active` 信号,
真正的移动交给 `panelWindow.startSystemMove()`):

- `TranslateView.qml` 标题栏(用户报的这一处);
- `Main.qml` 底部 `DragGrip`(同一个坑,只是手柄小、之前没被察觉)。

**验证**:app 编译链接通过、无编译警告;`qmllint` 无新增告警类别。
真机确认:① 拖翻译页标题栏 → 窗口移动、**标题与 ✕ 原地不动**;
② 拖面板底部手柄 → 同样只动窗口;③ 翻译页 ✕/Esc → **面板直接收起**,不再弹出列表。

---

## 2026-10-09(五)主界面头部也可拖动窗口(限定在"品牌+标题"展示区)

**背景**:主界面此前**故意**不在头部挂拖动 —— 早先把整个头部做成拖动区时,
在搜索框里按住拖选文字、指针一旦移出搜索框 `DragHandler` 就被激活,窗口跟着跑。
窗口移动只能靠底部状态栏左侧的 `DragGrip`。

**做法**:拖动只挂在头部里**唯一不参与交互**的那块 —— "品牌标记 + 标题" RowLayout
(`id: headerDragArea`)。头部其余部分是搜索框(占满剩余宽度)与四个动作按钮,
都不碰。搜索框里拖选文字的手势因此完全到不了这个 handler(命中区就是那块本身)。

```qml
RowLayout {
    id: headerDragArea
    Layout.fillHeight: true          // 命中区撑满头部高度,好按
    HoverHandler { cursorShape: Qt.SizeAllCursor }
    DragHandler {
        target: null                 // ⚠️ 不置 null 会去拖父项(见上一条记录)
        onActiveChanged: if (active) panelWindow.startSystemMove()
    }
}
```

底部 `DragGrip` **保留**,两个把手并存(不删是为了不动既有的使用习惯)。

**验证**:app 编译链接通过、无编译警告;`qmllint` 无新增告警类别。
真机确认:① 拖头部"Nextboard"字样区域 → 窗口移动、控件不动;
② 在搜索框里按住拖选文字(含指针移出搜索框)→ **窗口不跟着跑**(这是当初失败的场景);
③ 拖底部手柄 → 照旧可用。

**补记**:头部把手可用后,底部状态栏的 `DragGrip`(六个圆点 + "拖动移动"文字 + 前置分隔线)
与 `component DragGrip` 定义**已一并移除** —— 窗口移动现在只有头部「品牌+标题」这一个入口。
(上一条里"两个把手并存"的说法作废。)

---

## 2026-10-09(五)主面板改为**跟随光标**弹出(原先只居中于光标所在屏)

**问题**:`showAtCursor()` 名为"AtCursor",实际做的是"居中于**光标所在屏**" ——
`move(avail.center() - rect().center())`。单屏下就是"永远弹在屏幕正中",
每次都得把鼠标从工作区挪过去。

**改动**:新增 `PanelWindow::placeAtCursor()` —— 以**光标为窗口中心**摆放,
再夹回该屏可用区;`showAtCursor()` 与 `showTranslate()`(原本隐藏时)都走它。

```cpp
QPoint topLeft = cursor - rect().center();
// 靠近屏幕边缘时夹回可用区(此时光标自然偏离中心,是预期行为)
topLeft.setX(qBound(avail.left(), topLeft.x(), qMax(avail.left(), avail.right() - width() + 1)));
topLeft.setY(qBound(avail.top(),  topLeft.y(),  qMax(avail.top(),  avail.bottom() - height() + 1)));
```

摆位放在 `setMode()` **之后**:形态切换会改窗口尺寸(800×600 ⇄ 520×660),
必须按新尺寸算中心,否则翻译页会偏。

从列表点「翻译」(窗口已显示)时**不重新摆位** —— 只换内容与尺寸,位置保持不动。

**保留**:`m_userMoved` —— 用户手动拖动过之后不再自动摆放(那是明确的"我就放这儿")。
若希望"每次呼出都跟随光标、拖动只算临时",把这个判断去掉即可。

**验证**:app 编译链接通过、无编译警告。真机确认:
① 热键呼出 → 面板出现在**鼠标处**(鼠标大致落在面板中心),不再固定屏幕正中;
② 在屏幕四角附近呼出 → 面板被夹回屏内,不越界;
③ 拖动面板后再呼出 → 停在拖动后的位置(未被拉回光标处)。

**补记**:按用户决定,**取消"拖动后不再自动摆放"的原有设计**。
`m_userMoved` 字段、`startSystemMove()` 里的置位、`placeAtCursor()` 里的判断全部移除 ——
面板**每次呼出都按光标摆位**,拖动只是临时挪一下。
(定位语义定为"跟着人走",而不是"记住一个坑"。)

---

## 2026-10-09(五)修复:设置抽屉跨模式残留(浮在翻译页之上且关不掉)

**现象**:列表页打开设置抽屉 → 触发翻译页 → 抽屉**浮在翻译页上面**且关不掉。

**根因**(两条叠加):

1. **抽屉没被清掉**:切形态时没有任何人收掉列表页的浮层。
2. **`AppDrawer` 是 `Drawer` → 继承 `Popup`**:Popup 渲染在窗口的 overlay 层,
   **永远盖在所有普通 Item 之上**,与我插入 Loader 的先后顺序无关。
   而且此时焦点已交给翻译页(Loader 的 `onVisibleChanged` 会 `forceActiveFocus`),
   抽屉自己的 `closePolicy: CloseOnEscape` 也收不到 Esc。

**修复**:

| 位置 | 改动 |
| --- | --- |
| `Main.qml` | 新增 `dismissListOverlays()`:关设置抽屉 + 关右键菜单 + 清标签编辑;在 `openTranslate()` 开头调用 —— 它是进入翻译页的**唯一漏斗**(三条链路都经 `showTranslate` → `openTranslate`) |
| `TranslateView.qml` | 新增 `onVisibleChanged: if (!visible) dismissTopLayer()` —— 离开翻译页时收掉它自己的历史抽屉,否则下次进来会无征兆地又冒出来 |

**已知静态分析误报**:`qmllint` 会对 `translatePage.item.dismissTopLayer()` 报
`Member "dismissTopLayer" not found on type "QObject"` —— `Loader.item` 在静态类型里
只是 `QObject*`,解析不到动态函数。运行时由 QML 动态解析,行为正常(与 `Theme`
那批 `missing-property` 同属静态分析的能力边界)。

**验证**:app 编译链接通过、无编译警告。真机确认:
① 列表页开设置抽屉 → 触发翻译页 → **抽屉已收起**,翻译页正常;
② 翻译页打开历史抽屉 → 切回列表 → 再进翻译页 → 历史抽屉**不再自动弹出**;
③ 列表页开右键菜单 → 触发翻译页 → 菜单不残留。

---

## 2026-10-09(五)内存收口 P0:设置抽屉懒构建 + 列表委托回收复用

**动机**:上一轮把"尚未做的杠杆"排了序(P0 两项为低风险、不改交互),本轮落地。
两项都只动主面板自身的构建/回收策略,不碰任何交互语义。

### 1. `SettingsDrawer` 改 `Loader` 懒构建

`SettingsDrawer { id: settingsDrawer }` 原先是 `Main.qml` 的**直接子项**,启动即整棵
实例化(1200+ 行、6 组设置、多个自绘 `component`、Repeater),哪怕从不打开。改为
`Loader { active: false }`,首次点「设置」才构建,之后常驻(再次打开省去重建)。

| 位置 | 改动 |
| --- | --- |
| `Main.qml` | `SettingsDrawer` → `Loader{ id: settingsDrawerLoader; active:false }`;`openAndLoad()` 包装为"置 active 后转调 item.openAndLoad()" |
| `Main.qml` | 4 处引用改经 `settingsDrawerLoader.item` 并判空(Esc 分层 / 快捷键守卫 / 按钮 / `dismissListOverlays`) |
| `Main.qml` | `openSettings()` 自动化入口(见下) |

⚠️ **尺寸必须对齐面板根**:`AppDrawer` 用 `parent.width * 0.52` / `parent.height` 给自己
定尺寸,经 `Loader` 构建后它的 `parent` 就是 Loader —— 故 Loader 显式
`width: root.width; height: root.height`,否则抽屉会拿到 0×0 而不可见。

### 2. 列表 `reuseItems` + 限制 `cacheBuffer`

`ListView` 原先未设 `reuseItems`(默认 false)且 `cacheBuffer` 取"平台相关且通常 >0"的
默认值 —— 滚动时委托销毁重建、图片反复解码,是滚动期内存/CPU 反复抖动的来源。
现设 `reuseItems: true`、`cacheBuffer: 240`(约上下各一块卡片高度)。

⚠️ **复用会保留委托内部状态**,`EntryCard.qml` 补 `ListView.onReused` 复位两项:
`textExpanded = false`(否则上一条的"展开"会带进复用的下一条)、`fileCopyTimer.stop()`
(否则挂起的双击判定可能残留)。模型角色(entryId/kind/…)由 ListView 自动刷新,无需处理。

### 3. 验收设施

新增 `--settings`(与 `--screenshot` 联动):启动后先调 `PanelWindow::openSettingsPanel()`
→ QML `openSettings()`,等价点「设置」按钮,用于验证懒构建链路与抽屉尺寸。

### 验证与限制

- core `cargo build --release` 通过(本轮未改 Rust);app 编译链接通过、**无编译警告**;
  `qmllint` 无新增告警类别(仅多一条 `Loader.item` 的 `missing-property` 误报,与既有的
  `dismissTopLayer` 同属静态分析能力边界);
- 实测(真机,`--screenshot` 自截):
  | 时点 | 改前 | 改后 |
  | --- | --- | --- |
  | `mem[panel-built]`(工作集) | 119.5 MB | **108.4 MB** |
  | `mem[panel-built]`(私有) | 62.9 MB | **56.3 MB** |
  | `mem[startup]`(工作集) | 205.5 MB | **187.1 MB** |
  → **`SettingsDrawer` 的常驻成本(约 11 MB 工作集 / 6.6 MB 私有)从启动路径移除**;
- 自截确认:列表页渲染正常(卡片/图片/分类计数齐全);`--settings` 自截确认设置抽屉
  **正确打开**(占右侧 52%、全高,尺寸修正生效)且内容回填;
- ⚠️ 仍需真机确认的交互:
  1. 点「设置」→ 抽屉首次打开正常、字段回填;关闭再开 → 保留上次滚动位置;
  2. 列表**滚动**多屏(尤其含图片条目)→ 卡片内容不串味("展开"状态不跨行、无异常复制);
  3. Esc:抽屉打开时先关抽屉,再按才收起面板;
  4. 列表页开抽屉 → 触发翻译页 → 抽屉仍被收起(既有行为不回归)。

**下一步**:P1 —— 折叠卡片的展开体改二次 `Loader`;面板隐藏超时后卸载 QML 子树(带快路径)。

---

## 2026-10-09(五)内存收口调研:Qt Quick 渲染后端对内存的影响(`--software`)

**动机**:回答"把主面板从 QML/Quick 改写成 QWidget,内存能降多少"。改写量巨大
(14 个 QML 文件),故先用**零改写的等价实验**量出"GPU 渲染路径"这一块到底占多少 ——
它正是 QWidget + QPainter(CPU 光栅)不需要的部分。

### 做法:新增 `--software` 启动开关

```
nextboard.exe --software --screenshot <path>
```

- `main.cpp`:`QQuickWindow::setGraphicsApi(QSGRendererInterface::Software)`。
  ⚠️ 必须在**构造第一个 QQuickWindow 之前**调用(官方文档明确"before constructing the
  first QQuickWindow"),故放在 `qInstallMessageHandler` 之后、`PanelWindow` 构造之前
  —— 既满足时序,又能让日志落盘(放在日志处理器之前会丢这条 INFO)。
- 开关而非硬编码:同一份构建里 `--software` ⇄ 默认(硬件)可反复对比。

### 实测(真机,`--screenshot` 自截,同一构建)

| 时点 | 硬件(D3D11) | 软件(CPU 光栅) | 差 |
| --- | --- | --- | --- |
| `mem[panel-built]` 工作集 | 108.5 MB | 108.5 MB | **0** |
| `mem[panel-show]` 工作集 | 188.7 MB | 119.0 MB | **−69.7** |
| `mem[startup]` 工作集 | 190.1 MB | 120.3 MB | **−69.8** |
| `mem[startup]` 私有 | 129.6 MB | 64.7 MB | **−64.9** |

**结论**:

1. `panel-built` 两者**完全相等** → QML 对象树本身不吃这一块差;
2. 差额**全在"首次渲染"这一步** → 场景图 GPU 资源 + RHI(D3D11)设备/驱动 + 交换链
   + 着色器,合计约 **70MB 工作集 / 65MB 私有**;
3. 软件后端渲染画面**正确**(自截确认列表/图片/分类计数齐全);
4. 因此 QWidget 重写的"上限收益"主要是这 ~70MB(Qt 运行时 ~25MB 不变,
   QML 对象树换成 QWidget 可再省,但量级小得多);**而"软件后端 + 保留 QML"能拿到
   其中绝大部分,且不改 UI**。

### 与 QWidget 改写对比(判据)

- 改 QWidget 的收益 ≈ 上述 ~70MB + (QML 树 − QWidget 树,估计 20~40MB),粗略落在
  **60~100MB 工作集**;代价是整层 UI 重写 + 观感/手感回归风险(见下文待测)。
- 渐进迁移**不可取**:只要还剩一个 `QQuickWidget`,QtQuick/Qml 与 QQmlEngine 就无法
  卸载,中途会同时背负两套 UI 栈,内存可能不降反升。

### 待测(决定要不要改写的前提)

软件后端的**流畅度**是否可接受,重点两项:

1. **含大图的列表滚动**(缩略图较多时,CPU 光栅是否有掉帧/卡顿);
2. **设置抽屉滑入 / 卡片 hover / 新条目淡入**等动画是否顺滑。

两者过关 → 建议保留 QML 架构、默认软件后端,不启动 QWidget 重写。

---

## 2026-10-09(五)设置项:渲染后端可切换(重启生效)+ 托盘入口居中显示

### 1. 渲染后端设置项(重启生效)

把 `--software` 从"命令行临时开关"提升为**设置项**,供日常切换:

| 层 | 改动 |
| --- | --- |
| `core/settings.rs` | 新增 `software_render: bool`(默认 `false` = 硬件 D3D11);`Default` 补值;默认值用例补断言 |
| `app/main.cpp` | 启动时读 `get_settings().softwareRender`,为真则 `QQuickWindow::setGraphicsApi(Software)`;`--software` 仍作临时覆盖(优先级更高)。**必须在构造第一个 QQuickWindow 前设定**,故为"重启生效" |
| `SettingsDrawer.qml` | 「数据」区新增勾选框「软件渲染(省内存,重启生效)」+ 说明;`openAndLoad`/`save` 接入;新增 `renderDirty` 标记,保存后提示"渲染后端**重启后**生效" |

说明文案:用 CPU 光栅替代 GPU(D3D11),约省 70MB 内存;滚动/动画更吃 CPU。

### 2. 托盘入口改为"当前屏居中"(热键仍跟随光标)

先前把面板/翻译统一改成"跟随光标",但托盘操作时鼠标常停在任务栏角落,跟随光标会把
窗口顶到屏幕边缘。按入口区分摆放方式:

| 入口 | 摆放 |
| --- | --- |
| 全局热键(呼出面板 / 打开翻译) | **跟随光标**(`placeAtCursor`,不变) |
| 截图链路(面板按钮/条目右键/截图翻译回填) | 跟随光标(不变) |
| **托盘**:菜单「打开主面板」/「打开翻译」、图标左键/双击 | **当前屏居中**(`placeScreenCenter`) |

| 层 | 改动 |
| --- | --- |
| `panel_window.{hpp,cpp}` | 新增 `Orientation{AtCursor,ScreenCenter}` 与 `showCentered()` / `placeScreenCenter()`;`showTranslate(text, orientation=AtCursor)` |
| `main.cpp` | 连接 `translatePrefill`(光标) 与新增的 `translatePrefillCentered`(居中) 两条信号 |
| `core_facade.hpp` | 新增 `openTranslateCentered()` + 信号 `translatePrefillCentered`(供托盘用) |
| `tray.cpp` | 三处入口改用居中入口;`setStyleSheet` 加 `QMenu#nbTrayMenu` 限定 |

⚠️ 顺带修一处与"重启"相关的隐患:托盘菜单的浅色 QSS 原先无 selector,而样式表作用于
**目标窗口及其子窗口**——深色主题下若未打开过设置抽屉(无人重置 QSS),这段浅色规则会
残留并套到后续弹出的子菜单上。现限定为 `#nbTrayMenu`。

### 验证与限制

- core `cargo test`:**178 项**通过;app 编译链接通过、无编译警告;`qmllint` 无新增告警类别;
- 自截确认设置抽屉出现新勾选框(位于「数据」区),主题/其余字段正常;
- ⚠️ 需真机确认:
  1. 勾选「软件渲染」→ 保存后提示"重启后生效" → **重启** → 日志出现
     `Qt Quick graphics API = Software (设置项)`,`mem[startup]` 约 120MB;
  2. 取消勾选 → 重启 → 回到硬件(D3D11,约 190MB);
  3. 托盘菜单「打开主面板」/「打开翻译」→ **居中于当前屏**;热键呼出仍**跟随光标**。

**注**:本机沙箱不允许写 `%LOCALAPPDATA%`(设置文件位置),故"勾选→保存→重启"的
运行期读写闭环未能由我实测,代码路径已核实(默认值/取值/分支)。

---

## 2026-10-09(五)OCR 自动识别下线 + 缩略图清晰度 + OCR 峰值收口

按用户拍板的三项设置(缩略图 640 / 版本化文件名 / `MAX_OCR_EDGE=4096`)一次落地。

### 一、下线 OCR 自动识别(用户手动三条链全部保留)

删除「图片入库时后台跑 OCR 回填 `ocr_text`」这一条旁路及其设置项:

| 层 | 改动 |
| --- | --- |
| `clip.rs` | 删 `process()` 里的 `if settings.ocr_enabled { spawn_ocr_backfill(...) }` 与 `spawn_ocr_backfill()` 整个函数 |
| `settings.rs` | 删 `ocr_enabled` 字段、默认值与默认值断言 |
| `SettingsDrawer.qml` | 删「OCR 自动识别(截图文字可搜索)」勾选框及其读/写两处 |

**未动**:右键「识别文字」/ 截图取字热键 / 翻译页「截图取字」三条手动链;`ocr_text` 列、
FTS 触发器、`entry::set_ocr_text` 与 `ocr_language/backend/scale` 设置项全部保留(手动识别仍需要)。

> **核对发现(与上一轮分析的一处出入)**:`set_ocr_text` 的唯一生产调用点就是刚删掉的
> `spawn_ocr_backfill` —— 手动「识别文字」走 `ffi.rs` 的 `ocr_entry`,只 `return {"text": …}`,
> **从不写 `ocr_text`**。即"手动识别也回填 `ocr_text` 参与搜索"此前**并不成立**。
> 本次按批准范围只做删除(`pub fn` 不报 dead_code,schema 不动);若要"手动识别即回填、可搜索",
> 属**新增**行为,需另行确认。

兼容性:`Settings` 是 `#[serde(rename_all="camelCase", default)]` 且无 `deny_unknown_fields`,
旧配置里的 `ocrEnabled` 被静默忽略,**无需写迁移**。收益:去掉一条会误触发的旁路(默认本就关闭)。

### 二、OCR 峰值收口:自适应放大 + 去整图冗余拷贝

**根因**(经逐段内存账核对):整屏 3072×1920 在 `ocr_scale=2` 下被无条件放大到
6144×3840 —— 一份 RGBA 就是 **90MB**(原图仅 22.5MB),其后每一步都按 90MB 计。且
5120×2880(5K)×2 = 10240 > Windows OCR 的 `MaxImageDimension`(10000),会**直接失败**。

| # | 位置 | 改动 |
| --- | --- | --- |
| C.1 | `ocr.rs` 新增 `MAX_OCR_EDGE = 4096` + `effective_scale()` | 按"预处理后最长边 ≤ 4096"钳制实际倍数:**1080p 及以下行为不变**(仍 ×2),4K/5K 退到 ×1 |
| C.2 | `apply_preprocess` 改收 **owned** `Vec<u8>` | 不处理时直接沿用(原先 `scale<=1` 分支会白拷整张);需要处理时才 `from_raw` |
| C.3 | 所有权沿链串联 | `apply_preprocess` → `to_bitmap`(原地换 R/B,省 1 份整图)→ FFI `img.into_raw()` |
| C.4 | `write_thumbnail_from_rgba` 收 owned | 入库预生成缩略图不再 `to_vec()` 整图 |

**修正一处过度估计**:`SoftwareBitmap::CreateCopyFromBuffer` 函数名即 Copy,WinRT 无零拷贝
构造 API,**这份省不掉**;故 C.3 的可省是"1 份预处理图",不是"2×90MB"。

### 三、缩略图清晰度(320 → 640,版本化缓存)

**根因**:列表**没有**解码原图(`ImageUrlRole` 走 `.thumb.png`),模糊来自**缩略图被放大显示**:
卡片图片区约 533 逻辑 px、16:9 受高度上限 300 约束 → **320 的缩略图在 DPR 1.0 就被放大 1.67×、
1.5 屏 2.5×**。"RGBA 降采样"是反向操作,只会更糊。

| # | 位置 | 改动 |
| --- | --- | --- |
| B.1 | `clip.rs` | `THUMB_MAX_EDGE: 320 → 640`(DPR 1.0 下约 1.0×,不再放大) |
| B.2 | `encode_thumbnail` | `DynamicImage::thumbnail`(等权盒平均,文字边缘发糊)→ `resize(…, CatmullRom)`;新增 `thumbnail_dimensions()` 自算等比尺寸(`imageops::resize` **不保持宽高比**) |
| B.3 | `thumbnail_path` / `legacy_thumbnail_path` | 命名版本化:`xxx.png → xxx.thumb640.png`;旧无版本名 → 启动时(`spawn_thumbnail_backfill`)与删条目时(`remove_blob`)一并清理 |
| B.3 | `core_bridge.{hpp,cpp}` | 新增 `CoreBridge::thumbnailPathFor()` + `constexpr nbThumbVersion = 640`,命名规则集中一处 |
| B.3 | `clip_entry_model.cpp` / `bridge.cpp` | 两处 URL 拼接改调 `thumbnailPathFor`(与 core 侧对齐) |

**为什么版本化**:`spawn_thumbnail_backfill` 只在目标文件不存在时才建,光改常量则**老条目永远
停在 320**;版本号进文件名后,新名不存在 → 自动重建,自愈且无需手清缓存。

**代价**:纹理 0.23MB → 0.9MB/张(列表只解码可见 + `cacheBuffer:240`,估 **+3~6MB 工作集**);
磁盘约 **+150~300MB / 500 图**。嫌大可后续给 image crate 开 `jpeg` feature 改存 `.thumb.jpg`。

### 验证与限制

- core `cargo test`:**184 项**通过(178 → 新增 4 条 OCR 自适应/去拷贝 + 2 条缩略图等比与版本化);
  `cargo build --release` 通过;
- app 编译链接通过(`[13/13] Linking nextboard.exe`)、**无编译警告**;`qmllint` 无新增告警类别;
- ⚠️ **本机沙箱对测试二进制有额外限制**:`cargo test` 直接运行 `core/target/debug/deps/*.exe`
  报 `STATUS_DLL_INIT_FAILED (0xC0000142)` —— 复制到 `%TEMP%` 后**运行正常**(同样受影响的还有
  旧构建产物,故与本次改动无关)。另 `Set-Location`/`Push-Location` 被拒,`build.ps1` 整体不可用;
  app 构建改由 `cmd /c "vcvars64 && cd /d <build> && ninja"` 手工完成(脚本逻辑未改)。
- ⚠️ 需真机确认:
  1. 全屏(3072×1920)截图前后 `capture-end - capture-start` 应从 ~150MB 降到 **~50~70MB**
     (日志出现 OCR 预处理尺寸 = 原尺寸而非 ×2);或把 `ocrScale` 设 1 复测对照;
  2. DPR 1.25/1.5 屏上图片卡片明显更清晰;老条目缩略图被**重建**(`nextboard.log` 出现
     `缩略图缓存升级:清理旧版缩略图 N 张` 与 `缩略图后台回填完成:新建 N 张`);
  3. 右键「识别文字」/截图取字/翻译页取字三条链仍可用;**设置页不再有**「OCR 自动识别」项。

**不做**:❌ 再降缩略图分辨率(RGBA 降采样);❌ 列表改回加载原图(4K 一张 33MB);
❌ 把 `CreateCopyFromBuffer` 当可省。**后续项**:截图入库改走 RGBA 通道(省 base64/JSON 往返,
现为 `QImage→PNG→base64→QString(UTF-16)→JSON`,C++ 侧瞬时约 5×PNG)。

---

## 2026-10-09(五)界面字体总体收口:统一微软雅黑 + 技术字段等宽

### 一、根因(先定位,再改)

诉求是"主界面/设置/翻译界面字体总体改成微软雅黑"。排查后发现**不是字号问题,
而是同一个界面里并存两种中文字体**:

| 来源 | 位置 | 实际效果 |
| --- | --- | --- |
| C++ 应用字体(带回退链) | `qml_bootstrap.cpp` `setFamilies({Segoe UI, Microsoft YaHei UI, …})` | 拉丁 Segoe UI,中文 **微软雅黑** ✅ |
| QML 显式族名(单值) | `Theme.qml` `fontFamily = "Segoe UI"` | 拉丁 Segoe UI,中文 **宋体** ❌ |

机制:QML 的 `font.family` 底层是 `QFont::setFamily`,而 Qt 源码里它是
`setFamilies(QStringList(family))` —— **单元素**,会把应用字体的回退链整条覆盖掉。
中文缺字后由 Qt 的 font matching 决定落点,实测是 **SimSun**。

用 `QTextLayout` glyph run + `QRawFont::familyName()` 取证:

```
改前  setFamily("Segoe UI")                 中文 -> SimSun              ❌
改后  setFamily("Segoe UI")  + 替换表        中文 -> Microsoft YaHei UI ✅
```

于是凡显式写了 `font.family: Theme.fontFamily` 的 **35 处**(Main.qml 12、
TranslateView.qml 18、ThemedButton 1、ThemedMenuItem 1、SettingsDrawer 的
SectionTitle/FieldLabel/HintLabel 3)中文全是宋体;而**没写** `font.family` 的
(EntryCard 9 个文本项、KeyCap、SettingsDrawer 大部分内联 Label)继承应用字体,
中文是雅黑。最直观的一例:搜索框(设了 family)中文是宋体,而它下方的列表卡片
(未设 family)中文是雅黑 —— 两者相邻。

### 二、改动

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | **新增 `app/src/app_font.{hpp,cpp}`** | 字体族名的**唯一来源**:`uiFontFamilies()` / `monoFontFamilies()` / `installFontSubstitutions()` / `installApplicationFont()` / `uiFont()` / `monoFont()` |
| 2 | `CMakeLists.txt` | 注册 `src/app_font.cpp` |
| 3 | `qml_bootstrap.cpp` | 删掉本地字体块,改调 `installApplicationFont()`;并提到 `QQuickStyle::setStyle()` **之前**(替换表必须在任何 QFont 解析前生效) |
| 4 | `Theme.qml` | `fontFamily: "Segoe UI"` → `"Microsoft YaHei UI"`;新增 `fontMono: "Cascadia Mono"`;修正原注释里"中文自动回退到 YaHei UI"的错误说法 |
| 5 | `SettingsDrawer.qml` | `ReadableTextField` 组件加 `font.family: Theme.fontMono`(**一处覆盖 17 个技术字段**:接口地址/API Key/Token/模型名/数据目录/hex 色值/代理主机账号…);`cuHeaders`/`cuBody` 两个 TextArea、`HotkeyField` 内文本同样等宽 |
| 6 | `TranslateView.qml` | `targetCode` 语种代码框等宽 |
| 7 | `EntryCard.qml` | `collapsedText` / `fullEdit` 按 `kind === "file"` 切等宽(文件条目正文就是路径) |
| 8 | `pin_window.cpp` | `renderText` 的硬编码 `QFont("Microsoft YaHei")` 改 `nb::uiFont(style.fontSize)` —— 原来位图用经典雅黑、工具条按钮继承应用字体(雅黑 UI),两处不同族 |
| 9 | `tray.cpp` | 删掉 `if (pointSizeF() <= 0.0) setPointSizeF(9.0)`(见下方"托盘菜单字号"一节) |
| 10 | `main.cpp` | 新增 `--translate <text>` 自截钩子(与既有 `--query`/`--settings` 同约定),否则翻译页永远无法纳入自截回归 |
| 11 | `docs/theme.md` §39 | 重写为"默认微软雅黑 UI + 等宽令牌",并新增 §39.1 记录三条 Qt 约束 |
| 12 | `app/panel/main.htm` | Sciter 时代死代码(CMake 已不部署、无代码引用、依赖 `<input\|search>` 与 `NB.listEntries`):加显式废弃标记,字体同步为新策略 |

**顺带修掉一个从未生效的回退名**:原 `setFamilies` 链末位是 `Noto Sans CJK SC`,
而 Qt 字体库里实际只有 `Noto Sans SC` —— 那条回退从来没起作用过(静默失效)。

### 三、两个 Qt 限制(决定了改法)

1. **QML 无法表达回退链。** `font.family` 单值;`"A, B"` 逗号串会被当成一个完整
   族名去匹配,匹配不到由 Qt 随便挑(实测落 **Tahoma**);`Qt.font({families:[…]})`
   也不行 —— QML 的 font 值类型没有 `families` 属性,该键被静默忽略
   (只给 `families` 会直接报 `no valid font subproperties specified`)。
   → 回退链只在 C++ 侧表达一次,QML 只放主族名。
2. **等宽字体不含中文字形。** Cascadia Mono / Consolas 的 SC 支持均为 N。
   技术字段里的中文(中文数据目录、JSON 里的中文、`targetCode` 的占位"语种代码")
   若不处理会掉宋体。→ 用 `QFont::insertSubstitution` 把
   `Segoe UI` / `Cascadia Mono` / `Consolas` 的缺字回退钉到雅黑。

### 四、验证

**A. 构建**:cmake + ninja 全通过(43 步,QML 缓存生成 + C++ 编译链接),无警告。

**B. 三界面自截**(`--screenshot` + `--settings` + `--translate`):主界面/设置/翻译
三页中文均为雅黑,无宋体混入;翻译页 `targetCode` 显示等宽 `en`,翻译链路正常
(自截时 `--translate "剪贴板字体收口验证 Nextboard 123"` 成功返回英文译文)。

**C. 令牌落点探针**(链接**生产代码** `app_font.cpp`,调 `installApplicationFont()`
后按 QML 的 `font.family` 写法构造 QFont,报告 glyph run 实际命中字体):

```
Theme.fontFamily = "Microsoft YaHei UI"
  中文占位 / 中文目录 / JSON / 中英混排 / 纯英文  -> Microsoft YaHei UI   ✅
Theme.fontMono = "Cascadia Mono"
  中文占位"语种代码"     -> Microsoft YaHei UI            ✅(回退雅黑,非宋体)
  中文数据目录           -> Microsoft YaHei UI + Cascadia Mono  ✅
  纯英文 "Nextboard"    -> Cascadia Mono                  ✅(拉丁仍是等宽)
对照:替换表外的 Lucida Console + 中文 -> SimSun            ❌(证明替换表必需)
符号 ✓✕↻✎⧉             -> Segoe UI Symbol + MS UI Gothic + YaHei UI  ✅(仍有字形)
```

**D. 布局回归**:雅黑比 Segoe UI 拉丁宽约 8~9%(实测 `"Nextboard"@18px`
81.2→88.1、URL@14px 155.1→169.0)、行高紧 1px(14px 下 19→18)。三张自截里
头部按钮行、搜索框、设置抽屉均无溢出/省略号异常。

### 四.1 托盘菜单收窄(两轮)

**背景**:上面第 9 条改动把托盘菜单字体改成 `nb::uiFont()`(=界面正文 14px),
而它此前实际是 9pt(≈12px)—— 菜单变大,用户两次反馈"太大 / 还是太宽"。

**归因**:菜单尺寸 ≈ 边框 + 菜单内边距 + **勾选列** + 左内边距 + 最宽文案 + 右内边距。
逐项量测后,发现两个"白占"的来源:

1. **勾选列 18px(最大单项)**。`QMenu` 只要存在**任何一个**可勾选项,就会给
   **所有**条目预留一列勾选列宽。而"暂停监听"的状态本来就用文案表达
   (`暂停监听` ⇄ `恢复监听`),勾选标记是**冗余**的 → 去掉 `setCheckable`
   (同时 `toggled` 改 `triggered`,状态改为从设置现读)。
2. **右内边距 24px** 是给**子菜单箭头**留的,而本菜单没有子菜单 → 纯空占。

**逐项边际收益**(sizeHint 逻辑像素,12px 字号):

```
右内边距 20→12   -8      勾选列 18px 去掉   -18
右内边距 20→6    -14     左内边距 10→8      -2
菜单内边距 3→2    -2     文案换半角括号     -16(未采用,见下)
```

**最终改动**:

| 项 | 值 | 说明 |
| --- | --- | --- |
| 字号 | `kTrayMenuFontPixelSize = 12`(`app_font.hpp`) | **显式**写死;此前字号由 `if (pointSizeF() <= 0) setPointSizeF(9.0)` 这条**必然触发**的兜底意外决定 |
| 勾选态 | **去掉**(`setCheckable` → 无) | 状态已由文案表达;这一刀省 18px |
| 内存项 | **移除**(用户要求) | 原为验收期便利,不是常规功能;顺带省一行 |
| 条目内边距 | `6px 24px 6px 12px` → `4px 8px` | 右 24→8(无子菜单)、左 12→8(无勾选列);改**等宽**(不对称原本只为让出勾选列) |
| 菜单内边距 | `4px` → `2px` | |
| `::indicator` 规则 | 删除 | 已无可勾选项,规则无用。⚠️ **若日后加回可勾选项必须补回**,否则勾选标记走默认样式,深色下可能看不清 |
| 分隔线 | `margin: 4px 8px` 不动 | 不是宽度主因,动它会让量测数字失真 |

**未采用**:把两条最长文案的**全角括号改半角**(`贴图（最近内容）`→`贴图(最近内容)`)
还能再省 16px(→ 102px),但中文排版惯例用全角括号,属**文案/排版取舍**,留给用户定。

**尺寸对照**(sizeHint 逻辑像素):

```
最初(14px 6/24/12+4 有勾选 有内存)   176x316
上一版返工(12px 4/20/10+3 有勾选)     152x251
本次(12px 4px 8px+2 无勾选 无内存)    118x217    ← 宽 -34 / 高 -34(较上一版)
```

**⚠️ 一处度量纠错**:上一轮我报的"F 方案 133x219"是**错的** —— 我把 `grab()`
的物理像素按 `DPI=2` 换算,而实测 `devicePixelRatio = 1.75`,且 `grab()` 宽度
与 `sizeHint()` 还对不齐。本轮**统一改用 `sizeHint()`(逻辑像素,与 DPR 无关)**,
并打印 DPR 核对,上面的数字以此为准。教训:跨 DPI 换算必须**读** DPR,不能假定。

**验证方式**:链接生产代码 `app_font.cpp`,QSS 与 `tray.cpp` 逐字相同,离屏构造
菜单取 `sizeHint()`;并逐项验证"发布的 padding/字号/勾选态"与"量测程序用的一致"。
上一轮还用过"勾选/未勾选两图逐像素做差定位勾选标记"的像素分析法(本轮勾选列已
删除,该风险随之消失)。

### 四.2 翻译页输入/输出框的右键菜单(风格统一 + 调小)

**问题**:翻译页的原文/译文用的是 `TextArea`,其右键菜单由 **Basic 风格**
自动挂载(Basic 的 `TextArea.qml:27` `ContextMenu.menu: TextEditingContextMenu
{ control: control }`)。三处不合预期:

1. **风格不统一** —— 那是 Basic 原生 `Menu`/`MenuItem`,不走本项目的
   `ThemedMenu`/`ThemedMenuItem` 令牌,与条目右键菜单不是一套;
2. **偏大** —— 用应用字体(14px)与 Basic 默认行高;
3. **标签是英文** —— 它用 `QtQuick.Controls.impl` 的 `CutAction`/`CopyAction`/
   `PasteAction`/`DeleteAction`/`SelectAllAction`(`qsTr("Cut")` 等),而本应用
   `windeployqt` 带 `--no-translations`,**翻译不会被加载** → 中文界面里冒出
   英文菜单项(实测 `Cut Copy Paste Delete — Select All`)。

**做法**:用 `ContextMenu` 附加属性(`QtQuick.Controls.Basic/ContextMenu 6.9`)
整体替换默认菜单,新建 `app/panel/qml/TextEditMenu.qml`:

| 项 | 值 |
| --- | --- |
| 基类 | `ThemedMenu`(项目主题化菜单,与条目右键菜单同源) |
| 条目 | `ThemedMenuItem { compact: true }` + `ThemedMenuSeparator` |
| 宽度 | `minWidth: 100`(托盘菜单 118 同量级) |
| 条目 | 撤销 / 重做 / ─ / 剪切 / 复制 / 粘贴 / 删除 / ─ / 全选 |
| 只读框 | 复制 / ─ / 全选(不摆一排置灰项) |
| enabled | 撤销 `canUndo`、重做 `canRedo`、粘贴 `canPaste`(TextEdit 监听剪贴板维护)、剪切/复制/删除 看有无选中 |
| 重建时机 | `onAboutToShow: rebuild()`(与 Main.qml 条目菜单同套路) |

配套给共享组件加了两个**向后兼容**的开关(默认值不变,不影响条目右键菜单):

| 文件 | 新增 | 作用 |
| --- | --- | --- |
| `ThemedMenu.qml` | `property int minWidth: 200` | 背景 `implicitWidth` 由写死 200 改为 `control.minWidth` —— 那是菜单宽度的**下限**,不开口子则紧凑菜单被强行撑到 200px |
| `ThemedMenuItem.qml` | `property bool compact: false` | 高度 30→24、字号 13→12、内边距 10/18→12/12、放开 180px 宽度下限 |
| `ThemedMenuSeparator.qml` | **删掉** `implicitWidth: 160` | `QQuickMenuPrivate::resizeItem()` 会把条目拉伸到菜单内容宽(`qquickmenu.cpp:912`),分隔线自然铺满;写死 160 是无意的宽度地板 |

**尺寸前后对照**(探针实测,Window 内真实 open,`sizeHint` 取隐式尺寸):

```
Basic 默认菜单: 200x283  6 条:Cut Copy Paste Delete -- Select All   首条目 200x54
新建 TextEditMenu: 100x194  9 条:撤销 重做 -- 剪切 复制 粘贴 删除 -- 全选  首条目 48x24 px=12
```

即宽度 **-50%**、条目高 **54→24**、字号 **14→12**,标签由英文改为中文,
配色改走 Theme 令牌。

### ⚠️ 两个踩坑(都有明确报错特征,便于日后识别)

1. **函数名不能叫 `addItem`**。`Menu` 自身有 C++ 方法 `addItem(item)`,
   QML 里再定义同名函数会互相遮蔽 —— 函数体内 `addItem(...)` 变成递归调用
   自己,表现为 `createObject` 拿到的对象不可用并报
   `Could not set initial property text` + `Function.prototype.connect: target
   is not a function`,菜单最终 **count=0(全空)**。改名 `addAction` 即好
   (Main.qml 的条目菜单本来就叫 `addAction`,是同一条经验)。
2. **`ContextMenu.menu` 赋的 Menu 不能带 id**。Qt 文档明说会破坏
   该附加属性"首次请求时才创建菜单"的懒加载优化。用**独立组件文件**挂
   (`ContextMenu.menu: TextEditMenu { target: input }`)则组件内部的 id 无妨 ——
   与 Basic 自带的 `TextEditingContextMenu` 同构。

另(探针经验,非产品代码):QML 的 `addImplicitImport` 让**同目录兄弟组件**
可直接解析;`.impl` 子目录里的类型(如 `Basic.impl.TextEditingContextMenu`)
在探针里是显式 `import QtQuick.Controls.Basic.impl as BasicImpl` 后才用上的,
是否必须显式 import 未单独验证。

**验证**:
- 构建通过(moc/rcc/qmlcachegen + C++ 编译链接),`qmllint` 除既有的
  `Theme.*` 解析噪声外无新增(仅剩 `createObject` 返回 `QObject` 的类型推断
  误报,与 Main.qml 同类);
- 探针(Theme 存根 + 组件逐字副本)实际 open 菜单,确认:可编辑框 **9 条**、
  只读框 **3 条**、enabled 随选中态正确变化、`compact` 生效(24px / 12px /
  Microsoft YaHei UI)、宽度 100 不再被 200 撑住;
- **回归**:`minWidth` 默认 200 下条目右键菜单仍是 200 宽、条目 `width=192`
  (未被裁切)—— 确认收紧分隔线与背景宽度**没有**波及既有菜单;
- 真机运行 `--screenshot --translate` 增译一页,`nextboard.log` 中
  `TextEditMenu` / `Could not set initial property` / `target is not a function`
  出现次数均为 **0**。

### 五、限制与后续

- ⚠️ 设置页**下方**的技术字段(请求头/请求体 TextArea、自定义 HTTP 段)未进自截
  画面:抽屉内容可滚动,而自截链路没有滚动钩子。其字体落点已由探针 C 覆盖
  (`font.family = "Cascadia Mono"` 的 QFont 构造路径与 QML 完全一致),
  但**肉眼确认仍需人工滚到底部看一眼**。
- ⚠️ 贴图位图(`renderText`)未真机截图验证:需先复制文本再按热键,无自截钩子。
  代码路径已核实(统一走 `nb::uiFont()`)。
- ⚠️ 托盘菜单尺寸是**离屏量测**的,不是从真实托盘弹出的截图。字体、QSS、条目集合
  三者都取自生产代码,但 Windows 原生菜单可能有额外系统度量参与;
  **建议真机右键托盘图标确认一次观感** —— 尤其是右内边距收到 8px 后是否偏紧。
- ⚠️ **移除内存项是可观测性损失**:此前"验收内存不必另开任务管理器",现在要看得靠
  日志 `mem[...]`(`nextboard.log`)。`memorySummary()` 仍在 `memory_probe` 里,
  需要时可恢复。
- **后续项**:① 若希望设置页技术字段也可由用户自选字体,可在 `Theme` 上加
  `fontMono` 的设置化(需动 core `Settings`);② `docs/theme.md` §39 的字号表
  (20/16/14/13/12)与 `Theme.metrics`(18/15/14/13/12/11)仍未对齐,改动前先定以哪份为准。

---

## 2026-10-09(五)数据目录改为 exe 同级 data/(便携式布局)

### 诉求与结论

应用数据、日志、设置配置文件**统一落在 `<nextboard.exe 同级>\data\`**(原先数据在
`%LOCALAPPDATA%\Nextboard\Nextboard`,日志在 exe 同级根目录 —— 两处分离)。

| 文件 | 落点 | 原来 |
| --- | --- | --- |
| `clipboard.db`(+wal/shm)、`blobs\` | `data\` | `%LOCALAPPDATA%\Nextboard\Nextboard` |
| `app_config.json` | `data\` | 同上(core 的 `data_dir` 决定,自动跟随) |
| `nbcore.log` | `data\` | 同上(core `init_logging`) |
| `nextboard.log`(含崩溃兜底) | `data\` | **exe 同级根目录** |

### 实现

| 位置 | 改动 |
| --- | --- |
| **新增 `app/src/host/app_paths.{hpp,cpp}`** | 数据目录的**唯一来源**:`appDataDir()` / `ensureAppDataDir()` / `appDataFile()`;三处调用方(C++ 宿主、日志、界面提示)不再各写一份 |
| `core_bridge.cpp` | `QStandardPaths::AppLocalDataLocation` → `appDataDir()`;目录不可写时**立即返回可读错误**(`数据目录不可写:<路径>(安装目录需可写)`),不再让问题以"核心初始化失败"含混暴露 |
| `main.cpp` | 日志与崩溃日志路径 → `data\nextboard.log`;装日志处理器**之前**先建目录(否则首条日志无处可写) |
| `qml_bootstrap.cpp` | 界面加载失败提示里的硬编码 `app/build/nextboard.log` 改为**动态真实路径** |
| `app/CMakeLists.txt` | 注册 `src/host/app_paths.cpp` |

设置里的自定义数据目录(绝对路径、重启生效、复制式迁移)不受影响。

### 连带改动:安装器必须保护安装目录内的数据

数据进了安装目录,`tools/installer.nsi` 原先卸载时的 `RMDir /r "$INSTDIR"` 会**连剪贴板
历史一起删掉**。改为:

- 卸载时**逐项遍历顶层条目并跳过 `data`**(Windows 无"排除删除";遍历写法对以后新增的
  目录/文件自动覆盖),`Delete` + `RMDir /r` 对同一项都调用以同时覆盖文件/目录/空目录;
- 「同时删除个人数据」可选 Section 改为删 `$INSTDIR\data`,并顺带清理旧位置
  `%LOCALAPPDATA%\Nextboard\Nextboard`(0.2.0 早期版本遗留);
- 静默卸载跳过组件页且该节默认不选 → **数据必保留**。

### 验证与限制

- **逐个翻译单元编译验证**(用 `app/build/build.ninja` 里 nextboard 目标的**原样参数**:
  `/W3 /utf-8 -std:c++20` 等):`app_paths.cpp` / `core_bridge.cpp` / `qml_bootstrap.cpp` /
  `main.cpp` 四个文件 **exit 0 且无警告**;
- `tools/installer.nsi` 用 makensis 编译通过(LZMA,58.3MB → 16.3MB);
- ⚠️ **本会话无法完成整包构建**:`app\build` 是 Medium + `(NW)` 完整性标签,而本会话进程为
  **Low**(`S-1-16-4096`),写入/删除被"禁止向上写"拒绝 → `cmake --regenerate-during-build`
  报 Permission denied、`package.ps1` 清理旧 stage 失败。**真机(普通 Medium 权限)不受影响**;
- ⚠️ 需真机确认:启动后 `data\` 是否出现四个文件/目录(clipboard.db、app_config.json、
  nextboard.log、nbcore.log);卸载后 `data\` 是否仍在;勾选删除后是否清空。

### 踩坑(两条通用)

1. **编辑工具会剥掉 UTF-8 BOM** —— 本轮改了 `tools/installer.nsi` 与 `build.ps1` 后
   BOM 丢失:NSIS 报 `Bad text encoding: installer.nsi:1`(按 ACP 解析)、PowerShell 5.1 会
   因中文注释崩解析。**改完这两个文件必须补 BOM**,否则现象与"脚本写错"难以区分。
2. **沙箱拦截 `%TEMP%` 写入会拖垮构建链**:makensis 报 `error creating mmap`、CMake 重新
   配置卡死 —— 两者都靠把 `TMP/TEMP` 重定向到仓库内目录绕过(`package.ps1` 已固化)。

---

## 2026-10-10(六)修复:开机自启勾选后无法取消

### 现象与根因

设置页勾上「开机自启」后再点取消:**勾选态立刻弹回**,注册表 Run 值仍在,应用照旧开机自启。

链路:`CheckBox`(失败时 `checked = !checked` 还原)→ `CoreFacade::setAutoStart` →
`auto_start::setEnabled(false)`,`auto_start.cpp` 停用分支:

```cpp
const REGSAM access = enable ? KEY_SET_VALUE : DELETE;   // ← 错在这里
```

**`RegDeleteValue` 要求键句柄带 `KEY_SET_VALUE`**;`DELETE` 是 `RegDeleteKey` 删*整个子键*
用的权限。于是:以 DELETE 打开 Run 键**成功**,随后的删除被拒
(`ERROR_ACCESS_DENIED=5`)→ 函数返回 false → UI 把勾选弹回。

### 修复

| 位置 | 改动 |
| --- | --- |
| `host/auto_start.cpp` | 启用/停用**统一以 `KEY_SET_VALUE` 打开**;注释写明该 API 契约(避免再犯);操作后**回读复核**,与实际不符也返回 false 并告警 —— 安全软件/组策略拦截注册表写入时可被发现 |
| `SettingsDrawer.qml` | ① `openAndLoad()` 里按**注册表实况**同步勾选态:勾选态一旦被点击赋值,声明式绑定即失效,不显式回读就会一直停在旧值;② 失败时 `showToast` 说明原因(不再是"只有勾选态弹回、无任何解释") |

### 验证(实证,非推断)

**行为测试程序**(临时工具,直接链接生产代码 `auto_start.cpp` 打真实注册表;跑前备份、跑后恢复用户原值):

```
initial enabled()          = 1
setEnabled(true)  -> 1, enabled() = 1
LEGACY open(DELETE) rc=0, RegDeleteValueW rc=5, enabled() = 1   ← 旧 bug 精确复现
setEnabled(false) -> 1, enabled() = 0                           ← 修复生效
setEnabled(false) again -> 1 (幂等)
RESULT: PASS
```

第一行是**旧实现的复现证据**:`open(DELETE)` **成功(rc=0)** → `RegDeleteValueW` 返回
**5=ERROR_ACCESS_DENIED** → 值仍在。与线上现象一一对应。

> ⚠️ 测试程序放在 `dist\` 下运行会被以**受限令牌**启动(dist 里的文件带 Low 完整性标签),
> 注册表写入被拒、结果失真;复制到 Medium 目录(app\build)后行为正常。**环境现象,非代码缺陷。**

其余验证:

- 整包构建通过(`build.ps1`:`[10/10] Linking`、完整性标签 Medium);
- 应用真机运行(`--settings --screenshot`):`nbcore.log` 实测
  `nb_init: data_dir=...\app\build\data`;`app\build\data\` 齐备(clipboard.db+wal/shm、
  blobs、app_config.json、nextboard.log、nbcore.log)—— **数据目录改动同时得到端到端确认**;
- 设置抽屉打开**无 QML 加载错误、无 autoStart 相关报错**;`qmllint` **0 Error**;
- 重新打包:stage 143 文件 / 55.2MB、portable zip 22.1MB、Setup 15.9MB。

### 未决与观察

- ⚠️ **安装器运行时行为在本环境仍无法验证**:`Nextboard-Setup.exe /S /D=...` 无任何产出
  (无文件、无 ARP 项、无 `$PLUGINSDIR`、无进程),与会话早期"静默安装挂起无产出"一致
  → 安装/卸载/数据保留仍须真机按 [04 §6](./04-build-package-release.md) 冒烟清单验证;
- 环境遗留(未改动用户机器状态):HKCU Run 现有自启项指向
  `dist\Nextboard-0.2.0\nextboard.exe`(打包 stage 副本),修复后可在设置里正常关闭;
  `%LOCALAPPDATA%\Programs\Nextboard\probe_il.exe` 为早期排障残留。

### 追加:退出时 QML TypeError 噪声根治(原诊断已纠正)

**噪声**:每次退出日志固定刷 5 条

```
Main.qml:681 TypeError: Cannot read property 'mode' of null
Main.qml:663 / 665 / 348  ... 'pinned' of null
Main.qml:179 ... 'mode' of null
```

**先前的猜测是错的**(曾记为"列表 `release()` 与委托求值竞态"):报错行读的是
`panelWindow.mode` / `panelWindow.pinned`,所以为 null 的不是条目,而是
**`panelWindow` 这个 context property 本身**。

**真正机制**(QObject 析构顺序):

```
~PanelWindow → … → ~QObject:
   ① emit destroyed(this)      ← QML 侧 panelWindow 立刻变 null
   ② deleteChildren()          ← 此时才删 QQuickWidget 及其整棵 QML 树
```

夹在①②之间,QML 里所有 `panelWindow.*` **绑定**会因依赖失效而重新求值 → 读到 null →
TypeError。所以它与列表/委托无关,任何 `panelWindow.*` 绑定都会在同一瞬间报错。

**修复**(`panel_window.cpp` 析构体):先把 QML 树拆掉再走基类析构 ——

```cpp
PanelWindow::~PanelWindow() {
    delete m_quick;      // 此刻本对象仍是有效 QObject,绑定即便重算也读得到真值
    m_quick = nullptr;   // 树已销毁 → destroyed 发出时已无绑定可重算
}
```

**验证**(同一份 `--settings --screenshot` 自动化链路,前后对照):

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| `TypeError` 条数 | 5 | **0** |
| 全日志 `[WARN]` 条数 | 5 | **0** |
| 退出行为 | 正常 | 正常(截图产出、`mem[panel-hide]` 收尾) |

> 教训:错误信息里的 "of null" 要点明**哪个**基对象为 null。此处先入为主地当成条目
> 为 null,就把排查方向带偏了一轮;按报错行反查 `panelWindow` 才一步到位。

---

## 2026-10-10(六)安装包两处致命问题:完整性标签 + 卸载段命名

### 问题 A:双击安装包报 `Error writing temporary file. Make sure your temp folder is valid.`

**根因(非安装器缺陷,是产物标签)**:Windows 按**映像文件自身的完整性标签**决定新进程的
完整性级别。本工作区被沙箱打上 Low 标签,`dist` 下新建文件继承 Low ——
`dist\Nextboard-Setup-0.2.0.exe` 实测就是 `Low (I)(NW)`;于是安装器以 **Low 完整性**启动,
而 `%TEMP%` 是 Medium → "禁止向上写" → NSIS 建不出 `$PLUGINSDIR` → 报这句话。

证据链(同一现象三次复现):① 本会话早先两次静默安装"零产出、无 `$PLUGINSDIR`";
② 同目录下 Low 标签的测试程序注册表写入被拒,复制到 Medium 目录后正常;
③ 对同一个 setup 执行 `icacls /setintegritylevel Medium` 后,**立刻安装成功**。

**修复**:`package.ps1` 新增「产物完整性标签」步骤 —— `dist` 目录置 `(OI)(CI)Medium`
(后续产物继承)+ 对 zip/setup 显式置 Medium,并**复核**;仍为 Low 时打印可读处置
(复制到工作区外再运行 / 手工 `icacls`)。现在打包结尾会输出
`[OK] 产物完整性级别 = Medium(双击可直接运行)`。

> 便携版同理会中招:把 zip 解压到工作区内,解压出的 exe 继承 Low,双击一样跑不起来。

### 问题 B(更严重):卸载段被当成安装段 —— 安装时自我删除,卸载器 0 个 section

**现象**:静默安装 exit 0,但目标目录**什么都没装**,而且原有文件(`probe_il.exe`)被删了。

**根因(NSIS 命名即语义)**:NSIS 只把**精确名 `Uninstall`** 或 **`un.` 前缀**的 Section
编进卸载器,其余一律算"安装段"。我的卸载段名是 `"!卸载 Nextboard"` ——
既非精确名、也没有前缀,于是:

1. 它在**安装时**执行:把刚铺好的文件、ARP 项、Run 键、快捷方式全删掉(所以装完什么都不剩);
2. `WriteUninstaller` 生成的卸载器 **0 个 section**,运行它什么也不做。

而这个错误**在编译汇总行里早有痕迹**:`Uninstall: 3 pages, 0 sections (0 bytes)` ——
当时没当回事,是排查绕远的原因。

**修复**:`Section "Uninstall"` + `SectionIn RO`(卸载器主段:必选、组件页置灰、静默也必跑);
数据删除段改名 `"un.同时删除个人数据(历史记录/截图/设置)"`。修复后汇总行:

```
Install:   4 sections (1 required)
Uninstall: 2 sections (1 required)     ← 之前是 0 sections
```

### 最小探测:把 NSIS 卸载器语义钉死

| 探测项 | 结论 |
| --- | --- |
| `Section "Uninstall"` / `Section "un.X"` / 其它名 | 前两者归**卸载器**,第三者归**安装器**(按 makensis 汇总行计数对比) |
| 卸载段加 `SectionIn RO` | 汇总行显示 `(1 required)` → 卸载器识别 RO |
| 静默卸载时主段是否执行 | **执行**(probeA 实测标记文件生成) |
| 静默卸载时 `/o` 可选段是否执行 | **不执行**(probeA 实测无标记) → "数据默认保留"的依据成立 |

### 端到端实测(本会话已可运行安装器,补上了此前无法做的验收)

| 步骤 | 结果 |
| --- | --- |
| 静默安装(最终产物) | exit 0;144 文件;**qmldir 12**;`vcruntime140.dll` 在;ARP/开始菜单/uninstaller 齐 |
| 安装**不误删**已有文件 | 预置 `keepme.txt` 安装后仍在 ✔ |
| 装好的副本能跑 | `--screenshot` 出图;日志 `data dir = .../Programs/Nextboard/data` |
| 静默卸载 | exit 0;**仅剩 `data\`**;`clipboard.db`/用户标记保留;exe/Qt/uninstaller/ARP/开始菜单/Run 全清 |
| 测试残留 | 已全部清理(安装目录、探针、markers 均已删) |

### 两个通用教训

1. **编译汇总行要看**:`Install/Uninstall: N sections` 能一眼看出"卸载器是空的"这类结构性错误;
2. **产物可运行性取决于文件标签**:本环境下凡"打包/构建产出可执行文件"的流程,都应显式复核
   完整性级别 —— 现象是"双击没反应/报临时目录无效",根因却在标签上。

> 附:本轮为验证又踩了一次 **BOM 被编辑工具剥掉**(`installer.nsi` / `package.ps1` 各一次):
> NSIS 报 `Bad text encoding`、PowerShell 5.1 直接解析失败。改这两个文件后必须补 BOM。
>
> 环境说明:用户 HKCU Run 中原有一条自启项(指向 `dist\Nextboard-0.2.0\nextboard.exe`),
> 在问题 B 的安装测试中被卸载逻辑删除;该入口本就打算关闭,未再恢复。

---

## 2026-10-10(六)列表项图标 + 标签配色(类型一眼可辨)

### 诉求与结论

列表项原先只用**文本**区分类型(`kind` 徽标写"text/image/file"、"固定"写"固定"),
扫视时要逐字读才知道是什么;自定义标签则全部同一个蓝色,多个标签挤在一行时分不出来。
本轮:类型/固定/来源徽标统一改「**图标 + 文字**」,自定义标签改为「**按标签名派生颜色**」。

### 改动清单

| 位置 | 改动 |
| --- | --- |
| `panel/qml/HeaderIcon.qml` | 图标词汇从 3 种扩到 7 种:新增 `all`(2×2 网格)/`text`(段落横线)/`image`(画框+日+山)/`file`(文件夹);`pin` 兼作"固定"状态标识。新增 `size` 属性 + `unit` 缩放系数(几何数字仍按 16px 写,统一乘 unit),使同一份图标可用于导航标签页(14px)与元信息徽标(11px) |
| `panel/qml/Theme.qml` | 新增 8 色标签调色板 `tagRed…tagMagenta` + 纯函数 `tagColor/tagHash/tagRichText/escapeHtml` |
| `panel/qml/EntryCard.qml` | 新增局部组件 `MetaBadge`(图标+文字同色);类型/固定/来源徽标改用它;**标签徽标改 RichText 逐标签着色** |
| `panel/qml/Main.qml` | 导航标签页加图标;标签筛选条改为「标签色文字 + 同色边框 + 色点」 |
| `panel/qml/ThemedButton.qml` | 仅更新 `iconName` 的取值注释 |

### 几个关键决策(含"为什么不能那样做")

**1. 标签颜色只由名字派生,不落库。** 同名恒定、跨重启恒定、不依赖条目集合,
所以可以直接写在 QML 绑定里 —— 委托被 `reuseItems` 回收复用到别的行也不会"串味"。
代价是不同标签可能撞色(见下文实测),要严格不撞色就必须持久化"标签名→颜色"
(含分配顺序),那是 core 侧改动。

**2. 哈希不能用 djb2(这是本轮最值钱的一条)。**
直觉写法 `h = h*33 + c` 在这里有个隐蔽缺陷:**33 ≡ 1 (mod 8)**,于是
`h mod 8 ≡ (5381 + Σord(c)) mod 8` —— **只与字符码之和有关,与顺序无关**(短名字未被
模数归约时是严格恒等式,已实测验证)。后果:任意换序的标签必然同色(`重要`/`要重`、
`ab`/`ba`),且分布按"码和同余"聚集 —— 实测 `重要` 与 `学习` 稳定撞同一色位。
改用 FNV-1a 变体 + murmur3 finalizer 的雪崩,并约束乘数 < 2^22 使中间值 < 2^53
(QML 的 JS 是双精度,超 2^53 会丢低位)。
⚠️ 位运算必须写 `>>> 0` 而非 `& 0xffffffff`:后者在 JS 里是**有符号**的
(`0xffffffff` 按 int32 是 -1),结果变负数会让后续取模/乘法全错 —— 模拟两套实现
后色位完全不同(6/8 个标签会换色)。

**3. 调色板是"按约束搜出来的",不是手挑的。** 8 个色值满足:
① 对比度 —— 浅色值在 `surfaceSecondary` 上 ≥ 4.7:1、暗色 ≥ 6.5:1(11px 小字按正文
当量,留余量);② 两两可区分 —— 同主题内 CIEDE2000 ΔE ≥ 17.5;③ **不与"固定"徽标
撞色** —— 与 `warning` 的 ΔE ≥ 19(暗)/29(浅)。
③ 是硬要求:"固定"是**状态**标记且以实心色块出现,而标签也是彩色胶囊,一次误读就
会把"有标签"看成"已固定"。故色相 45°~100°(实测 warning 落在浅 58°/暗 84°)整段不用。
两个主题**共用同一套色相**,否则同一标签切换主题会换色相、失去"颜色=标签"的记忆锚点。

> 三条弯路记下来免得再走:① 只最大化两两 ΔE 会选出 `#000004` 这种"接近黑"的解
> (ΔE 大但不像标签色,且与正文黑字撞脸);② 不加 chroma 上限会选出 `#FF7548`/
> `#D3EF00` 霓虹色,与 theme.md §48「避免彩虹色」冲突;③ 手工挑色相会自己挤堆
> (曾在 15°/25°、150°/165° 各摆两个),最小 ΔE 反而更差 —— 等距铺开再局部微调。

**4. 标签活跃态不用"同色相半透明叠底"。** 直觉做法是选中 chip 用标签色自身 alpha 叠底,
但浅色调色板是按"在 surfaceSecondary 上 ≥ 4.7:1"定的,余量仅 0.2 —— 压一层同色相底
立刻跌破 4.5:1(实测 alpha 0.10 时最差 **4.02:1**;二分求出可用 alpha 上限仅 0.0295,
等于没有底色)。故活跃态改由**同色边框 + 色点实心**(非活跃为空心点)承担,实测
文字对比度 4.71(浅)/6.52(暗)、边框与底的 ΔE ≥ 58。色点实心/空心的区别还让"选中"
在灰度屏与色弱下也读得出。

**5. 标签徽标仍必须是单个 RichText Label。** 逐标签上色最自然的写法是"每个标签一个
彩色 chip 委托",但那会回退 `EntryCard.qml` 早已记下的坑:元信息行是 `RowLayout`,
动态增删委托会让布局的隐式尺寸失效(清空标签后卡片塌陷)。故走 RichText 单 Label;
`textFormat: RichText` 顺带断掉了"标签名被当标记渲染"的注入路径(`tagRichText` 仍显式转义)。

### 实测数据

- **构建**:`build.ps1` 通过(14/14 Linking),无新增编译警告;
- **qmllint**:按类别统计 `Warning 328 → 356`、**Error 0**;新增 28 条全部落在既有的
  `missing-property`(Theme.* 解析噪声)/`unqualified` 两类,无新类别。
  顺带修掉一处真隐患:`Repeater` 委托里写 `parent.lineWidths[...]`,运行期能取到但
  类型系统里 `parent` 是 Repeater —— 改用 `textIcon.lineWidths[...]` 明确引用;
- **对比度**(CIEDE2000/WCAG 计算,非目测):浅色 8 色 4.71~5.49:1,暗色 6.52~11.11:1;
  与既有语义色的最小 ΔE:浅 7.5(vs error)/暗 5.4(vs error),与 warning 的间隔
  29.3~57.0(浅)/19.3~63.1(暗);
- **撞色率**:32 个真实标签名在 8 色下同色 57 对(随机期望 62 对),即"同屏 3 个标签
  有一对同色"约 3 成概率 —— 这是"不落库"方案的固有上限,已写在 `Theme.tagColor` 注释里;
- **跨语言一致性**:Python 模型与 QML `Theme.tagHash` 逐值核对 6 个标签**全部相等**,
  顺序敏感性两边一致(`重要`→色位 6、`要重`→1);
- **图标几何**:逐图标 48px 渲染后按像素测量。`text` 三条线逻辑宽 12.00/12.00/7.67
  (设计 12/12/7.5),证实"末行短"分支生效;`file` 页签与主体的**描边重叠带**从 3.0px
  修正到 2.0px(其余描边 1.67px,同量级)——
  修正前两条描边只在一点相接、合计粗了一倍,48px 放大后肉眼可见"上沿特别厚";
- **真机自截**(`--screenshot`,亮/暗两主题 + 带搜索词):`nextboard.log` 中
  `WARN/ERROR/TypeError/undefined` 出现次数均为 **0**;
- 按像素核对自截图:设计色值 `tagOlive/tagTeal/tagAzure/tagBlue/tagPurple` 严格命中,
  "固定"徽标仍是实心 warning 块(49×22,宽高比符合胶囊),固定卡上同时出现
  sourceOcr + tagTeal(颜色各自独立)。

### 限制与后续

- ⚠️ **标签撞色**是已知上限(见上);若要根治需把"标签名→颜色"持久化(设置项或新表),
  属 core 侧改动;
- ⚠️ 自截链路无法覆盖 `reuseItems` 的委托复用(自截只出首屏)。复用安全性由"全是纯函数
  绑定、无委托内部状态"从设计上保证 —— 与本轮改动前的既有结论同源,但**未做滚动实测**;
- 图标造型已按 48px 像素测量核对几何,**观感(是否够醒目、11px 下是否发虚)仍建议真机确认**;
- `docs/theme.md` 未更新:§18 的 Clipboard Item 示意仍是纯文本,§38 的图标规范未覆盖
  "内容类型图标"这一类。若要长期维护该文档,建议补一节。
- 本轮验证在 `app\build\data\` 造过样本条目、并把主题切到 dark 各截一次图;结束后已还原
  配置、清理样本 blob。⚠️ 期间误删了该开发库的历史(WAL 未一并备份,只剩 4KB 空壳),
  已由应用重新建库 —— **dev 库无有效数据,不影响构建与产品**;后续若要在 dev 库造样本,
  应先停应用再做整目录备份(含 `-wal`/`-shm`),不要只拷 `.db`。

---

## 2026-10-11(六)卡片右下角按钮改纯图标 + ToolTip

### 诉求与决策

右下角的「贴图 / 编辑 / 删除」(以及条件出现的「展开/收起」)原来是文字按钮,与上一轮
"徽标带图标"的口径不一致且占行宽。用户拍板:**全部改纯图标按钮**(展开/收起也要),
文字说明由 ToolTip 承担。

### 改动清单

| 位置 | 改动 |
| --- | --- |
| `HeaderIcon.qml` | 图标词汇 7 → 10 种:新增 `chevron`(V 形折角,反向靠 rotation:180 翻转)/`pencil`(斜杆+菱形笔尖)/`trash`(提手+桶盖+桶身) |
| `EntryCard.qml` | `CardAction` 升级为三形态组件:纯图标(方形)/图标+文字/纯文字;`label` 在纯图标形态下转作 ToolTip 文案;新增 `danger`(删除按钮悬停转危险色) |
| `ThemedButton.qml` | 仅同步 iconName 取值注释 |

### 两个实测坑(都花了一轮定位)

**1. `Theme.colors.danger` 不存在 —— 按钮整排消失。**
`danger` 别名在 **Theme 根**上(`Theme.danger`),`colors` 里只有 `error`。
写成 `Theme.colors.danger` 得到 undefined,QColor 绑定整体失效,日志报
`Unable to assign [undefined] to QColor`(EntryCard.qml:78/85)—— 表象是
**四个按钮从列表里消失、无底色无图标**,且不含 "ERROR" 字样,只在 `WARN` 级。
教训:颜色别名在哪个层级要查 Theme.qml 而不是凭印象;这类失效只报 WARN,
"日志无 ERROR"不能当作 UI 正常的判据。

**2. `implicitWidth: iconOnly ? implicitHeight : ...` 让按钮叠到行中间。**
同对象隐式尺寸互引(implicitWidth 引 implicitHeight),RowLayout 拿不到确定宽度,
实测 4 个按钮**全部叠在 fillWidth spacer 之后、行中间同一处**,spacer 失效。
修复:`readonly property int buttonHeight: 22` 后两边都引用它。

### 验证(实证与未实证,分开写)

- **构建**:`build.ps1` 通过,无新增编译警告;运行日志(亮主题 + 带样本数据)
  `WARN/ERROR/Unable` 均为 0(修复 1 之后);
- **qmllint**:EntryCard/HeaderIcon 无新增 Error 类别,仅既有 Theme.* 解析噪声;
- **图标几何**:chevron/pencil/trash 以 48px 探针渲染并按像素核对 —— V 形两杆交点
  落在视口 (8,7.6)、铅笔笔杆+笔尖沿 45° 对角线衔接、垃圾桶三层齐备,均在 16px 视口内;
- **新旧 UI 对比**(git stash 切换构建后自截):像素差异集中在按钮区
  (逻辑 x 325..475、y 525..575),证明按钮区确实被本改动改变;
- ⚠️ **未做**(环境所限,不是推断为通过):①ToolTip 悬停弹出效果;②danger 悬停色;
  ③纯图标按钮在真实列表里的肉眼观感(自截链路的 DPI 会随窗口所在屏幕在 1.0/1.75 间漂移,
  且列表内容被运行中的应用实时捕获污染,像素级 A/B 对比不可复现)。这三项需真机确认;
- **探针环境的坑**(与产品无关):`qml.exe` 在无前台激活的窗口上用默认 D3D11 后端时
  `grabToImage` 回调永不触发(进程挂死无报错);加 `QT_QUICK_BACKEND=software` 即正常。
  另:chevron/pencil 初版在绑定里引用自身 `width` 配合 `rotation`,同样表现为挂死 ——
  图标定位一律用**常量数值**(由工具解出后代死),不要自引用。

### 新增图标几何备忘(16px 视口,unit 缩放)

| kind | 组成 | 关键数值 |
| --- | --- | --- |
| `chevron` | 两根 5.5×1.5 胶囊,旋转 ∓45°,上端交于 (8,7.6) | 左杆 x=3.305 y=8.795 rot=-45;右杆 x=7.195 y=8.795 rot=45 |
| `pencil` | 7.5×3 胶囊(45°)+ 2.8 方块(45°,菱形) | 杆 x=4.45 y=5.7;尖 x=10.85 y=9.85;衔接点 (10.85,9.85) |
| `trash` | 提手(5×2.5 Outline)+ 桶盖横杆(12×1.5)+ 桶身(9.5×7.5 Outline) | 提手 y=1.5;桶盖 y=5;桶身 y=7 |

### 限制与后续

- 「固定」徽标与「贴图」按钮同用 `pin` 图钉:徽标是琥珀实心胶囊、按钮是中性方形,
  并排可辨;若真机确认仍混淆,改"浮层"造型(圆角矩形+右上斜出短箭头);
- ToolTip 延迟取 600ms(防止扫过一排按钮时刷泡),若真机觉得迟钝可下调;
- `docs/theme.md` §38 未覆盖"操作按钮图标"这一类,与上轮的类型图标一样待补文档。

---

## 2026-10-11(六)补:纯图标按钮两处回归修复

### 现象(用户真机反馈)

1. **「展开/收起」的文字颠倒了** —— 图标转了,文字也跟着倒;
2. **删除按钮悬停后整体太红,看不见文字和图标**。

### 根因(两个都是首版实现错误)

**1. 纯图标形态从未生效。** 首版 `iconOnly = iconName !== "" && label === ""`,但四个按钮
都传了 `label`(本意是给 ToolTip 用)→ `iconOnly` 恒 false → 实际渲染成"图标+文字";
叠加在**整个按钮**上的 `rotation: 180`(为翻转 chevron 方向)于是把文字一起转倒了。
用户看到的"文字颠倒",其实是"本该纯图标、却带上了倒置的文字"。

**修复**:`label`(提示文案)与 `showLabel`(是否渲染文字)拆成两个属性,
`iconOnly = iconName !== "" && !showLabel`;方向翻转改用新增的 `iconRotation`
**只作用于图标**(HeaderIcon 上),按钮本体不再旋转。

**2. 悬停"红底红字"。** 首版把删除按钮的悬停**底色**和**前景**都换成 `Theme.danger`
→ 对比度 1.00:1(同色),必然不可读。修复:悬停底色统一 `accentSoft`(与其它按钮
同一悬停语言),只把**前景**换 danger —— danger 压 accentSoft 实测
**4.24:1(浅)/ 5.16:1(暗)**,比普通按钮的 accent(3.23/5.01)还高,
删除按钮反而更醒目,且不需要红底大色块(§47 的强调色预算不受影响)。

### 验证

- 构建通过、运行日志 `WARN/ERROR/Unable` 均为 0;
- 对比度按 WCAG 计算核实(见上,含旧方案 1.00:1 的对照);
- **CardAction 组件探针**(组件逐字复制 + Theme/HeaderIcon 探针副本,软件后端):
  四个按钮渲染为 4 个独立方形小按钮,chevron(V 形)/pin(图钉)/pencil(斜杆+菱形)/
  trash(桶身)形状与设计一致,**均无文字** —— iconOnly 判定修复得到实证;
- ⚠️ 仍需真机确认:ToolTip 弹出、「收起」态箭头方向(180° 翻转后)、删除悬停的观感。

### 教训

- **布尔判定不要拿"某属性恰好为空"来兼职**:label 身兼两职(渲染文本/提示文案)时,
  纯图标判定写 `label === ""` 就把 ToolTip 需求顶掉了。职责要拆成两个属性。
- **旋转/缩放这类视觉变换,挂在最小的作用域上**:要转图标就转图标容器,
  挂在按钮上等于把内部所有内容一起变换。





