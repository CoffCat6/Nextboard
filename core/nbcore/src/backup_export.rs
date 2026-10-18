//! 数据备份 / 导出 / 恢复 / 导入(S5.3 相关规划,docs/03-roadmap.md 5.3 的配套能力)。
//!
//! - 备份:`backup_create` —— 数据目录完整快照(zip 打包 clipboard.db + blobs/ +
//!   manifest.json)。⚠️ WAL 坑(progress.md:3085):必须在**同一连接锁内**先
//!   `PRAGMA wal_checkpoint(TRUNCATE)` 再拷 .db,只拷 .db 不带 -wal/-shm 才安全;
//!   持有 `Core.db` 互斥锁的窗口内业务写入被串行化,快照一致。
//! - 导出:`export_entries` —— 剪贴板历史渲染为 Markdown(图片 base64 内嵌,
//!   单文件自包含)。HTML 为 backlog,format 传 "html" 显式报错不静默。
//! - 恢复:`restore_backup` —— 选备份 zip → **替换**当前数据。执行前自动给当前
//!   数据打险备份 zip(恢复源同目录,`nextboard-pre-restore-*.zip`);解包/迁移
//!   全部在 staging 里预演通过后才动真数据,锁内换连接 + 换 blobs 目录,失败回滚。
//! - 导入:`import_backup` —— 备份 zip **并入**当前数据:按内容 hash 去重(重复
//!   跳过、现有条目不动),id 冲突重铸新 id,blob 文件随条目成对搬迁,todo_meta
//!   跟随条目。
//!
//! ⚠️ 恢复/导入的 schema 闸门:`manifest.schema_version` 与实际 `user_version`
//!    都不得高于 `store::LATEST_VERSION`(比本版新的数据读不懂,静默打开会误读)。

use std::collections::HashMap;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use chrono::Local;
use rusqlite::{params, Connection};

use crate::core::Core;
use crate::error::{AppError, AppResult};

/// 导出分页批量大小:条目流式写文件,内存只持有当前批。
const EXPORT_BATCH: i64 = 500;

/// 解包暂存目录名(数据目录下)。固定名:上次运行的残留会被下次开跑先清掉。
const STAGING_DIR: &str = "restore-staging";

// ---------------------------------------------------------------------------
// 备份
// ---------------------------------------------------------------------------

/// 创建备份 zip。`out_path` 由宿主(QFileDialog)提供;同名文件直接覆盖。
pub fn backup_create(core: &Core, out_path: &str) -> AppResult<serde_json::Value> {
    let started = std::time::Instant::now();
    let db = core.db.lock()?;
    let result = backup_snapshot(&db, &core.effective_dir, Path::new(out_path))?;
    Ok(serde_json::json!({
        "entryCount": result.entry_count,
        "blobCount": result.blob_count,
        "zipBytes": result.zip_bytes,
        "durationMs": started.elapsed().as_millis() as u64,
    }))
}

struct SnapshotResult {
    entry_count: i64,
    blob_count: usize,
    zip_bytes: u64,
}

/// 核心备份路径:checkpoint → 读 db 文件 → 打包。连接与锁由调用方保证。
fn backup_snapshot(
    conn: &Connection,
    data_dir: &Path,
    out_path: &Path,
) -> AppResult<SnapshotResult> {
    // WAL checkpoint:把 -wal 合并回主库并截断,之后只拷 .db 即完整
    // (progress.md:3085 的教训:停应用拷目录可以,只拷 .db 不 checkpoint 会丢数据)。
    let _: (i64, i64, i64) = conn.query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |r| {
        Ok((r.get(0)?, r.get(1)?, r.get(2)?))
    })?;
    let schema_version: i64 =
        conn.query_row("PRAGMA user_version", [], |row| row.get(0))?;
    let entry_count: i64 =
        conn.query_row("SELECT COUNT(*) FROM clip_entries", [], |row| row.get(0))?;

    let db_path = data_dir.join("clipboard.db");
    let db_bytes = std::fs::read(&db_path)?;

    let out_file = std::fs::File::create(out_path)?;
    let mut zip = zip::ZipWriter::new(out_file);
    let options =
        zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);

    // manifest:自描述快照;恢复(未来)用它校验 schema 版本与 blob 清单完整性。
    let manifest = serde_json::json!({
        "app": "nextboard",
        "schema_version": schema_version,
        "created_at": crate::now_ms() / 1000,
        "entry_count": entry_count,
    });
    zip.start_file("manifest.json", options)
        .map_err(zip_err)?;
    zip.write_all(manifest.to_string().as_bytes())?;

    zip.start_file("clipboard.db", options).map_err(zip_err)?;
    zip.write_all(&db_bytes)?;

    // blobs/:递归收录全部图片文件(数据目录迁移兼容:历史条目可能有子目录)。
    let blobs_dir = data_dir.join("blobs");
    let mut blob_count = 0usize;
    if blobs_dir.is_dir() {
        let mut names = Vec::new();
        collect_files(&blobs_dir, Path::new(""), &mut names)?;
        for rel in &names {
            let full = blobs_dir.join(rel);
            let bytes = std::fs::read(&full)?;
            // zip 内路径用正斜杠(zip 规范;Windows 下也如此)
            let zip_name = format!("blobs/{}", rel.to_string_lossy().replace('\\', "/"));
            zip.start_file(zip_name, options).map_err(zip_err)?;
            zip.write_all(&bytes)?;
            blob_count += 1;
        }
    }
    let zip_bytes = zip.finish().map_err(zip_err)?.metadata()?.len();

    log::info!(
        "backup_create: {} 条目 / {} blobs / {} bytes -> {}",
        entry_count,
        blob_count,
        zip_bytes,
        out_path.display()
    );
    Ok(SnapshotResult { entry_count, blob_count, zip_bytes })
}

/// 递归收集 `root` 下的文件,`rel` 为相对路径前缀(空 = 根)。
fn collect_files(root: &Path, rel: &Path, out: &mut Vec<std::path::PathBuf>) -> AppResult<()> {
    let dir = root.join(rel);
    for item in std::fs::read_dir(&dir)? {
        let item = item?;
        let child_rel = rel.join(item.file_name());
        if item.file_type()?.is_dir() {
            collect_files(root, &child_rel, out)?;
        } else {
            out.push(child_rel);
        }
    }
    Ok(())
}

fn zip_err(err: zip::result::ZipError) -> AppError {
    AppError::Internal(format!("zip 打包失败: {err}"))
}

// ---------------------------------------------------------------------------
// 导出(Markdown)
// ---------------------------------------------------------------------------

/// 导出全部条目为 Markdown 单文件。format 仅支持 "markdown"。
pub fn export_entries(core: &Core, out_path: &str, format: &str) -> AppResult<serde_json::Value> {
    if format != "markdown" {
        return Err(AppError::InvalidInput(format!(
            "不支持的导出格式: {format}(当前仅 markdown,HTML 在 backlog)"
        )));
    }
    let started = std::time::Instant::now();
    let db = core.db.lock()?;
    let result = export_markdown(&db, &core.blobs_dir, Path::new(out_path))?;
    log::info!(
        "export_entries: {} 条目 / {} bytes -> {}",
        result.entry_count,
        result.file_bytes,
        out_path
    );
    Ok(serde_json::json!({
        "entryCount": result.entry_count,
        "fileBytes": result.file_bytes,
        "durationMs": started.elapsed().as_millis() as u64,
    }))
}

struct ExportResult {
    entry_count: u64,
    file_bytes: u64,
}

