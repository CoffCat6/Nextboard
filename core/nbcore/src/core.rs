//! 核心共享状态:数据库、目录、设置(取代第一代 AppState 的 tauri Manager 模型)。
//!
//! 生命周期:`Core::init` 由 FFI `nb_init` 调用创建,存进程级 `OnceLock`;
//! `Core::shutdown` 由 `nb_shutdown` 调用。窗口相关能力(托盘/热键/窗口几何)
//! 全部在 C++ 宿主,核心只持有"通知 HWND"用于向宿主推送事件(02-migration-core.md §4.3)。

use std::path::PathBuf;
use std::sync::{Mutex, OnceLock, RwLock};

use crate::error::{AppError, AppResult};
use crate::settings::Settings;
use crate::store::Db;
use crate::{settings, store};

/// 通知消息:WM_APP 基址 + 偏移。与 docs/02-migration-core.md §4.3 的表一致。
pub const WM_APP_CLIP_CHANGED: u32 = 0x8000 + 1;
pub const WM_APP_SETTINGS_CHANGED: u32 = 0x8000 + 2;
/// 待办到点(remind/notify):只当信号,宿主收到后回查待播报列表(docs/05-todo.md §4.2)。
///
/// ⚠️ 取 +4 而非 +3:+3 已为 `CAPTURE_READY`(截图会话流)预留,虽暂未消费也是
///    规划位;+10 已被单实例广播占用(`single_instance.cpp`)。
pub const WM_APP_TODO_DUE: u32 = 0x8000 + 4;

/// 通知宿主(若有):PostMessage 不阻塞、不跨线程持有宿主指针。
/// 宿主收到后按拉模式回查核心(拉模式避免 FFI 回调的线程与重入问题)。
#[cfg(windows)]
pub fn notify_host(msg: u32) {
    use std::sync::atomic::Ordering;

    let hwnd = NOTIFY_HWND.load(Ordering::SeqCst);
    if hwnd == 0 {
        return;
    }
    // SAFETY: PostMessageW 只投递消息,不解引用窗口对象;hwnd 由宿主保证有效,
    // 宿主销毁窗口前必须先调 nb_shutdown 置零(见 ffi 文档约定)。
    unsafe {
        let _ = windows::Win32::UI::WindowsAndMessaging::PostMessageW(
            Some(windows::Win32::Foundation::HWND(hwnd as *mut _)),
            msg,
            windows::Win32::Foundation::WPARAM(0),
            windows::Win32::Foundation::LPARAM(0),
        );
    }
}

// Linux(M1/R3):通知走 **管道 fd**(宿主持读端接 QSocketNotifier,核心写 4 字节
// 消息 id)。信号语义与 Windows 的 PostMessageW 完全一致:不阻塞、不携带指针、
// 消息只是"该回查了"的信号,payload 由宿主拉模式回取。
#[cfg(not(windows))]
static NOTIFY_FD: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(-1);

/// 运行期更新通知 fd(FFI nb_set_notify_fd 调用;负数 = 通知丢弃)。
#[cfg(not(windows))]
pub fn set_notify_fd(fd: i32) {
    use std::sync::atomic::Ordering;
    NOTIFY_FD.store(fd, Ordering::SeqCst);
}

#[cfg(not(windows))]
pub fn notify_host(msg: u32) {
    use std::sync::atomic::Ordering;

    let fd = NOTIFY_FD.load(Ordering::SeqCst);
    if fd < 0 {
        return;
    }
    let bytes = msg.to_ne_bytes();
    // SAFETY: fd 由宿主经 nb_set_notify_fd 登记且保持打开(进程级生命周期);
    // 用 ManuallyDrop 包装避免 File 析构关闭宿主的 fd。
    // 写失败(EPIPE 宿主已退 / 非阻塞满)一律静默 —— 通知是信号,不是必须送达的数据。
    let mut file = std::mem::ManuallyDrop::new(unsafe {
        std::fs::File::from_raw_fd(fd)
    });
    let _ = std::io::Write::write_all(&mut *file, &bytes);
}

#[cfg(not(windows))]
use std::os::unix::io::FromRawFd;

#[cfg(windows)]
static NOTIFY_HWND: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// 运行期更新通知 HWND(FFI nb_set_notify_hwnd 调用;0 = 通知丢弃)。
#[cfg(windows)]
pub fn set_notify_hwnd(hwnd: usize) {
    use std::sync::atomic::Ordering;
    NOTIFY_HWND.store(hwnd, Ordering::SeqCst);
}

#[cfg(not(windows))]
pub fn set_notify_hwnd(_hwnd: usize) {}

