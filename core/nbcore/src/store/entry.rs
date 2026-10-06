//! 条目模型与 CRUD：列表/搜索、去重定位、固定、删除、清空、超限淘汰。

use rusqlite::{params, Connection, OptionalExtension, Row};
use serde::Serialize;

use crate::error::{AppError, AppResult};

/// 预览截断长度（设计文档 §5.3）。
pub const PREVIEW_MAX_CHARS: usize = 200;

/// 条目类型；DB 中存 `'text'` / `'image'` / `'file'`。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum EntryKind {
    Text,
    Image,
    File,
}

impl EntryKind {
    pub fn as_str(self) -> &'static str {
        match self {
            EntryKind::Text => "text",
            EntryKind::Image => "image",
            EntryKind::File => "file",
        }
    }

    /// 解析 IPC 传入的类型字符串；非法值返回 InvalidInput。
    pub fn parse(s: &str) -> AppResult<Self> {
        match s {
            "text" => Ok(EntryKind::Text),
            "image" => Ok(EntryKind::Image),
            "file" => Ok(EntryKind::File),
            other => Err(AppError::InvalidInput(format!("未知条目类型: {other}"))),
        }
    }

    fn from_db(s: &str) -> rusqlite::Result<Self> {
        match s {
            "text" => Ok(EntryKind::Text),
            "image" => Ok(EntryKind::Image),
            "file" => Ok(EntryKind::File),
            other => Err(rusqlite::Error::FromSqlConversionFailure(
                2,
                rusqlite::types::Type::Text,
                format!("未知条目类型: {other}").into(),
            )),
        }
    }
}

/// 跨 FFI 的条目结构，与第一代 `src/api/types.ts` 的 `ClipEntry` 镜像（编码规范 §1.5），
/// 另含 v5 新增的 `source`/`tags`（Nextboard 架构 §6，缺省序列化省略以兼容旧前端契约）。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipEntry {
    pub id: String,
    pub kind: EntryKind,
    pub text_content: Option<String>,
    /// 图片文件相对路径（相对 blobs 目录，宿主经文件桥加载）。
    pub blob_path: Option<String>,
    /// 文件条目的绝对路径列表（Phase 3；其余类型为 None）。
    pub file_paths: Option<Vec<String>>,
    pub width: Option<i64>,
    pub height: Option<i64>,
    /// 去重键，仅供后端使用，不随 IPC 下发（前端契约无此字段）。
    #[serde(skip_serializing)]
    pub hash: String,
    pub byte_size: i64,
    pub preview: String,
    pub pinned: bool,
    pub created_at: i64,
    pub last_used_at: i64,
    /// 条目来源：v5 起区分剪贴板/截图/翻译保存（一体化素材库）。DB 有 NOT NULL 默认值。
    pub source: String,
    /// 标签 JSON 数组字符串；v1 仅存储，过滤功能后续迭代。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tags: Option<String>,
}


impl ClipEntry {
    fn from_row(row: &Row<'_>) -> rusqlite::Result<Self> {
        let kind: String = row.get("kind")?;
        let file_paths: Option<String> = row.get("file_paths")?;
        let file_paths = file_paths
            .map(|json| {
                serde_json::from_str::<Vec<String>>(&json).map_err(|err| {
                    rusqlite::Error::FromSqlConversionFailure(
                        4,
                        rusqlite::types::Type::Text,
                        format!("file_paths 解析失败: {err}").into(),
                    )
                })
            })
            .transpose()?;
        Ok(ClipEntry {
            id: row.get("id")?,
            kind: EntryKind::from_db(&kind)?,
            text_content: row.get("text_content")?,
            blob_path: row.get("blob_path")?,
            file_paths,
            width: row.get("width")?,
            height: row.get("height")?,
            hash: row.get("hash")?,
            byte_size: row.get("byte_size")?,
            preview: row.get("preview")?,
            pinned: row.get::<_, i64>("pinned")? != 0,
            created_at: row.get("created_at")?,
            last_used_at: row.get("last_used_at")?,
            source: row.get("source").unwrap_or_else(|_| "clipboard".to_owned()),
            tags: row.get("tags").ok(),
        })
    }
}

/// 各分类全局计数，与 `src/api/types.ts` 的 `EntryCounts` 镜像（编码规范 §1.5）。
#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EntryCounts {
    pub all: i64,
    pub text: i64,
    pub image: i64,
    pub file: i64,
    pub pinned: i64,
}

/// 生成文本预览：按字符截断（设计文档 §5.3）。
pub fn make_preview(text: &str) -> String {
    text.chars().take(PREVIEW_MAX_CHARS).collect()
}

/// 生成文件条目预览：单文件取文件名，多文件取「{首个} 等 N 个文件」。
pub fn make_file_preview(paths: &[String]) -> String {
    let first = paths
        .first()
        .map(|p| {
            std::path::Path::new(p)
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| p.clone())
        })
        .unwrap_or_default();
    if paths.len() <= 1 {
        first
    } else {
        format!("{first} 等 {} 个文件", paths.len())
    }
}

/// `file_paths` 序列化为 JSON 存库。
fn file_paths_to_json(paths: &Option<Vec<String>>) -> AppResult<Option<String>> {
    match paths {
        Some(list) => {
            let json = serde_json::to_string(list)
                .map_err(|err| AppError::Internal(format!("file_paths 序列化失败: {err}")))?;
            Ok(Some(json))
        }
        None => Ok(None),
    }
}

/// 插入新条目。
pub fn insert(conn: &Connection, e: &ClipEntry) -> AppResult<()> {
    let file_paths_json = file_paths_to_json(&e.file_paths)?;
    conn.execute(
        "INSERT INTO clip_entries
            (id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size, preview, pinned, created_at, last_used_at, source, tags)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)",
        params![
            e.id,
            e.kind.as_str(),
            e.text_content,
            e.blob_path,
            file_paths_json,
            e.width,
            e.height,
            e.hash,
            e.byte_size,
            e.preview,
            i64::from(e.pinned),
            e.created_at,
            e.last_used_at,
            e.source,
            e.tags,
        ],
    )?;
    Ok(())
}

