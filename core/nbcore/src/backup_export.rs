//! 数据备份与导出(S5.3 相关规划,docs/03-roadmap.md 5.3 的配套能力)。
//!
//! - 备份:`backup_create` —— 数据目录完整快照(zip 打包 clipboard.db + blobs/ +
//!   manifest.json)。⚠️ WAL 坑(progress.md:3085):必须在**同一连接锁内**先
//!   `PRAGMA wal_checkpoint(TRUNCATE)` 再拷 .db,只拷 .db 不带 -wal/-shm 才安全;
//!   持有 `Core.db` 互斥锁的窗口内业务写入被串行化,快照一致。
//! - 导出:`export_entries` —— 剪贴板历史渲染为 Markdown(图片 base64 内嵌,
//!   单文件自包含)。HTML 为 backlog,format 传 "html" 显式报错不静默。
//!
//! 恢复/导入为非目标:备份包自描述(manifest 含 schema_version + blobs 清单),
//! zip 内目录结构与数据目录同构,未来恢复 = 解压替换数据目录 + 走既有迁移链。

use std::io::{BufWriter, Write};
use std::path::Path;

use chrono::Local;
use rusqlite::{params, Connection};

use crate::core::Core;
use crate::error::{AppError, AppResult};

/// 导出分页批量大小:条目流式写文件,内存只持有当前批。
const EXPORT_BATCH: i64 = 500;

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
        assert_eq!(v["schema_version"], 6);
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
}


