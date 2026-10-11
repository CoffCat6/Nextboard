//! FFI 门面:nbcore.dll 的全部导出。
//!
//! 设计(docs/02-migration-core.md §4):
//! - JSON 门面:nb_call(method, req_json) → resp_json,方法名沿用第一代 command 契约;
//! - RGBA 通道:截图像素走 nb_capture_*,避免 base64 进 JSON;
//! - panic 边界:全部导出包 catch_unwind,panic 转错误响应,绝不展开进宿主。
//!
//! 内存契约:
//! - nb_call 的 out_resp 由本 crate 分配,宿主用 nb_free_string 释放;
//! - nb_capture_* 的 out_rgba 由本 crate 分配,宿主用 nb_free_buffer 释放;
//! - 入参 C 字符串归宿主所有,本 crate 只读。

use std::ffi::{c_char, CStr, CString};
use std::panic::AssertUnwindSafe;
use std::sync::atomic::{AtomicBool, Ordering};

use base64::Engine as _;

use crate::core::{notify_host, CoreConfig, WM_APP_CLIP_CHANGED};
use crate::error::{AppError, AppResult};
use crate::store::entry::{self, EntryKind};
use crate::store::todo;

/// 成功码;NB_ERR 为通用失败(详情见响应 JSON 的 error 字段)。
const NB_OK: i32 = 0;
const NB_ERR: i32 = -1;

/// FFI 层已初始化标志(nb_init 只允许一次,nb_call 前必须已初始化)。
static INITIALIZED: AtomicBool = AtomicBool::new(false);

// ---------- 基础导出 ----------

#[no_mangle]
pub extern "C" fn nb_version() -> u32 {
    // 语义版本编码:major*10000 + minor*100 + patch
    0 * 10_000 + 1 * 100 + 0
}

/// ABI 版本:宿主加载时核对,不匹配拒绝启动(02 §7 接口冻结)。
#[no_mangle]
pub extern "C" fn nb_abi_version() -> u32 {
    1
}

/// 初始化核心。
///
/// config_json: `{ "data_dir": "...", "notify_hwnd": 12345 }`
/// 完成后启动剪贴板监听线程与缩略图回填线程。
///
/// # Safety
/// config_json 必须是合法的以 NUL 结尾的 UTF-8 C 字符串。
#[no_mangle]
pub unsafe extern "C" fn nb_init(config_json: *const c_char) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if INITIALIZED.swap(true, Ordering::SeqCst) {
            return Err(AppError::Internal("nbcore 已初始化".into()));
        }
        let text = cstr_to_string(config_json)?;
        let cfg: serde_json::Value = serde_json::from_str(&text)
            .map_err(|err| AppError::InvalidInput(format!("nb_init 配置非法: {err}")))?;
        let data_dir = cfg
            .get("data_dir")
            .and_then(|v| v.as_str())
            .ok_or_else(|| AppError::InvalidInput("nb_init 缺少 data_dir".into()))?;
        let data_dir = std::path::PathBuf::from(data_dir);

        // 日志最先初始化(监听线程/入库失败的排障依据)
        let _ = std::fs::create_dir_all(&data_dir);
        crate::init_logging(&data_dir);
        log::info!("nb_init: data_dir={}", data_dir.display());

        #[cfg(windows)]
        let notify_hwnd =
            (cfg.get("notify_hwnd").and_then(|v| v.as_u64()).unwrap_or(0)) as usize;
        #[cfg(not(windows))]
        let notify_hwnd = 0u64;

        crate::core::Core::init(CoreConfig {
            data_dir,
            notify_hwnd,
        })?;
        crate::clip::start_monitor();
        // 缩略图回填需要 'static Core;init 成功后 Core::get 恒可用
        if let Ok(core) = crate::core::Core::get() {
            crate::clip::spawn_thumbnail_backfill(core);
            // 待办调度线程:轮询 remind/notify 到点(docs/05-todo.md §4.1)
            crate::todo::spawn_scheduler(core);
        }
        log::info!("nb_init complete");
        Ok(())
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_init panic".into())));
    report(result)
}

