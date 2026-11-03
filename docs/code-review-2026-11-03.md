# Code Review 报告 — Nextboard 全项目潜在 bug 分析

> 审查日期:2026-11-03 · 审查方式:只读静态审查(未修改任何代码)
> 审查范围:`app/`(C++/Qt 6 宿主,含 QML 面板)与 `core/nbcore/`(Rust 核心,nbcore.dll 经 C ABI 桥接)全部源码

## 概览

**审查方法**:主审人负责热键/截图会话/遮罩/智能框选/FFI 桥接/主链路;另由三名并行审查员分审
`app/src/host/`(21 个文件)、面板与 UI 层(panel / qml / pin / note / editor / ui,约 9000 行)、
Rust 核心(nbcore 全部)。**每条发现均已阅读实际代码核实**,宁缺毋滥。

**关键证据**:

- 崩溃现场日志 `app/build/data/nextboard.log`(最后写入 2026-11-03 14:25):最后一次会话以剪贴板警告
  `Retrying to obtain clipboard.` / `Unable to obtain clipboard.` 戛然而止,**无 `[CRASH]` 条目、
  无正常退出标记** —— 典型的"静默进程死亡"。宿主装有 `SetUnhandledExceptionFilter` 兜底
  (0xC0000005 会落 `[CRASH]` 日志),因此这不是 SEH 访问违例,而是 `abort()` / fail-fast 类死法。
- git 历史:今日 14:27 的提交 `ee7c5de`("元素级改快照预取,修退出期崩溃")已按"退出期
  WM_HOTKEY 悬垂"完成修复并带回断(`--hotkey-lifecycle-flow`)。

---

## 一、专题结论:"截图快捷键冲突导致闪退"

### 1.1 已修根因确认(修复正确,但只覆盖一条路径)

`ee7c5de` 对 `app/src/host/hotkeys.cpp:72-146` 的修复逐行核实无误:

- `QPointer<Hotkeys> m_owner` 替代裸指针,宿主析构后自动变 null,访问前判空;
- 过滤器由 `std::unique_ptr` **成员**持有(不再裸 `new` 泄漏),生命周期严格跟随;
- `~Hotkeys` 显式 `removeNativeEventFilter`,且先注销热键再摘过滤器(顺序正确);
- `hotkey_lifecycle_flow.cpp` 把"退出期收到 WM_HOTKEY 不崩"变成可重复断言(含 5 轮建/删泄漏哨兵)。

原根因(`new HotkeyFilter` 裸泄漏 + 裸指针 + 栈对象 Hotkeys 先析构 → 悬垂 → 0xC0000005)**已真实修复**;
同一模式也正确应用于 `Tray` / `QuickCommands`(`core_facade.hpp:270` 的 `QPointer<QObject> m_quickCommands`)。

### 1.2 但同一"闪退"症状还有 3 条未被覆盖的路径

#### 🔴 路径 A(与现场日志特征完全吻合):剪贴板监听线程 panic → `extern "system"` 边界 → abort

`core/nbcore/src/clip.rs:307-341`:

```rust
let header_size = u32::from_le_bytes([dib[0], dib[1], dib[2], dib[3]]) as usize;
if header_size < 40 || dib.len() < header_size { return None; }   // 允许 dib.len() == header_size ∈ 52..=55
...
let (r_mask, g_mask, b_mask, a_mask) = if header_size >= 52 {
    read_masks(&dib[40..56])     // dib.len() 为 52~55 时切片越界 → panic
```

调用链:`DispatchMessageW → wndproc(unsafe extern "system") → handle_clipboard_update →
read_image_from_clipboard → decode_dib`。**Rust 规范保证 panic 不得穿出 `extern "system"`
函数 —— 直接 abort 整个进程**;`ffi.rs` 的 `catch_unwind` 只覆盖 `nb_*` 入口,覆盖不到这条 OS
回调路径;`core/Cargo.toml:25` 特意设的 `panic = "unwind"` 在这里也救不了。

