//! SQLite 持久化:连接管理、迁移与条目 CRUD(子模块 `entry`)。
//! 平移自 clipboard-plus src-tauri/src/store,零逻辑改动(02-migration-core.md §2.1)。

pub mod entry;
pub mod todo;
pub mod todo_note;

use std::path::Path;
use std::sync::Mutex;

use crate::error::AppResult;

/// 共享数据库连接。MVP 用互斥锁串行化访问(设计文档 §5.2)。
pub struct Db(pub Mutex<rusqlite::Connection>);

impl Db {
    /// 加锁获取连接;锁中毒(持锁线程 panic)映射为内部错误而非进程崩溃。
    pub fn lock(&self) -> AppResult<std::sync::MutexGuard<'_, rusqlite::Connection>> {
        // ⚠️ 中毒**必须恢复**而不是映射成永久错误(2026-11-03 修):把中毒映射为
        // Err 意味着"持锁线程 panic 一次 → 数据库功能直到进程重启全不可用" ——
        // 而本库的设计恰恰是"崩了也要能继续用"。SQLite 连接本身在持锁线程 panic
        // 时并未处于写到一半的状态(每次调用自成事务),恢复内部值是安全的。
        // 与 translate/http.rs:307、ocr/rapid.rs 的处理同一策略。
        Ok(self.0.lock().unwrap_or_else(|e| e.into_inner()))
    }
}

/// v1:初始表结构(迁移重建测试需要直接执行 v1)。
pub(crate) const MIGRATION_V1: &str = include_str!("migrations/v1.sql");

/// v2:分类过滤索引(Phase 2 F3)。
const MIGRATION_V2: &str = include_str!("migrations/v2.sql");

/// v3:文件类型支持(Phase 3,重建表迁移数据)。
const MIGRATION_V3: &str = include_str!("migrations/v3.sql");

/// v4:全文检索(FTS5)+ OCR 自动入库列(第一代 Phase 9 F27/F28)。
const MIGRATION_V4: &str = include_str!("migrations/v4.sql");

/// v5:一体化素材库扩展(source 来源标记 + tags 标签,Nextboard 架构 §6)。
const MIGRATION_V5: &str = include_str!("migrations/v5.sql");

/// v6:待办(Todo)类型(kind 增 'todo',重建表)+ todo_meta 扩展表(docs/05-todo.md §2)。
pub(crate) const MIGRATION_V6: &str = include_str!("migrations/v6.sql");

/// v7:待办贴屏(桌面便签)窗口状态表 todo_note(docs/20-todo-sticky-note.md §3)。
/// **只加表、不重建 clip_entries** —— 迁移风险低于 v6。
pub(crate) const MIGRATION_V7: &str = include_str!("migrations/v7.sql");

/// v8:待办「备注」列 + FTS 纳入备注(docs/21-todo-title-note.md)。
pub(crate) const MIGRATION_V8: &str = include_str!("migrations/v8.sql");

/// v9:应用来源追踪 source_owner 列(docs/25-source-owner-plan.md)。
/// 只 ALTER 加列,不重建。
pub(crate) const MIGRATION_V9: &str = include_str!("migrations/v9.sql");

/// 当前库结构版本;新增迁移时递增并追加执行分支。
/// ⚠️ 备份/恢复与导入用它做"版本闸门":比它新的备份包一律拒绝(本版代码读不懂
///    更新的 schema,放任打开会静默误读)—— 见 backup_export::stage_backup。
pub(crate) const LATEST_VERSION: i64 = 9;

/// 打开(或创建)数据库文件,启用 WAL 并执行迁移。
pub fn open(path: &Path) -> AppResult<rusqlite::Connection> {
    let mut conn = rusqlite::Connection::open(path)?;
    // journal_mode 会返回新模式的行,pragma_update 不接受返回行,故用 query_row
    let _mode: String = conn.query_row("PRAGMA journal_mode=WAL", [], |row| row.get(0))?;
    // WAL 默认 synchronous=FULL,每个事务 commit 都 fsync;NORMAL 在 WAL 下断电
    // 只丢最后几笔事务、不损坏库 —— 剪贴板历史完全可接受,换掉每次入库的同步盘。
    conn.pragma_update(None, "synchronous", "NORMAL")?;
    // busy_timeout:进程内已有 Mutex 串行化,但备份恢复、外部工具、以及
    // **单实例门禁失效时的第二个进程**都会摸同一个库 —— 没有超时的话
    // WAL 写冲突会立刻返回 SQLITE_BUSY(表现为"复制了但历史里没有")。
    // 3s 覆盖一次正常事务,又不会让 UI 卡到可见。
    conn.busy_timeout(std::time::Duration::from_millis(3000))?;
    migrate(&mut conn)?;
    Ok(conn)
}

/// 按 `PRAGMA user_version` 顺序执行迁移;历史迁移只追加,不修改。
pub fn migrate(conn: &mut rusqlite::Connection) -> AppResult<()> {
    let version: i64 = conn.query_row("PRAGMA user_version", [], |row| row.get(0))?;
    if version < 1 {
        conn.execute_batch(MIGRATION_V1)?;
    }
    if version < 2 {
        conn.execute_batch(MIGRATION_V2)?;
    }
    if version < 3 {
        conn.execute_batch(MIGRATION_V3)?;
    }
    if version < 4 {
        conn.execute_batch(MIGRATION_V4)?;
    }
    if version < 5 {
        conn.execute_batch(MIGRATION_V5)?;
    }
    if version < 6 {
        conn.execute_batch(MIGRATION_V6)?;
    }
    if version < 7 {
        conn.execute_batch(MIGRATION_V7)?;
    }
    if version < 8 {
        conn.execute_batch(MIGRATION_V8)?;
    }
    if version < 9 {
        conn.execute_batch(MIGRATION_V9)?;
    }
    if version < LATEST_VERSION {
        conn.pragma_update(None, "user_version", LATEST_VERSION)?;
    }
    Ok(())
}

/// 测试用:内存库 + 迁移。
#[cfg(test)]
pub(crate) fn memory() -> rusqlite::Connection {
    let mut conn = rusqlite::Connection::open_in_memory().expect("打开内存库失败");
    migrate(&mut conn).expect("内存库迁移失败");
    conn
}