/// 核心配置(FFI nb_init 的 JSON 入参)。
#[derive(Debug, Clone)]
pub struct CoreConfig {
    pub data_dir: PathBuf,
    /// Windows:宿主通知窗口句柄;其他平台传 0(通知走空实现)。
    /// M1 重设计通知协议(nb_set_notify_fd)时本字段将随整体重构(docs/29 §5.2)。
    pub notify_hwnd: usize,
}

/// 全局共享状态。
pub struct Core {
    pub db: Db,
    /// 图片文件目录(`<数据目录>/blobs`)。
    pub blobs_dir: PathBuf,
    /// 当前生效的数据目录(设置里启用自定义目录时与 default_dir 不同)。
    pub effective_dir: PathBuf,
    /// 默认数据目录(迁移标记所在处)。
    pub default_dir: PathBuf,
    settings: RwLock<Settings>,
}

static CORE: OnceLock<Core> = OnceLock::new();

impl Core {
    /// 初始化核心:开库迁移、建目录、装设置。
    pub fn init(config: CoreConfig) -> AppResult<()> {
        let loaded = settings::load(&config.data_dir)?;

        // 自定义数据目录:沿用第一代的待迁移标记机制(docs/phase4.md)
        let data_dir = settings::effective_data_dir(&loaded, &config.data_dir);
        settings::take_pending_move(&config.data_dir, &data_dir);
        std::fs::create_dir_all(&data_dir)?;
        let db = store::open(&data_dir.join("clipboard.db"))?;
        let blobs_dir = data_dir.join("blobs");
        std::fs::create_dir_all(&blobs_dir)?;

        #[cfg(windows)]
        {
            use std::sync::atomic::Ordering;
            NOTIFY_HWND.store(config.notify_hwnd, Ordering::SeqCst);
        }

        CORE.set(Core {
            db: Db(Mutex::new(db)),
            blobs_dir,
            effective_dir: data_dir,
            default_dir: config.data_dir,
            settings: RwLock::new(loaded),
        })
        .map_err(|_| AppError::Internal("核心已初始化,不能重复 nb_init".into()))?;
        Ok(())
    }

    /// 取全局核心;未初始化(或已 shutdown)时返回错误,FFI 层转错误码。
    pub fn get() -> AppResult<&'static Core> {
        CORE.get()
            .ok_or_else(|| AppError::Internal("核心未初始化".into()))
    }

    /// 是否已初始化(测试与 FFI 状态探测用)。
    pub fn ready() -> bool {
        CORE.get().is_some()
    }

    /// 关闭:清全局引用后的资源(连接在 Drop 时落盘)。
    /// 监听线程靠 `try_core()` 检测退出,不在此 join(进程退出随主线程结束)。
    pub fn shutdown() {
        if let Some(core) = CORE.get() {
            if let Ok(guard) = core.settings.write() {
                // 设置以 Core 持有的为准落盘一次,防最后一次 save_settings 后有内存漂移
                let _ = settings::save(&core.default_dir, &guard.clone());
            }
        }
        // OnceLock 无法清空:进程生命周期内 nb_init 只允许一次;
        // 监听线程感知 shutdown 用 SHUTDOWN 标志(见 clip::monitor)
        SHUTDOWN.store(true, std::sync::atomic::Ordering::SeqCst);
        #[cfg(windows)]
        NOTIFY_HWND.store(0, std::sync::atomic::Ordering::SeqCst);
    }
}

/// 关闭标志:监听线程每轮检查(事件驱动的 GetMessage 阻塞由 PostThreadMessage 解除,
/// 简化起见 v1 用短超时轮询标志,代价可忽略)。
pub(crate) static SHUTDOWN: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

/// 兼容第一代调用的快捷访问(clip/ffi 内部使用)。
pub(crate) fn try_core() -> Option<&'static Core> {
    if SHUTDOWN.load(std::sync::atomic::Ordering::SeqCst) {
        return None;
    }
    CORE.get()
}

impl Core {
    pub fn settings(&self) -> AppResult<Settings> {
        self.settings
            .read()
            .map(|s| s.clone())
            .map_err(|_| AppError::Internal("设置锁中毒".into()))
    }

    pub fn set_settings(&self, settings: Settings) -> AppResult<()> {
        // 设置变更后 OCR 引擎的可用性/后端可能变化 → 释放已建 session,下次识别按新设置重建。
        // (session 空闲销毁见 ocr::rapid,这里是"立即生效"路径。)
        crate::ocr::rapid::release_session();
        settings::save(&self.default_dir, &settings)?;
        self.settings
            .write()
            .map(|mut guard| *guard = settings)
            .map_err(|_| AppError::Internal("设置锁中毒".into()))?;
        notify_host(WM_APP_SETTINGS_CHANGED);
        Ok(())
    }
}

/// 第一代 lib.rs `now_ms` 的等价物已提升到 crate 根。
pub use crate::now_ms;