/// 核心导出路径:流式查询 + 逐条渲染。连接与锁由调用方保证。
fn export_markdown(conn: &Connection, blobs_dir: &Path, out_path: &Path) -> AppResult<ExportResult> {
    let out_file = std::fs::File::create(out_path)?;
    let mut w = BufWriter::new(out_file);

    writeln!(w, "# Nextboard 剪贴板历史导出")?;
    writeln!(w)?;
    writeln!(w, "- 导出时间:{}", Local::now().format("%Y-%m-%d %H:%M:%S"))?;
    let total: i64 = conn.query_row("SELECT COUNT(*) FROM clip_entries", [], |r| r.get(0))?;
    writeln!(w, "- 条目数:{total}")?;
    writeln!(w)?;

    // 流式分页:ORDER BY 与主列表一致(固定优先、最近在前)。
    let mut offset: i64 = 0;
    let mut count: u64 = 0;
    loop {
        let batch = load_batch(conn, EXPORT_BATCH, offset)?;
        let batch_len = batch.len() as i64;
        if batch_len == 0 {
            break;
        }
        for e in &batch {
            render_entry(&mut w, e, blobs_dir)?;
            count += 1;
        }
        offset += batch_len;
        if batch_len < EXPORT_BATCH {
            break;
        }
    }
    w.flush()?;
    let file_bytes = w.get_ref().metadata()?.len();
    Ok(ExportResult { entry_count: count, file_bytes })
}

struct RawEntry {
    kind: String,
    text_content: Option<String>,
    blob_path: Option<String>,
    file_paths: Option<String>,
    width: Option<i64>,
    height: Option<i64>,
    ocr_text: Option<String>,
    pinned: bool,
    created_at: i64,
    /// 条目来源(导出文档暂不展示,保留字段与 DB 对齐;标注以过 dead_code 检查)。
    #[allow(dead_code)]
    source: String,
    todo_done: Option<i64>,
    todo_due_at: Option<i64>,
}

fn load_batch(conn: &Connection, limit: i64, offset: i64) -> AppResult<Vec<RawEntry>> {
    let mut stmt = conn.prepare(
        "SELECT e.kind, e.text_content, e.blob_path, e.file_paths, e.width, e.height,
                e.ocr_text, e.pinned, e.created_at, e.source,
                m.done_at AS todo_done_at, m.due_at AS todo_due_at
         FROM clip_entries e
         LEFT JOIN todo_meta m ON m.id = e.id
         ORDER BY e.pinned DESC, e.last_used_at DESC
         LIMIT ?1 OFFSET ?2",
    )?;
    let rows = stmt.query_map(params![limit, offset], |row| {
        Ok(RawEntry {
            kind: row.get(0)?,
            text_content: row.get(1)?,
            blob_path: row.get(2)?,
            file_paths: row.get(3)?,
            width: row.get(4)?,
            height: row.get(5)?,
            ocr_text: row.get(6)?,
            pinned: row.get::<_, i64>(7)? != 0,
            created_at: row.get(8)?,
            source: row.get(9)?,
            todo_done: row.get(10)?,
            todo_due_at: row.get(11)?,
        })
    })?;
    let mut out = Vec::new();
    for r in rows {
        out.push(r?);
    }
    Ok(out)
}

/// 单条目渲染。section 之间空一行;固定条目标 📌(与主列表一致)。
fn render_entry(w: &mut impl Write, e: &RawEntry, blobs_dir: &Path) -> AppResult<()> {
    use chrono::TimeZone;
    let ts = chrono::Local
        .timestamp_millis_opt(e.created_at)
        .single()
        .map(|t| t.format("%Y-%m-%d %H:%M:%S").to_string())
        .unwrap_or_else(|| e.created_at.to_string());
    let pin = if e.pinned { " 📌" } else { "" };
    writeln!(w, "## {ts} · {}{pin}", kind_label(&e.kind))?;
    writeln!(w)?;

    match e.kind.as_str() {
        "text" | "todo" => {
            let body = e.text_content.as_deref().unwrap_or("");
            if e.kind == "todo" {
                let done = e.todo_done.is_some();
                writeln!(w, "- [{}] {body}", if done { "x" } else { " " })?;
                if let Some(due) = e.todo_due_at {
                    let due_ts = chrono::Local
                        .timestamp_millis_opt(due)
                        .single()
                        .map(|t| t.format("%Y-%m-%d %H:%M").to_string())
                        .unwrap_or_else(|| due.to_string());
                    writeln!(w, "  - 截止:{due_ts}")?;
                }
            } else {
                // 围栏转义:正文含 ``` 时升级为 4 反引号(足够覆盖常见场景)。
                let fence = if body.contains("```") { "````" } else { "```" };
                writeln!(w, "{fence}text")?;
                writeln!(w, "{body}")?;
                writeln!(w, "{fence}")?;
            }
        }
        "image" => {
            let dims = match (e.width, e.height) {
                (Some(wd), Some(ht)) => format!(" ({wd}×{ht})"),
                _ => String::new(),
            };
            writeln!(w, "图片{dims}:")?;
            match e.blob_path.as_deref() {
                Some(stored) => {
                    let full = crate::clip::resolve_blob_path(blobs_dir, stored);
                    match std::fs::read(&full) {
                        Ok(bytes) => {
                            use base64::Engine as _;
                            let b64 = base64::engine::general_purpose::STANDARD.encode(&bytes);
                            writeln!(w, "![image](data:image/png;base64,{b64})")?;
                        }
                        Err(_) => {
                            // blob 缺失不中断导出:占位说明,与其余条目继续。
                            writeln!(w, "> (图片文件缺失:{stored})")?;
                        }
                    }
                }
                None => writeln!(w, "> (图片路径为空)")?,
            }
            // OCR 文本附后(若有),不丢信息。
            if let Some(ocr) = e.ocr_text.as_deref().filter(|s| !s.is_empty()) {
                writeln!(w)?;
                writeln!(w, "OCR 文本:")?;
                writeln!(w, "```text")?;
                writeln!(w, "{ocr}")?;
                writeln!(w, "```")?;
            }
        }
        "file" => {
            let paths: Vec<String> = e
                .file_paths
                .as_deref()
                .and_then(|json| serde_json::from_str(json).ok())
                .unwrap_or_default();
            if paths.is_empty() {
                writeln!(w, "> (文件路径为空)")?;
            } else {
                for p in paths {
                    writeln!(w, "- {p}")?;
                }
            }
        }
        other => writeln!(w, "> (未知条目类型:{other})")?,
    }
    writeln!(w)?;
    Ok(())
}

fn kind_label(kind: &str) -> &'static str {
    match kind {
        "text" => "文本",
        "image" => "图片",
        "file" => "文件",
        "todo" => "待办",
        _ => "条目",
    }
}

// ---------------------------------------------------------------------------
// 恢复(替换式)
// ---------------------------------------------------------------------------

/// 恢复备份:选 zip → 替换当前数据。返回 summary(含险备份路径)。
/// ⚠️ 慢 I/O(解包 + 打险备份 + 拷贝),由宿主工作线程调用。
pub fn restore_backup(core: &Core, zip_path: &str) -> AppResult<serde_json::Value> {
    let mut guard = core.db.lock()?;
    let out = restore_snapshot(&mut guard, &core.effective_dir, Path::new(zip_path))?;
    log::info!(
        "restore_backup: {} 条目 / {} blobs;险备份 -> {}",
        out.entry_count,
        out.blob_count,
        out.safety_path.display()
    );
    Ok(serde_json::json!({
        "entryCount": out.entry_count,
        "blobCount": out.blob_count,
        "safetyBackupPath": out.safety_path.to_string_lossy(),
        "durationMs": out.duration_ms,
    }))
}

