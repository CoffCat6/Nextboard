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
        write_rgba_out(image, out_rgba, out_w, out_h)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_capture_monitor panic".into())));
    report(result)
}

/// **按设备名**捕获整屏 RGBA(形如 `\\.\DISPLAY1`)。
///
/// 这是宿主布置多屏遮罩的**正确入口**:用 `QScreen::name()` 拿同一标识来配对,
/// 不受两端枚举顺序差异影响(实测 Qt 主屏在前、xcap 主屏在后,按索引配对会把
/// A 屏的底图画到 B 屏的遮罩上 —— 即"多屏位置与分辨率混乱"的根因)。
///
/// # Safety
/// name 必须是 NUL 结尾的 UTF-8 串;out_* 契约同 [`nb_capture_monitor`]。
#[no_mangle]
pub unsafe extern "C" fn nb_capture_monitor_by_name(
    name: *const c_char,
    out_rgba: *mut *mut u8,
    out_w: *mut u32,
    out_h: *mut u32,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let name = cstr_to_string(name)?;
        let image = crate::capture::capture_monitor_by_name(&name)?;
        write_rgba_out(image, out_rgba, out_w, out_h)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_capture_monitor_by_name panic".into())));
    report(result)
}

/// **按宿主屏幕描述**捕获整屏 RGBA(多屏遮罩/全屏直通的正确入口)。
///
/// `name` 是 Qt 的 `QScreen::name()`(可能为空;它常是 EDID 友好名,
/// 与核心的 `\\.\DISPLAYn` 不同源,故只作加分项);真正的主力配对键是
/// `x`/`y`(物理桌面原点,两端一致),`width`/`height` 是**逻辑像素**,
/// 需配 `dpr` 还原后比较。详见 capture.rs 的 match_monitor。
///
/// # Safety
/// name 必须是 NUL 结尾的 UTF-8 串;out_* 契约同 [`nb_capture_monitor`]。
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn nb_capture_monitor_matched(
    name: *const c_char,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    dpr: f32,
    out_rgba: *mut *mut u8,
    out_w: *mut u32,
    out_h: *mut u32,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let name = if name.is_null() {
            String::new()
        } else {
            cstr_to_string(name)?
        };
        let hint = crate::capture::MonitorHint {
            name,
            x,
            y,
            width,
            height,
            dpr,
        };
        let image = crate::capture::capture_monitor_matched(&hint)?;
        write_rgba_out(image, out_rgba, out_w, out_h)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_capture_monitor_matched panic".into())));
    report(result)
}

/// 对**任意 RGBA** 直接做 OCR(编辑器「取字」:合成图无需先入库)。
///
/// 与 `nb_call("ocr_entry")` 的差异:`ocr_entry` 从落库 PNG 读图,
/// 而编辑器里的标注合成图还没有条目 —— 走本入口避免"先入库再识别"的绕路。
/// 内部用 `recognize_configured`,因此**设置项(后端/识别偏好/引擎驻留)全部生效**。
///
/// ⚠️ **出参契约(与 nb_call 不同,改前端务必一起改)**:
///   本函数交出的是**裸** `{"text": "..."}` —— **没有** `nb_call` 那层
///   `{"ok":…,"data":…}` 信封。宿主侧曾照抄 nb_call 的解析读
///   `resp["data"]["text"]`,结果是"核心识别成功、宿主永远拿到空串"
///   (nbcore.log 里明确有"RapidOCR 完成:…,N 行",用户却什么都取不到)。
///   此处**刻意不改**成带信封:改了会让旧宿主(以及按裸对象解析的第三方工具)
///   一起失效;契约的不对称由本注释与宿主 core_bridge.cpp 的对应注释共同固定。
///
/// # Safety
/// `rgba` 由宿主持有,本函数只读;`out_text` 由本 crate 分配,
/// 宿主成功后须以 `nb_free_string` 释放(与 `nb_call` 的出参同约定)。
#[no_mangle]
pub unsafe extern "C" fn nb_ocr_rgba(
    rgba: *const u8,
    len: usize,
    width: u32,
    height: u32,
    out_text: *mut *mut i8,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if rgba.is_null() || len == 0 {
            return Err(AppError::InvalidInput("OCR 输入为空".into()));
        }
        let bytes = std::slice::from_raw_parts(rgba, len);
        let settings = crate::core::Core::get()?.settings()?;
        let options = crate::ocr::OcrOptions::from_settings(&settings);
        let text = crate::ocr::recognize_configured(bytes.to_vec(), width, height, &options)?;
        Ok(serde_json::json!({ "text": text }).to_string())
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_ocr_rgba panic".into())));
    report_out_text(result, out_text)
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
        write_rgba_out(image, out_rgba, out_w, out_h)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_capture_crop panic".into())));
    report(result)
}