症状:**任何往剪贴板写了畸形 CF_DIB 的应用都能触发,无需用户任何操作**;线程死前无日志
(panic 写 stderr,托盘应用不可见),无 `[CRASH]` —— 与日志结尾的剪贴板警告 + 静默死亡完全一致。
崩溃前用户桌面开着 Firefox / Chrome / VSCodium 等多个会写剪贴板的程序。

#### 🔴 路径 B("快捷键冲突"体感的直接来源):单实例门禁完全失效

`app/src/main.cpp:253-262`:`SingleInstance instance;` 声明在 `if (!ownDataDir) { ... }`
**块作用域**内,260 行块结束即析构(`single_instance.cpp:37-40` 的 `ReleaseMutex` +
`CloseHandle`)—— 比 262 行的 `QApplication app` 还早。命名互斥量的内核对象在最后一个句柄
关闭后即销毁,**此后第二个进程的 `CreateMutexW` 会成功,`ensure()` 返回 true,两个完整实例并存**:
双托盘图标、两路剪贴板监听写同一个 `data/`、**第二个实例的全局热键全部注册失败并弹
「快捷键不可用:…(被其他程序占用)」toast** —— 这正是用户报的"快捷键冲突"的感知来源,
且会被误判成热键问题。附带问题:`single_instance.cpp:16-33` 的 `ActivateFilter` 是死代码
(全仓无实例化),即便修好门禁,"二次启动唤起已有实例"也未接通。

#### 🟠 路径 C:auto_paste 注释与实现互相矛盾

`app/src/host/auto_paste.cpp:20-21 vs 39`:注释明确"按下与抬起之间**必须**有小间隔,
一次性投 4 个事件时,部分应用(实测 Electron 系与部分远程桌面客户端)只能看到已按下而看不到
配对抬起,结果是 Ctrl 卡住",实现却是 `::SendInput(4, inputs, sizeof(INPUT))` 一把梭。
功能默认关闭,开启者会遇到系统级 Ctrl 卡键。

### 1.3 建议的排查/修复顺序(针对闪退)

1. `clip.rs`:掩码分支前补长度校验(`header_size >= 52` 时要求 `dib.len() >= 56`);
   并给 `handle_clipboard_update()` 整体包 `catch_unwind`,让消息循环线程与 FFI 入口
   享受同等 panic 边界(成本最低、收益最大)。
2. `main.cpp`:把 `SingleInstance` 提升为 `main()` 作用域对象(活到 `return`)。
3. 保留现有热键过滤器修复;可补充:`HotkeyFilter::nativeEventFilter` 里 `emit` 是同步执行的,
   槽函数(`panel.hide()`/`showAtCursor()` 等)跑在原生事件过滤器栈上,建议改为队列投递,
   彻底消除重入面。

---

## 二、发现清单(分级)

严重级别定义:**🔴 Blocker** = 会导致错误结果、崩溃、数据丢失或安全漏洞;
**🟠 Major** = 不会立刻出错但埋下隐患;**🟡 Minor** = 不影响正确性但增加维护成本;
**💡 Suggestion** = 可选改进。

### 🔴 Blocker(必须修复)

1. **`core/nbcore/src/clip.rs:335-341`** 畸形 CF_DIB 触发 slice panic,panic 穿
   `extern "system"` wndproc 边界直接 abort 进程(详见 §1.2 路径 A)。
   - 问题:`decode_dib` 只校验了 `dib.len() >= header_size`,但 `header_size ∈ 52..=55` 时
     切的是 `&dib[40..56]`,越界即 panic;`ffi.rs` 的 `catch_unwind` 覆盖不到 OS 回调路径。
   - 为什么重要:release profile 特意设 `panic = "unwind"`(core/Cargo.toml:25 注释明确
     abort 会杀死宿主),但这条路径的 panic 仍以 abort 收场;且线程死亡前无任何日志。
   - 建议:① 掩码分支前补长度校验;② 给 `handle_clipboard_update()` 包 `catch_unwind`。