/// 恢复核心路径(测试直驱,不依赖 Core 单例):conn 必须指向 data_dir 下的库,
/// 且调用方持锁保证无并发访问。全过程分三段:①staging 预演(不动真数据)
/// ②险备份 ③锁内热替换(失败回滚到旧数据)。
fn restore_snapshot(
    conn: &mut Connection,
    data_dir: &Path,
    zip_path: &Path,
) -> AppResult<RestoreOutcome> {
    let started = std::time::Instant::now();
    let staged = stage_backup(zip_path, data_dir)?;
    // 险备份在预演通过之后才打:zip 本身非法(版本更高/损坏)时不该产生垃圾文件。
    let safety_path = safety_backup_path(zip_path);
    backup_snapshot(conn, data_dir, &safety_path)?;
    swap_in(conn, data_dir, &staged)?;
    let _ = std::fs::remove_dir_all(&staged.dir); // staging 清场(残留也无害,下次开跑会先清)
    Ok(RestoreOutcome {
        entry_count: staged.entry_count,
        blob_count: staged.blob_count,
        safety_path,
        duration_ms: started.elapsed().as_millis() as u64,
    })
}

#[derive(Debug)]
struct RestoreOutcome {
    entry_count: i64,
    blob_count: usize,
    safety_path: PathBuf,
    duration_ms: u64,
}

/// 险备份路径:恢复源同目录,`nextboard-pre-restore-<时间戳>.zip`;
/// 同秒重名追加序号(不覆盖既有文件)。
fn safety_backup_path(zip_path: &Path) -> PathBuf {
    let dir = zip_path.parent().filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let stamp = Local::now().format("%Y%m%d-%H%M%S");
    let mut path = dir.join(format!("nextboard-pre-restore-{stamp}.zip"));
    let mut n = 1;
    while path.exists() {
        path = dir.join(format!("nextboard-pre-restore-{stamp}-{n}.zip"));
        n += 1;
    }
    path
}

/// staging 解包产物:与数据目录同构的暂存副本。
struct StagedBackup {
    dir: PathBuf,
    db: PathBuf,
    blobs: PathBuf,
    entry_count: i64,
    blob_count: usize,
}

/// 解包 + 校验到 `data_dir/restore-staging/`,通过后返回 staged 产物。
///
/// 校验链(任何一步失败都不动当前数据):
///   ① zip 可读、含 manifest.json、`app == "nextboard"`;
///   ② manifest 与库内 user_version 均 ≤ LATEST(更新的备份读不懂,拒绝);
///   ③ 解包防落滑(只收 manifest.json / clipboard.db / blobs/**,路径不得逃逸);
///   ④ staged 库可打开、含 clip_entries、条数与 manifest 一致(完整性);
///   ⑤ staged 库走一遍迁移链(换机/跨版本恢复的 schema 升级在这里预演)。
fn stage_backup(zip_path: &Path, data_dir: &Path) -> AppResult<StagedBackup> {
    let file = std::fs::File::open(zip_path).map_err(|err| {
        AppError::InvalidInput(format!("打开备份文件失败: {err}"))
    })?;
    let mut archive = zip::ZipArchive::new(file).map_err(|err| {
        AppError::InvalidInput(format!("不是有效的 zip 备份:{err}"))
    })?;

    // ① manifest 先行:版本闸门要在解包任何内容之前。
    let manifest_bytes = {
        let mut m = archive.by_name("manifest.json").map_err(|_| {
            AppError::InvalidInput("备份缺少 manifest.json(不是 Nextboard 备份?)".into())
        })?;
        let mut s = String::new();
        std::io::Read::read_to_string(&mut m, &mut s)?;
        s
    };
    let manifest: serde_json::Value = serde_json::from_str(&manifest_bytes)
        .map_err(|err| AppError::InvalidInput(format!("manifest.json 解析失败:{err}")))?;
    if manifest.get("app").and_then(|v| v.as_str()) != Some("nextboard") {
        return Err(AppError::InvalidInput("不是 Nextboard 备份(manifest.app 不匹配)".into()));
    }
    let manifest_version = manifest.get("schema_version").and_then(|v| v.as_i64()).unwrap_or(0);
    if manifest_version < 1 || manifest_version > crate::store::LATEST_VERSION {
        return Err(AppError::InvalidInput(format!(
            "备份来自更新的 Nextboard 版本(schema {manifest_version} > 本版 {})",
            crate::store::LATEST_VERSION
        )));
    }
    let expected_count = manifest.get("entry_count").and_then(|v| v.as_i64()).unwrap_or(-1);

    // ② 解包到 staging(固定目录名:先清上次残留)。
    let staging_root = data_dir.join(STAGING_DIR);
    let _ = std::fs::remove_dir_all(&staging_root);
    std::fs::create_dir_all(staging_root.join("blobs"))?;
    let staged_db = staging_root.join("clipboard.db");
    let staged_blobs = staging_root.join("blobs");
    let mut blob_count = 0usize;
    let mut has_db = false;
    for i in 0..archive.len() {
        let mut entry = archive.by_index(i).map_err(|err| {
            AppError::Internal(format!("读取 zip 条目失败: {err}"))
        })?;
        // enclosed_name:zip 落滑防护(带 `..`/绝对路径的条目在这里返回 None)
        let Some(rel) = entry.enclosed_name() else {
            return Err(AppError::InvalidInput(format!(
                "备份包含非法路径条目:{}(zip 落滑防护)",
                entry.name()
            )));
        };
        if entry.is_dir() {
            continue; // 目录条目(stdaging 目录按需现建)
        }
        if rel == Path::new("manifest.json") {
            continue; // 已单独读取
        }
        if rel == Path::new("clipboard.db") || rel.starts_with("blobs") {
            let target = staging_root.join(&rel);
            if let Some(parent) = target.parent() {
                std::fs::create_dir_all(parent)?;
            }
            let mut out = std::fs::File::create(&target)?;
            std::io::copy(&mut entry, &mut out)?;
            if rel == Path::new("clipboard.db") {
                has_db = true;
            } else {
                blob_count += 1;
            }
        } else {
            // 未来版本可能加文件:忽略未知顶层条目(前向兼容),但不解出到 staging 之外。
            log::info!("stage_backup: 忽略未知条目 {}", rel.display());
        }
    }
    if !has_db {
        return Err(AppError::InvalidInput("备份缺少 clipboard.db".into()));
    }

    // ③ staged 库校验:能打开、含表、条数一致;之后走一遍迁移链。
    {
        let raw = Connection::open(&staged_db)?;
        let uv: i64 = raw.query_row("PRAGMA user_version", [], |r| r.get(0))?;
        if uv > crate::store::LATEST_VERSION {
            return Err(AppError::InvalidInput(format!(
                "备份数据库版本较新(user_version {uv} > 本版 {})",
                crate::store::LATEST_VERSION
            )));
        }
        let count: i64 = raw
            .query_row("SELECT COUNT(*) FROM clip_entries", [], |r| r.get(0))
            .map_err(|_| AppError::InvalidInput("备份数据库缺少 clip_entries 表".into()))?;
        if expected_count >= 0 && count != expected_count {
            return Err(AppError::InvalidInput(format!(
                "备份元数据与数据库不一致(manifest {expected_count} 条 vs 库内 {count} 条,可能已损坏)"
            )));
        }
        drop(raw); // 先关连接再换 store::open(避免两连接同时持有文件)
    }
    {
        // 迁移链预演:跨版本恢复在这里升级 staging 副本;真库替换后无需再迁。
        let migrated = crate::store::open(&staged_db)?;
        let _: (i64, i64, i64) = migrated.query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })?;
        drop(migrated); // 关闭后 -wal/-shm 随之清掉,staging 里只剩单文件
    }
    let _ = std::fs::remove_file(staging_root.join("clipboard.db-wal"));
    let _ = std::fs::remove_file(staging_root.join("clipboard.db-shm"));

    let entry_count: i64 = {
        let c = Connection::open(&staged_db)?;
        let n = c.query_row("SELECT COUNT(*) FROM clip_entries", [], |r| r.get(0))?;
        drop(c);
        n
    };
    Ok(StagedBackup { dir: staging_root, db: staged_db, blobs: staged_blobs, entry_count, blob_count })
}