/// 按内容摘要查找已存在条目的 id（去重用）。
pub fn find_id_by_hash(conn: &Connection, hash: &str) -> AppResult<Option<String>> {
    let id = conn
        .query_row(
            "SELECT id FROM clip_entries WHERE hash = ?1",
            params![hash],
            |row| row.get(0),
        )
        .optional()?;
    Ok(id)
}

/// 更新最近使用时间（去重命中或复制回写时调用）。
pub fn touch(conn: &Connection, id: &str, last_used_at: i64) -> AppResult<()> {
    conn.execute(
        "UPDATE clip_entries SET last_used_at = ?1 WHERE id = ?2",
        params![last_used_at, id],
    )?;
    Ok(())
}

/// 按 id 读取单条。
pub fn get(conn: &Connection, id: &str) -> AppResult<Option<ClipEntry>> {
    let entry = conn
        .query_row(
            "SELECT id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size, preview, pinned, created_at, last_used_at, source, tags
             FROM clip_entries WHERE id = ?1",
            params![id],
            ClipEntry::from_row,
        )
        .optional()?;
    Ok(entry)
}

/// 分页列出条目：固定优先、最近使用在后；`query` 非空时按文本内容或文件路径模糊匹配，
/// `kind` / `pinned` 为分类过滤（Phase 2 F3），三者可叠加。
/// 返回 `(条目, 匹配总数)`。
pub fn list(
    conn: &Connection,
    query: Option<&str>,
    kind: Option<EntryKind>,
    pinned: Option<bool>,
    limit: i64,
    offset: i64,
) -> AppResult<(Vec<ClipEntry>, i64)> {
    let kind_str = kind.map(|k| k.as_str().to_owned());
    let pinned_i64 = pinned.map(i64::from);

    // 搜索分派（Phase 9 F27）：有关键词时先走 FTS5 全文索引（走 `idx_entries_*`
    // 之外的倒排索引，避免 LIKE 前导 % 的全表扫描）；FTS 无命中时回退 LIKE。
    // 回退的动机（方案 A，docs/phase9.md）：FTS5 的 unicode61 分词器按空格/标点切分，
    // 中文整句会成为单一 token——"打开菜单"这类无空格中文短语 MATCH 不到，
    // 兜底保证中文搜索行为不劣于迁移前。
    if let Some(term) = query.map(str::trim).filter(|q| !q.is_empty()) {
        if let Some(match_expr) = fts_expression(term) {
            let (items, total) = list_via_fts(
                conn,
                &match_expr,
                kind_str.as_deref(),
                pinned_i64,
                limit,
                offset,
            )?;
            if !items.is_empty() || total > 0 {
                return Ok((items, total));
            }
        }
    }

    let pattern = query.map(like_pattern);
    let items = conn
        .prepare(
            "SELECT id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size, preview, pinned, created_at, last_used_at, source, tags
             FROM clip_entries
             WHERE (?1 IS NULL OR text_content LIKE ?1 ESCAPE '\\'
                    OR (kind = 'file' AND file_paths LIKE ?1 ESCAPE '\\')
                    OR (kind = 'image' AND ocr_text LIKE ?1 ESCAPE '\\'))
               AND (?2 IS NULL OR kind = ?2)
               AND (?3 IS NULL OR pinned = ?3)
             ORDER BY pinned DESC, last_used_at DESC
             LIMIT ?4 OFFSET ?5",
        )?
        .query_map(params![pattern, kind_str, pinned_i64, limit, offset], ClipEntry::from_row)?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let total: i64 = conn.query_row(
        "SELECT COUNT(*) FROM clip_entries
         WHERE (?1 IS NULL OR text_content LIKE ?1 ESCAPE '\\'
                OR (kind = 'file' AND file_paths LIKE ?1 ESCAPE '\\')
                OR (kind = 'image' AND ocr_text LIKE ?1 ESCAPE '\\'))
           AND (?2 IS NULL OR kind = ?2)
           AND (?3 IS NULL OR pinned = ?3)",
        params![pattern, kind_str, pinned_i64],
        |row| row.get(0),
    )?;
    Ok((items, total))
}

/// 经 FTS5 虚拟表检索：MATCH 命中的行按 entry_id（UNINDEXED）JOIN 回主表取完整字段。
///
/// `kind`/`pinned` 过滤在 JOIN 之后做；`total` 用同一 WHERE 条件计数，
/// 保证前端分页的页数与实际一致。
fn list_via_fts(
    conn: &Connection,
    match_expr: &str,
    kind: Option<&str>,
    pinned: Option<i64>,
    limit: i64,
    offset: i64,
) -> AppResult<(Vec<ClipEntry>, i64)> {
    let items = conn
        .prepare(
            "SELECT e.id, e.kind, e.text_content, e.blob_path, e.file_paths, e.width, e.height,
                    e.hash, e.byte_size, e.preview, e.pinned, e.created_at, e.last_used_at, e.source, e.tags
             FROM clip_fts f
             JOIN clip_entries e ON e.id = f.entry_id
             WHERE clip_fts MATCH ?1
               AND (?2 IS NULL OR e.kind = ?2)
               AND (?3 IS NULL OR e.pinned = ?3)
             ORDER BY e.pinned DESC, e.last_used_at DESC
             LIMIT ?4 OFFSET ?5",
        )?
        .query_map(
            params![match_expr, kind, pinned, limit, offset],
            ClipEntry::from_row,
        )?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let total: i64 = conn.query_row(
        "SELECT COUNT(*)
         FROM clip_fts f
         JOIN clip_entries e ON e.id = f.entry_id
         WHERE clip_fts MATCH ?1
           AND (?2 IS NULL OR e.kind = ?2)
           AND (?3 IS NULL OR e.pinned = ?3)",
        params![match_expr, kind, pinned],
        |row| row.get(0),
    )?;
    Ok((items, total))
}