2. **`core/nbcore/src/backup_export.rs:986`(配合 `:1312-1319`)** 导入备份的 zip-slip 变体:
   落盘路径来自备份 DB 内容,绕过 `enclosed_name` 防护。
   - 问题:`stage_backup` 对 zip **条目名**做了 `enclosed_name()` 防护(`:740`),但
     `import_snapshot` 里 blob 的**目标路径**直接取自此备份里的 `clip_entries.blob_path` 列:
     `else { PathBuf::from(row.blob_path.as_deref().unwrap_or_default()) }` 未净化,
     `copy_blob` 里 `live_blobs.join(dst_rel)` 直接拼。
   - 为什么重要:恶意备份只需在 `clipboard.db` 放一行
     `blob_path = "../../../../Users/<user>/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup/x.png"`,
     再把任意字节放进 `blobs/<id>.png`,导入即写用户目录任意位置。"导入来源不明的备份"
     正是该功能的既定威胁模型;`:1731` 的 zip-slip 回归测试只覆盖了条目名。
   - 建议:对 `main_dst` 做与条目名同级的净化 —— 拒绝含 `..`/根前缀/盘符的相对路径,
     或规范化后断言 `dst.starts_with(blobs_dir)`,不合法跳过并计数。

3. **`app/src/main.cpp:253-262` + `single_instance.cpp:37-40`** 单实例互斥量在
   `QApplication` 构造之前就被释放,单实例保护完全失效(详见 §1.2 路径 B)。
   - 建议:把 `SingleInstance` 提升为 `main()` 作用域对象;或改为"句柄不归对象所有、
     显式 keepAlive"的设计;至少在 `ensure()` 成功后不再由析构函数提前归还句柄。

### 🟠 Major(应该修复)

4. **`core/nbcore/src/ffi.rs:55-57`** `nb_init` 失败后 `INITIALIZED` 永久为 true,
   核心进入不可恢复的楔死状态。
   - 问题:`if INITIALIZED.swap(true, Ordering::SeqCst) { return Err(...); }` 位于一切
     可能失败的操作**之前**;`Core::init` 失败(配置缺 data_dir / settings load 失败 /
     store open 失败,如 db 被占用)后,重试 `nb_init` 永远返回"已初始化";
     `nb_call` 能过初始化闸门但 `Core::get()` 报"核心未初始化"。
   - 连带问题:`nb_shutdown`(`ffi.rs:196-201`)只把标志位置 false 而不清 `OnceLock`,
     shutdown 后再次 `nb_init` 会走到 `CORE.set` 失败,留下"标志位=true、CORE 是旧实例、
     监听线程全死"的僵尸状态。
   - 建议:把 `swap` 移到初始化成功之后(或用 `compare_exchange`),失败路径保持
     `INITIALIZED=false`;`nb_shutdown` 后拒绝再 `nb_init`。

5. **`app/src/panel/core_facade.cpp:198`** nbcore 的"单门面串行化"不变量被多条调用路径绕过。
   - 问题:`AsyncCall::run()` 用函数内静态互斥锁把 `nb_call` 串行化,但
     `ocrImageDetailed` / `qrDecodeRgba`(`editor_window.cpp:596-598/623-625`、
     `capture_session.cpp:2199-2200/2236-2237`)、`captureMonitorRegion` / `stitchAppend`
     (`capture_session.cpp:637/720`)等 FFI 入口从 `QThreadPool` 工作线程直接进入 nbcore,
     完全没走这把锁;`CoreBridge` 自身无任何锁。
   - 为什么重要:`QtConcurrent::run` 与 `AsyncCall` 共用全局线程池,并发进入 nbcore 是常态。
     今天的实际后果更可能是相互阻塞/串行化(5 秒 OCR 卡住一次 save_settings),而非立刻的
     内存踩踏;但"nb_call 是进程级单门面"的不变量在宿主与核心两侧都有文档声明,宿主只覆盖了
     1/5 的入口 —— 与上次退出期 0xC0000005 落在 nbcore 是同一类根因。
   - 建议:把这把互斥锁提为 `CoreBridge` 的私有成员,让所有 FFI 入口统一加锁;锁的粒度保持
     "一次 FFI 调用",持锁期间不 emit / 不做 UI 操作。

6. **`app/src/host/auto_paste.cpp:39`** 注释与实现互相矛盾(详见 §1.2 路径 C)。
   - 建议:二选一 —— (a) 拆成两次 `SendInput`(2 个 KEYDOWN → 间隔几毫秒/一个事件循环 →
     2 个 KEYUP);(b) 若已实测证实无此现象,把注释改写为实情并说明当时的观测条件。

