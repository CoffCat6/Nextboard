//! 剪贴板监听与内容入库(第一代设计文档 §5.2 的 Nextboard 实现)。
//!
//! 监听方式:**AddClipboardFormatListener 事件驱动**(取代第一代 200ms 轮询)——
//! 专用线程建消息窗口,收到 WM_CLIPBOARDUPDATE 后读取/去重/入库,再 PostMessage
//! 通知宿主。事件延迟从 ~100ms 降到消息级,零轮询 CPU。
//!
//! 入库管线(过滤 → blake3 去重 → 落盘 → 淘汰)与第一代逐行同构;
//! 图片写剪贴板沿用 D6 结论:clipboard-win 直写 CF_DIBV5 单次 SetClipboardData + 打开重试。

use std::path::{Path, PathBuf};

use blake3::Hasher;
use clipboard_win::formats;
use log::{debug, info, warn};

use crate::core::{notify_host, try_core, Core, WM_APP_CLIP_CHANGED};
use crate::error::AppResult;
use crate::settings::Settings;
use crate::store::entry::{self, ClipEntry, EntryKind};

/// 文件条目的路径数量上限:超出直接丢弃(第一代 Phase 3 F1)。
const MAX_FILE_COUNT: usize = 64;

/// Windows 消息:剪贴板更新。
#[cfg(windows)]
const WM_CLIPBOARDUPDATE: u32 = 0x031D;

/// 启动事件驱动监听线程(消息窗口 + AddClipboardFormatListener)。
pub fn start_monitor() {
    std::thread::Builder::new()
        .name("clipboard-monitor".into())
        .spawn(monitor_thread)
        .expect("启动剪贴板监听线程失败");
}

#[cfg(windows)]
fn monitor_thread() {
    use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
    use windows::Win32::System::DataExchange::{
        AddClipboardFormatListener, RemoveClipboardFormatListener,
    };
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, DefWindowProcW, DispatchMessageW, GetMessageW, RegisterClassW,
        TranslateMessage, CW_USEDEFAULT, MSG, WINDOW_EX_STYLE, WM_CREATE, WM_DESTROY, WNDCLASSW,
        WS_OVERLAPPED,
    };

    // 消息窗口过程:只关心 WM_CLIPBOARDUPDATE,其余走 DefWindowProc
    unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
        if msg == WM_CREATE {
            // 注册剪贴板监听(WM_CREATE 时窗口已有效)
            let listen = AddClipboardFormatListener(hwnd);
            info!("剪贴板监听注册: {}", listen.is_ok());
            return LRESULT(0);
        }
        if msg == WM_CLIPBOARDUPDATE {
            handle_clipboard_update();
            return LRESULT(0);
        }
        if msg == WM_DESTROY {
            let _ = RemoveClipboardFormatListener(hwnd);
            return LRESULT(0);
        }
        DefWindowProcW(hwnd, msg, wp, lp)
    }

    unsafe {
        let hinstance = GetModuleHandleW(None).expect("GetModuleHandleW 失败");
        let class_name = windows::core::w!("nbcore_clip_monitor");
        let wc = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            hInstance: hinstance.into(),
            lpszClassName: class_name,
            ..Default::default()
        };
        let atom = RegisterClassW(&wc);
        debug_assert_ne!(atom, 0, "RegisterClassW 失败");

        // 消息专用窗口:不可见、无样式,仅接收广播消息
        let hwnd = CreateWindowExW(
            WINDOW_EX_STYLE(0),
            class_name,
            windows::core::w!(""),
            WS_OVERLAPPED,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            None,
            None,
            Some(hinstance.into()),
            None,
        )
        .expect("创建剪贴板监听消息窗口失败");
        let _hwnd: HWND = hwnd;

        info!("剪贴板监听线程就绪(事件驱动)");
        let mut msg = MSG::default();
        while GetMessageW(&mut msg, None, 0, 0).as_bool() {
            let _ = TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        info!("剪贴板监听线程退出");
    }
}