/// 锁内热替换:旧 db/blobs 改名保底 → 新数据就位 → 重开连接(迁移)。
/// 任一步失败都尝试把旧数据完整搬回;成功后旧副本由调用方删除。
fn swap_in(conn: &mut Connection, data_dir: &Path, staged: &StagedBackup) -> AppResult<()> {
    // ① 旧连接换成内存占位 → 释放旧库文件句柄(Windows 下不释放则 rename 必失败)。
    let old = std::mem::replace(conn, Connection::open_in_memory()?);
    drop(old);

    let db = data_dir.join("clipboard.db");
    let blobs = data_dir.join("blobs");
    let db_backup = data_dir.join("clipboard.db.pre-restore");
    let blobs_backup = data_dir.join("blobs.pre-restore");
    // 上次失败可能留下的 pre-restore 残留:先清,避免 rename 冲突。
    let _ = std::fs::remove_file(&db_backup);
    let _ = std::fs::remove_dir_all(&blobs_backup);
    // ⚠️ 旧库的 WAL 伴随文件必须一并清掉:新库进来后它们是污染(库文件与 WAL 不同源)。
    let _ = std::fs::remove_file(data_dir.join("clipboard.db-wal"));
    let _ = std::fs::remove_file(data_dir.join("clipboard.db-shm"));

    let moved_db = rename_if_exists(&db, &db_backup)?;
    let moved_blobs = rename_if_exists(&blobs, &blobs_backup)?;

    let result = (|| -> AppResult<Connection> {
        std::fs::rename(&staged.db, &db)?;
        std::fs::rename(&staged.blobs, &blobs)?;
        let reopened = crate::store::open(&db)?;
        Ok(reopened)
    })();

    match result {
        Ok(reopened) => {
            *conn = reopened;
            // 收尾删旧副本:险备份 zip 已在,磁盘不双份(删除失败仅留残留,不致命)。
            if moved_db {
                let _ = std::fs::remove_file(&db_backup);
            }
            if moved_blobs {
                let _ = std::fs::remove_dir_all(&blobs_backup);
            }
            Ok(())
        }
        Err(err) => {
            // 回滚:把半途新文件清掉,旧数据搬回原址。
            let _ = std::fs::remove_file(&db);
            let _ = std::fs::remove_dir_all(&blobs);
            if moved_db {
                let _ = std::fs::rename(&db_backup, &db);
            }
            if moved_blobs {
                let _ = std::fs::rename(&blobs_backup, &blobs);
            }
            // 尽力重开旧库;失败则保持内存占位(进程不崩,日志可查,重启可恢复)。
            match crate::store::open(&db) {
                Ok(old_conn) => *conn = old_conn,
                Err(reopen_err) => {
                    log::error!("swap_in 回滚后旧库重开失败(重启应用可恢复): {reopen_err}");
                }
            }
            Err(err)
        }
    }
}

/// 存在才改名;返回是否真的改了。
fn rename_if_exists(from: &Path, to: &Path) -> AppResult<bool> {
    if !from.try_exists().unwrap_or(false) {
        return Ok(false);
    }
    std::fs::rename(from, to)?;
    Ok(true)
}

// ---------------------------------------------------------------------------
// 导入(合并式)
// ---------------------------------------------------------------------------

/// 合并导入备份 zip:去重跳过重复条目,id 冲突重铸新 id,blob 随条目搬迁。
pub fn import_backup(core: &Core, zip_path: &str) -> AppResult<serde_json::Value> {
    let mut guard = core.db.lock()?;
    let out = import_snapshot(&mut guard, &core.effective_dir, Path::new(zip_path))?;
    log::info!(
        "import_backup: 导入 {} 条 / 跳过重复 {} 条 / 搬运 {} 个 blob(缺失 {})",
        out.imported,
        out.skipped,
        out.blob_count,
        out.blob_missing
    );
    Ok(serde_json::json!({
        "imported": out.imported,
        "skipped": out.skipped,
        "blobCount": out.blob_count,
        "blobMissing": out.blob_missing,
        "durationMs": out.duration_ms,
    }))
}

struct ImportOutcome {
    imported: i64,
    skipped: i64,
    blob_count: usize,
    blob_missing: usize,
    duration_ms: u64,
}