7. **QImage/QPainter 空值保护不全面** —— 与 `image_save.cpp:53-67` 已修的
   "分配失败 → painter 未激活 → 0xC0000005"同一类(团队已知真实触发路径,见该文件注释与
   `docs/29 §3`;**注:docs/29 文件在仓库中缺失,建议补回**):
   - `app/src/capture/capture_session.cpp:1281-1300` `maskedBase()`:`QImage img(size()*scale, ...)`
     无 `isNull()` / `isActive()` 判空(4K × 300% 约 300MB 分配)。目前是**死代码**
     (`paintEvent:1337` 的 `if (false && ...)` 永不启用),但注释明确"优先用预合成缓存"是
     既定意图,重新启用即复活崩溃;
   - `app/src/host/quick_commands.cpp:98` `hIconToImage`:`QImage color(w,h,...)` 后直接
     `GetDIBits(..., color.bits(), ...)`,w/h 为 0 或分配失败时 `bits()` 为空指针;
   - `app/src/editor/annotation_engine.cpp:1003-1007 / 1020-1023` `compose()`:
     `QImage out(size,...)` 无判空(长截图合成图可达数百 MB,恰是分配失败高发场景);
   - `app/src/pin/pin_window.cpp:453-454` `m_displayCache` 同样无判空。
   - 建议:按 `image_save.cpp` 的范式(`isNull()` 早退 + `isActive()` 双保险)统一补齐。

8. **`app/src/host/note_flow.cpp:261/280/298/319/339/370/394`** 大量
   `QTimer::singleShot(N, qApp, [id, window, …])` 未传 context 对象,回调里
   `window->geometry()` / `note->collapse()` 直接解引用 —— 窗口若在定时器触发前被销毁
   (STEP6 删待办会关窗、外部 close 也会)即 UAF。当前靠"流程顺序保证窗口活着"的脆弱契约兜底。
   - 建议:context 传 `window`(`QTimer::singleShot(400, window, …)`),或捕获 `QPointer` 并判空。

9. **`app/src/ui/ocr_result_panel.cpp:348/370-374/514-519`** 预览浮窗只 hide 不 delete,
   每次识别漏一个顶层窗口。
   - 问题:`buildPreview()` 建了**无父对象**的顶层窗口 `new QWidget(nullptr, Qt::ToolTip | ...)`,
     而两处"回收"都只调 `hidePreview()`(只 hide)。面板自身有 `WA_DeleteOnClose`,关闭即销毁;
     `m_preview` 没有父对象,于是**每个 OCR 结果面板都永久泄漏一个 QWidget + 2 个 QLabel +
     一份 stylesheet**。OCR 是高频动作,长驻托盘应用里这份泄漏单调累积。
   - 建议:`closeEvent` 里改 `m_preview->deleteLater(); m_preview = nullptr;`(连同
     `m_previewImage` / `m_previewCaption` 一并置空);`buildPreview()` 可加
     `if (!m_lines.isEmpty())` 守卫,让纯文本面板不付这份开销。

10. **`app/src/main.cpp:34-66`** 日志处理器不是线程安全的,且与崩溃处理器同写一个文件。
    - 问题:`fileMessageHandler` 用函数级 `static QFile log` + `static qint64 written`,无任何锁;
      而 `QtConcurrent` 工作线程里的 `qWarning/qInfo`(如 `capture_session.cpp:642/698`
      的长截图 worker)会并发进入。并发写会交错/半行丢失,`written` 竞态会让滚动提前或滞后触发;
      滚动时 `log.close()` 与另一线程的 `log.write()` 并发(不崩,但丢日志)。崩溃排障恰恰
      依赖这份文件的完整性。
    - 建议:加进程内 `QMutex` 保护写入与滚动;或把 `[CRASH]` 日志写到独立文件。

### 🟡 Minor(建议修复)