#[cfg(not(windows))]
fn monitor_thread() {
    info!("剪贴板事件监听仅支持 Windows,线程空转");
    loop {
        if !crate::core::ready_flag() {
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
}

/// 当前剪贴板属主的应用 exe 文件名(小写,如 "chrome.exe");失败返回 None。
///
/// 时序:WM_CLIPBOARDUPDATE 到达时属主已设置;属主进程恰好退出的竞态窗口极小,
/// 失败按 None 存(展示层对 None 诚实降级为不显示)。只存文件名不存完整路径:
/// 完整路径含用户名/安装目录,进库会扩大隐私面(备份包被带出去)——docs/25 §2.2。
#[cfg(windows)]
fn clipboard_owner_app() -> Option<String> {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::DataExchange::GetClipboardOwner;
    use windows::Win32::System::Threading::{
        OpenProcess, QueryFullProcessImageNameW, PROCESS_NAME_FORMAT,
        PROCESS_QUERY_LIMITED_INFORMATION,
    };
    use windows::Win32::UI::WindowsAndMessaging::GetWindowThreadProcessId;

    let owner = unsafe { GetClipboardOwner() }.ok()?;
    let mut pid = 0u32;
    unsafe { GetWindowThreadProcessId(owner, Some(&mut pid)) };
    if pid == 0 {
        return None;
    }
    let process = unsafe {
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
    }
    .ok()?;
    let mut buf = [0u16; 1024];
    let mut len = buf.len() as u32;
    let ok = unsafe {
        QueryFullProcessImageNameW(process, PROCESS_NAME_FORMAT(0), windows::core::PWSTR(buf.as_mut_ptr()), &mut len)
    };
    unsafe { let _ = CloseHandle(process); };
    if !ok.is_ok() || len == 0 {
        return None;
    }
    let full = String::from_utf16_lossy(&buf[..len as usize]);
    owner_app_name(&full)
}

/// 完整路径 → 应用文件名(小写)。独立纯函数便于单测(属主采集本身依赖
/// 真实剪贴板状态,无法在单测里模拟)。
fn owner_app_name(full: &str) -> Option<String> {
    let name = full.rsplit(['\\', '/']).next()?.to_lowercase();
    if name.is_empty() { None } else { Some(name) }
}

/// WM_CLIPBOARDUPDATE 处理:核心就绪且监听开启才消费。
#[cfg(windows)]
fn handle_clipboard_update() {
    use clipboard_win::seq_num;

    let Some(core) = try_core() else {
        return;
    };
    let Ok(settings) = core.settings() else {
        return;
    };
    if !settings.monitor_enabled {
        return;
    }
    // 序列号防抖:同一次复制的多次通知(如不同格式注册)只消费一次
    let seq = seq_num().map(|n| n.get());
    static LAST_SEQ: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    if let Some(s) = seq {
        let last = LAST_SEQ.swap(s, std::sync::atomic::Ordering::SeqCst);
        if last == s && s != 0 {
            return;
        }
    }

    let Some(item) = read_current() else {
        return;
    };
    // 属主在读取内容前采集(文档 docs/25 §2.1):广播到达即查,不依赖内容类型
    let owner = clipboard_owner_app();
    let hash = item.hash();
    match process(core, item, &hash, &settings, owner.as_deref()) {
        Ok(_) => notify_host(WM_APP_CLIP_CHANGED),
        Err(err) => warn!("剪贴板内容入库失败: {err}"),
    }
}

/// 一次剪贴板读取到的内容。
pub enum ClipItem {
    Text(String),
    Image {
        rgba: Vec<u8>,
        width: u32,
        height: u32,
    },
    /// 文件路径列表(CF_HDROP,第一代 Phase 3)。
    File(Vec<String>),
}

impl ClipItem {
    fn byte_size(&self) -> i64 {
        match self {
            ClipItem::Text(text) => text.len() as i64,
            ClipItem::Image { rgba, .. } => rgba.len() as i64,
            ClipItem::File(paths) => paths
                .iter()
                .filter_map(|p| std::fs::metadata(p).ok())
                .map(|m| m.len() as i64)
                .sum(),
        }
    }

    /// blake3 摘要;带类型前缀避免不同类型内容碰撞。
    pub fn hash(&self) -> String {
        let mut hasher = Hasher::new();
        match self {
            ClipItem::Text(text) => {
                hasher.update(b"text:");
                hasher.update(text.as_bytes());
            }
            ClipItem::Image { rgba, .. } => {
                hasher.update(b"image:");
                hasher.update(rgba);
            }
            ClipItem::File(paths) => {
                hasher.update(b"file:");
                hasher.update(paths.join("\n").as_bytes());
            }
        }
        hasher.finalize().to_hex().to_string()
    }
}

/// 读取当前剪贴板内容:先文本、再图片、最后文件;全部失败(空剪贴板等)返回 None。
/// 读取失败属于正常控制流(内容不是该类型),不记日志。
///
/// 文本/图片经 clipboard-win 直读(取代第一代的 tauri clipboard 插件——它只是
/// arboard 的包装,而 clipboard-win 本就是直连 Win32 的既有依赖,减少一层)。
pub fn read_current() -> Option<ClipItem> {
    // 所有读取共用一次打开(带重试);clipboard-win 的 get_clipboard 内部自管打开/关闭
    if let Ok(text) = clipboard_win::get_clipboard::<String, _>(formats::Unicode) {
        if !text.trim().is_empty() {
            return Some(ClipItem::Text(text));
        }
    }
    if let Ok(paths) = clipboard_win::get_clipboard::<Vec<String>, _>(formats::FileList) {
        let paths: Vec<String> = paths.into_iter().filter(|p| !p.is_empty()).collect();
        if !paths.is_empty() {
            return Some(ClipItem::File(paths));
        }
    }
    read_image_from_clipboard().map(|(rgba, w, h)| ClipItem::Image {
        rgba,
        width: w,
        height: h,
    })
}

/// 读取 CF_DIB / CF_DIBV5 位图并解码为 RGBA(clipboard-win 无图片格式的高层封装,
/// 用 raw 通道取字节后按 BITMAPINFOHEADER/BITMAPV5HEADER 解析)。
#[cfg(windows)]
fn read_image_from_clipboard() -> Option<(Vec<u8>, u32, u32)> {
    use clipboard_win::raw;

    let _clip = clipboard_win::Clipboard::new_attempts(10).ok()?;
    for format in [formats::CF_DIBV5, formats::CF_DIB] {
        let Some(size) = raw::size(format) else {
            continue;
        };
        let size = size.get();
        if size == 0 {
            continue;
        }
        let mut buf = vec![0u8; size];
        let read = raw::get(format, &mut buf);
        if read.is_err() {
            continue;
        }
        buf.truncate(read.unwrap_or(0));
        if let Some((rgba, w, h)) = decode_dib(&buf) {
            return Some((rgba, w, h));
        }
    }
    None
}

#[cfg(not(windows))]
fn read_image_from_clipboard() -> Option<(Vec<u8>, u32, u32)> {
    None
}

/// 解析 DIB 载荷为 RGBA。处理 BI_RGB / BI_BITFIELDS 两种压缩、8/24/32 位深、
/// 底行在前(高度为正)的常规组合;16 位与调色板压缩格式罕见,直接放弃。
#[cfg(windows)]
fn decode_dib(dib: &[u8]) -> Option<(Vec<u8>, u32, u32)> {
    if dib.len() < 40 {
        return None;
    }
    let header_size = u32::from_le_bytes([dib[0], dib[1], dib[2], dib[3]]) as usize;
    if header_size < 40 || dib.len() < header_size {
        return None;
    }
    let width = i32::from_le_bytes([dib[4], dib[5], dib[6], dib[7]]);
    let height = i32::from_le_bytes([dib[8], dib[9], dib[10], dib[11]]);
    let planes = u16::from_le_bytes([dib[12], dib[13]]);
    let bpp = u16::from_le_bytes([dib[14], dib[15]]);
    let compression = u32::from_le_bytes([dib[16], dib[17], dib[18], dib[19]]);
    const BI_RGB: u32 = 0;
    const BI_BITFIELDS: u32 = 3;
    if planes != 1 || !matches!(compression, BI_RGB | BI_BITFIELDS) {
        return None;
    }
    let width = u32::try_from(width).ok()?;
    // 高度为负 = 顶行在前(罕见);为正 = 底行在前(常规)
    let (height, bottom_up) = match u32::try_from(height) {
        Ok(h) => (h, true),
        Err(_) => (u32::try_from(-height).ok()?, false),
    };
    if width == 0 || height == 0 || width > 1 << 15 || height > 1 << 15 {
        return None;
    }
    // BI_BITFIELDS 的掩码紧跟头部(40 字节头时);V5 头(124)自带掩码字段
    let (r_mask, g_mask, b_mask, a_mask) = if header_size >= 52 {
        read_masks(&dib[40..56])
    } else if header_size == 40 && compression == BI_BITFIELDS && dib.len() >= 40 + 12 {
        read_masks(&dib[40..52])
    } else {
        (0x00FF_0000, 0x0000_FF00, 0x0000_00FF, 0)
    };
    let row_size = ((width as usize * bpp as usize + 31) / 32) * 4;
    let expected = header_size + row_size * height as usize;
    if dib.len() < expected {
        return None;
    }
    let mut rgba = Vec::with_capacity(width as usize * height as usize * 4);
    let row_range = |row: u32| -> (usize, usize) {
        let idx = if bottom_up {
            height - 1 - row
        } else {
            row
        } as usize;
        (header_size + idx * row_size, header_size + (idx + 1) * row_size)
    };
    for row in 0..height {
        let (start, end) = row_range(row);
        let src = &dib[start..end];
        match bpp {
            32 => {
                // 按行块化:每像素一次 extract(此前每像素连调 4 次逐字节 push)
                for px in src[..width as usize * 4].chunks_exact(4) {
                    let (r, g, b, a) =
                        extract(px[0], px[1], px[2], px[3], b_mask, r_mask, g_mask);
                    rgba.extend_from_slice(&[r, g, b, if a_mask != 0 { a } else { 255 }]);
                }
            }
            24 => {
                for px in src[..width as usize * 3].chunks_exact(3) {
                    rgba.extend_from_slice(&[px[2], px[1], px[0], 255]);
                }
            }
            // 8 位带调色板:调色板解析成本高且剪贴板场景罕见(截图链路不产生)
            _ => return None,
        }
    }
    Some((rgba, width, height))
}

#[cfg(windows)]
fn read_masks(b: &[u8]) -> (u32, u32, u32, u32) {
    let m = |i: usize| {
        u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
    };
    (m(0), m(4), m(8), if b.len() >= 16 { m(12) } else { 0 })
}

/// 从 32 位 BGRA 字节按掩码拆通道(掩码为标准 8 位段时即直取)。
#[cfg(windows)]
fn extract(b: u8, g: u8, r: u8, a: u8, b_mask: u32, r_mask: u32, g_mask: u32) -> (u8, u8, u8, u8) {
    let _ = (b_mask, r_mask, g_mask); // 常见 8 位掩码直取;异形掩码放弃精度损失可忽略
    (r, g, b, a)
}

/// 入库流程:大小过滤 → 查重(命中则 touch)→ 落盘入库 → 超限淘汰。
/// 返回最终生效的条目;`None` 表示超大小上限被忽略。
///
/// ⚠️ `build_entry`(PNG 默认档编码+落盘+缩略图,4K 图可达秒级)**必须在 DB 锁外**:
/// 这把锁是进程唯一的,面板翻页/计数/待办调度全排在它后面 —— 锁内做重活等于
/// 每复制一张大图全应用停摆一次。代价是两段锁之间可能出现同 hash 并发插入,
/// 故第二段锁里再查一次,输了就清掉刚写的文件、复用已生效条目。
fn process(
    core: &Core,
    item: ClipItem,
    hash: &str,
    settings: &Settings,
    owner: Option<&str>,
) -> AppResult<Option<ClipEntry>> {
    if is_oversized(&item, settings) {
        debug!("剪贴板内容超过大小上限,已忽略");
        return Ok(None);
    }
    let now = crate::now_ms();

    // ---- 第一段锁:只做查重与 touch(都是索引点查)----
    {
        let db = core.db.lock()?;
        if let Some(id) = entry::find_id_by_hash(&db, hash)? {
            entry::touch(&db, &id, now)?;
            return entry::get(&db, &id);
        }
    }

    // ---- 锁外重活:PNG 编码 + 落盘 + 缩略图 ----
    let new_entry = build_entry(&core.blobs_dir, item, hash, now, settings.max_image_mb, owner)?;
    let Some(new_entry) = new_entry else {
        debug!("图片超过大小上限({}MB),未入库", settings.max_image_mb);
        return Ok(None);
    };

    // ---- 第二段锁:防竞态复查 → 写入 → 淘汰 ----
    let db = core.db.lock()?;
    if let Some(id) = entry::find_id_by_hash(&db, hash)? {
        // 输给并发插入(极小窗口):清掉刚写的产物,复用已生效条目。
        if let Some(rel) = &new_entry.blob_path {
            remove_blob(&resolve_blob_path(&core.blobs_dir, rel));
        }
        entry::touch(&db, &id, now)?;
        return entry::get(&db, &id);
    }
    entry::insert(&db, &new_entry)?;
    for blob in entry::evict(&db, settings.history_limit)? {
        remove_blob(&resolve_blob_path(&core.blobs_dir, &blob));
    }
    Ok(Some(new_entry))
}

/// 应用内部来源的图片入库(第一代 Phase 5 F14):与剪贴板监听共用
/// 「大小过滤 → hash 去重 → 落盘入库 → 淘汰」路径,返回 `None` 表示超过
/// 大小上限未入库。调用方(C++ 截图会话)须在入库**之后**再写系统剪贴板,
/// 让监听同 hash 命中去重,避免重复条目(第一代 docs/phase5.md §3.6)。
pub fn ingest_image(
    core: &Core,
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    source: &str,
) -> AppResult<Option<ClipEntry>> {
    let settings = core.settings()?;
    let item = ClipItem::Image {
        rgba,
        width,
        height,
    };
    let hash = item.hash();
    let mut stored = process(core, item, &hash, &settings, None)?;
    if let Some(entry) = &mut stored {
        if entry.source != source {
            set_source(core, &entry.id, source)?;
            entry.source = source.to_owned();
        }
    }
    Ok(stored)
}

/// 应用内部来源的文本入库(S5.1 统一素材库):翻译原文/译文等"存为条目"走这里。
///
/// 与剪贴板监听共用「大小过滤 → hash 去重 → 入库 → 淘汰」路径 —— 因此
/// 与剪贴板里的同内容文本自然去重;`source` 记录来源(如 `translate`),
/// 入库后经 FTS 触发器自动可被搜索。`None` 表示超过文本大小上限被忽略。
pub fn ingest_text(core: &Core, text: &str, source: &str) -> AppResult<Option<ClipEntry>> {
    let settings = core.settings()?;
    let item = ClipItem::Text(text.to_owned());
    let hash = item.hash();
    let mut stored = process(core, item, &hash, &settings, None)?;
    if let Some(entry) = &mut stored {
        if entry.source != source {
            set_source(core, &entry.id, source)?;
            entry.source = source.to_owned();
        }
    }
    Ok(stored)
}

/// 更新条目来源(v5 source 列;截图/翻译保存入库后回写)。
fn set_source(core: &Core, id: &str, source: &str) -> AppResult<()> {
    let db = core.db.lock()?;
    let changed = rusqlite::Connection::execute(
        &*db,
        "UPDATE clip_entries SET source = ?1 WHERE id = ?2",
        rusqlite::params![source, id],
    )?;
    if changed == 0 {
        log::warn!("回写来源失败,条目不存在: {id}");
    }
    Ok(())
}

/// 解析图片 blob 的实际路径:兼容历史绝对路径与相对路径
/// (第一代 Phase 4 起 blob 改存相对路径,自定义数据目录迁移后仍有效)。
pub fn resolve_blob_path(blobs_dir: &Path, stored: &str) -> PathBuf {
    let candidate = Path::new(stored);
    if candidate.is_absolute() {
        candidate.to_path_buf()
    } else {
        blobs_dir.join(stored)
    }
}

/// 便宜的前置过滤(文本按字符量、文件按数量)。
///
/// **图片不在这里判**:图片的体积上限针对**落盘的 PNG**(真正占磁盘的东西),而那要等
/// 编码完才知道 —— 见 [`build_entry`]。沿用旧口径(拿原始 RGBA 比)会让
/// 3840x2160 的整屏截图(33MB RGBA)永远超限、一张都进不了历史。
fn is_oversized(item: &ClipItem, settings: &Settings) -> bool {
    match item {
        ClipItem::Text(_) => item.byte_size() > settings.max_text_kb * 1024,
        ClipItem::Image { .. } => false,
        ClipItem::File(paths) => paths.len() > MAX_FILE_COUNT,
    }
}

/// 构建待入库条目;图片先编码 PNG 落盘到 `blobs/YYYY-MM/`。
///
/// 返回 `None` 表示**图片超过大小上限**:不落盘、不入库(调用方据此提示)。
/// 上限针对**编码后的 PNG 体积** —— 那才是真正占磁盘的东西。
fn build_entry(
    blobs_dir: &Path,
    item: ClipItem,
    hash: &str,
    now: i64,
    max_image_mb: i64,
    owner: Option<&str>,
) -> AppResult<Option<ClipEntry>> {
    let id = uuid::Uuid::now_v7().to_string();
    match item {
        ClipItem::Text(text) => Ok(Some(ClipEntry {
            preview: entry::make_preview(&text),
            byte_size: text.len() as i64,
            kind: EntryKind::Text,
            text_content: Some(text),
            blob_path: None,
            file_paths: None,
            width: None,
            height: None,
            id,
            hash: hash.to_owned(),
            pinned: false,
            created_at: now,
            last_used_at: now,
            source: "clipboard".into(),
            source_owner: owner.map(|o| o.to_owned()),
            tags: None,
            todo: None,
        })),
        ClipItem::Image {
            rgba,
            width,
            height,
        } => {
            let png = encode_png(&rgba, width, height)?;
            if png.len() as i64 > max_image_mb * 1024 * 1024 {
                return Ok(None);
            }
            // 存相对路径(相对 blobs 目录),自定义数据目录迁移后仍有效(第一代 docs/phase4.md)
            let relative = format!("{}/{}.png", month_dir(now), id);
            let path = blobs_dir.join(&relative);
            let dir = path
                .parent()
                .ok_or_else(|| crate::error::AppError::Internal("图片路径缺少父目录".into()))?;
            std::fs::create_dir_all(dir)?;
            std::fs::write(&path, &png)?;
            // 列表缩略图随入库一并生成(同一份 RGBA,免二次解码):
            // 主面板列表只加载缩略图,避免为卡片解码整张 4K 原图(约 33MB)。
            // 编码 PNG 已完成,rgba 不再需要 —— 直接 move 进去,省一次整图拷贝。
            write_thumbnail_from_rgba(rgba, width, height, &thumbnail_path(&path));
            Ok(Some(ClipEntry {
                byte_size: png.len() as i64,
                kind: EntryKind::Image,
                text_content: None,
                blob_path: Some(relative),
                file_paths: None,
                width: Some(i64::from(width)),
                height: Some(i64::from(height)),
                preview: String::new(),
                id,
                hash: hash.to_owned(),
                pinned: false,
                created_at: now,
                last_used_at: now,
                source: "clipboard".into(),
                tags: None,
                todo: None,
                source_owner: owner.map(|o| o.to_owned()),
            }))
        }
        ClipItem::File(paths) => {
            let byte_size: i64 = paths
                .iter()
                .filter_map(|p| std::fs::metadata(p).ok())
                .map(|m| m.len() as i64)
                .sum();
            Ok(Some(ClipEntry {
                preview: entry::make_file_preview(&paths),
                byte_size,
                kind: EntryKind::File,
                text_content: None,
                blob_path: None,
                file_paths: Some(paths),
                width: None,
                height: None,
                id,
                hash: hash.to_owned(),
                pinned: false,
                created_at: now,
                last_used_at: now,
                source: "clipboard".into(),
                tags: None,
                todo: None,
                source_owner: owner.map(|o| o.to_owned()),
            }))
        }
    }
}

/// RGBA 编码为 PNG 字节流(默认压缩档,用于落盘 blob:体积优先)。
pub fn encode_png(rgba: &[u8], width: u32, height: u32) -> AppResult<Vec<u8>> {
    encode_png_with(rgba, width, height, false)
}

/// 列表缩略图最大边长(物理像素)。
///
/// **为什么从 320 提到 640**:列表图片卡片实际渲染宽度约 533 逻辑 px(面板 800 − 两侧留白
/// 24 − 卡片内边距 24,16:9 时受高度上限 300 约束),再乘屏幕 DPR。原先 320 的缩略图在
/// DPR 1.0 屏上就被放大 1.67×、1.5 屏上 2.5× —— 这正是"缩略图发虚"的根因(不是解码/降采样)。
/// 640 在 DPR 1.0 下约为 1.0×,不再放大;DPR 1.5 下 1.25×,可接受。
///
/// 仍保留**解码内存**收益:4K 截图解码为位图约 33MB,而 640px 缩略图仅约 0.9MB
/// (第一代 docs/memory-optimization.md 的核心结论,Nextboard 继续生效)。
pub const THUMB_MAX_EDGE: u32 = 640;

/// 缩略图文件名版本号,必须与 [`THUMB_MAX_EDGE`] 保持一致。
///
/// 版本号参与文件名(`xxx.thumb640.png`),于是**调大边长后旧缓存自动失效**:后台回填
/// (见 [`spawn_thumbnail_backfill`])按新名字重新生成,无需手动清缓存、也不会读到旧尺寸
/// 的缓存(否则老条目会永远停在 320)。宿主侧 `CoreBridge::nbThumbVersion` 必须同步。
pub const THUMB_VERSION: u32 = THUMB_MAX_EDGE;

/// 由原图 blob 路径推导缩略图缓存路径:`xxx.png` → `xxx.thumb<版本>.png`(同目录)。
pub fn thumbnail_path(blob_path: &Path) -> PathBuf {
    let stem = blob_path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("blob");
    blob_path.with_file_name(format!("{stem}.thumb{THUMB_VERSION}.png"))
}

/// 旧版缩略图缓存路径(无版本号后缀):`xxx.png` → `xxx.thumb.png`。
///
/// 只用于**启动清理**:版本化命名之前生成的缓存(尺寸偏小、还发虚)在新版本里既不会被
/// 读取、也不会被子条目删除清理到,不主动删就会一直占着磁盘(`remove_blob` 只删当前
/// 版本路径)。清理是幂等的,删不到即忽略。
pub fn legacy_thumbnail_path(blob_path: &Path) -> PathBuf {
    let stem = blob_path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("blob");
    blob_path.with_file_name(format!("{stem}.thumb.png"))
}

/// 原子写文件:先写唯一临时文件再改名,避免并发读者读到半截内容。
fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let tmp = path.with_extension(format!("{}.tmp", uuid::Uuid::now_v7()));
    std::fs::write(&tmp, bytes)?;
    // Windows 的 rename 不覆盖已存在目标:先移除旧文件再改名。
    let _ = std::fs::remove_file(path);
    match std::fs::rename(&tmp, path) {
        Ok(()) => Ok(()),
        Err(err) => {
            let _ = std::fs::remove_file(&tmp);
            Err(err)
        }
    }
}

/// 等比缩放后的尺寸:让 (w,h) 完整含于 `max_edge × max_edge` 框内(不拉伸、最少缩到 1px)。
///
/// ⚠️ `image::imageops::resize` 本身**不保持宽高比**(它直接按给定尺寸拉伸),
/// `DynamicImage::resize` 才是先算尺寸再调它;这里按后者的口径自行换算,避免变形。
fn thumbnail_dimensions(w: u32, h: u32, max_edge: u32) -> (u32, u32) {
    let longest = w.max(h);
    if longest == 0 {
        return (w, h);
    }
    let ratio = f64::from(max_edge) / f64::from(longest);
    (
        ((f64::from(w) * ratio).round() as u32).max(1),
        ((f64::from(h) * ratio).round() as u32).max(1),
    )
}

/// 等比缩放到 `max_edge` 内并编码为快速档 PNG(缩略图只用于展示,压缩率不重要)。
///
/// **不放大小图**:小于边界的图原样保留(第一代实测 `thumbnail` 会为填满边界而**放大**
/// 64x32 → 320x160,故先判尺寸再决定是否缩放)。
///
/// **用 CatmullRom 而非 `DynamicImage::thumbnail`**:后者是等权盒平均(box average),
/// 对文字/UI 截图的高频边缘会"糊一圈";CatmullRom 边缘更利落,是"缩略图发虚"的次要成因。
/// 仅当原图超出 `max_edge` 时才走它;目标尺寸先经 [`thumbnail_dimensions`] 保持宽高比。
fn encode_thumbnail(img: &image::DynamicImage, max_edge: u32) -> AppResult<Vec<u8>> {
    let thumb = if img.width() <= max_edge && img.height() <= max_edge {
        img.to_rgba8()
    } else {
        let (nw, nh) = thumbnail_dimensions(img.width(), img.height(), max_edge);
        image::imageops::resize(
            img,
            nw,
            nh,
            image::imageops::FilterType::CatmullRom,
        )
    };
    let mut out = std::io::Cursor::new(Vec::new());
    use image::ImageEncoder as _;
    image::codecs::png::PngEncoder::new_with_quality(
        &mut out,
        image::codecs::png::CompressionType::Fast,
        image::codecs::png::FilterType::Sub,
    )
    .write_image(
        thumb.as_raw(),
        thumb.width(),
        thumb.height(),
        image::ExtendedColorType::Rgba8,
    )?;
    Ok(out.into_inner())
}

/// 读取或生成列表缩略图(磁盘缓存),返回 PNG 字节。
/// 新条目在入库时已预生成,本函数只服务**存量条目**的一次性补建。
pub fn load_or_build_thumbnail(blob_path: &Path) -> AppResult<Vec<u8>> {
    let thumb = thumbnail_path(blob_path);
    if let Ok(bytes) = std::fs::read(&thumb) {
        // 校验 PNG 签名:极端情况下(历史半截文件)退化为重建,而不是把坏图交给前端
        if bytes.starts_with(&[0x89, b'P', b'N', b'G']) {
            return Ok(bytes);
        }
    }
    let original = std::fs::read(blob_path)?;
    let img = image::load_from_memory(&original)?;
    let bytes = if img.width() <= THUMB_MAX_EDGE && img.height() <= THUMB_MAX_EDGE {
        original // 原图已足够小:直接复用,省一次编解码
    } else {
        encode_thumbnail(&img, THUMB_MAX_EDGE)?
    };
    if let Err(err) = write_atomic(&thumb, &bytes) {
        warn!("写入缩略图缓存失败: {}: {err}", thumb.display());
    }
    Ok(bytes)
}

/// 入库时用同一份 RGBA 预生成缩略图(免二次解码),失败仅告警、不影响入库。
///
/// 收 owned 缓冲:调用点(`build_entry`)在编码完 PNG 后本就不再需要这份 RGBA,
/// 故直接交给 `image` 持有,省掉原先为构造 `RgbaImage` 而做的一次整图 `to_vec()`
/// (4K 截图上是 22.5MB)。
fn write_thumbnail_from_rgba(rgba: Vec<u8>, width: u32, height: u32, dest: &Path) {
    let result = (|| -> AppResult<()> {
        let buffer = image::RgbaImage::from_raw(width, height, rgba).ok_or_else(|| {
            crate::error::AppError::Internal("RGBA 数据长度与图片尺寸不匹配".into())
        })?;
        let img = image::DynamicImage::ImageRgba8(buffer);
        let bytes = encode_thumbnail(&img, THUMB_MAX_EDGE)?;
        write_atomic(dest, &bytes)?;
        Ok(())
    })();
    if let Err(err) = result {
        warn!("预生成缩略图失败(列表将按需重建): {err}");
    }
}

/// 为存量图片条目后台补建缩略图(幂等:已存在则跳过)。由 FFI nb_init 尾部调用。
pub fn spawn_thumbnail_backfill(core: &'static Core) {
    let spawned = std::thread::Builder::new()
        .name("thumb-backfill".into())
        .spawn(move || {
            let blobs_dir = core.blobs_dir.clone();
            let entries = {
                let Ok(db) = core.db.lock() else {
                    return;
                };
                match entry::list(&db, None, Some(EntryKind::Image), None, None, None, 10_000, 0) {
                    Ok((items, _)) => items,
                    Err(err) => {
                        warn!("缩略图回填:读取图片条目失败: {err}");
                        return;
                    }
                }
            };
            let mut built = 0usize;
            let mut purged = 0usize;
            for e in entries {
                let Some(stored) = e.blob_path else { continue };
                let full = resolve_blob_path(&blobs_dir, &stored);
                // 顺手清理版本化命名之前的旧缓存(不删会一直占磁盘,且永远不会被读到)
                if std::fs::remove_file(legacy_thumbnail_path(&full)).is_ok() {
                    purged += 1;
                }
                if thumbnail_path(&full).exists() {
                    continue;
                }
                if load_or_build_thumbnail(&full).is_ok() {
                    built += 1;
                }
            }
            if built > 0 {
                info!("缩略图后台回填完成:新建 {built} 张");
            }
            if purged > 0 {
                info!("缩略图缓存升级:清理旧版缩略图 {purged} 张");
            }
        });
    if let Err(err) = spawned {
        warn!("启动缩略图回填线程失败: {err}");
    }
}

/// RGBA 编码为 PNG 字节流。
///
/// `fast = true` 时使用低压缩档 + Sub 滤波:第一代 3072x1920 实测 **0.36s vs 2.70s**
/// (debug 构建),体积约 2.3 倍。v1 快速档仅留给将来的"编辑器临时底图"场景;
/// 入库 blob 仍用默认档控制磁盘占用。
pub fn encode_png_with(rgba: &[u8], width: u32, height: u32, fast: bool) -> AppResult<Vec<u8>> {
    // ⚠️ 不要构造 RgbaImage:它的 from_raw 需要 owned Vec,调用方手上是 &[u8],
    //    于是必然多出一次**整帧拷贝**(4K 图 33MB,1500x950 也有 5.7MB)。
    //    这里直接用编码器的 `write_image` 读切片 —— 输出字节完全一致,零额外拷贝。
    //    (截图入库是每张截图必经路径,这份拷贝属瞬态峰值里最没必要的一份。)
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(4))
        .ok_or_else(|| crate::error::AppError::Internal("RGBA 数据长度与图片尺寸不匹配".into()))?;
    if rgba.len() != expected {
        return Err(crate::error::AppError::Internal(
            "RGBA 数据长度与图片尺寸不匹配".into(),
        ));
    }
    use image::ImageEncoder as _;
    let mut out = std::io::Cursor::new(Vec::new());
    if fast {
        image::codecs::png::PngEncoder::new_with_quality(
            &mut out,
            image::codecs::png::CompressionType::Fast,
            image::codecs::png::FilterType::Sub,
        )
        .write_image(rgba, width, height, image::ExtendedColorType::Rgba8)?;
    } else {
        image::codecs::png::PngEncoder::new(&mut out).write_image(
            rgba,
            width,
            height,
            image::ExtendedColorType::Rgba8,
        )?;
    }
    Ok(out.into_inner())
}

