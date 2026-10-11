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

pub mod capture;
pub mod clip;
pub mod core;
pub mod error;
pub mod ffi;
pub mod ocr;
pub mod screen;
pub mod settings;
pub mod store;
pub mod todo;
pub mod translate;

/// 初始化日志(log crate → 数据目录 nbcore.log)。
/// 必须在任何日志调用前执行(nb_init 首行);多次调用无害。
pub fn init_logging(data_dir: &std::path::Path) {
    let file_path = data_dir.join("nbcore.log");
    let file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&file_path);
    if let Ok(file) = file {
        let _ = log::set_boxed_logger(Box::new(SimpleFileLogger {
            file: std::sync::Mutex::new(file),
        }));
        log::set_max_level(log::LevelFilter::Info);
    }
}

/// 极简文件日志器(单文件追加;排障用,不追求轮转)。
struct SimpleFileLogger {
    file: std::sync::Mutex<std::fs::File>,
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
        if let Ok(mut f) = self.file.lock() {
            let _ = writeln!(
                f,
                "[{}] [{}] {}",
                chrono::Local::now().format("%Y-%m-%d %H:%M:%S%.3f"),
                record.level(),
                record.args()
            );
        }
    }

    fn flush(&self) {
        use std::io::Write;
        if let Ok(mut f) = self.file.lock() {
            let _ = f.flush();
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