11. **`app/src/host/hotkeys.cpp:29`** `VkKeyScanW` 失败(返回 -1)未校验,得到 `vk=0xFF`
    并把失败报告成"被其他程序占用"。
    - 问题:`LOBYTE(0xFFFF)==0xFF`,非 0 通过 `if (mods == 0 || key == 0) return false;` 检查,
      最终 `RegisterHotKey(nullptr, id, mods, 0xFF)` 失败,toast 文案是
      「快捷键不可用:Ctrl+?(被其他程序占用)」(`hotkeys.cpp:186`)—— 失败原因被错误归类,
      用户按提示去"关掉占用程序"永远解决不了。
    - 建议:`const SHORT sc = VkKeyScanW(...); if (sc == -1) return false;` 走"格式非法"分支。

12. **`app/src/host/hotkeys.cpp:184` + `hotkey_lifecycle_flow.cpp:100-133`** 热键 id 分配
    从 1 起算,验收流反复 `new/delete Hotkeys` 会注销主实例同 id 的热键。
    - 问题:`kHotkeyIdBase + m_nextId++`,每个实例都从 0xB001 开始;验收流创建临时
      `Hotkeys` 时同 id 注册失败(被主实例占着),`delete` 时却把主实例的 0xB001… 一并
      `UnregisterHotKey`(同线程同 id 互斥)—— 验收跑完产品热键全灭,直到下次
      `applyFromSettings`。产品路径只有单实例,不触发。
    - 建议:id 用进程级原子计数器分配,不与实例生命周期绑定。

13. **`app/src/host/tray.cpp:53` + `tray.hpp:13-43`** 托盘菜单树泄漏,且 `Tray` 无析构函数兜底。
    - 问题:`m_menu = new QMenu();` 无父对象,`Tray` 未声明析构函数;Qt 文档明确
      `QSystemTrayIcon::setContextMenu()` **不取得菜单所有权**,整棵菜单树(含两个子菜单与
      全部 action)在进程退出前永不释放。所有 action 的 lambda 都按 `this` 捕获。
    - 建议:`Tray` 增加析构函数 `delete m_menu;`,或构造时 `new QMenu(this)`。

14. **`app/src/host/tray.cpp:101` 与 `:117`** 两个二级菜单的 `objectName` 都叫
    `nbTrayTodoMenu`(复制粘贴错误)。样式表选择器 `QMenu#nbTrayTodoMenu`(`:85`)会同时命中
    两者,视觉上看不出;但任何 `findChild<QMenu*>("nbTrayTodoMenu")` 都会拿到不确定结果。
    建议便签子菜单改名 `nbTrayNoteMenu` 并同步补进 QSS 选择器。

