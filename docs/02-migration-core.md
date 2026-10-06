# nbcore 改写路线:从 clipboard-plus 剥离核心为 DLL

| 项 | 内容 |
| --- | --- |
| 版本 | v1.0 |
| 日期 | 2026-10-05 |
| 状态 | 已定稿,进入实施 |
| 上游 | D:/3project/rust/clipboard-plus(只读参考,不改) |
| 关联文档 | [总体架构](./01-architecture.md) · [开发路线图](./03-roadmap.md) |

**目标**:把 clipboard-plus 的 Rust 业务核心剥离为独立 crate `nbcore`,以 `cdylib`
形态交付 `nbcore.dll`,供 Qt 宿主(C++/MSVC)加载调用。**改写而非重写**:业务逻辑
原样平移,只动与 Tauri 的耦合点;31 个单测随代码保留。

**工期预估:2–3 周**(M1 剥离 1–1.5 周,M2 FFI 1 周,M3 验收 3–5 天)。

---

## 1. 总原则

1. **先平移后改造**:第一步原样复制文件让单测全绿,再做耦合点改造,两步分开提交,
   出问题可二分回退;
2. **Tauri 依赖归零**:core/ 下不得出现任何 `tauri::` 引用;
3. **契约不变**:对外的 JSON 结构、DB schema、设置文件格式,除架构文档 §6 明确的
   `source`/`tags` 扩展外,一律与第一代兼容(用户数据目录可直接指向第一代数据
   完成导入);
4. **panic 不得穿 FFI**(架构 §5)。

## 2. 逐文件映射表(clipboard-plus src-tauri/src/ → core/nbcore/)

### 2.1 原样平移(无 Tauri 耦合或耦合可忽略)