/// 统一入口:执行一个方法并返回 JSON 响应。
///
/// - method:见 `dispatch` 的方法表(与第一代 command 名一致);
/// - req_json:请求参数;可为 null(无参方法);
/// - out_resp:接收 `{ "ok": true, "data": … }` 或 `{ "ok": false, "error": {code,message} }`。
///
/// 返回 NB_OK 或 NB_ERR(错误详情在 out_resp 里)。
///
/// # Safety
/// method/req_json 必须是合法 C 字符串;out_resp 必须指向可写的指针槽位,
/// 调用方随后用 nb_free_string 释放 *out_resp。
#[no_mangle]
pub unsafe extern "C" fn nb_call(
    method: *const c_char,
    req_json: *const c_char,
    out_resp: *mut *mut c_char,
) -> i32 {
    let panicked = AtomicBool::new(false);
    let result: Result<serde_json::Value, AppError> =
        std::panic::catch_unwind(AssertUnwindSafe(|| {
            if !INITIALIZED.load(Ordering::SeqCst) {
                return Err(AppError::Internal("nbcore 未初始化(先调 nb_init)".into()));
            }
            let method = cstr_to_string(method)?;
            let req = if req_json.is_null() {
                serde_json::Value::Null
            } else {
                let text = cstr_to_string(req_json)?;
                serde_json::from_str(&text)
                    .map_err(|err| AppError::InvalidInput(format!("请求 JSON 非法: {err}")))?
            };
            dispatch(&method, &req)
        }))
        .unwrap_or_else(|_| {
            panicked.store(true, Ordering::SeqCst);
            Err(AppError::Panicked("nb_call 执行异常(panic 已拦截)".into()))
        });

    let body = match &result {
        Ok(value) => serde_json::json!({ "ok": true, "data": value }),
        Err(err) => serde_json::json!({ "ok": false, "error": err.to_json() }),
    };
    let code = if result.is_ok() && !panicked.load(Ordering::SeqCst) {
        NB_OK
    } else {
        NB_ERR
    };
    match CString::new(body.to_string()) {
        Ok(c) => {
            if !out_resp.is_null() {
                // SAFETY: out_resp 指向宿主提供的合法指针槽位(契约见上)
                unsafe { *out_resp = c.into_raw() };
            }
            code
        }
        Err(_) => NB_ERR,
    }
}

/// 释放 nb_call 返回的字符串缓冲。
///
/// # Safety
/// ptr 必须来自 nbcore 的 nb_call 出参,且只能释放一次。
#[no_mangle]
pub unsafe extern "C" fn nb_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        // SAFETY: ptr 由 CString::into_raw 产生,配套重建并释放
        drop(unsafe { CString::from_raw(ptr) });
    }
}

/// 释放 nb_capture_* 返回的像素缓冲。
///
/// # Safety
/// ptr 必须来自 nbcore 的 nb_capture_* 出参,且只能释放一次。
#[no_mangle]
pub unsafe extern "C" fn nb_free_buffer(ptr: *mut u8, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    if let Ok(layout) = std::alloc::Layout::from_size_align(len, 1) {
        // SAFETY: ptr 由 write_rgba_out 按同一 layout 分配
        unsafe { std::alloc::dealloc(ptr, layout) };
    }
}

/// 运行期更新通知 HWND(主面板窗口创建后调用;0 = 通知丢弃)。
///
/// # Safety
/// hwnd 须为有效窗口句柄或 0。
#[no_mangle]
pub unsafe extern "C" fn nb_set_notify_hwnd(hwnd: u64) {
    crate::core::set_notify_hwnd(hwnd as usize);
}

/// 关闭核心:停监听、落盘设置。进程退出前调用;此后 nb_call 返回"未初始化"。
///
/// # Safety
/// 只能在宿主确认没有任何 nb_call 在执行时调用(单线程退出路径)。
#[no_mangle]
pub unsafe extern "C" fn nb_shutdown() {
    let _ = std::panic::catch_unwind(|| {
        INITIALIZED.store(false, Ordering::SeqCst);
        crate::core::Core::shutdown();
    });
}

// ---------- RGBA 通道(截图专用,绕开 JSON) ----------

/// 捕获指定序号显示器的整屏 RGBA。
///
/// # Safety
/// out_rgba/out_w/out_h 必须指向合法可写槽位;成功后宿主负责
/// nb_free_buffer(*out_rgba, (*out_w * *out_h * 4) as usize)。
#[no_mangle]
pub unsafe extern "C" fn nb_capture_monitor(
    index: u32,
    out_rgba: *mut *mut u8,
    out_w: *mut u32,
    out_h: *mut u32,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let image = crate::capture::capture_monitor(index)?;
        write_rgba_out(&image, out_rgba, out_w, out_h)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_capture_monitor panic".into())));
    report(result)
}