/// 导入核心路径(测试直驱)。数据流:
///   staging 解包(复用恢复的校验链)→ 读 staged 全部行 → 逐行并入(单事务)→ 清场。
fn import_snapshot(
    conn: &mut Connection,
    data_dir: &Path,
    zip_path: &Path,
) -> AppResult<ImportOutcome> {
    let started = std::time::Instant::now();
    let staged = stage_backup(zip_path, data_dir)?;

    // staged 行全量读出(条数 ≤ 历史上限,内存持有整表可接受;迁移已在 stage 里做完)。
    let staged_conn = Connection::open(&staged.db)?;
    let rows = read_staged_rows(&staged_conn)?;
    // blob 索引:staged blobs 目录下按"文件名第一个点前的段(即条目 id)"聚类 ——
    // 一条图片的 `<id>.png` 与缩略图 `<id>.thumb<ver>.png` 同前缀,随条目一起搬。
    let blob_index = index_blob_files(&staged.blobs)?;

    let blobs_dir = data_dir.join("blobs");
    std::fs::create_dir_all(&blobs_dir)?;

    let mut imported = 0i64;
    let mut skipped = 0i64;
    let mut blob_count = 0usize;
    let mut blob_missing = 0usize;

    let tx = conn.transaction()?;
    for row in &rows {
        // 去重口径 = 内容 hash(文本=正文 blake3,图片=png blake3,待办="todo:<id>")。
        // 重复的**跳过且不动现有条目**:用户当前的固定/使用时间等状态优先。
        if hash_exists(&tx, &row.hash)? {
            skipped += 1;
            continue;
        }
        // id 冲突(两台机器各自演进后合并,极小概率):重铸新 id,blob 随之改名。
        let remap = id_exists(&tx, &row.id)?;
        let new_id = if remap { uuid::Uuid::now_v7().to_string() } else { row.id.clone() };

        // blob 目标位置:
        //  - 重铸 id → 主图/缩略图一律改名为新 id(路径同步改写);
        //  - 未重铸但存的是历史**绝对路径** → 规范化为 staged 里的相对位置(拉正口径);
        //  - 未重铸且相对路径 → 以库里的 blob_path 为权威(文件照它放)。
        let mut blob_path = row.blob_path.clone();
        let is_absolute = row
            .blob_path
            .as_deref()
            .map(|p| Path::new(p).is_absolute())
            .unwrap_or(false);
        if let Some(src_rel) = matched_rel_path(&blob_index, &row.id) {
            let main_dst = if remap {
                rename_stem(&src_rel, &row.id, &new_id)
            } else if is_absolute {
                src_rel.clone()
            } else {
                PathBuf::from(row.blob_path.as_deref().unwrap_or_default())
            };
            if remap || is_absolute {
                blob_path = Some(main_dst.to_string_lossy().replace('\\', "/"));
            }
            if copy_blob(&staged.blobs, &blobs_dir, &src_rel, &main_dst) {
                blob_count += 1;
            }
            // 缩略图等其它同前缀文件:同改名规则逐个搬(未重铸时保持 staged 相对路径)。
            for other in blob_files_with_stem(&blob_index, &row.id) {
                if *other == src_rel {
                    continue;
                }
                let other_dst = if remap || is_absolute {
                    rename_stem(other, &row.id, &new_id)
                } else {
                    other.clone()
                };
                if copy_blob(&staged.blobs, &blobs_dir, other, &other_dst) {
                    blob_count += 1;
                }
            }
        } else if row.kind == "image" {
            // 图片但 staged 里找不到文件:保留原路径(可能是仍可用的绝对路径),只计数。
            blob_missing += 1;
        }

        tx.execute(
            "INSERT INTO clip_entries
               (id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size,
                preview, pinned, created_at, last_used_at, source, tags, ocr_text)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)",
            params![
                new_id,
                row.kind,
                row.text_content,
                blob_path,
                row.file_paths,
                row.width,
                row.height,
                row.hash,
                row.byte_size,
                row.preview,
                row.pinned,
                row.created_at,
                row.last_used_at,
                row.source,
                row.tags,
                row.ocr_text,
            ],
        )?;
        // 待办扩展行随条目(1:1;重铸 id 时同样换键)。
        // ⚠️ 便签窗口状态(todo_note)也要跟着搬(docs/20 §3.4):只搬 todo_meta 的话
        //    "导入备份后待办都在、桌面便签一张不剩"—— 这正是 v6 待办导入时踩过的同一处。
        if row.kind == "todo" {
            if let Some(note) = read_todo_note(&staged_conn, &row.id)? {
                tx.execute(
                    "INSERT OR REPLACE INTO todo_note
                       (id, x, y, w, h, screen, edge, auto_hide, collapsed, top_most,
                        visible, opacity, font_size, updated_at)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)",
                    params![
                        new_id,
                        note.0,
                        note.1,
                        note.2,
                        note.3,
                        note.4,
                        note.5,
                        note.6,
                        note.7,
                        note.8,
                        note.9,
                        note.10,
                        note.11,
                        note.12
                    ],
                )?;
            }
            if let Some(meta) = read_todo_meta(&staged_conn, &row.id)? {
                tx.execute(
                    "INSERT OR REPLACE INTO todo_meta
                       (id, due_at, remind_at, notify_at, done_at, reminded, notified,
                        bubble_shown, note_text)
                     VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
                    params![
                        new_id,
                        meta.0,
                        meta.1,
                        meta.2,
                        meta.3,
                        meta.4,
                        meta.5,
                        meta.6,
                        meta.7
                    ],
                )?;
            }
        }
        imported += 1;
    }
    tx.commit()?;
    drop(staged_conn);
    let _ = std::fs::remove_dir_all(&staged.dir);

    Ok(ImportOutcome {
        imported,
        skipped,
        blob_count,
        blob_missing,
        duration_ms: started.elapsed().as_millis() as u64,
    })
}

/// staged 条目行(列清单与 v5 后的最终 schema 一致,见 v6.sql)。
struct StagedRow {
    id: String,
    kind: String,
    text_content: Option<String>,
    blob_path: Option<String>,
    file_paths: Option<String>,
    width: Option<i64>,
    height: Option<i64>,
    hash: String,
    byte_size: i64,
    preview: String,
    pinned: i64,
    created_at: i64,
    last_used_at: i64,
    source: String,
    tags: Option<String>,
    ocr_text: Option<String>,
}

fn read_staged_rows(conn: &Connection) -> AppResult<Vec<StagedRow>> {
    let mut stmt = conn.prepare(
        "SELECT id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size,
                preview, pinned, created_at, last_used_at, source, tags, ocr_text
         FROM clip_entries",
    )?;
    let rows = stmt.query_map([], |r| {
        Ok(StagedRow {
            id: r.get(0)?,
            kind: r.get(1)?,
            text_content: r.get(2)?,
            blob_path: r.get(3)?,
            file_paths: r.get(4)?,
            width: r.get(5)?,
            height: r.get(6)?,
            hash: r.get(7)?,
            byte_size: r.get(8)?,
            preview: r.get(9)?,
            pinned: r.get(10)?,
            created_at: r.get(11)?,
            last_used_at: r.get(12)?,
            source: r.get(13)?,
            tags: r.get(14)?,
            ocr_text: r.get(15)?,
        })
    })?;
    let mut out = Vec::new();
    for row in rows {
        out.push(row?);
    }
    Ok(out)
}

#[allow(clippy::type_complexity)]
/// staged 里的便签窗口状态(docs/20 §3.4)。返回 13 元组避免为导入再定义一个结构体
/// (与 read_todo_meta 的取舍一致:只在本文件内用一次)。
///
/// 顺序:x, y, w, h, screen, edge, auto_hide, collapsed, top_most, visible,
///       opacity, font_size, updated_at
#[allow(clippy::type_complexity)]
fn read_todo_note(
    conn: &Connection,
    id: &str,
) -> AppResult<
    Option<(
        i64,
        i64,
        i64,
        i64,
        String,
        String,
        i64,
        i64,
        i64,
        i64,
        i64,
        i64,
        i64,
    )>,
> {
    // 旧备份(v7 之前)没有这张表 —— 查询会报 "no such table"。这里**宽容处理**:
    // 表不存在 = 该备份没有便签数据,返回 None 而不是让整次导入失败。
    let sql = "SELECT x, y, w, h, screen, edge, auto_hide, collapsed, top_most, \
               visible, opacity, font_size, updated_at FROM todo_note WHERE id = ?1";
    let mut stmt = match conn.prepare(sql) {
        Ok(stmt) => stmt,
        Err(_) => return Ok(None),
    };
    let mut rows = stmt.query(params![id])?;
    let Some(row) = rows.next()? else {
        return Ok(None);
    };
    Ok(Some((
        row.get(0)?,
        row.get(1)?,
        row.get(2)?,
        row.get(3)?,
        row.get(4)?,
        row.get(5)?,
        row.get(6)?,
        row.get(7)?,
        row.get(8)?,
        row.get(9)?,
        row.get(10)?,
        row.get(11)?,
        row.get(12)?,
    )))
}

/// staged 里的待办扩展行。
/// 顺序:due_at, remind_at, notify_at, done_at, reminded, notified, bubble_shown, note_text
#[allow(clippy::type_complexity)]
fn read_todo_meta(
    conn: &Connection,
    id: &str,
) -> AppResult<
    Option<(
        Option<i64>,
        Option<i64>,
        Option<i64>,
        Option<i64>,
        i64,
        i64,
        i64,
        Option<String>,
    )>,
