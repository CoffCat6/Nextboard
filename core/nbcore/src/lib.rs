//! nbcore:Nextboard 业务核心。
//!
//! 从 clipboard-plus 剥离的业务层(store/clip/translate/ocr/settings),
//! 以 cdylib 形态交付 nbcore.dll 供 Qt 宿主调用;对外契约见
//! docs/02-migration-core.md §4(JSON 门面 + RGBA 通道 + PostMessage 通知)。
//!
//! 本 crate 不得引用任何 tauri 类型(02-migration-core.md §1 总原则)。

// cdylib 链接时 MSVC 必然产出 .lib/.exp 导入库,link.exe 会把这件事打印到
// stdout,rustc 的 `linker_messages` lint 将其转发为 warning —— 纯噪音
// (每个版本 2 行),真实链接错误仍会以 error 报告,不受本 allow 影响。
#![allow(linker_messages)]

pub mod backup_export;
pub mod capture;
pub mod clip;
pub mod core;
pub mod error;
pub mod ffi;
pub mod ocr;
pub mod qr;
pub mod settings;
pub mod stitch;
pub mod store;
pub mod todo;
pub mod translate;
pub mod update;

/// 单份日志的大小上限(字节)。与宿主侧 `app/src/host/log_rotate.hpp` 的
/// `kLogRotateBytes` 取同一数值:两个文件都是排障用的,不该一个 2MB 一个 40MB。
pub const LOG_ROTATE_BYTES: u64 = 2 * 1024 * 1024;   // 2MB

/// 初始化日志(log crate → 数据目录 nbcore.log)。
/// 必须在任何日志调用前执行(nb_init 首行);多次调用无害。
///
/// 单代滚动(与宿主 nextboard.log 同一套语义):打开前若已超阈值,先
/// `nbcore.log` → `nbcore.log.1`(覆盖旧归档)再新建。托盘常驻程序一开几周,
/// FTS/OCR/剪贴板事件源源不断,不设上限会把用户磁盘写满。
pub fn init_logging(data_dir: &std::path::Path) {
    let file_path = data_dir.join("nbcore.log");
    rotate_log_if_needed(&file_path, LOG_ROTATE_BYTES);
    let file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&file_path);
    if let Ok(file) = file {
        // 已有大小作为累计起点(见 LoggerState::written 注释)。
        let written = file.metadata().map(|m| m.len()).unwrap_or(0);
        let _ = log::set_boxed_logger(Box::new(SimpleFileLogger {
            file: std::sync::Mutex::new(LoggerState {
                writer: Some(std::io::LineWriter::new(file)),
                written,
                path: file_path.clone(),
            }),
        }));
        log::set_max_level(log::LevelFilter::Info);
    }
    install_panic_logger();
}

/// 全局 panic 钩子:panic 消息+位置+线程名落 nbcore.log。
///
/// 为什么必须有:默认钩子只打 stderr,而托盘应用的 stderr 常被吞(宿主以
/// `>/dev/null` 拉起是常态)——2026-11-08 排查 K6 时,剪贴板 monitor 线程
/// 中途死亡只剩下游痕迹(arboard 的 manager 移交 WARN),panic 本体彻底丢失。
/// 记录后线程 unwind 行为不变(钩子只记录,不 abort),FFI 的 catch_unwind
/// 拦截照常工作。先取默认钩子再链式调用:保留 stderr 行为,调试时
/// RUST_BACKTRACE 依旧有效。
fn install_panic_logger() {
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let loc = info
            .location()
            .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
            .unwrap_or_else(|| "<未知位置>".into());
        let thread = std::thread::current();
        let thread_name = thread.name().unwrap_or("<unnamed>");
        let msg = if let Some(s) = info.payload().downcast_ref::<&str>() {
            (*s).to_string()
        } else if let Some(s) = info.payload().downcast_ref::<String>() {
            s.clone()
        } else {
            "(非字符串 payload)".into()
        };
        log::error!("panic 线程[{thread_name}] @ {loc}: {msg}");
        default_hook(info);
    }));
}

/// 若 `path` 已达/超过 `limit` 字节,把它改名为 `<path>.1`(覆盖旧归档)。
///
/// 返回是否滚动成功;不存在 / 未超限 / 改名失败都返回 false —— **滚动失败
/// 不影响写日志**,下次再试,不让排障工具本身变成故障源。
pub fn rotate_log_if_needed(path: &std::path::Path, limit: u64) -> bool {
    let Ok(meta) = std::fs::metadata(path) else {
        return false;
    };
    if meta.len() < limit {
        return false;
    }
    let mut target = path.as_os_str().to_owned();
    target.push(".1");
    let target = std::path::PathBuf::from(target);
    // 先删旧归档:Windows 上 rename 到已存在目标会失败;删不掉(被占用)则
    // 本次放弃滚动,保留现状继续写 —— 宁可日志大一点,也不能丢诊断能力。
    let _ = std::fs::remove_file(&target);
    std::fs::rename(path, &target).is_ok()
}