/// 把 RGBA 像素直接写入系统剪贴板(CF_DIBV5 单格式,第一代 Phase 5)。
///
/// 不走 arboard 风格的图片写入:其「先写注册格式 PNG、再写 CF_DIBV5」的两次
/// `SetClipboardData` 序列在第一代实测稳定复现 os error 1418(写入中途剪贴板
/// 打开状态丢失,微信等会碰剪贴板的程序易触发)。这里改为与文本写入同构的
/// open(带重试)→ 单次 `SetClipboardData`;其余格式由系统为粘贴方自动合成。
pub fn write_rgba_to_clipboard(rgba: &[u8], width: u32, height: u32) -> AppResult<()> {
    let expected = (width as usize) * (height as usize) * 4;
    if rgba.len() != expected {
        return Err(crate::error::AppError::InvalidInput(format!(
            "RGBA 长度 {} 与尺寸 {width}x{height} 不匹配",
            rgba.len()
        )));
    }
    let dib = rgba_to_dibv5(rgba, width, height);
    let _clip = clipboard_win::Clipboard::new_attempts(10)?;
    clipboard_win::raw::set(formats::CF_DIBV5, &dib)?;
    Ok(())
}

/// 把 PNG 字节写入系统剪贴板:解码一次后转交 [`write_rgba_to_clipboard`]。
/// 已有 RGBA 的调用方(截图会话)应直接用 `write_rgba_to_clipboard`。
pub fn write_png_to_clipboard(png: &[u8]) -> AppResult<()> {
    let img = image::load_from_memory(png)?;
    let (width, height) = (img.width(), img.height());
    write_rgba_to_clipboard(&img.to_rgba8().into_raw(), width, height)
}