> {
    // ⚠️ 旧备份(v8 之前)的 todo_meta 没有 note_text 列 —— 查询会报 no such column。
    //    与 read_todo_note 同一取舍:列不存在 = 该备份没有备注,返回 None 让整次导入
    //    继续,而不是因为少一列就整包失败。
    let has_note = conn
        .prepare("SELECT note_text FROM todo_meta LIMIT 0")
        .is_ok();
    let sql = if has_note {
        "SELECT due_at, remind_at, notify_at, done_at, reminded, notified, bubble_shown,
                note_text
         FROM todo_meta WHERE id = ?1"
    } else {
        "SELECT due_at, remind_at, notify_at, done_at, reminded, notified, bubble_shown,
                NULL
         FROM todo_meta WHERE id = ?1"
    };
    let mut stmt = conn.prepare(sql)?;
    let mut rows = stmt.query(params![id])?;
    if let Some(row) = rows.next()? {
        return Ok(Some((
            row.get(0)?,
            row.get(1)?,
            row.get(2)?,
            row.get(3)?,
            row.get(4)?,
            row.get(5)?,
            row.get(6)?,
            row.get(7)?,
        )));
    }
    Ok(None)
}

fn hash_exists(conn: &Connection, hash: &str) -> AppResult<bool> {
    let n: i64 = conn.query_row(
        "SELECT COUNT(*) FROM clip_entries WHERE hash = ?1",
        params![hash],
        |r| r.get(0),
    )?;
    Ok(n > 0)
}

fn id_exists(conn: &Connection, id: &str) -> AppResult<bool> {
    let n: i64 = conn.query_row(
        "SELECT COUNT(*) FROM clip_entries WHERE id = ?1",
        params![id],
        |r| r.get(0),
    )?;
    Ok(n > 0)
}

/// 把 staged blobs 目录下的文件按"文件名第一个点前的段"聚类(即条目 id 前缀)。
/// `2026-10/<id>.png` 与 `2026-10/<id>.thumb3.png` 前缀同为 `<id>`。
fn index_blob_files(blobs_dir: &Path) -> AppResult<HashMap<String, Vec<PathBuf>>> {
    let mut map: HashMap<String, Vec<PathBuf>> = HashMap::new();
    let mut files = Vec::new();
    if blobs_dir.is_dir() {
        collect_files(blobs_dir, Path::new(""), &mut files)?;
    }
    for rel in files {
        let name = rel.file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
        let stem = name.split('.').next().unwrap_or("").to_string();
        if !stem.is_empty() {
            map.entry(stem).or_default().push(rel);
        }
    }
    Ok(map)
}

/// 条目 id 对应的首个 blob 文件(staged blobs 下的相对路径;约定 `<id>.png` 优先)。
fn matched_rel_path(index: &HashMap<String, Vec<PathBuf>>, id: &str) -> Option<PathBuf> {
    let files = index.get(id)?;
    files
        .iter()
        .find(|p| p.file_name().map(|n| n.to_string_lossy().ends_with(".png")).unwrap_or(false)
                  && p.file_name().map(|n| n.to_string_lossy().matches('.').count() == 1).unwrap_or(false))
        .or_else(|| files.first())
        .cloned()
}

/// 同前缀(同条目)的全部 blob 文件(含缩略图)。
fn blob_files_with_stem<'a>(
    index: &'a HashMap<String, Vec<PathBuf>>,
    id: &str,
) -> &'a [PathBuf] {
    index.get(id).map(|v| v.as_slice()).unwrap_or(&[])
}

/// 把文件名里的旧 id 前缀换成新 id(保留目录与扩展名/缩略图后缀)。
fn rename_stem(rel: &Path, old_id: &str, new_id: &str) -> PathBuf {
    let name = rel.file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    let new_name = match name.strip_prefix(old_id) {
        Some(rest) => format!("{new_id}{rest}"),
        None => name,
    };
    match rel.parent() {
        Some(p) if !p.as_os_str().is_empty() => p.join(new_name),
        _ => PathBuf::from(new_name),
    }
}

/// 把 staged blobs 里的文件按目标相对路径拷进线上 blobs 目录;成功返回 true。
fn copy_blob(staged_blobs: &Path, live_blobs: &Path, src_rel: &Path, dst_rel: &Path) -> bool {
    let dst = live_blobs.join(dst_rel);
    if let Some(parent) = dst.parent() {
        if std::fs::create_dir_all(parent).is_err() {
            return false;
        }
    }
    std::fs::copy(staged_blobs.join(src_rel), &dst).is_ok()
}