15. **`app/src/host/single_instance.cpp:16-33/46`** `ActivateFilter` 是死代码("二次启动唤起
    已有实例"链路实际未接通);`ensure()` 忽略调用方传入的 key(`Q_UNUSED(key)`),硬编码
    互斥量名 —— "同一数据目录只允许一个实例"的语义没有实现,两个用不同 `--data-dir` 的
    合法实例也会互斥。建议把数据目录规范化路径的哈希编进互斥量名;在 `PanelWindow` 建好后
    `installNativeEventFilter`(内部用 `QPointer<PanelWindow>`),或在 `PanelWindow::nativeEvent`
    中直接处理 `WM_APP_ACTIVATE`。

16. **`core/nbcore/src/settings.rs:1518-1520`** `save()` 用固定名 `app_config.json.tmp`
    且无保存互斥。宿主若从两个线程并发 `save_settings`,两个写会交错到同一 tmp 文件,
    `rename` 上去的可能是半截内容(`load()` 对损坏配置的整体回退 = 全部设置重置,
    正是本文件注释竭力避免的事故);另外 `set_settings`(`core.rs:163-174`)的"落盘"与
    "写内存"不原子,存在丢更新。建议 tmp 名带 uuid(`clip.rs:683 write_atomic` 已有现成写法),
    并对 save+swap 加一把进程内锁。

17. **`core/nbcore/src/store/entry.rs:1014-1015`、`store/todo.rs:344`** `limit` 直接来自
    宿主 JSON 且未校验正负,SQLite 中 `LIMIT -1` 语义是"无上限" —— 传 -1 会把整表读进内存
    (大历史下即内存事故)。建议 `limit.max(0).min(1000)` 之类钳制,offset 负值同样归一为 0。

18. **`core/nbcore/src/clip.rs:69、95`** `GetModuleHandleW(...).expect(...)`、
    `CreateWindowExW(...).expect(...)` 在监听线程启动路径上。线程内 panic 不会 abort 进程,
    但会**静默杀死剪贴板监听**(日志在 stderr,用户可见现象是"历史突然不记录了")。
    建议改造成返回错误并 `log::error!`,与 `spawn_thumbnail_backfill`(`:836-838`)的容错写法一致。

19. **`core/nbcore/src/store/mod.rs:18-22`、`stitch/session.rs:104-110`、`ocr/rapid.rs:360-362`**
    三处 Mutex 中毒都映射为"永久错误",而同类代码 `translate/http.rs:307`、`rapid.rs:156-160`
    用了 `unwrap_or_else(PoisonError::into_inner)` 恢复。持锁线程一旦 panic 一次,对应功能
    直到进程重启都不可用。建议统一 poisoning 恢复策略,至少 DB 锁要能恢复。

20. **`core/nbcore/src/clip.rs:887`** `write_rgba_to_clipboard` 的
    `(width as usize) * (height as usize) * 4` 是未 checked 的乘法,同文件 `encode_png_with`
    (`:851-854`)就用了 `checked_mul`,两处口径不一致;debug 构建下极端尺寸会 panic。
    `capture.rs:368/381-382`、`qr.rs:33`、`ocr/system.rs:234/297`、`ocr/preprocess.rs:37`
    是同一模式。建议统一走 checked 乘法 + 显式错误。

21. **`core/nbcore/src/backup_export.rs:735-768`** 解包阶段只有条目名/版本闸门,没有解包
    总字节上限,恶意备份(zip bomb)可写满磁盘。建议累计 `io::copy` 字节数并设上限。

22. **`core/nbcore/src/backup_export.rs:991 vs 1089`** `import_snapshot` 在 `tx.commit()`
    **之前**就 copy blob 文件,事务失败(唯一约束/磁盘满)时已搬运的文件成孤儿。建议 commit
    成功后再做文件搬迁,或失败路径按已搬清单回清。

23. **`app/src/note/note_registry.cpp:165-177`** `QHash::operator[]` 隐式插入 + 返回引用
    在 `close()` 后可能失效。
    - 问题:`QPointer<NoteWindow>& ref = m_windows[todoId];` —— 键不存在时会插入一个空
      QPointer(查询带上修改语义,与 `open()`/`describe()` 用的 `value()`/`contains()` 不一致);
      且 `ref` 是哈希槽的引用,而 `window->close()` 会同步触发
      `destroyed → forget(id) → m_windows.remove(id)`,哈希可能重排、`ref` 失效。当前 `ref`
      在 `close()` 之后没有被再使用,所以今天不会崩,但这是一个等着被后人踩的陷阱。
    - 建议:改为 `const QPointer<NoteWindow> p = m_windows.value(todoId); if (!p) return;`。

24. **`app/src/note/note_registry.hpp:56-58`** `changed()` 信号零订阅者(全仓无 connect,
    唯一消费方 `tray.cpp:196` 在 `aboutToShow` 里主动拉 `describe()`),四处 `emit changed()`
    全是空转。这是一条"看起来有人听、其实没人听"的通道,将来按注释依赖它会静默失效。
    建议删掉信号,或把注释改成实情。

25. **`app/src/host/capture_toolbar_flow.cpp:461/463/465/469`** `QPointer<OcrResultPanel> lp`
    未判空即解引用(`lp->sourceRectForLine(0)`)。同文件 327/347 等处都做了判空,唯独 B11 段漏掉;
    `showResult` 失败/极端环境下返回空指针即 UAF。属验收代码,但与"宁缺毋滥"的断言风格不一致。

26. **各 `*_flow.cpp` 失败路径直接 `QApplication::quit()`** 会静默跳过后续断言
    (`log_rotate_flow.cpp:59-63` 临时目录失败即 quit、`quick_commands_flow.cpp:172`
    嵌套链尾 quit、`smart_capture_flow.cpp:296-299` 无屏幕即 quit 但不计入 failures),
    会出现"断言其实没跑完、日志却显示干净退出"的假绿。建议抽一个
    `flowFailAndQuit(what)` 帮助函数,统一 `++failures; qWarning; quit()`。

### 💡 Suggestion(可选改进)

27. **`app/src/host/hotkeys.cpp:41-47`** `hidePanelThenCapture`:150ms `singleShot` 无防抖窗口,
    快速连按两次截图热键会起两个会话(第二个被 `pruneOverlays()` 拦住,但第一次的
    `panel.hide()` 已被无条件执行)。建议加"会话进行中/150ms 内不重复触发"的闸。

28. **`app/src/host/quick_commands.cpp:150`** `::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`
    没有配对的 `CoUninitialize()`(含 156 行的失败早退路径)。主线程可接受(进程级一次性),
    但该函数一旦被非主线程调用就会在线程退出后泄漏 COM 初始化;且若线程已被初始化为 MTA,
    `CoCreateInstance` 对 apartment 线程模型的 `IShellLink` 走代理,注释里"自增无副作用"的
    结论并不覆盖这种情形。建议记录本线程是否由本函数初始化,出作用域时按需 Uninitialize。

29. **`app/src/capture/capture_session.cpp:130`** `runAfterDestroyed()` 定义后全仓库无调用点
    (死代码)。删除,或在后续"等遮罩析构后再 X"的场景复用。

30. **`app/src/editor/annotation_toolbar.cpp:118-122`** `rebuildActions()` 重建时泄漏旧的
    QActionGroup 与全部 QAction:`QToolBar::clear()` 只 `removeAction` 不 delete
    (已对照本机 Qt 6.9.0 源码实证),每轮重建白留 ~22 个 QAction;四个旧 `QActionGroup`
    被覆盖后永不释放。不会悬空(`~QAction` 有反向清理),但量级随调用放大。
    建议开头先 delete 旧 group,或只在构造期建一次。

31. **`app/src/panel/panel_window.cpp:105`** `applyTopmost()` 的 `winId()` 兜底是同一类
    "创建期调 winId"陷阱:本文件 `nativeEvent`(`:583-584`)明确警告创建期间调 `winId()` 会
    递归触发 `CreateWindowEx` → 创建失败。当前全部调用方都在窗口创建之后,所以安全;但兜底意味着
    任何将来更早的调用路径都会复现这个已记载过的崩溃。建议改成 `if (!hwnd) return;`。

32. **`app/src/pin/pin_window.cpp:474-487`** 贴图窗口没有"屏幕增删后重夹"通路:
    `event()` 只处理 DPR 变化,`core_facade.cpp:536-539` 的 `pinRegistry()` 也没有屏幕变化
    兜底。对照便签接了 `screenAdded/screenRemoved → reclampAll`(`main.cpp:633-636`)、
    编辑器会检查屏幕相交,**只有贴图这一路是空的**。副屏拔掉后,停在该屏坐标上的贴图留在虚空,
    用户没有任何手段找回(贴图是 `WindowStaysOnTopHint | Qt::Tool`,无任务栏入口、托盘无列表)。
    建议复用 `NoteWindow::reclampToCurrentScreen()` 的思路补一个 `pinRegistry()->reclampAll()`。

33. **`core/nbcore/src/store/mod.rs:60-69`** `open()` 未设 `PRAGMA busy_timeout`。进程内有
    Mutex 串行化,但备份恢复、外部工具、第二个实例同时摸库时会直接 `SQLITE_BUSY`。设 2~5s
    成本为零。

34. **`core/nbcore/src/capture.rs:75-99`** `XCapCapturer::capture` 内部又全量枚举一次
    `Monitor::all()`,与 `capture_monitor_matched` 的枚举重复;且 `target.name` 为空(坐标回退
    匹配)时,`find(|m| m.name().ok() == Some(picked_name))` 找的是"第一台空名显示器"而非坐标
    匹配到的那台 —— 多台无名显示器时会取错屏(Windows 上 Monitor 恒有 `\\.\DISPLAYn`,
    目前不可达,但契约上是隐患)。建议 `capture` 直接接收 `&Monitor` 或以下标传递。

35. **`core/nbcore/src/stitch/session.rs:102`** 会话表无总量/空闲回收,宿主若崩溃前未 `abort`,
    会话(含整张结果图 RGBA,可达百 MB 级)留到进程退出。可加"N 分钟无 append 自动回收"的兜底。

36. **`core/nbcore/src/translate/http.rs:477-482`、`translate/socks.rs:55-63`** WinHTTP 路径
    按 `available` 一次性 `vec![0u8; to_read]`(恶意/异常服务端可诱导单块大分配),4MB 上限是
    "读完才查";SOCKS5 路径 `read_to_string` 完全无增量上限,与 WinHTTP 路径的
    `MAX_RESPONSE_BYTES` 行为不一致。建议两块都改增量读 + 超限即断。

37. **`app/panel/qml/TodoDatePicker.qml:272-277`** 年份列 `values` 绑定无响应式依赖:
    `new Date()` 不产生绑定依赖,该绑定只在首次读取时求值一次。跨年运行的实例会拿着旧年份表
    (好在有 ±3 年余量,约 3 年后才露出问题)。若要稳妥,可在 `Component.onCompleted` 里赋一次,
    或接受现状并加一行注释说明"余量 3 年,过期需重启"。

---

## 三、做得好的地方(值得保留的实践)

1. **退出期对象生命周期纪律是全场最佳实践**:`HotkeyFilter` 的 QPointer + 成员持有 + 显式摘除、
   `QuickCommands` 的 QPointer 注入、`closeAllOverlays`(`capture_session.cpp:146-190`)的
   `destroyed` 信号计数 + 跨栈 lambda 全按值捕获、长截图 worker 的 `SampleReturn` RAII 闸 +
   `QueuedConnection` 回投 —— 这套纪律正是"点结束立即 0xC0000005"类问题的正解。

2. **FFI 边界工程扎实**:Rust 侧全部导出包 `catch_unwind`、RGBA 缓冲移交前 `shrink_to_fit` +
   复核 `capacity==len` 才 `mem::forget`(`ffi.rs:542-547/955-997`);宿主侧 `CoreBuffer`
   零拷贝托管 + `shutdown()` 刻意不 `FreeLibrary` 的注释 —— 裸内存跨 ABI 最容易写错的地方
   处理得正确且有注释说明为什么。

3. **SQL 层全线参数化**:`IN (...)` 动态占位符先落变量再引用、LIKE 通配符显式转义、FTS5 MATCH
   对用户输入做字符白名单 + 短语包裹防语法歧义与语义篡改;批量删除/清空坚持单事务;备份链路的
   "预演再动真数据"(schema 版本闸门 + `enclosed_name` + staging 试迁移 + 失败回滚)。

4. **QML 生命周期管理规整,没有一处悬垂 context**(`qml_bootstrap` 四个 lambda、编辑器
   QFutureWatcher 以 `this` 为 context);撤销栈与 QGraphicsItem 所有权有显式析构顺序规定和
   guard;多屏/DPI 处理(`NoteWindow` 三件套、`dpr()` 从底图反推、按物理桌面原点配对)是真实
   踩坑后的收敛;验收流把历史缺陷逐条钉成可重复断言。

---

## 四、总结与修复优先级

- **用户报告的"快捷键冲突闪退"**:已提交的 `ee7c5de` 修复了退出期悬垂这条根因(已验证),
  但同一症状至少还有 3 条活跃路径。最优先:
  1. 🔴 `clip.rs` wndproc panic → abort(与现场日志"无 CRASH 的静默死亡"特征完全吻合);
  2. 🔴 单实例门禁失效(同时也是"快捷键冲突"感知的来源);
  3. 🟠 auto_paste 注释/实现矛盾。
- **全项目未发现除上述外的产品路径必然崩溃点**;Major 级共 7 项(本文 §二.4–10),
  建议按 **Blocker 3 项 → Major 4–7 项 → Minor** 的顺序排期。
- 本次审查只做分析,未改动任何代码文件。

---

*报告生成:DeepSeek Harness 代码审查(主审 + 三名并行审查员交叉核实)*