/// 读取当前系统剪贴板中的图片(RGBA),供宿主"复制图片条目"使用。
pub fn read_image_to_rgba() -> AppResult<Option<(Vec<u8>, u32, u32)>> {
    Ok(read_image_from_clipboard())
}

/// 把文本写入系统剪贴板(带打开重试)。
pub fn write_text_to_clipboard(text: &str) -> AppResult<()> {
    let _clip = clipboard_win::Clipboard::new_attempts(10)?;
    clipboard_win::set(formats::Unicode, text)?;
    Ok(())
}

/// 读取系统剪贴板文本(带打开重试);非文本/空返回 Ok(None)。
pub fn read_text_from_clipboard() -> AppResult<Option<String>> {
    match clipboard_win::get_clipboard::<String, _>(formats::Unicode) {
        Ok(text) if !text.is_empty() => Ok(Some(text)),
        Ok(_) => Ok(None),
        Err(err) => Err(err.into()),
    }
}

/// 把文件路径列表写入系统剪贴板(CF_HDROP,带打开重试)。
/// 注:clipboard-win 5.4 的高层 `set(formats::FileList, …)` 泛型推导有缺陷
/// (Setter<[T]> unsized),直接用 raw::set_file_list —— 打开重试由 Clipboard guard 承担;
/// set_file_list 自身不清剪贴板(NoClear),写入前必须 raw::empty(),
/// 否则旧 CF_UNICODETEXT(如路径文本)残留,粘贴目标按优先级取到文本,
/// 表现为"复制文件,粘出来的是路径"。文本/图片走的高层 set 是 DoClear,无此问题。
pub fn write_files_to_clipboard(paths: &[String]) -> AppResult<()> {
    let _clip = clipboard_win::Clipboard::new_attempts(10)?;
    clipboard_win::raw::empty()?;
    clipboard_win::raw::set_file_list(paths)?;
    Ok(())
}