/// 把用户输入转成 FTS5 MATCH 表达式；不适合 MATCH 时返回 `None`（交给 LIKE）。
///
/// 规则（保守为先）：
/// * 只保留字母数字与 CJK 字符，其余（标点/空格）折叠为空格分隔——FTS5 查询语法
///   里的 `"` `(` `*` `NEAR` `AND`/`OR`/`NOT` 等都有特殊含义，透传用户输入既可能
///   语法报错、也可能改变语义（如 `a OR b` 变成或查询）。每个词用双引号包裹成短语。
/// * 纯 ASCII → 逐词短语：`open menu` → `"open" "menu"`。FTS5 中空格分隔的短语是
///   **隐式 AND**——两词都出现即命中（与顺序/相邻无关），强于 LIKE 的子串连写。
/// * 含 CJK → 整串合并为单一短语：unicode61 把无空格中文折叠为一个 token，
///   短语匹配等价于子串连写；中英混排时 CJK 边界上的英文单词仍会被正确切分，
///   故把全部词拼接成一个短语以覆盖「中文包着英文」的常见形态。
/// * 空串/全是标点 → `None`（走 LIKE，行为同无索引路径）。
fn fts_expression(query: &str) -> Option<String> {
    let mut words: Vec<String> = Vec::new();
    let mut current = String::new();
    let mut has_cjk = false;
    for ch in query.chars() {
        if ch.is_alphanumeric() {
            // is_alphanumeric 对 CJK 同样为真；单独标记以决定短语策略
            if is_cjk(ch) {
                has_cjk = true;
            }
            current.push(ch);
        } else if !current.is_empty() {
            words.push(std::mem::take(&mut current));
        }
    }
    if !current.is_empty() {
        words.push(current);
    }
    if words.is_empty() {
        return None;
    }
    // 双引号包裹防 FTS5 语法歧义；词内的 `"` 已被上面的字符过滤剔除
    if has_cjk {
        // 中文：整串一个短语（词与词之间被标点隔开时合并为相邻短语，与手写直觉一致）
        Some(format!("\"{}\"", words.join("")))
    } else {
        Some(
            words
                .iter()
                .map(|w| format!("\"{w}\""))
                .collect::<Vec<_>>()
                .join(" "),
        )
    }
}

/// 是否为 CJK 统一表意文字（含扩展 A）与兼容表意文字。
fn is_cjk(ch: char) -> bool {
    matches!(ch as u32,
        0x4E00..=0x9FFF        // CJK 统一表意文字
        | 0x3400..=0x4DBF      // 扩展 A
        | 0xF900..=0xFAFF      // 兼容表意文字
    )
}

/// 各分类的全局计数（一次查询取全，供前端标签页徽标）。
pub fn counts(conn: &Connection) -> AppResult<EntryCounts> {
    let c = conn.query_row(
        "SELECT COUNT(*),
                COALESCE(SUM(CASE WHEN kind = 'text' THEN 1 ELSE 0 END), 0),
                COALESCE(SUM(CASE WHEN kind = 'image' THEN 1 ELSE 0 END), 0),
                COALESCE(SUM(CASE WHEN kind = 'file' THEN 1 ELSE 0 END), 0),
                COALESCE(SUM(CASE WHEN pinned = 1 THEN 1 ELSE 0 END), 0)
         FROM clip_entries",
        [],
        |row| {
            Ok(EntryCounts {
                all: row.get(0)?,
                text: row.get(1)?,
                image: row.get(2)?,
                file: row.get(3)?,
                pinned: row.get(4)?,
            })
        },
    )?;
    Ok(c)
}

/// 删除单条；id 不存在时报错。返回其图片文件路径供调用方删除文件。
pub fn delete(conn: &Connection, id: &str) -> AppResult<Option<String>> {
    let row: Option<Option<String>> = conn
        .query_row(
            "SELECT blob_path FROM clip_entries WHERE id = ?1",
            params![id],
            |row| row.get(0),
        )
        .optional()?;
    if row.is_none() {
        return Err(AppError::InvalidInput(format!("条目不存在: {id}")));
    }
    conn.execute("DELETE FROM clip_entries WHERE id = ?1", params![id])?;
    Ok(row.flatten())
}