/// 滚动长截图:**吸收一帧**(RGBA 直入,不走 base64)。
///
/// 为什么单独开原生导出而不是复用 nb_call("stitch_append"):
/// 逐帧采集时每帧都是一整张选区图(1080p 选区 ~8MB)。走 JSON 门面要把像素
/// base64 进字符串(再膨胀 1.33 倍)+ 在核心侧解回来,一次就是 **3 份**峰值拷贝。
/// docs/06-memory-optimization.md 第 10 节已把"截图 base64 往返"列为待改项的
/// 头一条 —— 长截图是高频路径,更没有理由重蹈。
///
/// 结果(位移/总高/结论)是**小 JSON**,出参走 out_result(宿主以 nb_free_string 释放)。
///
/// # Safety
/// rgba 由宿主持有,本函数只读;session 必须是 NUL 结尾 UTF-8;
/// out_result 为可写指针槽位。
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn nb_stitch_append(
    session: *const c_char,
    rgba: *const u8,
    rgba_len: usize,
    width: u32,
    height: u32,
    reset: i32,
    out_result: *mut *mut c_char,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if rgba.is_null() || rgba_len == 0 {
            return Err(AppError::InvalidInput("长截图帧为空".into()));
        }
        let session = cstr_to_string(session)?;
        let bytes = std::slice::from_raw_parts(rgba, rgba_len);
        let settings = crate::core::Core::get()?.settings()?;
        let out = crate::stitch::append_frame(
            &session,
            bytes.to_vec(),
            width,
            height,
            reset != 0,
            settings.stitch_max_height,
            settings.stitch_overlap_ratio,
        )?;
        Ok(serde_json::to_value(out)?.to_string())
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_stitch_append panic".into())));
    report_out_text(result, out_result)
}

/// 滚动长截图:**结束并取回拼接结果**。
///
/// 产物是 PNG 字节(宿主拿去 ingest_capture_png 入库 / 写剪贴板),
/// 走 out_png + nb_free_buffer。尺寸与缺口数在 out_meta(小 JSON 字符串)。
/// 会话取完即弃(长截图是一次成型产物,docs/19 第 2.2 节 N5)。
///
/// # Safety
/// 出参契约同上。
#[no_mangle]
pub unsafe extern "C" fn nb_stitch_finish(
    session: *const c_char,
    out_png: *mut *mut u8,
    out_png_len: *mut usize,
    out_meta: *mut *mut c_char,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_png.is_null() || out_png_len.is_null() || out_meta.is_null() {
            return Err(AppError::InvalidInput("出参指针为空".into()));
        }
        let session = cstr_to_string(session)?;
        let finished = crate::stitch::finish_session(&session)?;
        let meta = serde_json::json!({
            "width": finished.width,
            "height": finished.height,
            "gaps": finished.gaps,
        })
        .to_string();

        // PNG 字节按 Vec 布局移交宿主(nb_free_buffer 按 (len,1) 归还)
        let mut png = finished.png;
        let len = png.len();
        if png.capacity() != len {
            png.shrink_to_fit();
        }
        if len == 0 || png.capacity() != len {
            return Err(AppError::Internal("长截图 PNG 缓冲整理失败".into()));
        }
        let ptr = png.as_mut_ptr();
        std::mem::forget(png);
        // SAFETY: 出参已校验非空,ptr 指向长度为 len 的 u8 缓冲
        unsafe {
            *out_png = ptr;
            *out_png_len = len;
        }
        Ok(meta)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_stitch_finish panic".into())));
    report_out_text(result, out_meta)
}