// ---------------------------------------------------------------------------
// 测试(不依赖 Core 单例:直接对临时库操作)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use std::path::PathBuf;

    /// 测试临时目录:放 `<manifest>/target/tmp` 而非 std::env::temp_dir() ——
    /// DSH 沙箱会把"工作区内 exe"的进程对 %TEMP% 的写入判为拒绝,对工作区内放行;
    /// 而 CARGO_TARGET_TMPDIR 只对 integration test 生效,lib 单元测试取不到,
    /// 故统一用 CARGO_MANIFEST_DIR 拼路径(两种测试形态都可用)。
    fn temp_dir(tag: &str) -> PathBuf {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target").join("tmp");
        let dir = base.join(format!("nbcore-backup-test-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("blobs")).unwrap();
        dir
    }

    fn insert_text(conn: &Connection, id: &str, text: &str, pinned: bool, at: i64) {
        conn.execute(
            "INSERT INTO clip_entries (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at, source)
             VALUES (?1, 'text', ?2, 'h' || ?1, 0, ?2, ?3, ?4, ?4, 'clipboard')",
            rusqlite::params![id, text, pinned as i64, at],
        )
        .unwrap();
    }

    #[test]
    fn backup_snapshot_roundtrip() {
        let dir = temp_dir("zip");
        let conn = crate::store::open(&dir.join("clipboard.db")).unwrap();
        insert_text(&conn, "id-1", "hello", false, 1700000000000);
        insert_text(&conn, "id-2", "pinned", true, 1700000100000);
        std::fs::write(dir.join("blobs").join("a.png"), b"png-bytes").unwrap();

        let out = dir.join("backup.zip");
        let r = backup_snapshot(&conn, &dir, &out).unwrap();
        assert_eq!(r.entry_count, 2);
        assert_eq!(r.blob_count, 1);
        assert!(r.zip_bytes > 0);

        // 解包校验:manifest + db + blob 三件齐全,schema 版本 = 当前版本
        let f = std::fs::File::open(&out).unwrap();
        let mut archive = zip::ZipArchive::new(f).unwrap();
        assert_eq!(archive.len(), 3); // manifest + db + 1 blob
        let manifest_bytes = {
            let mut manifest = archive.by_name("manifest.json").unwrap();
            let mut s = String::new();
            manifest.read_to_string(&mut s).unwrap();
            s
        };
        let v: serde_json::Value = serde_json::from_str(&manifest_bytes).unwrap();
        // 不写死版本号:迁移一升级(v6→v7 就撞过一次)这条断言就会误红,
        // 而它真正要钉住的是"manifest 版本 = 核心当前 schema 版本",不是"等于 6"。
        assert_eq!(
            v["schema_version"],
            crate::store::LATEST_VERSION,
            "manifest 的 schema_version 必须跟随核心当前版本"
        );
        assert_eq!(v["entry_count"], 2);
        let blob_bytes = {
            let mut blob = archive.by_name("blobs/a.png").unwrap();
            let mut bytes = Vec::new();
            blob.read_to_end(&mut bytes).unwrap();
            bytes
        };
        assert_eq!(blob_bytes, b"png-bytes");

        drop(conn); // 关闭 WAL 连接后再清理,否则 -wal/-shm 被占用,remove_dir_all 报 code 32
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn export_markdown_all_kinds() {
        let dir = temp_dir("md");
        let conn = crate::store::open(&dir.join("clipboard.db")).unwrap();
        insert_text(&conn, "t1", "plain text", false, 1700000000000);
        insert_text(&conn, "t2", "has ```fence``` inside", false, 1700000060000);
        conn.execute(
            "INSERT INTO clip_entries (id, kind, hash, byte_size, preview, pinned, created_at, last_used_at, source)
             VALUES ('f1', 'file', 'hf', 0, 'p', 0, 1700000100, 1700000100, 'clipboard')",
            [],
        )
        .unwrap();
        conn.execute(
            "UPDATE clip_entries SET file_paths = '[\"D:\\\\a.txt\"]' WHERE id = 'f1'",
            [],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO clip_entries (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at, source)
             VALUES ('d1', 'todo', 'buy milk', 'hd', 0, 'buy', 0, 1700000200, 1700000200, 'clipboard')",
            [],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO todo_meta (id, done_at, due_at) VALUES ('d1', NULL, 1700009000000)",
            [],
        )
        .unwrap();
        // 图片条目(blob 存在)+ 一个缺失 blob 的图片条目(占位不中断)
        std::fs::write(dir.join("blobs").join("img.png"), b"\x89PNG").unwrap();
        conn.execute(
            "INSERT INTO clip_entries (id, kind, blob_path, width, height, hash, byte_size, preview, pinned, created_at, last_used_at, source)
             VALUES ('i1', 'image', 'img.png', 10, 20, 'hi', 4, 'i', 1, 1700000300, 1700000300, 'clipboard')",
            [],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO clip_entries (id, kind, blob_path, hash, byte_size, preview, pinned, created_at, last_used_at, source)
             VALUES ('i2', 'image', 'missing.png', 'hi2', 0, 'm', 0, 1700000400, 1700000400, 'clipboard')",
            [],
        )
        .unwrap();

        let out = dir.join("export.md");
        let r = export_markdown(&conn, &dir.join("blobs"), &out).unwrap();
        assert_eq!(r.entry_count, 6);
        let md = std::fs::read_to_string(&out).unwrap();
        assert!(md.contains("plain text"));
        // 围栏升级:含 ``` 的正文用 4 反引号包裹
        assert!(md.contains("````text"));
        assert!(md.contains("- [ ] buy milk"));
        assert!(md.contains("- 截止:"));
        assert!(md.contains("- D:\\a.txt"));
        // 图片 base64 内嵌("x89PNG" 的 base64 前缀)与缺失占位
        assert!(md.contains("![image](data:image/png;base64,iVBOR"));
        assert!(md.contains("(图片文件缺失:missing.png)"));
        assert!(md.contains("📌"));
        // 时间戳按毫秒解析:导出小节标题为可读日期(2023-11-14 前后,UTC+8)
        assert!(md.contains("## 2023-11-"), "timestamp should be human readable");

        drop(conn); // 同上:先关连接再清理
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn export_rejects_unknown_format() {
        let dir = temp_dir("fmt");
        let conn = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let err = export_markdown(&conn, &dir.join("blobs"), &dir.join("x.html"));
        // format 校验在 ffi::export_entries;这里直接验证未知类型条目不炸
        assert!(err.is_ok()); // export_markdown 不做 format 校验(由 ffi 层负责)
        drop(conn);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    // ---- 恢复 / 导入 ----

    /// 带显式 hash 的插入(去重测试需要控制 hash 是否相同,不能沿用 'h'||id 的假 hash)。
    fn insert_entry(
        conn: &Connection,
        id: &str,
        kind: &str,
        text: Option<&str>,
        blob_path: Option<&str>,
        hash: &str,
        at: i64,
    ) {
        conn.execute(
            "INSERT INTO clip_entries
               (id, kind, text_content, blob_path, hash, byte_size, preview, pinned,
                created_at, last_used_at, source)
             VALUES (?1, ?2, ?3, ?4, ?5, 0, COALESCE(?3,''), 0, ?6, ?6, 'clipboard')",
            rusqlite::params![id, kind, text, blob_path, hash, at],
        )
        .unwrap();
    }

    fn count(conn: &Connection) -> i64 {
        conn.query_row("SELECT COUNT(*) FROM clip_entries", [], |r| r.get(0)).unwrap()
    }

    /// 在 blobs 下按相对路径写一个 blob 文件(自动建月目录)。
    fn write_blob(dir: &Path, rel: &str, bytes: &[u8]) {
        let p = dir.join("blobs").join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, bytes).unwrap();
    }

    /// 读取主库文件字节(先 checkpoint —— WAL 模式下表结构可能还在 -wal 里,
    /// 直拷 .db 会得到"缺表"的旧快照;错拷 .db 不 checkpoint 正是 progress.md:3085 的教训)。
    fn read_db_bytes(conn: &Connection, dir: &Path) -> Vec<u8> {
        let _: (i64, i64, i64) = conn
            .query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get(2)?))
            })
            .unwrap();
        std::fs::read(dir.join("clipboard.db")).unwrap()
    }

    fn get_text(conn: &Connection, id: &str) -> Option<String> {
        conn.query_row(
            "SELECT text_content FROM clip_entries WHERE id = ?1",
            rusqlite::params![id],
            |r| r.get(0),
        )
        .ok()
    }

    #[test]
    fn restore_replaces_data_and_keeps_safety_backup() {
        let dir = temp_dir("restore");
        let mut conn = crate::store::open(&dir.join("clipboard.db")).unwrap();
        insert_entry(&conn, "a1", "text", Some("keep me"), None, "ha1", 1_700_000_000_000);
        insert_entry(&conn, "a2", "image", None, Some("2026-01/a2.png"), "hi2", 1_700_000_010_000);
        write_blob(&dir, "2026-01/a2.png", b"png-a2");
        // 建备份(与产品同一函数):此后的一切变更都应被恢复清掉/还原。
        let zip = dir.join("b.zip");
        backup_snapshot(&conn, &dir, &zip).unwrap();

        // 变更:加一条、删一条、并把 a2 的图片文件从磁盘抹掉(验证恢复会还原文件)。
        insert_entry(&conn, "c1", "text", Some("should vanish"), None, "hc1", 1_700_000_020_000);
        conn.execute("DELETE FROM clip_entries WHERE id='a1'", []).unwrap();
        std::fs::remove_file(dir.join("blobs").join("2026-01").join("a2.png")).unwrap();

        let out = restore_snapshot(&mut conn, &dir, &zip).unwrap();
        assert_eq!(out.entry_count, 2);
        assert_eq!(out.blob_count, 1);
        assert_eq!(count(&conn), 2);
        assert_eq!(get_text(&conn, "a1").as_deref(), Some("keep me")); // 被删的回来了
        assert_eq!(get_text(&conn, "c1"), None);                      // 新增的没了
        assert_eq!(
            std::fs::read(dir.join("blobs").join("2026-01").join("a2.png")).unwrap(),
            b"png-a2"                                                  // 图片文件还原
        );
        assert!(out.safety_path.exists(), "险备份 zip 应落在恢复源同目录");
        // staging 与 pre-restore 残留都应清掉(data 目录里只剩 db/blobs/staging 之外的正常内容)
        assert!(!dir.join(STAGING_DIR).exists());
        assert!(!dir.join("clipboard.db.pre-restore").exists());
        assert!(!dir.join("blobs.pre-restore").exists());

        drop(conn);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// 伪造 zip 的公用工具:manifest + 可选附加文件。
    fn write_zip(zip: &Path, manifest: &serde_json::Value, extras: &[(&str, &[u8])]) {
        let f = std::fs::File::create(zip).unwrap();
        let mut w = zip::ZipWriter::new(f);
        let opts = zip::write::SimpleFileOptions::default();
        w.start_file("manifest.json", opts).unwrap();
        std::io::Write::write_all(&mut w, manifest.to_string().as_bytes()).unwrap();
        for (name, bytes) in extras {
            w.start_file(*name, opts).unwrap();
            std::io::Write::write_all(&mut w, bytes).unwrap();
        }
        w.finish().unwrap();
    }

    #[test]
    fn restore_rejects_newer_schema_and_foreign_zip() {
        let dir = temp_dir("gate");
        let conn = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let db_bytes = read_db_bytes(&conn, &dir);

        let newer = dir.join("newer.zip");
        write_zip(
            &newer,
            &serde_json::json!({"app": "nextboard", "schema_version": 999, "entry_count": 0}),
            &[("clipboard.db", &db_bytes)],
        );
        let mut c2 = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let err = restore_snapshot(&mut c2, &dir, &newer).unwrap_err();
        assert!(format!("{err}").contains("更新的 Nextboard 版本"), "{err}");
        drop(c2);

        let foreign = dir.join("foreign.zip");
        write_zip(
            &foreign,
            &serde_json::json!({"app": "other-app", "schema_version": 1, "entry_count": 0}),
            &[("clipboard.db", &db_bytes)],
        );
        let mut c3 = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let err = restore_snapshot(&mut c3, &dir, &foreign).unwrap_err();
        assert!(format!("{err}").contains("不是 Nextboard 备份"), "{err}");
        // 两次失败都不该动到数据/产生险备份
        assert_eq!(count(&c3), 0);
        let safety_left = std::fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .any(|e| e.file_name().to_string_lossy().starts_with("nextboard-pre-restore"));
        assert!(!safety_left, "校验失败不应产生险备份 zip");

        drop(conn);
        drop(c3);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn restore_rejects_zip_slip_and_count_mismatch() {
        let dir = temp_dir("slip");
        let conn = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let db_bytes = read_db_bytes(&conn, &dir);

        let slip = dir.join("slip.zip");
        write_zip(
            &slip,
            &serde_json::json!({"app": "nextboard", "schema_version": 6, "entry_count": 0}),
            &[("clipboard.db", &db_bytes), ("../evil.txt", b"boom")],
        );
        let mut c1 = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let err = restore_snapshot(&mut c1, &dir, &slip).unwrap_err();
        assert!(format!("{err}").contains("非法路径"), "{err}");
        assert!(!dir.parent().unwrap().join("evil.txt").exists(), "不得写出 staging 之外");
        drop(c1);

        let mismatch = dir.join("mismatch.zip");
        write_zip(
            &mismatch,
            &serde_json::json!({"app": "nextboard", "schema_version": 6, "entry_count": 5}),
            &[("clipboard.db", &db_bytes)],
        );
        let mut c2 = crate::store::open(&dir.join("clipboard.db")).unwrap();
        let err = restore_snapshot(&mut c2, &dir, &mismatch).unwrap_err();
        assert!(format!("{err}").contains("不一致"), "{err}");

        drop(conn);
        drop(c2);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn import_merges_dedups_and_copies_blobs() {
        // A = 当前数据;B = 另一台机器的数据(打包后导入)。
        let dir_a = temp_dir("imp-a");
        let dir_b = temp_dir("imp-b");
        let mut conn_a = crate::store::open(&dir_a.join("clipboard.db")).unwrap();
        let conn_b = crate::store::open(&dir_b.join("clipboard.db")).unwrap();

        insert_entry(&conn_a, "a1", "text", Some("shared"), None, "h-shared", 1_700_000_000_000);
        insert_entry(&conn_b, "b1", "text", Some("shared"), None, "h-shared", 1_700_000_001_000); // 同内容 → 应跳过
        insert_entry(&conn_b, "b2", "text", Some("only-b"), None, "h-only-b", 1_700_000_002_000);
        insert_entry(&conn_b, "b3", "image", None, Some("2026-02/b3.png"), "h-img", 1_700_000_003_000);
        write_blob(&dir_b, "2026-02/b3.png", b"png-b3");
        let zip = dir_b.join("b.zip");
        backup_snapshot(&conn_b, &dir_b, &zip).unwrap();

        let out = import_snapshot(&mut conn_a, &dir_a, &zip).unwrap();
        assert_eq!(out.imported, 2, "b2/b3 应导入");
        assert_eq!(out.skipped, 1, "b1 与 a1 同 hash 应跳过");
        assert_eq!(count(&conn_a), 3);
        assert_eq!(get_text(&conn_a, "b2").as_deref(), Some("only-b"));
        // blob 成对搬到 A 的同名相对位置
        assert_eq!(
            std::fs::read(dir_a.join("blobs").join("2026-02").join("b3.png")).unwrap(),
            b"png-b3"
        );
        // 幂等:再导一次全部跳过,数量不变
        let again = import_snapshot(&mut conn_a, &dir_a, &zip).unwrap();
        assert_eq!(again.imported, 0);
        assert_eq!(again.skipped, 3);
        assert_eq!(count(&conn_a), 3);

        drop(conn_a);
        drop(conn_b);
        std::fs::remove_dir_all(&dir_a).unwrap();
        std::fs::remove_dir_all(&dir_b).unwrap();
    }

    #[test]
    fn import_mints_new_id_on_collision_with_blob_rename() {
        let dir_a = temp_dir("imp-c-a");
        let dir_b = temp_dir("imp-c-b");
        let mut conn_a = crate::store::open(&dir_a.join("clipboard.db")).unwrap();
        let conn_b = crate::store::open(&dir_b.join("clipboard.db")).unwrap();

        // 两端各自演进后出现同 id 不同内容(合并时 id 冲突,hash 不冲突)。
        insert_entry(&conn_a, "same-id", "text", Some("A 的正文"), None, "h-a", 1_700_000_000_000);
        insert_entry(&conn_b, "same-id", "text", Some("B 的正文"), None, "h-b", 1_700_000_001_000);
        insert_entry(&conn_b, "img-1", "image", None, Some("2026-03/img-1.png"), "h-img-b", 1_700_000_002_000);
        insert_entry(&conn_a, "img-1", "image", None, Some("2026-03/img-1.png"), "h-img-a", 1_700_000_003_000);
        write_blob(&dir_b, "2026-03/img-1.png", b"png-b");
        let zip = dir_b.join("b.zip");
        backup_snapshot(&conn_b, &dir_b, &zip).unwrap();

        let out = import_snapshot(&mut conn_a, &dir_a, &zip).unwrap();
        assert_eq!(out.imported, 2);
        assert_eq!(count(&conn_a), 4);
        // 冲突条目的 id 已重铸,旧条目完好
        assert_eq!(get_text(&conn_a, "same-id").as_deref(), Some("A 的正文"));
        let new_id: String = conn_a
            .query_row(
                "SELECT id FROM clip_entries WHERE text_content='B 的正文'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_ne!(new_id, "same-id");
        // 图片 blob 也按新 id 改名搬运,blob_path 与磁盘文件同源
        let new_img: String = conn_a
            .query_row(
                "SELECT blob_path FROM clip_entries WHERE hash='h-img-b'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_ne!(new_img, "2026-03/img-1.png");
        assert!(dir_a.join("blobs").join(&new_img).exists());
        drop(conn_a);
        drop(conn_b);
        std::fs::remove_dir_all(&dir_a).unwrap();
        std::fs::remove_dir_all(&dir_b).unwrap();
    }
}