/// 清空全部条目，返回所有图片文件路径供调用方删除文件。
pub fn clear(conn: &Connection) -> AppResult<Vec<String>> {
    let paths: Vec<String> = conn
        .prepare("SELECT blob_path FROM clip_entries WHERE blob_path IS NOT NULL")?
        .query_map([], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    conn.execute("DELETE FROM clip_entries", [])?;
    Ok(paths)
}

/// 设置固定状态；id 不存在时报错。
pub fn set_pinned(conn: &Connection, id: &str, pinned: bool) -> AppResult<()> {
    let changed = conn.execute(
        "UPDATE clip_entries SET pinned = ?1 WHERE id = ?2",
        params![i64::from(pinned), id],
    )?;
    if changed == 0 {
        return Err(AppError::InvalidInput(format!("条目不存在: {id}")));
    }
    Ok(())
}

/// 写入图片条目的 OCR 文本（Phase 9 F28 自动入库）。
///
/// UPDATE 命中 `clip_fts_update` 触发器 → FTS 索引同步更新，文本立即可被搜索。
/// id 不存在时报错（与 `set_pinned` 同一口径：调用方拿到的 id 来自刚入库的条目，
/// 不存在说明有并发删除，宁可报错也不静默写丢）。
pub fn set_ocr_text(conn: &Connection, id: &str, text: &str) -> AppResult<()> {
    let changed = conn.execute(
        "UPDATE clip_entries SET ocr_text = ?1 WHERE id = ?2",
        params![text, id],
    )?;
    if changed == 0 {
        return Err(AppError::InvalidInput(format!("条目不存在: {id}")));
    }
    Ok(())
}

/// 超限淘汰（设计文档 §5.3）：总数超过 `keep` 时，未固定条目按最近使用时间
/// 从旧到新删除，返回被删条目的图片文件路径。
pub fn evict(conn: &Connection, keep: i64) -> AppResult<Vec<String>> {
    let total: i64 = conn.query_row("SELECT COUNT(*) FROM clip_entries", [], |row| row.get(0))?;
    let excess = total - keep;
    if excess <= 0 {
        return Ok(Vec::new());
    }
    let victims: Vec<(String, Option<String>)> = {
        let mut stmt = conn.prepare(
            "SELECT id, blob_path FROM clip_entries WHERE pinned = 0
             ORDER BY last_used_at ASC, created_at ASC LIMIT ?1",
        )?;
        // 先绑定到局部变量再出块，避免块尾临时值与 stmt 的借用冲突
        let rows = stmt
            .query_map(params![excess], |row| Ok((row.get(0)?, row.get(1)?)))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        rows
    };
    let mut blobs = Vec::new();
    for (id, blob) in &victims {
        conn.execute("DELETE FROM clip_entries WHERE id = ?1", params![id])?;
        if let Some(path) = blob {
            blobs.push(path.clone());
        }
    }
    Ok(blobs)
}

/// 最近使用的可贴图条目（一键贴屏用）：**图片优先**，没有图片时回退最近文本。
///
/// 口径：图片是主要贴图对象；文本作为无图时的兜底（用户要求"无图时也能贴文本内容"）。
/// 重复复制同一内容会 `touch` 置顶，故取到的就是"刚复制/刚入库的那条"；不受 `pinned`
/// 排序影响。文件条目不可贴图，排除在外。
/// 命中索引 `idx_entries_kind (kind, last_used_at DESC)`。
pub fn latest_pinnable(conn: &Connection) -> AppResult<Option<ClipEntry>> {
    for kind in ["image", "text"] {
        let entry = conn
            .query_row(
                "SELECT id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size, preview, pinned, created_at, last_used_at, source, tags
                 FROM clip_entries
                 WHERE kind = ?1
                 ORDER BY last_used_at DESC, created_at DESC
                 LIMIT 1",
                [kind],
                ClipEntry::from_row,
            )
            .optional()?;
        if entry.is_some() {
            return Ok(entry);
        }
    }
    Ok(None)
}

/// 构造 LIKE 匹配串并转义通配符（`%` `_` `\`），防止用户输入被当作模式。
fn like_pattern(query: &str) -> String {
    let mut pattern = String::with_capacity(query.len() + 2);
    pattern.push('%');
    for c in query.chars() {
        if matches!(c, '%' | '_' | '\\') {
            pattern.push('\\');
        }
        pattern.push(c);
    }
    pattern.push('%');
    pattern
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 构造测试条目：text 类型、固定 0/1、指定最近使用时间。
    fn entry(id: &str, hash: &str, text: &str, pinned: bool, used: i64) -> ClipEntry {
        ClipEntry {
            id: id.into(),
            kind: EntryKind::Text,
            text_content: Some(text.into()),
            blob_path: None,
            file_paths: None,
            width: None,
            height: None,
            hash: hash.into(),
            byte_size: text.len() as i64,
            preview: make_preview(text),
            pinned,
            created_at: used,
            last_used_at: used,
            source: "clipboard".into(),
            tags: None,
        }
    }

    fn image_entry(id: &str, hash: &str, blob: &str, pinned: bool, used: i64) -> ClipEntry {
        let mut e = entry(id, hash, "", pinned, used);
        e.kind = EntryKind::Image;
        e.text_content = None;
        e.blob_path = Some(blob.into());
        e.byte_size = 1234;
        e
    }

    fn file_entry(id: &str, hash: &str, paths: &[&str], pinned: bool, used: i64) -> ClipEntry {
        let mut e = entry(id, hash, "", pinned, used);
        e.kind = EntryKind::File;
        e.text_content = None;
        e.file_paths = Some(paths.iter().map(|p| p.to_string()).collect());
        e.preview = make_file_preview(&e.file_paths.clone().unwrap());
        e.byte_size = 4096;
        e
    }

    #[test]
    fn preview_truncates_by_chars() {
        let long = "剪".repeat(PREVIEW_MAX_CHARS + 50);
        assert_eq!(make_preview(&long).chars().count(), PREVIEW_MAX_CHARS);
        assert_eq!(make_preview("短文本"), "短文本");
    }

    #[test]
    fn insert_and_list_orders_by_pinned_then_last_used() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "alpha", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "bravo", false, 300)).unwrap();
        insert(&conn, &entry("c", "h-c", "charlie", true, 100)).unwrap();
        let (items, total) = list(&conn, None, None, None, 10, 0).unwrap();
        assert_eq!(total, 3);
        let ids: Vec<&str> = items.iter().map(|e| e.id.as_str()).collect();
        assert_eq!(ids, ["c", "b", "a"]);
    }

    #[test]
    fn list_respects_limit_offset_and_total() {
        let conn = crate::store::memory();
        for i in 0..5 {
            insert(
                &conn,
                &entry(
                    &format!("id{i}"),
                    &format!("h{i}"),
                    &format!("t{i}"),
                    false,
                    i,
                ),
            )
            .unwrap();
        }
        let (page, total) = list(&conn, None, None, None, 2, 1).unwrap();
        assert_eq!(total, 5);
        assert_eq!(page.len(), 2);
        assert_eq!(page[0].id, "id3");
    }

    #[test]
    fn find_by_hash_and_touch_updates_last_used() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-1", "alpha", false, 100)).unwrap();
        assert_eq!(find_id_by_hash(&conn, "h-1").unwrap().as_deref(), Some("a"));
        assert_eq!(find_id_by_hash(&conn, "h-2").unwrap(), None);
        touch(&conn, "a", 999).unwrap();
        assert_eq!(get(&conn, "a").unwrap().unwrap().last_used_at, 999);
    }

    #[test]
    fn search_matches_content_and_escapes_wildcards() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "折扣 50%_off 活动", false, 1)).unwrap();
        insert(&conn, &entry("b", "h-b", "plain text", false, 2)).unwrap();

        let (hits, _) = list(&conn, Some("50"), None, None, 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].id, "a");

        // "_" 是通配符，未转义时会匹配任意字符；转义后只匹配字面量
        let (hits, _) = list(&conn, Some("%_off"), None, None, 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].id, "a");

        let (hits, _) = list(&conn, Some("_"), None, None, 10, 0).unwrap();
        assert_eq!(hits.len(), 1, "转义后下划线应只匹配含字面量 '_' 的文本");
        assert_eq!(hits[0].id, "a");
    }

    #[test]
    fn delete_returns_blob_path_and_errors_when_missing() {
        let conn = crate::store::memory();
        insert(
            &conn,
            &image_entry("img", "h-i", "C:/blobs/a.png", false, 1),
        )
        .unwrap();
        insert(&conn, &entry("txt", "h-t", "text", false, 2)).unwrap();

        assert_eq!(
            delete(&conn, "img").unwrap().as_deref(),
            Some("C:/blobs/a.png")
        );
        assert_eq!(delete(&conn, "txt").unwrap(), None);
        assert!(delete(&conn, "nope").is_err());
    }

    #[test]
    fn clear_removes_all_and_collects_blobs() {
        let conn = crate::store::memory();
        insert(
            &conn,
            &image_entry("img", "h-i", "C:/blobs/a.png", false, 1),
        )
        .unwrap();
        insert(&conn, &entry("txt", "h-t", "text", false, 2)).unwrap();
        let blobs = clear(&conn).unwrap();
        assert_eq!(blobs, vec!["C:/blobs/a.png".to_string()]);
        assert_eq!(list(&conn, None, None, None, 10, 0).unwrap().1, 0);
    }

    #[test]
    fn set_pinned_toggles_and_errors_when_missing() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "alpha", false, 1)).unwrap();
        set_pinned(&conn, "a", true).unwrap();
        assert!(get(&conn, "a").unwrap().unwrap().pinned);
        assert!(set_pinned(&conn, "ghost", true).is_err());
    }

    #[test]
    fn evict_keeps_pinned_and_newest() {
        let conn = crate::store::memory();
        insert(
            &conn,
            &image_entry("old", "h-1", "C:/blobs/old.png", false, 100),
        )
        .unwrap();
        insert(
            &conn,
            &image_entry("mid", "h-2", "C:/blobs/mid.png", false, 200),
        )
        .unwrap();
        insert(
            &conn,
            &image_entry("pin", "h-3", "C:/blobs/pin.png", true, 300),
        )
        .unwrap();
        insert(
            &conn,
            &image_entry("new", "h-4", "C:/blobs/new.png", false, 400),
        )
        .unwrap();

        // 总数 4 > 保留 2：应淘汰最旧的未固定两条
        let blobs = evict(&conn, 2).unwrap();
        assert_eq!(
            blobs,
            vec![
                "C:/blobs/old.png".to_string(),
                "C:/blobs/mid.png".to_string()
            ]
        );
        let ids: Vec<String> = list(&conn, None, None, None, 10, 0)
            .unwrap()
            .0
            .into_iter()
            .map(|e| e.id)
            .collect();
        assert_eq!(ids, ["pin", "new"]);
        // 未超限时不动任何条目
        assert!(evict(&conn, 2).unwrap().is_empty());
    }

    #[test]
    fn list_filters_by_kind_pinned_and_query() {
        let conn = crate::store::memory();
        insert(&conn, &entry("t1", "h-1", "alpha 报价单", false, 100)).unwrap();
        insert(&conn, &entry("t2", "h-2", "bravo", true, 200)).unwrap();
        insert(
            &conn,
            &image_entry("i1", "h-3", "C:/blobs/1.png", false, 300),
        )
        .unwrap();
        insert(
            &conn,
            &image_entry("i2", "h-4", "C:/blobs/2.png", true, 400),
        )
        .unwrap();

        let ids =
            |items: Vec<ClipEntry>| -> Vec<String> { items.into_iter().map(|e| e.id).collect() };

        assert_eq!(
            ids(list(&conn, None, Some(EntryKind::Text), None, 10, 0)
                .unwrap()
                .0),
            ["t2", "t1"]
        );
        assert_eq!(
            ids(list(&conn, None, Some(EntryKind::Image), None, 10, 0)
                .unwrap()
                .0),
            ["i2", "i1"]
        );
        assert_eq!(
            ids(list(&conn, None, None, Some(true), 10, 0).unwrap().0),
            ["i2", "t2"]
        );
        assert_eq!(
            ids(list(&conn, None, Some(EntryKind::Text), Some(true), 10, 0)
                .unwrap()
                .0),
            ["t2"]
        );
        // 分类与搜索叠加
        assert_eq!(
            ids(
                list(&conn, Some("alpha"), Some(EntryKind::Text), None, 10, 0)
                    .unwrap()
                    .0
            ),
            ["t1"]
        );
        assert_eq!(
            list(&conn, None, Some(EntryKind::Image), None, 10, 0)
                .unwrap()
                .1,
            2
        );
    }

    #[test]
    fn counts_reports_per_category() {
        let conn = crate::store::memory();
        let empty = counts(&conn).unwrap();
        assert_eq!(
            (empty.all, empty.text, empty.image, empty.pinned),
            (0, 0, 0, 0)
        );

        insert(&conn, &entry("t1", "h-1", "alpha", false, 100)).unwrap();
        insert(&conn, &entry("t2", "h-2", "bravo", true, 200)).unwrap();
        insert(
            &conn,
            &image_entry("i1", "h-3", "C:/blobs/1.png", false, 300),
        )
        .unwrap();
        insert(&conn, &file_entry("f1", "h-4", &["C:/a.txt"], true, 400)).unwrap();
        let c = counts(&conn).unwrap();
        assert_eq!((c.all, c.text, c.image, c.file, c.pinned), (4, 2, 1, 1, 2));
    }

    #[test]
    fn file_entries_roundtrip_and_search() {
        let conn = crate::store::memory();
        let paths = ["C:/work/报告.docx", "C:/work/数据.xlsx"];
        insert(&conn, &file_entry("f1", "h-f1", &paths, false, 100)).unwrap();

        let got = get(&conn, "f1").unwrap().unwrap();
        assert_eq!(got.kind, EntryKind::File);
        let expected: Vec<String> = paths.iter().map(|p| p.to_string()).collect();
        assert_eq!(got.file_paths.as_deref(), Some(expected.as_slice()));
        assert_eq!(got.preview, "报告.docx 等 2 个文件");

        // 文件名与路径片段均可命中搜索；分类过滤正确
        assert_eq!(list(&conn, Some("报告"), None, None, 10, 0).unwrap().1, 1);
        assert_eq!(list(&conn, Some("docx"), None, None, 10, 0).unwrap().1, 1);
        let (items, total) = list(&conn, None, Some(EntryKind::File), None, 10, 0).unwrap();
        assert_eq!(total, 1);
        assert_eq!(items[0].id, "f1");
    }

    #[test]
    fn file_preview_names() {
        assert_eq!(make_file_preview(&["C:/a/b.txt".to_string()]), "b.txt");
        assert_eq!(
            make_file_preview(&["C:/a/b.txt".to_string(), "C:/a/c.png".to_string()]),
            "b.txt 等 2 个文件"
        );
    }

    #[test]
    fn latest_pinnable_prefers_image_then_falls_back_to_text() {
        let conn = crate::store::memory();
        insert(&conn, &entry("t1", "h-t1", "文本", true, 500)).unwrap();
        insert(
            &conn,
            &image_entry("i-old", "h-i1", "C:/blobs/old.png", false, 100),
        )
        .unwrap();
        insert(
            &conn,
            &image_entry("i-new", "h-i2", "C:/blobs/new.png", false, 300),
        )
        .unwrap();
        // 图片优先：即使文本 last_used_at 更晚，也优先贴图片；固定项不影响排序
        assert_eq!(latest_pinnable(&conn).unwrap().unwrap().id, "i-new");

        // 再次复制旧图 → touch 置顶后应换成它
        touch(&conn, "i-old", 900).unwrap();
        assert_eq!(latest_pinnable(&conn).unwrap().unwrap().id, "i-old");

        // 无图片时回退到最近文本（用户要求：没有图也能贴文本内容）
        let text_only = crate::store::memory();
        insert(&text_only, &entry("t1", "h-t1", "早", false, 100)).unwrap();
        insert(&text_only, &entry("t2", "h-t2", "晚", false, 900)).unwrap();
        let picked = latest_pinnable(&text_only).unwrap().unwrap();
        assert_eq!(picked.id, "t2");
        assert_eq!(picked.kind, EntryKind::Text);

        // 空库返回 None
        let empty = crate::store::memory();
        assert!(latest_pinnable(&empty).unwrap().is_none());
    }

    #[test]
    fn migration_v3_rebuilds_table_preserving_data() {
        let mut conn = rusqlite::Connection::open_in_memory().unwrap();
        // 手工构造 v1 库（尚无 file 支持）并写入一条文本数据
        conn.execute_batch(crate::store::MIGRATION_V1).unwrap();
        conn.pragma_update(None, "user_version", 1).unwrap();
        conn.execute(
            "INSERT INTO clip_entries
                (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at)
             VALUES ('t1', 'text', '旧数据', 'h-old', 6, '旧数据', 0, 1, 1)",
            [],
        )
        .unwrap();

        crate::store::migrate(&mut conn).unwrap();

        // 既有数据保留；新表允许 'file'；版本到位
        let count: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM clip_entries WHERE id = 't1' AND text_content = '旧数据'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(count, 1);
        conn.execute(
            "INSERT INTO clip_entries
                (id, kind, file_paths, hash, byte_size, preview, pinned, created_at, last_used_at)
             VALUES ('f1', 'file', '[\"C:/a.txt\"]', 'h-new', 10, 'a.txt', 0, 2, 2)",
            [],
        )
        .unwrap();
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .unwrap();
        assert_eq!(version, crate::store::LATEST_VERSION);
    }

    #[test]
    fn migrate_is_idempotent() {
        let mut conn = rusqlite::Connection::open_in_memory().unwrap();
        crate::store::migrate(&mut conn).unwrap();
        crate::store::migrate(&mut conn).unwrap();
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .unwrap();
        assert_eq!(version, crate::store::LATEST_VERSION);
        let index_count: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_entries_kind'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(index_count, 1, "v2 的 kind 索引应已创建");
    }

    // ---------- FTS5 全文检索（Phase 9 F27） ----------

    /// **迁移**：v4 建 FTS 表并为存量数据建索引。
    #[test]
    fn migration_v4_indexes_existing_rows() {
        let conn = rusqlite::Connection::open_in_memory().unwrap();
        conn.execute_batch(crate::store::MIGRATION_V1).unwrap();
        conn.execute_batch(crate::store::MIGRATION_V2).unwrap();
        conn.execute_batch(crate::store::MIGRATION_V3).unwrap();
        // v4 之前插入的存量数据
        conn.execute(
            "INSERT INTO clip_entries
                (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at)
             VALUES ('old1', 'text', 'legacy document content', 'h1', 10, 'legacy', 0, 1, 1)",
            [],
        )
        .unwrap();
        conn.execute_batch(crate::store::MIGRATION_V4).unwrap();

        let indexed: i64 = conn
            .query_row("SELECT COUNT(*) FROM clip_fts", [], |row| row.get(0))
            .unwrap();
        assert_eq!(indexed, 1, "存量行应已被 rebuild 进索引");
        // list() 面向**最新 schema**(v5 起含 source/tags 列):补到 v5 再验证可搜索性
        conn.execute_batch(crate::store::MIGRATION_V5).unwrap();
        let (items, _) = list(&conn, Some("legacy"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "迁移后存量数据应可被搜到");
    }

    /// **迁移**：ocr_text 列存在且参与索引。
    #[test]
    fn migration_v4_adds_ocr_text_column() {
        let conn = crate::store::memory();
        let has_column: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM pragma_table_info('clip_entries') WHERE name = 'ocr_text'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(has_column, 1, "v4 应加 ocr_text 列");
        // OCR 文本写入后应可被搜到（模拟回填）
        insert(&conn, &entry("img", "h-img", "", false, 1)).unwrap();
        conn.execute(
            "UPDATE clip_entries SET ocr_text = 'screenshot recognized words' WHERE id = 'img'",
            [],
        )
        .unwrap();
        let (items, _) = list(&conn, Some("recognized"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "OCR 文本应参与全文检索");
    }

    /// **索引一致性**：增删改后 FTS 与主表不漂移。
    ///
    /// 重点覆盖 DELETE 触发器（`DELETE FROM clip_fts WHERE entry_id = …`）——
    /// 外部内容表曾因 rowid 类型不匹配整体被否决，普通表的删除路径必须确认干净。
    #[test]
    fn fts_index_stays_in_sync_after_insert_delete_update() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "alpha token", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "bravo token", false, 200)).unwrap();

        // UPDATE：touch 走的是 last_used_at（不触发索引重建），文本更新才触发
        conn.execute(
            "UPDATE clip_entries SET text_content = 'alpha revised' WHERE id = 'a'",
            [],
        )
        .unwrap();
        let (items, _) = list(&conn, Some("revised"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "更新后的文本应可被搜到");
        let (stale, _) = list(&conn, Some("token"), None, None, 10, 0).unwrap();
        assert_eq!(stale.len(), 1, "旧词应只出现在未改的 b 条目");

        // DELETE：删除后旧词不应再命中
        conn.execute("DELETE FROM clip_entries WHERE id = 'b'", [])
            .unwrap();
        let (items, total) = list(&conn, Some("bravo"), None, None, 10, 0).unwrap();
        assert_eq!((items.len(), total), (0, 0), "已删除条目不应再被搜到");
        // FTS 表行数与主表一致（索引不残留）
        let fts_count: i64 = conn
            .query_row("SELECT COUNT(*) FROM clip_fts", [], |row| row.get(0))
            .unwrap();
        let main_count: i64 = conn
            .query_row("SELECT COUNT(*) FROM clip_entries", [], |row| row.get(0))
            .unwrap();
        assert_eq!(fts_count, main_count, "FTS 与主表行数应一致");
    }

    /// **英文多词**：FTS5 里空格分隔的多个短语是**隐式 AND**——两词都出现即命中，
    /// 与出现顺序无关。这是对 LIKE 的**行为增强**（LIKE 要求子串连写），
    /// 也是选 FTS5 的主要收益：搜 "menu settings" 能命中
    /// "…menu is under settings…" 这类两词分离的文本。
    #[test]
    fn fts_matches_english_words_as_implicit_and() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "open the main menu", false, 1)).unwrap();
        // 相邻词命中
        let (items, _) = list(&conn, Some("main menu"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "相邻词应命中");
        // 乱序（两词都出现但不相邻/顺序不同）——FTS 的 AND 语义**命中**，
        // LIKE 的连写语义不命中：这是升级而非回归，测试固定该行为
        let (items, total) = list(&conn, Some("menu open"), None, None, 10, 0).unwrap();
        assert_eq!(
            (items.len(), total),
            (1, 1),
            "两词都出现即命中（FTS 隐式 AND，强于 LIKE 连写）"
        );
        // 只出现其中一词 → 不命中（AND 语义）
        let (_, total) = list(&conn, Some("menu missingword"), None, None, 10, 0).unwrap();
        assert_eq!(total, 0, "缺任一词不应命中");
        // 单词命中
        let (items, _) = list(&conn, Some("menu"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1);
    }

    /// **中文**：无空格整句按单一短语匹配（方案 A 的 FTS 通道）；
    /// FTS 通道命中不了的场景由 LIKE 兜底，两者结果一致。
    #[test]
    fn fts_handles_chinese_phrase_with_like_fallback() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "打开主菜单查看设置", false, 1)).unwrap();
        // 子串命中（整句的一部分）
        let (items, _) = list(&conn, Some("主菜单"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "中文子串应命中（FTS 或 LIKE 兜底）");
        // 纯标点输入：fts_expression 返回 None，直接走 LIKE，不应报错
        let (items, _) = list(&conn, Some("！？"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 0, "纯标点不命中任何内容但不报错");
    }

    /// **文件路径**：FTS 对 file_paths 列同样生效（LIKE 路径只对 file 条目生效）。
    #[test]
    fn fts_matches_file_paths() {
        let conn = crate::store::memory();
        insert(
            &conn,
            &file_entry("f", "h-f", &["C:/reports/q3-summary.xlsx"], false, 1),
        )
        .unwrap();
        let (items, _) = list(&conn, Some("summary"), None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "文件路径词应可被搜到");
    }

    /// **特殊字符**：FTS5 查询语法中的保留字符不得破坏查询（用户输入被折叠）。
    #[test]
    fn fts_expression_neutralizes_special_syntax() {
        // 这些输入在 FTS5 MATCH 语法里都有特殊含义，透传会语法报错或改变语义
        for malicious in ["a\"OR\"b", "NEAR(a b)", "(a*", "a AND b", "\"quoted\""] {
            let conn = crate::store::memory();
            insert(&conn, &entry("a", "h-a", "alpha beta", false, 1)).unwrap();
            // 不 panic、不报错即通过（命中与否取决于折叠后的词）
            let _ = list(&conn, Some(malicious), None, None, 10, 0).unwrap();
        }
    }

    /// **表达式构造**：中英文分别走短语与 AND 策略；纯标点返回 None。
    #[test]
    fn fts_expression_building_rules() {
        assert_eq!(
            fts_expression("open menu").as_deref(),
            Some("\"open\" \"menu\""),
            "英文多词 → 逐词短语（AND 语义）"
        );
        assert_eq!(
            fts_expression("打开菜单").as_deref(),
            Some("\"打开菜单\""),
            "中文 → 整串一个短语"
        );
        assert_eq!(
            fts_expression("OpenAI 是什么").as_deref(),
            Some("\"OpenAI是什么\""),
            "中英混排 → 合并短语（CJK 优先）"
        );
        assert_eq!(fts_expression("!!!"), None, "纯标点 → 交给 LIKE");
        assert_eq!(fts_expression("   "), None);
    }

    /// **回归**：图片 OCR 文本搜不到（真机缺陷，Phase 9）的端到端链路验证。
    #[test]
    fn ocr_text_searchable_via_list() {
        let conn = crate::store::memory();
        conn.execute(
            "INSERT INTO clip_entries (id, kind, blob_path, hash, byte_size, preview, pinned, created_at, last_used_at)
             VALUES ('img1', 'image', 'x/y.png', 'h1', 100, '', 0, 1, 1)",
            [],
        )
        .unwrap();
        let fts_rows: i64 = conn
            .query_row("SELECT COUNT(*) FROM clip_fts", [], |r| r.get(0))
            .unwrap();
        assert_eq!(fts_rows, 1, "环节[2]：插入图片条目后 FTS 应有 1 行");

        set_ocr_text(&conn, "img1", "会议纪要 项目进度").unwrap();

        let ocr_in_fts: Option<String> = conn
            .query_row(
                "SELECT ocr_text FROM clip_fts WHERE entry_id = 'img1'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(
            ocr_in_fts.as_deref(),
            Some("会议纪要 项目进度"),
            "环节[4]：UPDATE 触发器应把 ocr_text 同步进 FTS"
        );

        // 环节[5]：FTS5 unicode61 把"会议纪要 项目进度"按空格切成两个**整串** token，
        // 子串"会议"MATCH 不命中——这是 FTS5 对中文的固有行为（见
        // `debug_cjk_tokenization` 的逐粒度探针结果），不是索引同步问题。
        // 中文子串的命中靠环节[6] 的 LIKE 兜底。
        let token_hit: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM clip_fts WHERE clip_fts MATCH '\"会议\"'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(
            token_hit, 0,
            "FTS 对中文子串不命中（固有行为）；若将来换分词器此断言需同步更新"
        );

        // 环节[6]：list() 走 FTS（未命中）→ LIKE 兜底（ocr_text 纳入后应命中）。
        // **这里曾是真实缺陷**：LIKE 回退只查 text_content/file_paths，
        // 图片条目 text_content 为 NULL，OCR 文本永远搜不到。
        let (items, total) = list(&conn, Some("会议"), None, None, 10, 0).unwrap();
        assert_eq!(
            (items.len(), total),
            (1, 1),
            "中文子串搜 OCR 文本应经 LIKE 兜底命中"
        );

        // 环节[7]：整串词（与 OCR 文本里的 token 完全一致）应走 FTS 命中
        let (items, total) = list(&conn, Some("会议纪要"), None, None, 10, 0).unwrap();
        assert_eq!((items.len(), total), (1, 1), "FTS 通道应命中完整中文词");
    }

    /// 【行为记录】FTS5 unicode61 对连续中文的分词粒度探针。
    ///
    /// 结论固定为断言：无空格中文整串 = 一个 token（"会议纪要"命中、"会议"不命中）；
    /// 英文按空格分词正常。这解释了为什么中文子串必须依赖 LIKE 兜底。
    #[test]
    fn fts_cjk_tokenization_is_whole_run() {
        let conn = crate::store::memory();
        conn.execute(
            "INSERT INTO clip_entries (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at)
             VALUES ('t1', 'text', '会议纪要 项目进度 meeting notes', 'h1', 10, '', 0, 1, 1)",
            [],
        )
        .unwrap();
        // FTS5 没有独立 vocab 表可查，用逐粒度 MATCH 试探并**断言**分词行为
        for (probe, expected) in [
            ("会议纪要", 1), // 完整无空格串 = 一个 token，命中
            ("会议", 0),     // 子串：token 是整串，MATCH 不命中
            ("纪要", 0),     // 同上
            ("项目进度", 1), // 空格分隔的另一半
            ("项目", 0),     // 子串不命中
            ("meeting", 1),  // 英文按空格分词，整词命中
            ("notes", 1),
        ] {
            let expr = fts_expression(probe).unwrap_or_default();
            let hit: i64 = conn
                .query_row(
                    "SELECT COUNT(*) FROM clip_fts WHERE clip_fts MATCH ?1",
                    params![expr],
                    |r| r.get(0),
                )
                .unwrap();
            assert_eq!(
                hit, expected,
                "MATCH {expr:?}：unicode61 的中文分词行为变化时此测试会暴露"
            );
        }
    }
}