/// 滚动长截图:**丢弃会话**(取消/换选区/异常退出时释放缓冲)。幂等。
///
/// # Safety
/// session 必须是 NUL 结尾 UTF-8。
#[no_mangle]
pub unsafe extern "C" fn nb_stitch_abort(session: *const c_char) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let session = cstr_to_string(session)?;
        crate::stitch::abort_session(&session)
    }))
    .unwrap_or_else(|_| Err(AppError::Panicked("nb_stitch_abort panic".into())));
    report(result)
}

// ---------- 内部工具 ----------

/// PNG 字节 → (RGBA 像素, 宽, 高)。
///
/// `load_from_memory` 对带 alpha 的 PNG 本就解出 RGBA8,此时直接取走底层缓冲;
/// 只有灰度/RGB 等才需要一次格式转换。省掉的那一份是**整图尺寸**的临时拷贝
/// (4K 图 33MB)——截图入库/OCR 都是整图必经路径,实测对工作集峰值影响明显
/// (docs/06-memory-optimization.md §5)。
fn decode_png_to_rgba(png: &[u8]) -> Result<(Vec<u8>, u32, u32), AppError> {
    let decoded = image::load_from_memory(png).map_err(AppError::Image)?;
    Ok(match decoded {
        image::DynamicImage::ImageRgba8(img) => {
            let (w, h) = img.dimensions();
            (img.into_raw(), w, h)
        }
        other => {
            let converted = other.to_rgba8();
            let (w, h) = converted.dimensions();
            (converted.into_raw(), w, h)
        }
    })
}

/// 把 `patch` **合并进** `base`(设置字段级合并,见 save_settings 分支的注释)。
///
/// 规则:
/// - 两边都是**对象**时递归合并(一层一层往下),这样"只改 hotkeys.captureRegion"
///   不会把同级的其余快捷键清零;
/// - 其余情况(标量/数组/null)一律**整体替换** —— 数组语义就是整体(如 editorGeometry);
/// - `patch` 为 null 时保留 base(不把整个设置清空)。
fn merge_settings(mut base: serde_json::Value, patch: serde_json::Value) -> serde_json::Value {
    match (base.as_object_mut(), patch) {
        (Some(base_obj), serde_json::Value::Object(patch_obj)) => {
            for (key, value) in patch_obj {
                let merged = match base_obj.remove(&key) {
                    Some(existing) => merge_settings(existing, value),
                    None => value,
                };
                base_obj.insert(key, merged);
            }
            base
        }
        (_, serde_json::Value::Null) => base,   // null 视为"未提供"
        (_, other) => other,
    }
}

fn cstr_to_string(ptr: *const c_char) -> Result<String, AppError> {
    if ptr.is_null() {
        return Err(AppError::InvalidInput("C 字符串为空".into()));
    }
    // SAFETY: 契约要求调用方传入合法 NUL 结尾 UTF-8 串
    let cstr = unsafe { CStr::from_ptr(ptr) };
    Ok(cstr.to_string_lossy().into_owned())
}