| 第一代文件 | 去向 | 说明 |
| --- | --- | --- |
| store/mod.rs | store/mod.rs | 连接管理、迁移执行。`Mutex<Connection>` 保留(单写毫秒级) |
| store/entry.rs | store/entry.rs | 条目 CRUD/FTS/去重/淘汰,~1200 行纯 rusqlite |
| store/migrations/*.sql | store/migrations/ | v1–v4 原样;新增 v5(source/tags,见架构 §6) |
| translate/mod.rs | translate/mod.rs | provider 分派/编排;**改动一处**:emit 日志事件改为 tracing |
| translate/http.rs | translate/http.rs | WinHTTP 状态机,unsafe 全收敛于此,零改动 |
| translate/openai.rs / microsoft.rs / custom.rs / json_path.rs / lang.rs / socks.rs | 同名 | 零改动 |
| translate/test_server.rs | translate/test_server.rs | #[cfg(test)] 环回服务,随测试平移 |
| ocr.rs | ocr.rs | Windows OCR + 预处理;耦合仅日志,零逻辑改动 |
| error.rs | error.rs | AppError 增加 `to_json()`,供 FFI 层统一错误响应 |
| clip.rs 中 ingest_image / hash / 缩略图 / write_atomic | clip/ingest.rs, clip/thumbnail.rs | 与窗口无关的入库管线平移;监听循环部分见 2.2 |
| screen.rs | screen.rs | 显示器枚举/几何计算(纯逻辑部分) |
| settings.rs | settings.rs | **见 2.2**:去掉 store 插件,格式不变 |

### 2.2 需改造(耦合点逐个方案)

| 第一代位置 | 耦合点 | 改造方案 |
| --- | --- | --- |
| clip.rs `monitor_loop` | 200ms 轮询线程 + AppHandle | 重写为 **AddClipboardFormatListener** 事件驱动:专用线程建消息窗口,C++ 经 `nb_init` 传入通知 HWND 或由 core 自建;收到 WM_CLIPBOARDUPDATE → 读取/去重/入库 → `NotifyHost(WM_APP_CLIP_CHANGED)`。**保留** clipboard-win 直写 CF_DIBV5 与打开重试封装(第一代 D6 结论) |
| clip.rs `emit("clip:added"/"clip:updated")` | tauri Emitter | 改为 `NotifyHost`:PostMessage 到通知 HWND,携带事件 id;C++ 收到后自行调 `nb_call` 拉数据(拉模式,避免 FFI 回调线程问题) |
| settings.rs 读写 | tauri-plugin-store | serde 直读写同一 JSON 文件(app_config.json);第一代热键迁移等逻辑保留 |
| lib.rs `AppState` | tauri Manager | 变为 `Core` 结构:`Core::new(CoreConfig{ data_dir, notify_hwnd, .. })`,全部方法不带窗口概念 |
| pin.rs / capture/session.rs | 整个文件操作 Tauri 窗口 | **不迁移**。窗口逻辑移交 C++(遮罩/贴图/编辑器);core 只提供数据与像素:`nb_capture_monitor`(RGBA)、`nb_capture_crop`、`nb_pin_list`(贴图源数据查询) |
| tray.rs / hotkey.rs | Tauri 托盘/热键插件 | **不迁移**。托盘与 RegisterHotKey 在 C++ 宿主实现;core 暴露 `nb_set_hotkey_actions` 不需要——热键动作经 `nb_call("action", {name})` 触发即可 |
| lib.rs `clipimg`/`captureimg` 协议 | 自定义 URI scheme | **不迁移**。图片经路径直读(C++ QImage 从 blobs/ 加载,缩略图走既有缓存文件);无前端协议层 |

### 2.3 放弃(被本项目决策取代)

- 多窗口 emit/listen 事件模型 → 拉模式 + PostMessage;
- WebView2 加速键禁用钩子(lib.rs `disable_browser_accelerator_keys`)→ 无 WebView,整体消失;
- 贴图窗口管理(PinState)→ C++。

## 3. Cargo 工程与关键配置

```toml
# core/nbcore/Cargo.toml 要点
[package]
name = "nbcore"
edition = "2021"

[lib]
name = "nbcore"
crate-type = ["cdylib", "rlib"]   # rlib 供 nbcore-tests 与未来复用

[dependencies]
# 与第一代同版本,直接抄 Cargo.lock 的已验证版本组合:
rusqlite(bundled) / blake3 / uuid(v7) / image(png) / clipboard-win
/ chrono / serde / serde_json / thiserror / tracing(+tracing-subscriber 落盘)
ureq(socks-proxy, native-tls)
windows(按第一代 feature 裁剪:Media_Ocr / Storage_Streams / Win32_System_Com
        / Win32_System_WinRT / Win32_UI_WindowsAndMessaging(消息窗口))

[profile.release]
# ⚠️ 与第一代唯一的 profile 差异:panic=abort 会杀死宿主进程,必须 unwind
panic = "unwind"
lto = true
strip = true
```

**windows crate feature 变更**:新增 `Win32_UI_WindowsAndMessaging`、
`Win32_System_LibraryLoader`(核心自建消息窗口用);**保留**
`Win32_Networking_WinHttp`(translate/http.rs 零改动,必须继续可用);
Media_Ocr / Storage_Streams / Win32_System_Com / Win32_System_WinRT 沿用第一代。

## 4. FFI 门面设计

### 4.1 导出函数全集

```rust
#[no_mangle] pub extern "C" fn nb_version() -> u32;                 // 语义版本编码
#[no_mangle] pub extern "C" fn nb_init(config_json: *const c_char) -> i32;
//   config: { "data_dir": "...", "notify_hwnd": 12345, "log_level": "info" }
//   负责:开库+迁移、加载设置、启动剪贴板监听线程、初始化 tracing
#[no_mangle] pub extern "C" fn nb_call(method: *const c_char, req_json: *const c_char,
                                       out_resp: *mut *mut c_char) -> i32;
//   返回 0=成功,负数=错误类别(与 AppError::to_json() 配合);
//   out_resp 为 UTF-8 JSON(响应体含 { ok, data|error }),调用方负责 nb_free
#[no_mangle] pub extern "C" fn nb_capture_monitor(index: u32, out_rgba: *mut *mut u8,
                                       out_w: *mut u32, out_h: *mut u32) -> i32;
//   截图专用二进制通道;返回的 buffer 用 nb_free 释放
#[no_mangle] pub extern "C" fn nb_free(ptr: *mut c_void);
#[no_mangle] pub extern "C" fn nb_shutdown();   // 停监听线程、落盘、释放
```

### 4.2 nb_call 方法表(第一代 38 个 command 的去向)

| 保留(原样/微调) | 移交 C++(不再存在) |
| --- | --- |
| list_entries, entry_counts, get_entry, copy_entry, delete_entry, clear_history, set_pinned | overlay_init / overlay_ready / confirm_region / cancel_capture(遮罩会话状态机 → C++) |
| get_settings, save_settings | editor_init / edit_entry / commit_capture(编辑器 → C++;commit 经 nb_call 入库) |
| create_pin 拆为数据部分:pin_sources(返回可贴图条目+图片路径) | create_pin / set_pin_zoom / set_pin_opacity / fit_pin(窗口操作 → C++) |
| ocr_entry, ocr_probe | open_translate_window / translate_init(窗口 → C++;翻译调用走 translate_text) |
| translate_text, translate_history, clear_translate_history, translate_probe, proxy_probe | suspend_hotkeys / resume_hotkeys(热键归 C++ 托管) |
| copy_text_to_clipboard, read_clipboard_text | save_entry_image / save_text_file / reveal_entry(C++ 直接走 Qt 文件对话框 / explorer) |
| capture_begin(新), capture_crop(新), action(新:热键动作统一入口) | save_text_file 亦可留在 core(二选一,M3 定) |

`req_json`/`resp_json` 的字段名沿用第一代 `src/api/types.ts`(camelCase),
`list_entries` 等核心接口的第一代前端调用代码可平移到 Sciter 主面板。

### 4.3 通知协议

| 消息 | 载荷 | C++ 收到后 |
| --- | --- | --- |
| WM_APP+1(CLIP_CHANGED) | wParam=entry id 精确/0 全量 | nb_call(list_entries) 刷新主面板 |
| WM_APP+2(SETTINGS_CHANGED) | 0 | 重读设置,应用热键/行为变更 |
| WM_APP+3(CAPTURE_READY) | wParam=monitor index | 创建/更新遮罩窗口(会话流见架构 §4.1) |

## 5. 测试策略

1. **单测随迁**:31 个第一代单测平移后必须全绿(M1 验收线);
2. **FFI 层集成测试**(nbcore-tests,_release 与 unwind 配置下跑_):
   - 全方法表冒烟(nb_call 逐个调用,JSON schema 校验);
   - panic 隔离:构造非法输入使内部 panic,断言返回错误码而非进程崩溃;
   - 内存:nb_call ×10⁵ 无泄漏(手写计数或简单分配器统计);
   - 真机行为(手动,沿用第一代清单):剪贴板打开竞争重试、CF_DIBV5 写入、
     中文 FTS+LIKE 搜索、FTS TEXT 主键、监听事件延迟。
3. **宿主联调验收**(M3):C++ 冒烟程序加载 DLL 完成 list→copy→translate→ocr 链路。

## 6. 实施顺序(M1→M3 里程碑分解)

| 步骤 | 内容 | 验收 |
| --- | --- | --- |
| M1.1 | 建 core 工作区,平移 store 全家 + error,迁移 SQL v1–v4 | 单测(store 部分)绿 |
| M1.2 | 平移 translate 全家 + ocr + settings(serde 化)+ screen | 单测全绿,tauri 依赖归零 |
| M1.3 | 平移 clip 入库管线;重写监听为事件驱动 + NotifyHost | nbcore-tests 真机监听项过 |
| M2.1 | ffi 模块:nb_init/nb_call/nb_free + panic 边界 + 方法表 | panic 隔离测试过 |
| M2.2 | RGBA 通道(capture_begin/capture_crop)+ nb_shutdown | 集成测试全绿 |
| M2.3 | cbindgen 产出 nbcore.h;新增 v5 迁移(source/tags) | 头文件评审通过 |
| M3 | C++ 冒烟宿主(非 Qt,Win32 控制台即可)联调 | 链路验收清单过 |

## 7. 与 C++ 侧的接口冻结

M2.3 产出的 `nbcore.h` + 本文档 §4 方法表即 **v1 接口冻结基线**;此后变更走版本号
递增(nb_call 增加 `nb_abi_version()` 校验),宿主加载时核对不匹配即拒绝启动并提示升级。