/// 极简文件日志器(单文件追加 + 单代滚动;排障用,不追求多代保留)。
///
/// **每行一次 write(2)**:`LineWriter` 见 `\n` 即写穿到 OS 页缓存。此前用
/// 8KB BufWriter、仅 Warn+ flush —— Info 行要等缓冲攒满才见系统调用,
/// 2026-11-08 在 Ubuntu VM 实测暴露:干净启动零 WARN + 强杀退出 =
/// nbcore.log 0 字节,Info 字节全堵在用户态缓冲里从未到达 OS(strace 证实
/// openat 与时间戳格式化俱在、write 调用为零;宿主 nextboard.log 踩到 Qt
/// QIODevice 的同款用户态写缓冲,docs/29 §9.1 K9)。日志量级(轮询事件,
/// 每秒 ~10 行)下逐行 write(2) 的开销可忽略,可靠性优先。
///
/// 滚动:累计已写字节数,跨过 `LOG_ROTATE_BYTES` 时把当前文件改名归档并新建
/// 一个空文件继续写。**先写后滚**:那条让文件越线的日志先落盘,再换文件 ——
/// 崩溃前的最后一条不会因为滚动而丢。
struct SimpleFileLogger {
    file: std::sync::Mutex<LoggerState>,
}

struct LoggerState {
    /// `None` = 文件句柄已被主动释放(滚动改名期间)。取 `Option` 而不是裸
    /// LineWriter,是因为**Windows 上被本进程打开着的文件 rename 会静默失败** ——
    /// 必须先 drop 掉持有它的 writer 才能把 `x.log` 改名成 `x.log.1`。
    writer: Option<std::io::LineWriter<std::fs::File>>,
    /// 当前文件已写字节数(打开时从文件实际大小起算,而不是 0 —— 否则
    /// 上次攒下的接近上限的日志要等到本次再写满一个阈值才会滚动)。
    written: u64,
    path: std::path::PathBuf,
}

impl log::Log for SimpleFileLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= log::Level::Info
    }

    fn log(&self, record: &log::Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        use std::io::Write;
        let Ok(mut state) = self.file.lock() else {
            return;
        };
        let line = format!(
            "[{}] [{}] {}\n",
            chrono::Local::now().format("%Y-%m-%d %H:%M:%S%.3f"),
            record.level(),
            record.args()
        );
        if let Some(writer) = state.writer.as_mut() {
            let _ = writer.write_all(line.as_bytes());
        }
        state.written += line.len() as u64;
        if record.level() <= log::Level::Warn {
            if let Some(writer) = state.writer.as_mut() {
                let _ = writer.flush();
            }
        }
        // 跨阈值:先 flush(缓冲里还有数据,不刷会跟着旧句柄进归档位),
        // 再 **drop 掉 writer 释放句柄**,才做 rename,最后打开新文件继续写。
        // 顺序错了的表现是"阈值到了却永远不滚动"(Windows 静默失败,2026-11-02 实测)。
        if state.written >= LOG_ROTATE_BYTES && state.writer.is_some() {
            if let Some(mut writer) = state.writer.take() {
                let _ = writer.flush();
            }
            if rotate_log_if_needed(&state.path, LOG_ROTATE_BYTES) {
                state.written = 0;
            }
            // 无论滚动成功与否都要重新打开:成功 = 新空文件;失败(被占用)= 原文件续写。
            // 失败时 written 保持原值,下一行会再试一次。
            if let Ok(file) = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&state.path)
            {
                state.writer = Some(std::io::LineWriter::new(file));
            }
        }
    }

    fn flush(&self) {
        use std::io::Write;
        if let Ok(mut state) = self.file.lock() {
            if let Some(writer) = state.writer.as_mut() {
                let _ = writer.flush();
            }
        }
    }
}

/// 当前 Unix 毫秒时间戳;系统时钟异常时回退 0。
pub fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::rotate_log_if_needed;

    /// 临时目录:放 `<manifest>/target/tmp` —— 受限环境(%TEMP% 不可写)下也能跑,
    /// 与 settings.rs 的 tmp_dir 同一做法。
    fn tmp_dir(tag: &str) -> std::path::PathBuf {
        let base = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("tmp");
        let _ = std::fs::create_dir_all(&base);
        let dir = base.join(format!("nb_logrotate_{tag}_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn rotate_skips_when_under_limit() {
        let dir = tmp_dir("under");
        let path = dir.join("x.log");
        std::fs::write(&path, b"hello").unwrap();
        assert!(!rotate_log_if_needed(&path, 1024), "未达阈值不滚动");
        assert!(path.exists(), "原文件仍在");
        assert!(!dir.join("x.log.1").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rotate_skips_when_missing() {
        let dir = tmp_dir("missing");
        let path = dir.join("nope.log");
        assert!(!rotate_log_if_needed(&path, 0), "文件不存在返回 false 而非 panic");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rotate_moves_file_to_single_generation() {
        let dir = tmp_dir("move");
        let path = dir.join("x.log");
        std::fs::write(&path, b"0123456789").unwrap();
        assert!(rotate_log_if_needed(&path, 5), "超限应滚动");
        assert!(!path.exists(), "原文件已改名");
        assert_eq!(
            std::fs::read(dir.join("x.log.1")).unwrap(),
            b"0123456789",
            "内容进归档位"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rotate_overwrites_previous_generation() {
        let dir = tmp_dir("overwrite");
        let path = dir.join("x.log");
        std::fs::write(dir.join("x.log.1"), b"OLD").unwrap();
        std::fs::write(&path, b"NEW").unwrap();
        assert!(rotate_log_if_needed(&path, 1));
        assert_eq!(
            std::fs::read(dir.join("x.log.1")).unwrap(),
            b"NEW",
            "旧归档被覆盖,单代不累积(.log.2 之类永远不会出现)"
        );
        assert!(!dir.join("x.log.2").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