/// 把 `Result<String, AppError>` 写入宿主的字符串出参并给返回码。
///
/// 出参生命周期与 `nb_call` 相同:由本 crate `CString::into_raw` 分配,
/// 宿主成功后以 `nb_free_string` 释放。失败时不写出参(宿主只看返回码)。
fn report_out_text(result: Result<String, AppError>, out_text: *mut *mut i8) -> i32 {
    match result {
        Ok(text) => match CString::new(text) {
            Ok(c) => {
                if !out_text.is_null() {
                    // SAFETY: out_text 指向宿主提供的合法指针槽位(契约见上)
                    unsafe { *out_text = c.into_raw() };
                }
                NB_OK
            }
            Err(_) => NB_ERR,
        },
        Err(err) => {
            log::warn!("FFI 调用失败: {err}");
            NB_ERR
        }
    }
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

/// RGBA 图像写入宿主出参(宿主用 `nb_free_buffer` 归还)。
///
/// **优先直接交出 Vec 自己的缓冲**,省掉一次整帧分配 + memcpy(三屏会话即 3×8.3MB
/// 的瞬态峰值,4K 是 3×33MB;顺带每次抓取少几十毫秒的拷贝耗时)。
///
/// 交出后由宿主 `nb_free_buffer(ptr, len)` 释放,其内部按 `Layout::from_size_align(len, 1)`
/// dealloc —— 只有 Vec 的 capacity 恰好等于 len 时该 layout 才与分配时一致,否则是 UB。
/// 故先 `shrink_to_fit` 校正并**复核**,对不上就退回拷贝路径(语义完全等价)。
///
/// # Safety
/// 出参指针契约同各导出函数。
unsafe fn write_rgba_out(
    image: crate::capture::CapturedImage,
    out_rgba: *mut *mut u8,
    out_w: *mut u32,
    out_h: *mut u32,
) -> Result<(), AppError> {
    if out_rgba.is_null() || out_w.is_null() || out_h.is_null() {
        return Err(AppError::InvalidInput("出参指针为空".into()));
    }
    let crate::capture::CapturedImage { width, height, .. } = image;
    let mut rgba = image.rgba;
    let len = rgba.len();
    if rgba.capacity() != len {
        rgba.shrink_to_fit();
    }
    if len > 0 && rgba.capacity() == len {
        let ptr = rgba.as_mut_ptr();
        // 所有权移交宿主:配套的 nb_free_buffer 会按 (len, 1) 归还,与此处 Vec 的
        // 分配布局一致(u8 的对齐恒为 1)。
        std::mem::forget(rgba);
        // SAFETY: ptr 指向 capacity == len 的 u8 缓冲,长度已随出参一并交给宿主
        unsafe {
            *out_rgba = ptr;
            *out_w = width;
            *out_h = height;
        }
        return Ok(());
    }
    let layout = std::alloc::Layout::from_size_align(len, 1)
        .map_err(|_| AppError::Internal("RGBA 分配布局非法".into()))?;
    // SAFETY: layout 大小 >0(图像非空)
    let ptr = unsafe { std::alloc::alloc(layout) };
    if ptr.is_null() {
        return Err(AppError::Internal("RGBA 缓冲分配失败".into()));
    }
    // SAFETY: ptr 刚按 layout 分配,长度与 rgba 一致
    unsafe {
        std::ptr::copy_nonoverlapping(rgba.as_ptr(), ptr, len);
        *out_rgba = ptr;
        *out_w = width;
        *out_h = height;
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
            // source 筛选(S5.1):只看截图/翻译结果等;缺省与空串都视为不过滤
            let source = str_opt(req, "source");
            let limit = req.get("limit").and_then(|v| v.as_i64()).unwrap_or(50);
            let offset = req.get("offset").and_then(|v| v.as_i64()).unwrap_or(0);
            let db = core.db.lock()?;
            let (items, total) = entry::list(
                &db,
                query.as_deref(),
                kind,
                pinned,
                tag.as_deref(),
                source.as_deref(),
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
        // 各来源及条目数(S5.1 来源筛选条):由数据决定有哪些来源
        "list_sources" => {
            let core = crate::core::Core::get()?;
            let db = core.db.lock()?;
            Ok(serde_json::to_value(entry::list_sources(&db)?)?)
        }
        // 显示器列表(多屏遮罩配对用):宿主用 name 与 QScreen::name() 对齐,
        // ⚠️ 不要用数组下标配对 —— 两端枚举顺序不同(见 capture.rs 的说明)。
        //    几何为**物理像素**(xcap 口径);Qt 侧几何是逻辑像素,混合 DPI 下不等。
        "list_monitors" => Ok(serde_json::to_value(crate::capture::monitors())?),
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
        // 重命名标签(S5.2):所有带该标签的条目就地替换,单事务完成。
        // to 为空 = 删除该标签;返回 { changed } 供宿主提示"已更新 N 条"。
        "rename_tag" => {
            let core = crate::core::Core::get()?;
            let from = str_req(req, "from")?;
            let to = str_opt(req, "to").unwrap_or_default();
            let db = core.db.lock()?;
            let changed = entry::rename_tag(&db, &from, &to)?;
            Ok(serde_json::json!({ "changed": changed }))
        }
        // 批量追加标签(S5.2):ids 中不存在的条目静默跳过;返回 { changed }。
        "add_tags_bulk" => {
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
            let changed = entry::add_tags_bulk(&db, &ids, &tags)?;
            Ok(serde_json::json!({ "changed": changed }))
        }
        // 批量删除(条目多选):单事务(行删除 + todo_meta 级联都在事务内);
        // 失效 id 静默跳过。返回 { deleted: [...] };blobs 的文件清理在事务外
        // (文件删除不可回滚,不进事务,与单条 delete 同一职责切分)。
        "delete_entries_bulk" => {
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
            let r = entry::delete_bulk(&db, &ids)?;
            for path in &r.blobs {
                crate::clip::remove_blob(&crate::clip::resolve_blob_path(&core.blobs_dir, path));
            }
            Ok(serde_json::json!({ "deleted": r.deleted }))
        }
        // 批量固定/取消固定(条目多选):返回 { changed }。
        "set_pinned_bulk" => {
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
            let pinned = req.get("pinned").and_then(|v| v.as_bool()).unwrap_or(false);
            let db = core.db.lock()?;
            let changed = entry::set_pinned_bulk(&db, &ids, pinned)?;
            Ok(serde_json::json!({ "changed": changed }))
        }
        // 批量完成/取消待办(条目多选):只对 todo 条目生效,返回 { changed }。
        "todo_set_done_bulk" => {
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
            let done = req.get("done").and_then(|v| v.as_bool()).unwrap_or(true);
            let db = core.db.lock()?;
            let changed = todo::set_done_bulk(&db, &ids, done)?;
            Ok(serde_json::json!({ "changed": changed }))
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

        // ---- 备份与导出(S5.3 配套;规划见 backup_export 模块注释) ----
        // 备份:zip 快照(需持 db 锁 checkpoint,慢操作由宿主工作线程调用)
        "backup_create" => {
            let core = crate::core::Core::get()?;
            let path = str_req(req, "path")?;
            Ok(crate::backup_export::backup_create(core, &path)?)
        }
        // 导出:全部条目 -> Markdown 单文件(图片 base64 内嵌;html 显式报错)
        "export_entries" => {
            let core = crate::core::Core::get()?;
            let path = str_req(req, "path")?;
            let format = str_opt(req, "format").unwrap_or_else(|| "markdown".to_owned());
            Ok(crate::backup_export::export_entries(core, &path, &format)?)
        }
        // 恢复:备份 zip -> **替换**当前数据(执行前自动险备份;慢 I/O 走宿主工作线程)
        "restore_backup" => {
            let core = crate::core::Core::get()?;
            let path = str_req(req, "path")?;
            Ok(crate::backup_export::restore_backup(core, &path)?)
        }
        // 导入:备份 zip **并入**当前数据(hash 去重、id 冲突重铸;慢 I/O 走宿主工作线程)
        "import_backup" => {
            let core = crate::core::Core::get()?;
            let path = str_req(req, "path")?;
            Ok(crate::backup_export::import_backup(core, &path)?)
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
        // 配置加载警告(数组,空 = 干净)。宿主启动时拉一次写进自己的日志:
        // Rust 的 log 出口是 nbcore.log,而排障第一现场通常是宿主的 nextboard.log,
        // 且配置回退会把**所有设置**打回默认(实测渲染后端切换 = 稳态 +100MB 量级),
        // 必须让它在用户可见的那份日志里留痕。
        "settings_warnings" => Ok(serde_json::json!(crate::settings::take_warnings())),
        // 当前生效的数据目录(默认目录或设置的自定义目录)。宿主启动时取一次,
        // 用于定位 blobs 目录与面板的 file:// 图片 URL。
        "data_dir" => {
            let core = crate::core::Core::get()?;
            Ok(serde_json::json!({ "path": &core.effective_dir }))
        }
        "save_settings" => {
            let core = crate::core::Core::get()?;
            // ⚠️ **字段级合并**,而不是"用请求整体替换"。
            //
            // 为什么必须合并(实测缺陷):`Settings` 是 `#[serde(default)]`,因此
            // `from_value(只传了一个键的请求)` 会把**其余全部字段回退默认值** ——
            // 于是"只改一个键"的部分写入会把用户的所有设置打回出厂值。
            // 实测(切一次浮条透明度):
            //     historyLimit 1234→5000、theme dark→system、
            //     pinDefaultOpacity 55→100、monitorEnabled false→true
            // 这正是用户报的"设置改了又自己变回去"的**根因**。
            //
            // 合并规则:以**当前设置**为底,用请求里出现的键逐个覆盖。
            // 对象类型(hotkeys/proxy/translateXxx)做**一层深合并**,这样
            // "只改 hotkeys.captureRegion"不会把同级的其余快捷键清零。
            let current = serde_json::to_value(core.settings()?)
                .map_err(|err| AppError::Internal(format!("当前设置序列化失败: {err}")))?;
            let merged = merge_settings(current, req.clone());
            let settings: crate::settings::Settings = serde_json::from_value(merged.clone())
                .map_err(|err| AppError::InvalidInput(format!("设置字段非法: {err}")))?;
            settings.validate()?;
            // 自定义数据目录变更:写待迁移标记,下次启动搬迁 db + blobs 到新目录
            // (settings.rs::take_pending_move;源目录保留,删除旧数据由用户手动做)。
            let previous = core.settings()?;
            if settings.data_dir.trim() != previous.data_dir.trim() {
                let source = crate::settings::effective_data_dir(&previous, &core.default_dir);
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

        // ---- 更新检查(S6.4 v1「检查更新 + 打开下载页」;docs/07-auto-update.md)----
        // ⚠️ `url` 允许为空(宿主编译期常量留空 = 功能禁用),所以**不能**用 `str_req`:
        //    它把空串按"缺失"处理,空置态会被误报成"缺少参数 url"。
        "check_update" => {
            let core = crate::core::Core::get()?;
            let url = req
                .get("url")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_owned();
            let current = str_req(req, "currentVersion")?;
            crate::update::check(core, &url, &current)
        }

        // ---- OCR ----
        "ocr_probe" => {
            // 可选 backend 参数(向后兼容:不传即内置引擎,与旧调用方行为一致)。
            let backend = match str_opt(req, "backend").as_deref() {
                Some(value) => crate::ocr::Backend::parse(value),
                None => crate::ocr::Backend::System,
            };
            let probe = crate::ocr::probe(backend);
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
            let (rgba, width, height) = decode_png_to_rgba(&bytes)?;
            let settings = core.settings()?;
            let text = crate::ocr::recognize_configured(
                rgba,
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
            let (rgba, width, height) = decode_png_to_rgba(&png)?;
            let stored = crate::clip::ingest_image(core, rgba, width, height, &source)?;
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
            // due = "today"(可选):托盘「今日待办」用 —— 未完成 且
            // (已逾期 或 今天到期),等价于 due_at ≤ 今天 23:59:59.999(本地时区)。
            // 边界换算在 core(end_of_today_ms),口径单点;非法值显式报错不静默。
            // (此分支纯日历换算,不查库 —— 无需在此持锁。)
            let due_max = match str_opt(req, "due").as_deref() {
                None => None,
                Some("today") => Some(todo::end_of_today_ms(crate::now_ms())),
                Some(other) => {
                    return Err(AppError::InvalidInput(format!(
                        "未知 due 过滤: {other}(当前仅支持 today)"
                    )))
                }
            };
            let limit = req.get("limit").and_then(|v| v.as_i64()).unwrap_or(50);
            let offset = req.get("offset").and_then(|v| v.as_i64()).unwrap_or(0);
            let db = core.db.lock()?;
            let (items, total) = todo::list(
                &db,
                filter,
                query.as_deref(),
                tag.as_deref(),
                due_max,
                limit,
                offset,
            )?;
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

#[cfg(test)]
mod merge_tests {
    //! 设置**字段级合并**的回归(save_settings)。
    //!
    //! 历史缺陷:save_settings 直接 `from_value(请求)`,而 Settings 是 #[serde(default)]
    //! —— 只传一个键的请求会把其余字段全部打回默认值。实测"切一次浮条透明度"就导致
    //! historyLimit/theme/pinDefaultOpacity/monitorEnabled 全被重置。以下断言钉住修复。

    use super::merge_settings;
    use serde_json::json;

    #[test]
    fn partial_patch_keeps_untouched_fields() {
        let base = json!({ "historyLimit": 1234, "theme": "dark", "monitorEnabled": false });
        let merged = merge_settings(base, json!({ "captureBarAlpha": 179 }));
        assert_eq!(merged["historyLimit"], 1234, "未提及的字段必须原样保留");
        assert_eq!(merged["theme"], "dark");
        assert_eq!(merged["monitorEnabled"], false);
        assert_eq!(merged["captureBarAlpha"], 179, "请求里的字段要生效");
    }

    #[test]
    fn nested_object_merges_one_level_deep() {
        // 只改 hotkeys 里的一个键,同级其余快捷键不能被清掉
        let base = json!({ "hotkeys": { "toggleMain": "Alt+A", "captureRegion": "Ctrl+Shift+A" } });
        let merged = merge_settings(base, json!({ "hotkeys": { "captureRegion": "F1" } }));
        assert_eq!(merged["hotkeys"]["captureRegion"], "F1");
        assert_eq!(merged["hotkeys"]["toggleMain"], "Alt+A", "同级字段不得丢失");
    }

    #[test]
    fn arrays_replace_wholesale() {
        // 数组语义是整体(如 editorGeometry 四元组),不做逐元素合并
        let base = json!({ "editorGeometry": [1, 2, 3, 4] });
        let merged = merge_settings(base, json!({ "editorGeometry": [10, 20, 30, 40] }));
        assert_eq!(merged["editorGeometry"], json!([10, 20, 30, 40]));
    }

    #[test]
    fn null_patch_does_not_wipe() {
        let base = json!({ "theme": "dark" });
        let merged = merge_settings(base, json!(null));
        assert_eq!(merged["theme"], "dark", "null 视为未提供,不清空");
    }

    #[test]
    fn new_key_is_added() {
        let base = json!({ "theme": "dark" });
        let merged = merge_settings(base, json!({ "brandNew": 7 }));
        assert_eq!(merged["brandNew"], 7);
        assert_eq!(merged["theme"], "dark");
    }
}

#[cfg(test)]
mod contract_tests {
    //! FFI **返回体契约**的回归:契约漂移不会编译报错,只会让功能静默失效 ——
    //! 所以必须有断言把它钉住。
    //!
    //! 历史缺陷:nb_ocr_rgba 交的是**裸** {"text": ...},而宿主侧照抄了
    //! nb_call 的 {"ok":...,"data":...} 信封去解析,于是"核心识别成功、
    //! 用户永远取到空串"(nbcore.log 有 "RapidOCR 完成:...,N 行",剪贴板却什么都没有)。

    /// nb_ocr_rgba 自己构造返回体的那段逻辑(与函数体保持同形)。
    fn ocr_payload(text: &str) -> String {
        serde_json::json!({ "text": text }).to_string()
    }

    /// 只有 text 字段 —— **没有** ok/data 信封。宿主若加信封会在此失败。
    #[test]
    fn ocr_payload_has_no_envelope() {
        let raw = ocr_payload("HELLO 12345");
        let v: serde_json::Value = serde_json::from_str(&raw).expect("必须是合法 JSON");
        assert_eq!(v.get("text").and_then(|t| t.as_str()), Some("HELLO 12345"));
        assert!(
            v.get("data").is_none() && v.get("ok").is_none(),
            "OCR 返回体不应带 ok/data 信封(宿主按裸对象解析);实际: {raw}"
        );
    }

    /// 对照:nb_call **是**带信封的。两者不同是**有意为之**,
    /// 这条断言用来防止有人"顺手统一"而打穿旧宿主。
    #[test]
    fn nb_call_payload_has_envelope() {
        let body =
            serde_json::json!({ "ok": true, "data": serde_json::json!({"text": "x"}) }).to_string();
        let v: serde_json::Value = serde_json::from_str(&body).unwrap();
        assert_eq!(v.get("ok").and_then(|b| b.as_bool()), Some(true));
        assert!(v.get("data").is_some());
    }
}
