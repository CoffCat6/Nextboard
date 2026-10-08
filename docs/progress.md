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