/// 裁剪整屏 RGBA 的子区域(框选确认)。
///
/// # Safety
/// 同 nb_capture_monitor;src 由宿主持有,本函数只读。
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn nb_capture_crop(
    src: *const u8,
    src_len: usize,
    screen_w: u32,
    screen_h: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    out_rgba: *mut *mut u8,
    out_w: *mut u32,
    out_h: *mut u32,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if src.is_null() || src_len == 0 {
            return Err(AppError::InvalidInput("裁剪源为空".into()));
        }
        let bytes = std::slice::from_raw_parts(src, src_len);
        let image = crate::capture::crop_rgba(bytes, screen_w, screen_h, x, y, width, height)?;
        write_rgba_out(&image, out_rgba, out_w, out_h)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_capture_crop panic".into())));
    report(result)
}

// ---------- 内部工具 ----------

fn cstr_to_string(ptr: *const c_char) -> Result<String, AppError> {
    if ptr.is_null() {
        return Err(AppError::InvalidInput("C 字符串为空".into()));
    }
    // SAFETY: 契约要求调用方传入合法 NUL 结尾 UTF-8 串
    let cstr = unsafe { CStr::from_ptr(ptr) };
    Ok(cstr.to_string_lossy().into_owned())
}

fn report(result: Result<(), AppError>) -> i32 {
    match result {
        Ok(()) => NB_OK,
        Err(err) => {
            log::warn!("FFI 调用失败: {err}");
            NB_ERR
        }
    }
}

/// RGBA 图像写入宿主出参(分配缓冲拷贝,宿主 nb_free_buffer 释放)。
///
/// # Safety
/// 出参指针契约同各导出函数。
unsafe fn write_rgba_out(
    image: &crate::capture::CapturedImage,
    out_rgba: *mut *mut u8,
    out_w: *mut u32,
    out_h: *mut u32,
) -> Result<(), AppError> {
    if out_rgba.is_null() || out_w.is_null() || out_h.is_null() {
        return Err(AppError::InvalidInput("出参指针为空".into()));
    }
    let layout = std::alloc::Layout::from_size_align(image.rgba.len(), 1)
        .map_err(|_| AppError::Internal("RGBA 分配布局非法".into()))?;
    // SAFETY: layout 大小 >0(图像非空)
    let ptr = unsafe { std::alloc::alloc(layout) };
    if ptr.is_null() {
        return Err(AppError::Internal("RGBA 缓冲分配失败".into()));
    }
    // SAFETY: ptr 刚按 layout 分配,长度与 rgba 一致
    unsafe {
        std::ptr::copy_nonoverlapping(image.rgba.as_ptr(), ptr, image.rgba.len());
        *out_rgba = ptr;
        *out_w = image.width;
        *out_h = image.height;
    }
    Ok(())
}

