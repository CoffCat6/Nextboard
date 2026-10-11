//! 待办调度:轮询 remind/notify 到点并通知宿主(docs/05-todo.md §4)。
//!
//! 职责边界:CRUD 在 [`crate::store::todo`];本模块只有调度线程 ——
//! 到点给宿主发 `WM_APP_TODO_DUE` 信号(不带载荷),宿主再回查待播报列表
//! (`todo_due_pending`),WM_APP 本身不带 payload(与 CLIP_CHANGED 同哲学)。

use std::sync::OnceLock;
use std::time::Duration;

use crate::core::{notify_host, WM_APP_TODO_DUE};
use crate::store::todo;

/// 轮询间隔:到点精度要求是分钟级,15s 足够且实现简单(§4.1)。
const TICK: Duration = Duration::from_secs(15);

/// 单轮扫描上限:防止同一分钟大量待办同时到点造成气泡轰炸(§8)。
const SCAN_LIMIT: i64 = 8;

/// 本进程启动时刻(毫秒):用于判定"错过的提醒"补发(§3)。
static STARTUP_MS: OnceLock<i64> = OnceLock::new();

/// 本进程启动时刻;首次调用时固化。
pub fn startup_ms() -> i64 {
    *STARTUP_MS.get_or_init(|| {
        // 启动即固化:补发判定必须稳定,不能随扫描时刻漂移
        crate::now_ms()
    })
}

/// 启动调度线程:每 [`TICK`] 扫描一次到期项,命中即写标志位并通知宿主。
pub fn spawn_scheduler(core: &'static crate::core::Core) {
    let spawned = std::thread::Builder::new()
        .name("todo-scheduler".into())
        .spawn(move || {
            log::info!("todo scheduler started");
            loop {
                if crate::core::SHUTDOWN.load(std::sync::atomic::Ordering::SeqCst) {
                    break;
                }
                if let Err(err) = tick(core) {
                    log::warn!("todo scheduler tick 失败: {err}");
                }
                std::thread::sleep(TICK);
            }
            log::info!("todo scheduler stopped");
        });
    if spawned.is_err() {
        log::warn!("todo scheduler 线程启动失败");
    }
}

/// 单轮扫描:remind / notify 各自"未完成、未触发、已到点"的命中。
///
/// 触发即写标志位(不依赖宿主确认),避免宿主未起时反复轰炸;
/// "宿主未起"场景由下次启动的首轮扫描兜住(§3 补发)。
fn tick(core: &'static crate::core::Core) -> crate::error::AppResult<()> {
    let now = crate::now_ms();
    let db = core.db.lock()?;
    let mut hit = false;
    for item in todo::due_reminds(&db, now, SCAN_LIMIT)? {
        todo::mark_reminded(&db, &item.id)?;
        log::info!("todo remind due: {} ({})", item.id, first_line(&item.text));
        hit = true;
    }
    for item in todo::due_notifies(&db, now, SCAN_LIMIT)? {
        todo::mark_notified(&db, &item.id)?;
        log::info!("todo notify due: {} ({})", item.id, first_line(&item.text));
        hit = true;
    }
    drop(db);
    if hit {
        notify_host(WM_APP_TODO_DUE);
    }
    Ok(())
}

/// 正文首行(日志与气泡标题用)。
fn first_line(text: &str) -> &str {
    text.lines().next().unwrap_or("")
}