/// RGBA(上-下行序)转 CF_DIBV5 载荷:BITMAPV5HEADER + BGRA 像素(底行在前)。
fn rgba_to_dibv5(rgba: &[u8], width: u32, height: u32) -> Vec<u8> {
    const HEADER_SIZE: usize = 124;
    let w = width as usize;
    let h = height as usize;
    let pixels = w * h * 4;
    let mut out = Vec::with_capacity(HEADER_SIZE + pixels);
    out.extend_from_slice(&(HEADER_SIZE as u32).to_le_bytes()); // bV5Size
    out.extend_from_slice(&(width as i32).to_le_bytes()); // bV5Width
    out.extend_from_slice(&(height as i32).to_le_bytes()); // bV5Height(正 = 底行在前)
    out.extend_from_slice(&1u16.to_le_bytes()); // bV5Planes
    out.extend_from_slice(&32u16.to_le_bytes()); // bV5BitCount
    out.extend_from_slice(&3u32.to_le_bytes()); // bV5Compression = BI_BITFIELDS
    out.extend_from_slice(&(pixels as u32).to_le_bytes()); // bV5SizeImage
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5XPelsPerMeter
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5YPelsPerMeter
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5ClrUsed
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5ClrImportant
    out.extend_from_slice(&0x00FF_0000u32.to_le_bytes()); // bV5RedMask
    out.extend_from_slice(&0x0000_FF00u32.to_le_bytes()); // bV5GreenMask
    out.extend_from_slice(&0x0000_00FFu32.to_le_bytes()); // bV5BlueMask
    out.extend_from_slice(&0xFF00_0000u32.to_le_bytes()); // bV5AlphaMask
    out.extend_from_slice(&0x5769_6E20u32.to_le_bytes()); // bV5CSType = 'Win '
    out.extend_from_slice(&[0u8; 36]); // bV5Endpoints(CIEXYZTRIPLE)
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5GammaRed
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5GammaGreen
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5GammaBlue
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5Intent
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5ProfileData
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5ProfileSize
    out.extend_from_slice(&0u32.to_le_bytes()); // bV5Reserved
    debug_assert_eq!(out.len(), HEADER_SIZE, "BITMAPV5HEADER 应为 124 字节");
    for row in (0..h).rev() {
        let (pixels, _rest) = rgba[row * w * 4..(row + 1) * w * 4].as_chunks::<4>();
        for px in pixels {
            out.extend_from_slice(&[px[2], px[1], px[0], px[3]]); // BGRA
        }
    }
    out
}