fn dispatch(method: &str, req: &serde_json::Value) -> Result<serde_json::Value, AppError> {
    match method {
        // ---- 条目查询/操作 ----
        "list_entries" => {
            let core = crate::core::Core::get()?;
            let query = str_opt(req, "query");
            let kind = match str_opt(req, "kind") {
                Some(k) => Some(EntryKind::parse(&k)?),
                None => None,
            };
            let pinned = req.get("pinned").and_then(|v| v.as_bool());
            let tag = str_opt(req, "tag");   // S5.2 标签筛选
            let limit = req.get("limit").and_then(|v| v.as_i64()).unwrap_or(50);
            let offset = req.get("offset").and_then(|v| v.as_i64()).unwrap_or(0);
            let db = core.db.lock()?;
            let (items, total) = entry::list(
                &db,
                query.as_deref(),
                kind,
                pinned,
                tag.as_deref(),
                limit,
                offset,
            )?;
            Ok(serde_json::json!({ "items": items, "total": total }))
        }
        // 全部标签及条目数(S5.2 标签筛选条)
        "list_tags" => {
            let core = crate::core::Core::get()?;
            let db = core.db.lock()?;
            Ok(serde_json::to_value(entry::list_tags(&db)?)?)
        }
        // 覆盖设置条目标签(S5.2);tags 为字符串数组
        "set_entry_tags" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let tags: Vec<String> = req
                .get("tags")
                .and_then(|v| v.as_array())
                .map(|arr| {
                    arr.iter()
                        .filter_map(|v| v.as_str().map(str::to_owned))
                        .collect()
                })
                .ok_or_else(|| AppError::InvalidInput("缺少 tags 数组".into()))?;
            let db = core.db.lock()?;
            entry::set_tags(&db, &id, &tags)?;
            Ok(serde_json::Value::Null)
        }
        "entry_counts" => {
            let core = crate::core::Core::get()?;
            let db = core.db.lock()?;
            Ok(serde_json::to_value(entry::counts(&db)?)?)
        }
        "get_entry" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let db = core.db.lock()?;
            Ok(serde_json::to_value(
                entry::get(&db, &id)?.ok_or_else(|| AppError::InvalidInput("条目不存在".into()))?,
            )?)
        }
        // 一键贴图(pinLatest)取数:口径由设置 pinPreferImage 决定——
        // 默认"最近复制优先"(文本/图片按最近使用时间竞争);开启后"图片优先、无图回退文本"。
        // 空库返回 null。
        "latest_pinnable" => {
            let core = crate::core::Core::get()?;
            let prefer_image = core.settings()?.pin_prefer_image;
            let db = core.db.lock()?;
            Ok(match entry::latest_pinnable(&db, prefer_image)? {
                Some(found) => serde_json::to_value(found)?,
                None => serde_json::Value::Null,
            })
        }
        "delete_entry" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let db = core.db.lock()?;
            // 待办的扩展行随条目一起删(非 todo 条目无 meta,静默通过),
            // 否则 todo_meta 会留下孤儿行、counts 的 CASE 也会算错。
            todo::delete_meta(&db, &id)?;
            let blob = entry::delete(&db, &id)?;
            if let Some(path) = blob {
                crate::clip::remove_blob(&crate::clip::resolve_blob_path(&core.blobs_dir, &path));
            }
            Ok(serde_json::Value::Null)
        }
        "clear_history" => {
            let core = crate::core::Core::get()?;
            let db = core.db.lock()?;
            for path in entry::clear(&db)? {
                crate::clip::remove_blob(&crate::clip::resolve_blob_path(&core.blobs_dir, &path));
            }
            Ok(serde_json::Value::Null)
        }
        "set_pinned" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let pinned = req
                .get("pinned")
                .and_then(|v| v.as_bool())
                .ok_or_else(|| AppError::InvalidInput("缺少 pinned".into()))?;
            let db = core.db.lock()?;
            entry::set_pinned(&db, &id, pinned)?;
            Ok(serde_json::Value::Null)
        }

        // ---- 剪贴板读写 ----
        "copy_text_to_clipboard" => {
            crate::clip::write_text_to_clipboard(&str_req(req, "text")?)?;
            Ok(serde_json::Value::Null)
        }
        "read_clipboard_text" => Ok(serde_json::json!(crate::clip::read_text_from_clipboard()?)),
        // 直接把一张 PNG 写进系统剪贴板(不经过条目)。
        // 用途:截图超过大小上限、未入库时,仍要让用户的"截一下"落到剪贴板 ——
        // 第一代对超限截图的处理就是"未入库,仅写剪贴板",否则表现为"按了没反应"。
        "copy_image_png" => {
            let png_b64 = str_req(req, "png_base64")?;
            let png = base64::engine::general_purpose::STANDARD
                .decode(png_b64.as_bytes())
                .map_err(|err| AppError::InvalidInput(format!("PNG base64 非法: {err}")))?;
            crate::clip::write_png_to_clipboard(&png)?;
            Ok(serde_json::Value::Null)
        }
        "copy_entry" => {
            // 把条目内容写回系统剪贴板(第一代 commands.rs 的 copy_entry 语义):
            // 文本直写;图片读 blob → 解码 RGBA → CF_DIBV5 直写;文件重写 CF_HDROP。
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let kind_blob = {
                let db = core.db.lock()?;
                let entry = entry::get(&db, &id)?
                    .ok_or_else(|| AppError::InvalidInput("条目不存在".into()))?;
                (entry.kind, entry.text_content, entry.blob_path, entry.file_paths)
            };
            match kind_blob {
                (EntryKind::Text, Some(text), _, _) => {
                    crate::clip::write_text_to_clipboard(&text)?;
                }
                (EntryKind::Image, _, Some(blob), _) => {
                    let path = crate::clip::resolve_blob_path(&core.blobs_dir, &blob);
                    let bytes = std::fs::read(path).map_err(AppError::Io)?;
                    crate::clip::write_png_to_clipboard(&bytes)?;
                }
                (EntryKind::File, _, _, Some(paths)) => {
                    crate::clip::write_files_to_clipboard(&paths)?;
                }
                _ => return Err(AppError::InvalidInput("条目内容为空".into())),
            }
            Ok(serde_json::Value::Null)
        }

        // ---- 设置 ----
        "get_settings" => {
            let core = crate::core::Core::get()?;
            Ok(serde_json::to_value(core.settings()?)?)
        }
        // 当前生效的数据目录(默认目录或设置的自定义目录)。宿主启动时取一次,
        // 用于定位 blobs 目录与面板的 file:// 图片 URL。
        "data_dir" => {
            let core = crate::core::Core::get()?;
            Ok(serde_json::json!({ "path": &core.effective_dir }))
        }
        "save_settings" => {
            let core = crate::core::Core::get()?;
            let settings: crate::settings::Settings = serde_json::from_value(req.clone())
                .map_err(|err| AppError::InvalidInput(format!("设置字段非法: {err}")))?;
            settings.validate()?;
            // 自定义数据目录变更:写待迁移标记,下次启动搬迁 db + blobs 到新目录
            // (settings.rs::take_pending_move;源目录保留,删除旧数据由用户手动做)。
            let current = core.settings()?;
            if settings.data_dir.trim() != current.data_dir.trim() {
                let source = crate::settings::effective_data_dir(&current, &core.default_dir);
                crate::settings::write_pending_move(&core.default_dir, &source)?;
            }
            core.set_settings(settings)?;
            Ok(serde_json::Value::Null)
        }

        // ---- 翻译 ----
        "translate_text" => {
            let core = crate::core::Core::get()?;
            let text = str_req(req, "text")?;
            let result = crate::translate::translate_and_record(
                &core.settings()?,
                &text,
                str_opt(req, "from").as_deref(),
                str_opt(req, "to").as_deref(),
            )?;
            Ok(serde_json::to_value(result)?)
        }
        "translate_history" => Ok(serde_json::to_value(crate::translate::history())?),
        "clear_translate_history" => {
            crate::translate::clear_history();
            Ok(serde_json::Value::Null)
        }
        "translate_probe" => {
            let core = crate::core::Core::get()?;
            let provider = str_req(req, "provider")?;
            let probe = crate::translate::probe(&core.settings()?, &provider);
            Ok(serde_json::to_value(probe)?)
        }
        "proxy_probe" => {
            let core = crate::core::Core::get()?;
            let probe = crate::translate::proxy_probe(&core.settings()?);
            Ok(serde_json::to_value(probe)?)
        }

        // ---- OCR ----
        "ocr_probe" => {
            let probe = crate::ocr::probe(crate::ocr::Backend::System);
            Ok(serde_json::to_value(probe)?)
        }
        "ocr_entry" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let path = {
                let db = core.db.lock()?;
                let entry = entry::get(&db, &id)?
                    .ok_or_else(|| AppError::InvalidInput("条目不存在".into()))?;
                entry
                    .blob_path
                    .ok_or_else(|| AppError::InvalidInput("非图片条目".into()))?
            };
            let full = crate::clip::resolve_blob_path(&core.blobs_dir, &path);
            let bytes = std::fs::read(full).map_err(AppError::Io)?;
            let img = image::load_from_memory(&bytes)
                .map_err(AppError::Image)?
                .to_rgba8();
            let (width, height) = img.dimensions();
            let settings = core.settings()?;
            let text = crate::ocr::recognize_configured(
                img.into_raw(),
                width,
                height,
                &crate::ocr::OcrOptions::from_settings(&settings),
            )?;
            Ok(serde_json::json!({ "text": text }))
        }

        // ---- 截图会话(编辑器产物等已合成 PNG 的入库路径) ----
        "ingest_capture_png" => {
            let core = crate::core::Core::get()?;
            let png_b64 = str_req(req, "png_base64")?;
            let source = str_opt(req, "source").unwrap_or_else(|| "capture".to_owned());
            let png = base64::engine::general_purpose::STANDARD
                .decode(png_b64.as_bytes())
                .map_err(|err| AppError::InvalidInput(format!("PNG base64 非法: {err}")))?;
            let img = image::load_from_memory(&png)
                .map_err(AppError::Image)?
                .to_rgba8();
            let (width, height) = img.dimensions();
            let stored = crate::clip::ingest_image(core, img.as_raw(), width, height, &source)?;
            notify_host(WM_APP_CLIP_CHANGED);
            Ok(serde_json::json!({ "entry": stored }))
        }

        // 文本「存为条目」(S5.1 统一素材库):翻译原文/译文等应用内文本入库。
        // 与剪贴板共用 hash 去重;source 记录来源(translate 等),入库后可被搜索。
        "save_text_entry" => {
            let core = crate::core::Core::get()?;
            let text = str_req(req, "text")?;
            let source = str_opt(req, "source").unwrap_or_else(|| "clipboard".to_owned());
            let stored = crate::clip::ingest_text(core, &text, &source)?
                .ok_or_else(|| AppError::InvalidInput("文本超过大小上限,未入库".into()))?;
            notify_host(WM_APP_CLIP_CHANGED);
            Ok(serde_json::json!({ "entry": stored }))
        }

        // ---- 待办(docs/05-todo.md §5) ----
        // 业务语义见 store::todo;本层只做参数解包与响应装配。
        "todo_create" => {
            let core = crate::core::Core::get()?;
            let text = str_req(req, "text")?;
            let due_at = ms_opt(req, "dueAt")?;
            let remind_at = ms_opt(req, "remindAt")?;
            let notify_at = ms_opt(req, "notifyAt")?;
            let tags = tags_opt(req)?;
            let id = {
                let mut db = core.db.lock()?;
                todo::create(&mut db, &text, due_at, remind_at, notify_at, tags, crate::now_ms())?
            };
            notify_host(WM_APP_CLIP_CHANGED); // 列表新增条目,复用既有刷新信号
            Ok(serde_json::json!({ "id": id }))
        }
        "todo_update" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let text = str_opt(req, "text");
            let due_at = ms_opt_opt(req, "dueAt")?;
            let remind_at = ms_opt_opt(req, "remindAt")?;
            let notify_at = ms_opt_opt(req, "notifyAt")?;
            let tags = tags_opt(req)?;
            {
                let mut db = core.db.lock()?;
                todo::update(&mut db, &id, text.as_deref(), due_at, remind_at, notify_at, tags)?;
            }
            notify_host(WM_APP_CLIP_CHANGED);
            Ok(serde_json::Value::Null)
        }
        "todo_list" => {
            let core = crate::core::Core::get()?;
            let filter = todo::TodoFilter::parse(str_opt(req, "done").as_deref())?;
            // 与 list_entries 同语义的叠加过滤:标签筛选/搜索切到待办页不能失效,
            // 否则同一筛选条件在两个视图给出不一致的结果集。
            let query = str_opt(req, "query");
            let tag = str_opt(req, "tag");
            let limit = req.get("limit").and_then(|v| v.as_i64()).unwrap_or(50);
            let offset = req.get("offset").and_then(|v| v.as_i64()).unwrap_or(0);
            let db = core.db.lock()?;
            let (items, total) =
                todo::list(&db, filter, query.as_deref(), tag.as_deref(), limit, offset)?;
            Ok(serde_json::json!({ "items": items, "total": total }))
        }
        "todo_get" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let db = core.db.lock()?;
            Ok(match todo::get(&db, &id)? {
                Some(item) => serde_json::to_value(item)?,
                None => serde_json::Value::Null,
            })
        }
        "todo_set_done" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            let done = req
                .get("done")
                .and_then(|v| v.as_bool())
                .ok_or_else(|| AppError::InvalidInput("缺少 done".into()))?;
            {
                let db = core.db.lock()?;
                todo::set_done(&db, &id, done)?;
            }
            notify_host(WM_APP_CLIP_CHANGED);
            Ok(serde_json::Value::Null)
        }
        "todo_delete" => {
            let core = crate::core::Core::get()?;
            let id = str_req(req, "id")?;
            {
                let db = core.db.lock()?;
                todo::delete(&db, &id)?;
            }
            notify_host(WM_APP_CLIP_CHANGED);
            Ok(serde_json::Value::Null)
        }
        // 待办计数并入 entry_counts(EntryCounts 已含 todoOpen/todoDone);
        // 保留独立方法名以便宿主/脚本直接取。
        "todo_counts" => {
            let core = crate::core::Core::get()?;
            let db = core.db.lock()?;
            Ok(serde_json::to_value(entry::counts(&db)?)?)
        }
        // 调度到点后宿主拉取待播报列表(§4.2:WM_APP 只当信号,payload 宿主拉取)
        "todo_due_pending" => {
            let core = crate::core::Core::get()?;
            let db = core.db.lock()?;
            Ok(serde_json::to_value(todo::pending_bubbles(&db, 8)?)?)
        }
        // 气泡播报确认(幂等)
        "todo_due_mark_shown" => {
            let core = crate::core::Core::get()?;
            let ids: Vec<String> = req
                .get("ids")
                .and_then(|v| v.as_array())
                .map(|arr| {
                    arr.iter()
                        .filter_map(|v| v.as_str().map(str::to_owned))
                        .collect()
                })
                .ok_or_else(|| AppError::InvalidInput("缺少 ids 数组".into()))?;
            let db = core.db.lock()?;
            todo::mark_bubble_shown(&db, &ids)?;
            Ok(serde_json::Value::Null)
        }
        // 「清空已完成」(归档清理;与 clear_history 分离 —— 见 store::todo::clear_done)
        "todo_clear_done" => {
            let core = crate::core::Core::get()?;
            {
                let db = core.db.lock()?;
                todo::clear_done(&db)?;
            }
            notify_host(WM_APP_CLIP_CHANGED);
            Ok(serde_json::Value::Null)
        }

        _ => Err(AppError::InvalidInput(format!("未知方法: {method}"))),
    }
}

fn str_opt(req: &serde_json::Value, key: &str) -> Option<String> {
    req.get(key)
        .and_then(|v| v.as_str())
        .map(str::to_owned)
        .filter(|s| !s.is_empty())
}

fn str_req(req: &serde_json::Value, key: &str) -> Result<String, AppError> {
    str_opt(req, key).ok_or_else(|| AppError::InvalidInput(format!("缺少参数 {key}")))
}

/// 可空毫秒时间戳(存在但为 null → None;缺失 → None;非整数 → InvalidInput)。
/// 用于 create 这类"没有就是没有"的场景(update 的三态语义见 [`ms_opt_opt`])。
fn ms_opt(req: &serde_json::Value, key: &str) -> AppResult<Option<i64>> {
    match req.get(key) {
        None | Some(serde_json::Value::Null) => Ok(None),
        Some(v) => v
            .as_i64()
            .map(Some)
            .ok_or_else(|| AppError::InvalidInput(format!("{key} 应为毫秒时间戳整数"))),
    }
}

/// 三态时间参数(update 用):键缺失 = None(不改);null = Some(None)(清空);
/// 整数 = Some(Some(ms))(设置)。设计文档 §5 的 todo_update 语义。
fn ms_opt_opt(req: &serde_json::Value, key: &str) -> AppResult<Option<Option<i64>>> {
    match req.get(key) {
        None => Ok(None),
        Some(serde_json::Value::Null) => Ok(Some(None)),
        Some(v) => v
            .as_i64()
            .map(|ms| Some(Some(ms)))
            .ok_or_else(|| AppError::InvalidInput(format!("{key} 应为毫秒时间戳整数或 null"))),
    }
}

/// 可空标签数组(缺失 → None;与 str_opt 不同,**空数组是合法值 = 清空标签**)。
fn tags_opt(req: &serde_json::Value) -> AppResult<Option<Vec<String>>> {
    match req.get("tags") {
        None => Ok(None),
        Some(serde_json::Value::Null) => Ok(None),
        Some(v) => Ok(Some(
            v.as_array()
                .ok_or_else(|| AppError::InvalidInput("tags 应为字符串数组".into()))?
                .iter()
                .filter_map(|x| x.as_str().map(str::to_owned))
                .collect(),
        )),
    }
}