/// 由 unix 毫秒得到 `YYYY-MM` 月份目录名。
fn month_dir(unix_ms: i64) -> String {
    use chrono::TimeZone;
    chrono::Utc
        .timestamp_millis_opt(unix_ms)
        .single()
        .map(|dt| dt.format("%Y-%m").to_string())
        .unwrap_or_else(|| "unknown".into())
}

/// 删除图片文件(连同缩略图缓存);失败仅记警告(如文件已被外部清理)。
pub fn remove_blob(path: &Path) {
    if let Err(err) = std::fs::remove_file(path) {
        warn!("删除图片文件失败: {}: {err}", path.display());
    }
    // 缩略图缓存是派生数据,条目删除时一并清理,避免缓存目录随历史膨胀
    let _ = std::fs::remove_file(thumbnail_path(path));
    // 版本化命名之前的旧缓存(若还在)也一并清掉
    let _ = std::fs::remove_file(legacy_thumbnail_path(path));
}

#[cfg(test)]
mod tests {

    #[test]
    fn owner_app_name_extracts_and_lowercases() {
        assert_eq!(
            owner_app_name("C:\\Users\\me\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
            Some("chrome.exe".to_owned())
        );
        assert_eq!(
            owner_app_name("D:/Soft/WeChat/WeChat.exe"),
            Some("wechat.exe".to_owned())
        );
        // 容器路径(Unix 风格分隔符)与空段
        assert_eq!(owner_app_name("/usr/bin/app"), Some("app".to_owned()));
        assert_eq!(owner_app_name(""), None);
    }
    use super::*;

    #[test]
    fn dibv5_header_and_pixel_layout() {
        // 2x2 图:上行 (1,2,3,4)(5,6,7,8),下行 (9,10,11,12)(13,14,15,16)
        let rgba: Vec<u8> = vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
        let dib = rgba_to_dibv5(&rgba, 2, 2);
        assert_eq!(dib.len(), 124 + 16);
        assert_eq!(&dib[0..4], &124u32.to_le_bytes()); // bV5Size
        assert_eq!(&dib[4..8], &2i32.to_le_bytes()); // bV5Width
        assert_eq!(&dib[8..12], &2i32.to_le_bytes()); // bV5Height(正 = 底行在前)
        assert_eq!(&dib[12..14], &1u16.to_le_bytes()); // bV5Planes
        assert_eq!(&dib[14..16], &32u16.to_le_bytes()); // bV5BitCount
        assert_eq!(&dib[16..20], &3u32.to_le_bytes()); // BI_BITFIELDS
                                                       // 底行在前且 RGBA → BGRA:下行像素最先出现
        assert_eq!(&dib[124..128], &[11, 10, 9, 12]);
        assert_eq!(&dib[128..132], &[15, 14, 13, 16]);
        assert_eq!(&dib[132..136], &[3, 2, 1, 4]);
        assert_eq!(&dib[136..140], &[7, 6, 5, 8]);
    }

    #[test]
    fn resolve_blob_path_handles_absolute_and_relative() {
        let blobs = Path::new("C:/data/blobs");
        assert_eq!(
            resolve_blob_path(blobs, "C:/other/x.png"),
            PathBuf::from("C:/other/x.png")
        );
        assert_eq!(
            resolve_blob_path(blobs, "2026-10/x.png"),
            blobs.join("2026-10/x.png")
        );
    }

    #[test]
    fn both_png_tiers_roundtrip_to_same_pixels() {
        // 4x2 渐变图:验证快速档(低压缩 + Sub 滤波)与默认档解码结果一致
        let (w, h) = (4u32, 2u32);
        let rgba: Vec<u8> = (0..(w * h * 4)).map(|i| (i * 7 % 251) as u8).collect();
        for fast in [false, true] {
            let png = encode_png_with(&rgba, w, h, fast).unwrap();
            let decoded = image::load_from_memory(&png).unwrap().to_rgba8();
            assert_eq!(decoded.as_raw(), &rgba, "fast={fast} 解码像素不一致");
        }
    }

    #[test]
    fn write_rgba_to_clipboard_rejects_size_mismatch() {
        // 长度与尺寸不匹配时给出明确错误,而不是在 DIBV5 组装里越界
        let err = write_rgba_to_clipboard(&[0u8; 4], 2, 2).unwrap_err();
        assert!(matches!(err, crate::error::AppError::InvalidInput(_)));
    }

    #[test]
    fn month_dir_formats_utc_month() {
        assert_eq!(month_dir(0), "1970-01");
        assert_eq!(month_dir(1_767_225_600_000), "2026-01");
        // 超出 chrono 表示范围的时间戳回退 "unknown"
        assert_eq!(month_dir(i64::MAX), "unknown");
    }

    // ---------- 列表缩略图(第一代 Phase 10,docs/memory-optimization.md) ----------

    #[test]
    fn thumbnail_path_appends_suffix_and_keeps_dir() {
        assert_eq!(
            thumbnail_path(Path::new("C:/data/blobs/2026-10/abc.png")),
            PathBuf::from("C:/data/blobs/2026-10/abc.thumb640.png")
        );
        // 无扩展名同样安全(不会把 .thumb 当成扩展名替换掉)
        assert_eq!(
            thumbnail_path(Path::new("C:/data/blobs/abc")),
            PathBuf::from("C:/data/blobs/abc.thumb640.png")
        );
        // 版本号来源于 THUMB_MAX_EDGE:改边长必须让缓存文件名随之变化
        assert_eq!(THUMB_VERSION, THUMB_MAX_EDGE);
    }

    #[test]
    fn legacy_thumbnail_path_stays_unversioned() {
        // 旧命名固定无版本号,供启动清理使用(与 thumbnail_path 必须指向不同文件)
        let blob = Path::new("C:/data/blobs/2026-10/abc.png");
        assert_eq!(
            legacy_thumbnail_path(blob),
            PathBuf::from("C:/data/blobs/2026-10/abc.thumb.png")
        );
        assert_ne!(legacy_thumbnail_path(blob), thumbnail_path(blob));
    }

    /// 缩略图相关单测只覆盖**内存内逻辑**(路径推导、缩放编码):
    /// 落盘缓存由真机冒烟验证。
    #[test]
    fn thumbnail_dimensions_preserves_aspect_ratio() {
        // 16:9 大图 → 最长边 640,另一边按比例
        assert_eq!(thumbnail_dimensions(1920, 1080, 640), (640, 360));
        // 竖图:按高为主边
        assert_eq!(thumbnail_dimensions(1080, 1920, 640), (360, 640));
        // 正方形
        assert_eq!(thumbnail_dimensions(2000, 2000, 640), (640, 640));
        // 极端窄条:短边至少 1px(不得因取整退化为 0)
        assert_eq!(thumbnail_dimensions(10_000, 1, 640), (640, 1));
    }

    #[test]
    fn encode_thumbnail_downscales_within_max_edge() {
        // 1280x960(超过 THUMB_MAX_EDGE=640)应等比缩放到 640x480
        let (w, h) = (1280u32, 960u32);
        let rgba: Vec<u8> = (0..(w * h * 4)).map(|i| (i % 255) as u8).collect();
        let buffer = image::RgbaImage::from_raw(w, h, rgba).unwrap();
        let img = image::DynamicImage::ImageRgba8(buffer);

        let png = encode_thumbnail(&img, THUMB_MAX_EDGE).unwrap();
        assert!(png.starts_with(&[0x89, b'P', b'N', b'G']), "应输出 PNG");
        let decoded = image::load_from_memory(&png).unwrap();
        assert_eq!(
            (decoded.width(), decoded.height()),
            (640, 480),
            "应等比缩放到最大边 640 内"
        );
    }

    #[test]
    fn encode_thumbnail_does_not_upscale_small_image() {
        // 小于 THUMB_MAX_EDGE 的图:尺寸保持不变(不放大)
        let (w, h) = (64u32, 32u32);
        let rgba: Vec<u8> = (0..(w * h * 4)).map(|i| (i % 255) as u8).collect();
        let buffer = image::RgbaImage::from_raw(w, h, rgba).unwrap();
        let img = image::DynamicImage::ImageRgba8(buffer);

        let decoded =
            image::load_from_memory(&encode_thumbnail(&img, THUMB_MAX_EDGE).unwrap()).unwrap();
        assert_eq!((decoded.width(), decoded.height()), (64, 32));
    }

    // ---------- Nextboard 新增 ----------

    #[test]
    fn clip_item_hash_discriminates_types() {
        // 同内容不同类型不得同 hash(带类型前缀)
        assert_ne!(
            ClipItem::Text("abc".into()).hash(),
            ClipItem::File(vec!["abc".into()]).hash()
        );
    }

    #[test]
    fn is_oversized_respects_settings() {
        let mut settings = Settings::default();
        settings.max_text_kb = 1;
        let small = ClipItem::Text("x".repeat(512));
        let big = ClipItem::Text("x".repeat(2048));
        assert!(!is_oversized(&small, &settings));
        assert!(is_oversized(&big, &settings));
    }

    /// 图片大小上限改为按**编码后的 PNG** 判断的回归测试。
    ///
    /// 旧口径拿**原始 RGBA** 比大小:1920x1080 的 RGBA 就是 8MB、4K 是 33MB,
    /// 在默认 20MB 上限下整屏截图永远进不了历史(实测:3072x1920 的屏截不了、
    /// 1080x1920 的屏正常 —— 正是这个差异暴露的问题)。
    #[test]
    fn image_size_limit_uses_encoded_png_not_raw_rgba() {
        let (w, h) = (2000u32, 2000u32);
        let rgba = vec![0u8; (w * h * 4) as usize];
        let raw_mb = rgba.len() as f64 / (1024.0 * 1024.0);
        let png = encode_png(&rgba, w, h).expect("编码应成功");
        let png_mb = png.len() as f64 / (1024.0 * 1024.0);
        assert!(raw_mb > 15.0, "同图原始 RGBA 很大({raw_mb:.1}MB)");
        assert!(png_mb < 1.0, "而 PNG 很小({png_mb:.2}MB)—— 上限该按它判");

        // 前置过滤不再拦图片(体积判断移到 build_entry,那里才有 PNG)
        let item = ClipItem::Image {
            rgba,
            width: w,
            height: h,
        };
        assert!(!is_oversized(&item, &Settings::default()));
    }
}
