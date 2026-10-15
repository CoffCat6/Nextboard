//! 条目模型与 CRUD：列表/搜索、去重定位、固定、删除、清空、超限淘汰。

use rusqlite::{params, Connection, OptionalExtension, Row};
use serde::Serialize;

use crate::error::{AppError, AppResult};

/// 预览截断长度（设计文档 §5.3）。
pub const PREVIEW_MAX_CHARS: usize = 200;

/// 条目类型；DB 中存 `'text'` / `'image'` / `'file'` / `'todo'`。
///
/// ⚠️ 新增类型必须**三处同时**扩展(`as_str` / `parse` / `from_db`):`from_db` 漏了
/// 会让老版本读新库时整表炸(FromSqlConversionFailure),发布说明须标注
/// "先升 core 再升宿主,或同包升级"(本应用 exe 与 nbcore.dll 同目录,天然同步)。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum EntryKind {
    Text,
    Image,
    File,
    /// 待办(docs/05-todo.md):主体是文本,携带 due/remind/notify 三组可选时刻。
    Todo,
}

impl EntryKind {
    pub fn as_str(self) -> &'static str {
        match self {
            EntryKind::Text => "text",
            EntryKind::Image => "image",
            EntryKind::File => "file",
            EntryKind::Todo => "todo",
        }
    }

    /// 解析 IPC 传入的类型字符串；非法值返回 InvalidInput。
    pub fn parse(s: &str) -> AppResult<Self> {
        match s {
            "text" => Ok(EntryKind::Text),
            "image" => Ok(EntryKind::Image),
            "file" => Ok(EntryKind::File),
            "todo" => Ok(EntryKind::Todo),
            other => Err(AppError::InvalidInput(format!("未知条目类型: {other}"))),
        }
    }

    fn from_db(s: &str) -> rusqlite::Result<Self> {
        match s {
            "text" => Ok(EntryKind::Text),
            "image" => Ok(EntryKind::Image),
            "file" => Ok(EntryKind::File),
            "todo" => Ok(EntryKind::Todo),
            other => Err(rusqlite::Error::FromSqlConversionFailure(
                2,
                rusqlite::types::Type::Text,
                format!("未知条目类型: {other}").into(),
            )),
        }
    }
}

/// 主列表联查下发的待办徽标(docs/05-todo.md §2.3):卡片正面只需完成态/截止/逾期
/// 三样;remind/notify 不随行下发(编辑预填走 todo_get 单独取)。
#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TodoBadge {
    /// 截止时刻(unix 毫秒);None = 无截止。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub due_at: Option<i64>,
    /// 是否已完成(归档)。
    pub done: bool,
    /// 未完成且已过截止。
    pub overdue: bool,
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
    /// 待办徽标(docs/05-todo.md §2.3):list_entries 对 todo 条目 LEFT JOIN todo_meta
    /// 联查所得;非 todo 条目与不联查的查询(get 单条等)恒为 None,序列化整键省略 ——
    /// 与 tags 同一兼容手法,旧宿主读到多余键也只是忽略。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub todo: Option<TodoBadge>,
}


impl ClipEntry {
    /// 从行构造;按**列名**取值,便于 JOIN 查询复用(todo::list 等)。
    pub(crate) fn from_row(row: &Row<'_>) -> rusqlite::Result<Self> {
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
        // 待办徽标列只在 list()/list_via_fts() 的联查里有;缺列(get 单条等)记 None。
        // ⚠️ 这里按**列名**取值,缺列会返回 InvalidColumnName —— 用 ok() 吞成 None,
        //    这正是"本查询没联查 todo_meta"的合法情形。
        let is_todo = kind == EntryKind::Todo.as_str();
        let due_at: Option<i64> = row.get("todo_due_at").ok().flatten();
        let done_at: Option<i64> = row.get("todo_done_at").ok().flatten();
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
            // todo 条目即使 meta 缺行(脏数据)也按"有徽标但无时刻"处理,
            // 否则卡片会退化成普通文本卡(复选框消失),比丢时刻更难排查。
            todo: is_todo.then_some(TodoBadge {
                due_at,
                done: done_at.is_some(),
                // overdue 需要 now,from_row 拿不到 —— 由 list() 在返回前统一补算
                // (见 stamp_todo_overdue),避免为一行数据把 now 透传进 SQL。
                overdue: false,
            }),
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
    /// 待办未完成数(todo_meta.done_at IS NULL);导航"待办"页与角标显示用。
    /// all 含已完成待办(它们仍是列表里的条目),故 todo_open ≤ all - text - image - file。
    pub todo_open: i64,
    /// 已完成的待办(归档,仍在列表里)。
    pub todo_done: i64,
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

/// 给联查出的待办徽标补算逾期(from_row 没有 now,构造时只能置 false)。
/// DB 存绝对毫秒,按当前时刻比较即可;done 的条目不算逾期(§3)。
fn stamp_todo_overdue(items: &mut [ClipEntry], now: i64) {
    for e in items.iter_mut() {
        if let Some(badge) = e.todo.as_mut() {
            badge.overdue = !badge.done && badge.due_at.is_some_and(|d| d < now);
        }
    }
}

/// 分页列出条目：固定优先、最近使用在后；`query` 非空时按文本内容或文件路径模糊匹配，
/// `kind` / `pinned` / `tag` / `source` 为过滤条件（Phase 2 F3 / S5.2 / S5.1），可叠加。
/// 返回 `(条目, 匹配总数)`。
///
/// `source` 按条目来源精确匹配（clipboard/capture/translate/ocr/todo）——
/// 与卡片上的来源徽标同口径，"只看截图/只看翻译结果"用它。
pub fn list(
    conn: &Connection,
    query: Option<&str>,
    kind: Option<EntryKind>,
    pinned: Option<bool>,
    tag: Option<&str>,
    source: Option<&str>,
    limit: i64,
    offset: i64,
) -> AppResult<(Vec<ClipEntry>, i64)> {
    let kind_str = kind.map(|k| k.as_str().to_owned());
    let pinned_i64 = pinned.map(i64::from);
    // 标签筛选：tags 是 JSON 数组字符串，用 json_each 展开后精确匹配单个标签
    // （SQLite ≥3.38 的 JSON 函数是内置的，bundled 版无需额外编译开关）。
    let tag_str = tag.map(str::trim).filter(|t| !t.is_empty());
    // 来源筛选：空串视为不过滤（与 tag 同一口径，宿主传 "" 表示"全部来源"）
    let source_str = source.map(str::trim).filter(|s| !s.is_empty());

    // 搜索分派（Phase 9 F27）：有关键词时先走 FTS5 全文索引（走 `idx_entries_*`
    // 之外的倒排索引，避免 LIKE 前导 % 的全表扫描）；FTS 无命中时回退 LIKE。
    // 回退的动机（方案 A，docs/phase9.md）：FTS5 的 unicode61 分词器按空格/标点切分，
    // 中文整句会成为单一 token——"打开菜单"这类无空格中文短语 MATCH 不到，
    // 兜底保证中文搜索行为不劣于迁移前。
    if let Some(term) = query.map(str::trim).filter(|q| !q.is_empty()) {
        if let Some(match_expr) = fts_expression(term) {
            let (mut items, total) = list_via_fts(
                conn,
                &match_expr,
                kind_str.as_deref(),
                pinned_i64,
                tag_str,
                source_str,
                limit,
                offset,
            )?;
            stamp_todo_overdue(&mut items, crate::now_ms());
            if !items.is_empty() || total > 0 {
                return Ok((items, total));
            }
        }
    }

    // 待办扩展联查(docs/05-todo.md §2.3):主列表混排的待办卡片正面就要复选框/
    // 截止/逾期,按条目二次拉取会让列表放大成 N 次 FFI 调用 —— LEFT JOIN 一次带出,
    // 非 todo 条目的 todo 键为 NULL,序列化时整键省略(与 tags 同一兼容手法)。
    let pattern = query.map(like_pattern);
    let mut items = conn
        .prepare(
            "SELECT e.id, e.kind, e.text_content, e.blob_path, e.file_paths, e.width, e.height,
                    e.hash, e.byte_size, e.preview, e.pinned, e.created_at, e.last_used_at,
                    e.source, e.tags,
                    m.due_at AS todo_due_at, m.done_at AS todo_done_at
             FROM clip_entries e
             LEFT JOIN todo_meta m ON m.id = e.id
             WHERE (?1 IS NULL OR e.text_content LIKE ?1 ESCAPE '\\'
                    OR (e.kind = 'file' AND e.file_paths LIKE ?1 ESCAPE '\\')
                    OR (e.kind = 'image' AND e.ocr_text LIKE ?1 ESCAPE '\\'))
               AND (?2 IS NULL OR e.kind = ?2)
               AND (?3 IS NULL OR e.pinned = ?3)
               AND (?4 IS NULL OR EXISTS (SELECT 1 FROM json_each(e.tags)
                                          WHERE json_each.value = ?4))
               AND (?5 IS NULL OR e.source = ?5)
             ORDER BY e.pinned DESC, e.last_used_at DESC
             LIMIT ?6 OFFSET ?7",
        )?
        .query_map(
            params![pattern, kind_str, pinned_i64, tag_str, source_str, limit, offset],
            ClipEntry::from_row,
        )?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    stamp_todo_overdue(&mut items, crate::now_ms());
    let total: i64 = conn.query_row(
        "SELECT COUNT(*) FROM clip_entries e
         WHERE (?1 IS NULL OR e.text_content LIKE ?1 ESCAPE '\\'
                OR (e.kind = 'file' AND e.file_paths LIKE ?1 ESCAPE '\\')
                OR (e.kind = 'image' AND e.ocr_text LIKE ?1 ESCAPE '\\'))
           AND (?2 IS NULL OR e.kind = ?2)
           AND (?3 IS NULL OR e.pinned = ?3)
           AND (?4 IS NULL OR EXISTS (SELECT 1 FROM json_each(e.tags)
                                      WHERE json_each.value = ?4))
           AND (?5 IS NULL OR e.source = ?5)",
        params![pattern, kind_str, pinned_i64, tag_str, source_str],
        |row| row.get(0),
    )?;
    Ok((items, total))
}

/// 经 FTS5 虚拟表检索：MATCH 命中的行按 entry_id（UNINDEXED）JOIN 回主表取完整字段。
///
/// `kind`/`pinned`/`tag` 过滤在 JOIN 之后做；`total` 用同一 WHERE 条件计数，
/// 保证前端分页的页数与实际一致。
fn list_via_fts(
    conn: &Connection,
    match_expr: &str,
    kind: Option<&str>,
    pinned: Option<i64>,
    tag: Option<&str>,
    source: Option<&str>,
    limit: i64,
    offset: i64,
) -> AppResult<(Vec<ClipEntry>, i64)> {
    let items = conn
        .prepare(
            "SELECT e.id, e.kind, e.text_content, e.blob_path, e.file_paths, e.width, e.height,
                    e.hash, e.byte_size, e.preview, e.pinned, e.created_at, e.last_used_at,
                    e.source, e.tags,
                    m.due_at AS todo_due_at, m.done_at AS todo_done_at
             FROM clip_fts f
             JOIN clip_entries e ON e.id = f.entry_id
             LEFT JOIN todo_meta m ON m.id = e.id
             WHERE clip_fts MATCH ?1
               AND (?2 IS NULL OR e.kind = ?2)
               AND (?3 IS NULL OR e.pinned = ?3)
               AND (?4 IS NULL OR EXISTS (SELECT 1 FROM json_each(e.tags)
                                          WHERE json_each.value = ?4))
               AND (?5 IS NULL OR e.source = ?5)
             ORDER BY e.pinned DESC, e.last_used_at DESC
             LIMIT ?6 OFFSET ?7",
        )?
        .query_map(
            params![match_expr, kind, pinned, tag, source, limit, offset],
            ClipEntry::from_row,
        )?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let total: i64 = conn.query_row(
        "SELECT COUNT(*)
         FROM clip_fts f
         JOIN clip_entries e ON e.id = f.entry_id
         WHERE clip_fts MATCH ?1
           AND (?2 IS NULL OR e.kind = ?2)
           AND (?3 IS NULL OR e.pinned = ?3)
           AND (?4 IS NULL OR EXISTS (SELECT 1 FROM json_each(e.tags)
                                      WHERE json_each.value = ?4))
           AND (?5 IS NULL OR e.source = ?5)",
        params![match_expr, kind, pinned, tag, source],
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
                COALESCE(SUM(CASE WHEN pinned = 1 THEN 1 ELSE 0 END), 0),
                COALESCE(SUM(CASE WHEN kind = 'todo' AND done_at IS NULL THEN 1 ELSE 0 END), 0),
                COALESCE(SUM(CASE WHEN kind = 'todo' AND done_at IS NOT NULL THEN 1 ELSE 0 END), 0)
         FROM clip_entries LEFT JOIN todo_meta ON todo_meta.id = clip_entries.id",
        [],
        |row| {
            Ok(EntryCounts {
                all: row.get(0)?,
                text: row.get(1)?,
                image: row.get(2)?,
                file: row.get(3)?,
                pinned: row.get(4)?,
                todo_open: row.get(5)?,
                todo_done: row.get(6)?,
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

/// 清空**全部**条目(剪贴板历史 + 待办)并删净待办扩展行，返回图片文件路径供调用方删除。
///
/// ⚠️ 语义(2026-10-15 调整，docs/05-todo.md §1.3):「清空全部数据」是设置页危险区的
/// **全量重置** —— 待办(含归档)随剪贴板历史一并删除；原先"不动待办"的排除已废除
/// (危险区提示计数 `counts.all` 本来就含待办，"删除全部 N 条"的承诺必须真删光)。
/// 只想清归档走 `todo::clear_done`。待办无 blob 文件；todo_meta 无外键(v6.sql 注释),
/// 须显式 DELETE 且与条目删除**同事务** —— 半途失败不得留"meta 孤儿"错位状态。
pub fn clear(conn: &Connection) -> AppResult<Vec<String>> {
    let tx = conn.unchecked_transaction()?;
    let paths: Vec<String> = tx
        .prepare("SELECT blob_path FROM clip_entries WHERE blob_path IS NOT NULL")?
        .query_map([], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    tx.execute("DELETE FROM clip_entries", [])?;
    tx.execute("DELETE FROM todo_meta", [])?;
    tx.commit()?;
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

// ---------- 多选批量操作(条目多选,S6 前置能力) ----------

/// 批量删除结果:删除成功的 id 与应清理的 blob 路径。
///
/// blob 路径返回给调用方(FFI 层)统一删文件,与单条 `delete` 同一职责切分:
/// 事务管数据库行,文件系统清理放事务外(文件删除不可回滚,不该进事务)。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BulkDeleteResult {
    pub deleted: Vec<String>,
    pub blobs: Vec<String>,
}

/// 批量删除:**单事务**完成,失效 id 静默跳过。
///
/// 语义(与单条 delete 的差异及理由):
/// - 不对"id 不存在"报错 —— 多选期间条目可能已被淘汰/在另一端删除,
///   部分失效是常态,整体失败反而让用户没法继续;返回值里带
///   `deleted` 供宿主核对"删了几条"。
/// - 必须单事务:中途失败(如磁盘满)不能留下"删了一半"的状态 ——
///   FTS 触发器会逐行同步删除索引,半途而废的删除会让搜索结果与
///   列表不一致(历史上 v6 迁移踩过"资产不一致"的坑)。
/// - FTS 触发器随行删除自动同步,无需手动清理。
pub fn delete_bulk(conn: &Connection, ids: &[String]) -> AppResult<BulkDeleteResult> {
    if ids.is_empty() {
        return Ok(BulkDeleteResult {
            deleted: Vec::new(),
            blobs: Vec::new(),
        });
    }
    let tx = conn.unchecked_transaction()?;
    let mut deleted = Vec::with_capacity(ids.len());
    let mut blobs = Vec::new();
    {
        let mut stmt = tx.prepare(
            "DELETE FROM clip_entries WHERE id = ?1 \
             RETURNING blob_path",
        )?;
        for id in ids {
            // RETURNING 一次拿到 blob_path 并删行:不存在时返回 0 行 = 静默跳过
            let rows = stmt
                .query_map(params![id], |row| row.get::<_, Option<String>>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            if rows.is_empty() {
                continue;
            }
            // ⚠️ todo_meta 级联**必须在本事务内**:放 FFI 层先删的话,
            //    行删除一旦失败会留下"meta 没了、待办还在"的错位状态
            //    (待办列表/计数全部读 meta)。DELETE FROM clip_entries 的
            //    触发器只管 FTS,不管 meta,所以这里显式删。
            tx.execute("DELETE FROM todo_meta WHERE id = ?1", params![id])?;
            deleted.push(id.clone());
            if let Some(Some(path)) = rows.into_iter().next() {
                blobs.push(path);
            }
        }
    }
    tx.commit()?;
    Ok(BulkDeleteResult { deleted, blobs })
}

/// 批量设置固定状态:单语句 IN 更新,返回实际改动的条数。
///
/// 不存在的 id 自然不计入 changed(与 delete_bulk 的"静默跳过"同一取舍)。
/// ⚠️ IN 占位符按 ids 长度动态拼 —— SQLite 无数组绑定,拼参数个数是标准做法;
///    ids 来自宿主多选(≤ 已加载行数,几十条量级),无长度风险,仍加空集短路。
/// 固定是**幂等**操作,部分失败不回滚(与 delete_bulk 的整体事务取舍不同:
/// 删除不可逆需要原子性,固定可重试不需要)。
pub fn set_pinned_bulk(conn: &Connection, ids: &[String], pinned: bool) -> AppResult<i64> {
    if ids.is_empty() {
        return Ok(0);
    }
    // ⚠️ placeholders 必须先绑定到变量:写进 format! 的临时值活不过整条
    //    语句,SQL 字符串里留下的引用会悬垂(E0716,实测编译错)。
    let placeholders = vec!["?"; ids.len()].join(",");
    let sql = format!(
        "UPDATE clip_entries SET pinned = ?1 WHERE id IN ({placeholders})"
    );
    // ⚠️ 临时值必须先落变量再取引用:`&i64::from(pinned)` 的临时 i64 活不过
    //    当前语句,塞进 Vec<&dyn ToSql> 后调用时已悬垂(E0716,实测编译错)。
    let pinned_i64 = i64::from(pinned);
    let mut params_vec: Vec<&dyn rusqlite::ToSql> = vec![&pinned_i64];
    for id in ids {
        params_vec.push(id);
    }
    let changed = conn.execute(&sql, params_vec.as_slice())?;
    Ok(changed as i64)
}

/// 标签及其条目数（S5.2 标签筛选条用），按条目数降序、同名升序。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TagCount {
    pub tag: String,
    pub count: i64,
}

/// 来源及其条目数（S5.1 来源筛选条用）。
///
/// 与 TagCount 同一形态：**由数据决定有哪些来源**（而不是硬编码 5 种），
/// 计数为 0 的来源自动不出现，侧栏不会给用户展示无法命中的筛选项。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SourceCount {
    pub source: String,
    pub count: i64,
}

/// 各来源的条目数，按条目数降序、同名升序（与 list_tags 同一排序口径）。
pub fn list_sources(conn: &Connection) -> AppResult<Vec<SourceCount>> {
    let mut stmt = conn.prepare(
        "SELECT source, COUNT(*) AS n
         FROM clip_entries
         GROUP BY source
         ORDER BY n DESC, source ASC",
    )?;
    let rows = stmt
        .query_map([], |row| {
            Ok(SourceCount {
                source: row.get(0)?,
                count: row.get(1)?,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(rows)
}

/// 覆盖设置条目的标签；id 不存在时报错。
///
/// 归一化：去首尾空白、丢弃空串、按出现顺序去重；全空则写 NULL。
/// 存 JSON 数组字符串（v5 预留的 `tags` 列），筛选侧用 `json_each` 精确匹配。
pub fn set_tags(conn: &Connection, id: &str, tags: &[String]) -> AppResult<()> {
    let mut normalized: Vec<String> = Vec::with_capacity(tags.len());
    for raw in tags {
        let tag = raw.trim();
        if tag.is_empty() || normalized.iter().any(|t| t == tag) {
            continue;
        }
        normalized.push(tag.to_owned());
    }
    let stored: Option<String> = if normalized.is_empty() {
        None
    } else {
        Some(serde_json::to_string(&normalized)?)
    };
    let changed = conn.execute(
        "UPDATE clip_entries SET tags = ?1 WHERE id = ?2",
        params![stored, id],
    )?;
    if changed == 0 {
        return Err(AppError::InvalidInput(format!("条目不存在: {id}")));
    }
    Ok(())
}

/// 全部标签及其条目数。
///
/// `json_each(NULL)` 会产出一行全 NULL（而不是零行），故必须显式过滤 `je.value IS NOT NULL`，
/// 否则"没有标签的条目"会在统计里变成一个空标签。
pub fn list_tags(conn: &Connection) -> AppResult<Vec<TagCount>> {
    let mut stmt = conn.prepare(
        "SELECT je.value AS tag, COUNT(*) AS n
         FROM clip_entries e, json_each(e.tags) je
         WHERE je.value IS NOT NULL
         GROUP BY je.value
         ORDER BY n DESC, tag ASC",
    )?;
    let rows = stmt
        .query_map([], |row| {
            Ok(TagCount {
                tag: row.get(0)?,
                count: row.get(1)?,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(rows)
}

/// 重命名一个标签:**所有**带该标签的条目的 tags 数组里就地替换。
///
/// 为什么在 core 做而不是宿主逐条改写:标签散在各条目的 JSON 数组里,
/// 宿主侧要"列出→逐条 set_tags"会产生 N 次 FFI 调用与 N 次事务,
/// 中途失败还会留下"改了一半"的不一致状态。这里单事务完成。
///
/// 返回被改动的条目数;`to` 已存在于某条目的标签里时**去重**(不产生重复标签)。
/// ⚠️ `from`/`to` 都去首尾空白;`to` 为空视为"删除该标签"(等价 clear)。
pub fn rename_tag(conn: &Connection, from: &str, to: &str) -> AppResult<i64> {
    let from = from.trim();
    if from.is_empty() {
        return Err(AppError::InvalidInput("原标签名不能为空".into()));
    }
    let to = to.trim();
    // 先取出所有命中的条目(需要在 Rust 侧改 JSON 数组:顺序保持、去重)
    let rows: Vec<(String, Option<String>)> = conn
        .prepare(
            "SELECT e.id, e.tags FROM clip_entries e, json_each(e.tags) je
             WHERE je.value = ?1",
        )?
        .query_map(params![from], |row| Ok((row.get(0)?, row.get(1)?)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    if rows.is_empty() {
        return Ok(0);
    }
    let tx = conn.unchecked_transaction()?;
    let mut changed = 0i64;
    for (id, tags_json) in rows {
        let mut list: Vec<String> = tags_json
            .as_deref()
            .and_then(|json| serde_json::from_str(json).ok())
            .unwrap_or_default();
        // 就地替换(保持原位置),随后去重
        let mut out: Vec<String> = Vec::with_capacity(list.len());
        for tag in list.drain(..) {
            let replacement = if tag == from { to } else { &tag };
            if replacement.is_empty() {
                continue;   // to 为空 = 删除该标签
            }
            if !out.iter().any(|t| t == replacement) {
                out.push(replacement.to_owned());
            }
        }
        let stored: Option<String> = if out.is_empty() {
            None
        } else {
            Some(serde_json::to_string(&out)?)
        };
        tx.execute(
            "UPDATE clip_entries SET tags = ?1 WHERE id = ?2",
            params![stored, id],
        )?;
        changed += 1;
    }
    tx.commit()?;
    Ok(changed)
}

/// 批量给多条条目**追加**标签(S5.2 批量打标)。
///
/// 语义是"加"而非"覆盖":已有该标签的条目原样保留,不重复添加;
/// 条目原有标签一个不动。返回实际改动的条目数(id 不存在的静默跳过 ——
/// 多选场景下条目可能已被删除,不该整体失败)。
pub fn add_tags_bulk(conn: &Connection, ids: &[String], tags: &[String]) -> AppResult<i64> {
    let mut incoming: Vec<String> = Vec::new();
    for raw in tags {
        let t = raw.trim();
        if !t.is_empty() && !incoming.iter().any(|x| x == t) {
            incoming.push(t.to_owned());
        }
    }
    if incoming.is_empty() || ids.is_empty() {
        return Ok(0);
    }
    let tx = conn.unchecked_transaction()?;
    let mut changed = 0i64;
    for id in ids {
        let existing: Option<Option<String>> = tx
            .query_row(
                "SELECT tags FROM clip_entries WHERE id = ?1",
                params![id],
                |row| row.get(0),
            )
            .optional()?;
        let Some(existing) = existing else {
            continue;   // id 不存在:静默跳过(多选期间被删)
        };
        let mut list: Vec<String> = existing
            .as_deref()
            .and_then(|json| serde_json::from_str(json).ok())
            .unwrap_or_default();
        let mut added = false;
        for tag in &incoming {
            if !list.iter().any(|t| t == tag) {
                list.push(tag.clone());
                added = true;
            }
        }
        if !added {
            continue;   // 已有全部待加标签:不算改动,也不写库(避免无谓的 FTS 同步)
        }
        tx.execute(
            "UPDATE clip_entries SET tags = ?1 WHERE id = ?2",
            params![Some(serde_json::to_string(&list)?), id],
        )?;
        changed += 1;
    }
    tx.commit()?;
    Ok(changed)
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
///
/// ⚠️ 待办(kind='todo')**不参与淘汰、也不计入上限**(docs/05-todo.md §2.4):
/// 否则待办会被剪贴板历史上限静默吞掉,且上限会被用户任务永久占用。
pub fn evict(conn: &Connection, keep: i64) -> AppResult<Vec<String>> {
    let total: i64 = conn.query_row(
        "SELECT COUNT(*) FROM clip_entries WHERE kind != 'todo'",
        [],
        |row| row.get(0),
    )?;
    let excess = total - keep;
    if excess <= 0 {
        return Ok(Vec::new());
    }
    let victims: Vec<(String, Option<String>)> = {
        let mut stmt = conn.prepare(
            "SELECT id, blob_path FROM clip_entries WHERE pinned = 0 AND kind != 'todo'
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

/// 最近使用的可贴图条目（一键贴屏用）。两种口径由 `prefer_image` 选择：
///
/// - `prefer_image = false`（默认）：**按最近使用时间取最新一条**，文本与图片平等参与排序
///   ——谁最近复制/入库就贴谁。这是热键「一键贴图」的默认语义：复制文字后按热键即贴该文字。
/// - `prefer_image = true`：**图片优先**——只要历史里有图片就贴最近的图片，没有图片时才回退
///   最近文本（第一代 clipboard-plus F17 口径，由设置 `pinPreferImage` 选用）。
///
/// 重复复制同一内容会 `touch` 置顶，故取到的就是"刚复制/刚入库的那条"；不受 `pinned`
/// 排序影响。文件条目不可贴图，排除在外。
/// 命中索引 `idx_entries_kind (kind, last_used_at DESC)`。
pub fn latest_pinnable(conn: &Connection, prefer_image: bool) -> AppResult<Option<ClipEntry>> {
    if prefer_image {
        // 图片优先：逐类查询、命中即返回（无图片时回退最近文本）
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
        return Ok(None);
    }
    // 默认：文本/图片平等，按最近使用时间取最新
    let entry = conn
        .query_row(
            "SELECT id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size, preview, pinned, created_at, last_used_at, source, tags
             FROM clip_entries
             WHERE kind IN ('text', 'image')
             ORDER BY last_used_at DESC, created_at DESC
             LIMIT 1",
            [],
            ClipEntry::from_row,
        )
        .optional()?;
    Ok(entry)
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
            todo: None,
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
        let (items, total) = list(&conn, None, None, None, None, None, 10, 0).unwrap();
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
        let (page, total) = list(&conn, None, None, None, None, None, 2, 1).unwrap();
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

        let (hits, _) = list(&conn, Some("50"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].id, "a");

        // "_" 是通配符，未转义时会匹配任意字符；转义后只匹配字面量
        let (hits, _) = list(&conn, Some("%_off"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].id, "a");

        let (hits, _) = list(&conn, Some("_"), None, None, None, None, 10, 0).unwrap();
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
        assert_eq!(list(&conn, None, None, None, None, None, 10, 0).unwrap().1, 0);
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
        let ids: Vec<String> = list(&conn, None, None, None, None, None, 10, 0)
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
            ids(list(&conn, None, Some(EntryKind::Text), None, None, None, 10, 0)
                .unwrap()
                .0),
            ["t2", "t1"]
        );
        assert_eq!(
            ids(list(&conn, None, Some(EntryKind::Image), None, None, None, 10, 0)
                .unwrap()
                .0),
            ["i2", "i1"]
        );
        assert_eq!(
            ids(list(&conn, None, None, Some(true), None, None, 10, 0).unwrap().0),
            ["i2", "t2"]
        );
        assert_eq!(
            ids(list(&conn, None, Some(EntryKind::Text), Some(true), None, None, 10, 0)
                .unwrap()
                .0),
            ["t2"]
        );
        // 分类与搜索叠加
        assert_eq!(
            ids(
                list(&conn, Some("alpha"), Some(EntryKind::Text), None, None, None, 10, 0)
                    .unwrap()
                    .0
            ),
            ["t1"]
        );
        assert_eq!(
            list(&conn, None, Some(EntryKind::Image), None, None, None, 10, 0)
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
        assert_eq!(list(&conn, Some("报告"), None, None, None, None, 10, 0).unwrap().1, 1);
        assert_eq!(list(&conn, Some("docx"), None, None, None, None, 10, 0).unwrap().1, 1);
        let (items, total) = list(&conn, None, Some(EntryKind::File), None, None, None, 10, 0).unwrap();
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
    fn latest_pinnable_default_is_recency_across_text_and_image() {
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
        // 默认口径（prefer_image=false）：最近使用优先——文本 last_used_at 最新 → 贴文本
        assert_eq!(latest_pinnable(&conn, false).unwrap().unwrap().id, "t1");

        // 再次复制旧图 → touch 置顶后应换成它（图片同样按 recency 参与竞争）
        touch(&conn, "i-old", 900).unwrap();
        assert_eq!(latest_pinnable(&conn, false).unwrap().unwrap().id, "i-old");

        // 再复制文本 → 又回到文本（文本/图片平等，谁最新贴谁）
        touch(&conn, "t1", 1000).unwrap();
        let picked = latest_pinnable(&conn, false).unwrap().unwrap();
        assert_eq!(picked.id, "t1");
        assert_eq!(picked.kind, EntryKind::Text);

        // 文件条目不可贴图：即便它 last_used_at 最新也不参与（无 text/image 时返回 None）
        let file_only = crate::store::memory();
        insert(
            &file_only,
            &file_entry("f1", "h-f1", &["C:/a/b.txt"], false, 900),
        )
        .unwrap();
        assert!(latest_pinnable(&file_only, false).unwrap().is_none());

        // 空库返回 None
        let empty = crate::store::memory();
        assert!(latest_pinnable(&empty, false).unwrap().is_none());
    }

    #[test]
    fn latest_pinnable_prefer_image_keeps_first_gen_semantics() {
        let conn = crate::store::memory();
        // 文本 last_used_at 更晚，但开启"图片优先"后仍应贴图片
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
        assert_eq!(latest_pinnable(&conn, true).unwrap().unwrap().id, "i-new");

        // 复制旧图 → 取到它
        touch(&conn, "i-old", 900).unwrap();
        assert_eq!(latest_pinnable(&conn, true).unwrap().unwrap().id, "i-old");

        // 无图片时回退最近文本
        let text_only = crate::store::memory();
        insert(&text_only, &entry("t1", "h-t1", "早", false, 100)).unwrap();
        insert(&text_only, &entry("t2", "h-t2", "晚", false, 900)).unwrap();
        let picked = latest_pinnable(&text_only, true).unwrap().unwrap();
        assert_eq!(picked.id, "t2");
        assert_eq!(picked.kind, EntryKind::Text);
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
        // list() 面向**最新 schema**(v5 起含 source/tags,v6 起联查 todo_meta):
        // 补到最新再验证可搜索性
        conn.execute_batch(crate::store::MIGRATION_V5).unwrap();
        conn.execute_batch(crate::store::MIGRATION_V6).unwrap();
        let (items, _) = list(&conn, Some("legacy"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "迁移后存量数据应可被搜到");
    }

    /// **迁移**：v5 → v6 保留全部旧数据,且新类型可用(docs/05-todo.md §7.1)。
    #[test]
    fn migration_v6_preserves_rows_and_allows_todo() {
        let conn = rusqlite::Connection::open_in_memory().unwrap();
        for m in [
            crate::store::MIGRATION_V1,
            crate::store::MIGRATION_V2,
            crate::store::MIGRATION_V3,
            crate::store::MIGRATION_V4,
            crate::store::MIGRATION_V5,
        ] {
            conn.execute_batch(m).unwrap();
        }
        conn.execute(
            "INSERT INTO clip_entries
                (id, kind, text_content, blob_path, file_paths, width, height, hash,
                 byte_size, preview, pinned, created_at, last_used_at, source, tags, ocr_text)
             VALUES ('t1', 'text', '旧文本', NULL, NULL, NULL, NULL, 'h1',
                     9, '旧文本', 1, 100, 200, 'translate', '[\"旧标签\"]', '识别文字')",
            [],
        )
        .unwrap();

        conn.execute_batch(crate::store::MIGRATION_V6).unwrap();

        // 旧行逐字段保留(重建表最容易漏列)
        let row: (String, String, i64, String, String, Option<String>) = conn
            .query_row(
                "SELECT id, kind, pinned, source, tags, ocr_text FROM clip_entries WHERE id = 't1'",
                [],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?)),
            )
            .unwrap();
        assert_eq!(row.0, "t1");
        assert_eq!(row.1, "text");
        assert_eq!(row.2, 1, "pinned 保留");
        assert_eq!(row.3, "translate", "source 保留");
        assert_eq!(row.4, "[\"旧标签\"]", "tags 保留");
        assert_eq!(row.5.as_deref(), Some("识别文字"), "ocr_text 保留");

        // 新类型可插入;未知类型仍被 CHECK 拒绝
        conn.execute(
            "INSERT INTO clip_entries
                (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at)
             VALUES ('td', 'todo', '待办', 'todo:td', 6, '待办', 0, 1, 1)",
            [],
        )
        .unwrap();
        assert!(
            conn.execute(
                "INSERT INTO clip_entries
                    (id, kind, text_content, hash, byte_size, preview, pinned, created_at, last_used_at)
                 VALUES ('x', 'html', 'x', 'h', 1, 'x', 0, 1, 1)",
                [],
            )
            .is_err(),
            "CHECK 应拒绝未知类型"
        );

        // FTS 触发器在重建后仍工作(插入新行应同步进索引)
        let indexed: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM clip_fts WHERE entry_id = 'td'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(indexed, 1, "重建表后 FTS 触发器必须仍生效");

        // 索引重建齐全
        for idx in ["idx_entries_list", "idx_entries_hash", "idx_entries_kind", "idx_entries_source"] {
            let n: i64 = conn
                .query_row(
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name = ?1",
                    [idx],
                    |r| r.get(0),
                )
                .unwrap();
            assert_eq!(n, 1, "{idx} 应已重建");
        }

        // 完整性自检
        let ok: String = conn
            .query_row("PRAGMA integrity_check", [], |r| r.get(0))
            .unwrap();
        assert_eq!(ok, "ok");
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
        let (items, _) = list(&conn, Some("recognized"), None, None, None, None, 10, 0).unwrap();
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
        let (items, _) = list(&conn, Some("revised"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "更新后的文本应可被搜到");
        let (stale, _) = list(&conn, Some("token"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(stale.len(), 1, "旧词应只出现在未改的 b 条目");

        // DELETE：删除后旧词不应再命中
        conn.execute("DELETE FROM clip_entries WHERE id = 'b'", [])
            .unwrap();
        let (items, total) = list(&conn, Some("bravo"), None, None, None, None, 10, 0).unwrap();
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
        let (items, _) = list(&conn, Some("main menu"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "相邻词应命中");
        // 乱序（两词都出现但不相邻/顺序不同）——FTS 的 AND 语义**命中**，
        // LIKE 的连写语义不命中：这是升级而非回归，测试固定该行为
        let (items, total) = list(&conn, Some("menu open"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(
            (items.len(), total),
            (1, 1),
            "两词都出现即命中（FTS 隐式 AND，强于 LIKE 连写）"
        );
        // 只出现其中一词 → 不命中（AND 语义）
        let (_, total) = list(&conn, Some("menu missingword"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(total, 0, "缺任一词不应命中");
        // 单词命中
        let (items, _) = list(&conn, Some("menu"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1);
    }

    /// **中文**：无空格整句按单一短语匹配（方案 A 的 FTS 通道）；
    /// FTS 通道命中不了的场景由 LIKE 兜底，两者结果一致。
    #[test]
    fn fts_handles_chinese_phrase_with_like_fallback() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "打开主菜单查看设置", false, 1)).unwrap();
        // 子串命中（整句的一部分）
        let (items, _) = list(&conn, Some("主菜单"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(items.len(), 1, "中文子串应命中（FTS 或 LIKE 兜底）");
        // 纯标点输入：fts_expression 返回 None，直接走 LIKE，不应报错
        let (items, _) = list(&conn, Some("！？"), None, None, None, None, 10, 0).unwrap();
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
        let (items, _) = list(&conn, Some("summary"), None, None, None, None, 10, 0).unwrap();
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
            let _ = list(&conn, Some(malicious), None, None, None, None, 10, 0).unwrap();
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
        let (items, total) = list(&conn, Some("会议"), None, None, None, None, 10, 0).unwrap();
        assert_eq!(
            (items.len(), total),
            (1, 1),
            "中文子串搜 OCR 文本应经 LIKE 兜底命中"
        );

        // 环节[7]：整串词（与 OCR 文本里的 token 完全一致）应走 FTS 命中
        let (items, total) = list(&conn, Some("会议纪要"), None, None, None, None, 10, 0).unwrap();
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

    // ---------- S5.2 标签与分组 ----------

    #[test]
    fn tags_normalize_filter_and_count() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "alpha", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "bravo", false, 200)).unwrap();

        // 归一化：去首尾空白、丢空串、按出现顺序去重
        set_tags(
            &conn,
            "a",
            &[
                " 工作 ".to_string(),
                String::new(),
                "工作".to_string(),
                "重要".to_string(),
            ],
        )
        .unwrap();
        set_tags(&conn, "b", &["重要".to_string()]).unwrap();
        assert_eq!(
            get(&conn, "a").unwrap().unwrap().tags.as_deref(),
            Some(r#"["工作","重要"]"#)
        );

        // 标签筛选（LIKE 通道）
        assert_eq!(
            list(&conn, None, None, None, Some("重要"), None, 10, 0).unwrap().1,
            2
        );
        let (items, total) = list(&conn, None, None, None, Some("工作"), None, 10, 0).unwrap();
        assert_eq!(total, 1);
        assert_eq!(items[0].id, "a");

        // 标签筛选与 FTS 通道叠加
        let (items, total) = list(&conn, Some("alpha"), None, None, Some("重要"), None, 10, 0).unwrap();
        assert_eq!(total, 1);
        assert_eq!(items[0].id, "a");

        // 标签统计：按条目数降序
        let counts = list_tags(&conn).unwrap();
        assert_eq!(counts.len(), 2);
        assert_eq!((counts[0].tag.as_str(), counts[0].count), ("重要", 2));

        // 清空 b 的标签 → NULL，只影响它自己的计数
        set_tags(&conn, "b", &[]).unwrap();
        assert!(get(&conn, "b").unwrap().unwrap().tags.is_none());
        // 清空后条目本身仍应完整可列（标签为 NULL 不得影响其他列/文本内容）
        let (items, total) = list(&conn, None, None, None, None, None, 10, 0).unwrap();
        assert_eq!(total, 2);
        let b = items.iter().find(|e| e.id == "b").expect("清空标签后 b 仍应在列表里");
        assert_eq!(b.text_content.as_deref(), Some("bravo"));
        assert!(b.tags.is_none());
        let counts = list_tags(&conn).unwrap();
        assert_eq!(counts.len(), 2);
        let important = counts.iter().find(|c| c.tag == "重要").unwrap();
        assert_eq!(important.count, 1, "b 清空后「重要」应只剩 a 一条");

        // 全部清空 → 统计为空(顺带验证 json_each(NULL) 不会产出空标签行)
        set_tags(&conn, "a", &[]).unwrap();
        assert!(list_tags(&conn).unwrap().is_empty());

        // 不存在的 id 报错
        assert!(set_tags(&conn, "nope", &["x".to_string()]).is_err());
    }

    // ---------- S5.1 来源筛选 ----------

    /// `source` 过滤:与卡片上的来源徽标同口径,"只看截图/翻译结果"用它。
    #[test]
    fn list_filters_by_source() {
        let conn = crate::store::memory();
        // 三条不同来源的文本条目(clipboard 默认 / capture / translate)
        insert(&conn, &entry("c", "h-c", "剪贴板来的", false, 100)).unwrap();
        let mut shot = entry("s", "h-s", "截图识别结果", false, 200);
        shot.source = "capture".into();
        insert(&conn, &shot).unwrap();
        let mut tr = entry("t", "h-t", "翻译保存的原文", false, 300);
        tr.source = "translate".into();
        insert(&conn, &tr).unwrap();

        // 不过滤:三条都在(空串与 None 同义,宿主传 "" 表示"全部来源")
        assert_eq!(list(&conn, None, None, None, None, None, 10, 0).unwrap().1, 3);
        assert_eq!(list(&conn, None, None, None, None, Some(""), 10, 0).unwrap().1, 3);

        // 按来源精确匹配
        let (items, total) = list(&conn, None, None, None, None, Some("capture"), 10, 0).unwrap();
        assert_eq!(total, 1);
        assert_eq!(items[0].id, "s");
        let (items, total) = list(&conn, None, None, None, None, Some("translate"), 10, 0).unwrap();
        assert_eq!(total, 1);
        assert_eq!(items[0].id, "t");

        // 与其它条件叠加:来源 + 关键词(LIKE 通道)
        let (_, total) = list(&conn, Some("识别"), None, None, None, Some("capture"), 10, 0).unwrap();
        assert_eq!(total, 1, "来源与关键词应同时生效");
        let (_, total) = list(&conn, Some("识别"), None, None, None, Some("translate"), 10, 0).unwrap();
        assert_eq!(total, 0, "关键词命中但来源不符时不得返回");

        // FTS 通道也须过滤(中文短语走 LIKE 回退,英文走 FTS)
        let (items, total) = list(&conn, None, Some(EntryKind::Text), None, None, Some("clipboard"), 10, 0)
            .unwrap();
        assert_eq!(total, 1);
        assert_eq!(items[0].id, "c");
    }

    /// 来源统计:由数据决定有哪些来源(不硬编码),按条目数降序。
    #[test]
    fn list_sources_counts_by_source() {        let conn = crate::store::memory();
        assert!(list_sources(&conn).unwrap().is_empty(), "空库无来源");

        insert(&conn, &entry("c1", "h-c1", "剪贴板1", false, 100)).unwrap();
        insert(&conn, &entry("c2", "h-c2", "剪贴板2", false, 200)).unwrap();
        let mut shot = entry("s1", "h-s1", "截图", false, 300);
        shot.source = "capture".into();
        insert(&conn, &shot).unwrap();

        let counts = list_sources(&conn).unwrap();
        assert_eq!(counts.len(), 2, "只有出现过的来源");
        // 逐个断言明细,不依赖 tie-break 之外的顺序假设
        let by = |name: &str| counts.iter().find(|c| c.source == name).unwrap().count;
        assert_eq!(by("clipboard"), 2);
        assert_eq!(by("capture"), 1);
        // 未出现的来源不该有 0 计数的条目(避免筛选条出现无效项)
        assert!(counts.iter().all(|c| c.count > 0));
    }

    // ---------- S5.2 标签管理(重命名 / 批量打标) ----------

    #[test]
    fn rename_tag_updates_all_entries_in_place() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "A", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "B", false, 200)).unwrap();
        insert(&conn, &entry("c", "h-c", "C", false, 300)).unwrap();
        set_tags(&conn, "a", &["工作".into(), "重要".into()]).unwrap();
        set_tags(&conn, "b", &["工作".into()]).unwrap();
        set_tags(&conn, "c", &["生活".into()]).unwrap();

        let n = rename_tag(&conn, "工作", "项目").unwrap();
        assert_eq!(n, 2, "两条带「工作」的都该被改");

        // 就地替换**保持原位置**(a 的「重要」仍在第二位),其它标签不受影响
        assert_eq!(
            get(&conn, "a").unwrap().unwrap().tags.as_deref(),
            Some(r#"["项目","重要"]"#)
        );
        assert_eq!(get(&conn, "b").unwrap().unwrap().tags.as_deref(), Some(r#"["项目"]"#));
        // 未被命中的条目一个字都不动
        assert_eq!(get(&conn, "c").unwrap().unwrap().tags.as_deref(), Some(r#"["生活"]"#));
        // 统计随之刷新:旧名消失、新名出现
        let tags: Vec<String> = list_tags(&conn).unwrap().into_iter().map(|t| t.tag).collect();
        assert!(!tags.contains(&"工作".to_string()));
        assert!(tags.contains(&"项目".to_string()));

        // 归并重名:把「重要」并进「项目」,同一数组里不得出现重复
        let n = rename_tag(&conn, "重要", "项目").unwrap();
        assert_eq!(n, 1);
        assert_eq!(get(&conn, "a").unwrap().unwrap().tags.as_deref(), Some(r#"["项目"]"#));

        // to 为空 = 删除该标签
        let n = rename_tag(&conn, "项目", "").unwrap();
        assert_eq!(n, 2);
        assert!(get(&conn, "a").unwrap().unwrap().tags.is_none(), "删空后应为 NULL");

        // 不存在的标签:0 条改动,不报错(幂等)
        assert_eq!(rename_tag(&conn, "不存在", "x").unwrap(), 0);
        // 空原名报错
        assert!(rename_tag(&conn, "", "x").is_err());
    }

    #[test]
    fn add_tags_bulk_appends_without_duplicating() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "A", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "B", false, 200)).unwrap();
        set_tags(&conn, "a", &["工作".into()]).unwrap();

        let ids = vec!["a".to_string(), "b".to_string(), "ghost".to_string()];
        let tags = vec!["重要".to_string(), "重要".to_string(), "  ".to_string()];
        let n = add_tags_bulk(&conn, &ids, &tags).unwrap();
        assert_eq!(n, 2, "两条真实条目被改;ghost 静默跳过;重复/空白标签被归一");

        // 是"追加"不是"覆盖":原有「工作」还在
        assert_eq!(
            get(&conn, "a").unwrap().unwrap().tags.as_deref(),
            Some(r#"["工作","重要"]"#)
        );
        assert_eq!(get(&conn, "b").unwrap().unwrap().tags.as_deref(), Some(r#"["重要"]"#));

        // 再打一次同样的标签:无新增 → 0 条改动(不写库、不触发 FTS 同步)
        assert_eq!(add_tags_bulk(&conn, &ids, &["重要".to_string()]).unwrap(), 0);
        // 空入参:直接 0,不报错
        assert_eq!(add_tags_bulk(&conn, &[], &["x".to_string()]).unwrap(), 0);
        assert_eq!(add_tags_bulk(&conn, &ids, &["  ".to_string()]).unwrap(), 0);
    }

    // ---------- 多选批量操作 ----------

    #[test]
    fn delete_bulk_removes_rows_and_collects_blobs() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "甲", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "乙", false, 200)).unwrap();
        insert(&conn, &image_entry("img", "h-i", "blobs/p1.png", false, 300)).unwrap();
        // 造一条 todo(带 meta 行):验证 bulk 删除的 **meta 级联** ——
        // 行与 meta 必须一起走,残留孤儿行会让待办计数/列表错乱。
        let mut td = entry("td", "h-td", "待办条目", false, 400);
        td.kind = EntryKind::Todo;
        insert(&conn, &td).unwrap();
        conn.execute(
            "INSERT INTO todo_meta (id, due_at, remind_at, notify_at) VALUES ('td', NULL, NULL, NULL)",
            [],
        )
        .unwrap();

        let ids = vec![
            "a".to_string(),
            "img".to_string(),
            "td".to_string(),
            "ghost".to_string(),   // 不存在:静默跳过
        ];
        let r = delete_bulk(&conn, &ids).unwrap();
        assert_eq!(r.deleted, vec!["a", "img", "td"], "失效 id 不计入 deleted");
        assert_eq!(r.blobs, vec!["blobs/p1.png"], "只有图片条目带 blob");
        // meta 级联:td 的 todo_meta 行必须一起消失
        let meta_left: i64 = conn
            .query_row("SELECT COUNT(*) FROM todo_meta WHERE id = 'td'", [], |r| r.get(0))
            .unwrap();
        assert_eq!(meta_left, 0, "todo_meta 不得残留孤儿行");
        // 库里只剩 b
        let (_, total) = list(&conn, None, None, None, None, None, 10, 0).unwrap();
        assert_eq!(total, 1);
        assert_eq!(get(&conn, "b").unwrap().unwrap().id, "b");
        // 全部失效 id:空结果而非报错(幂等重试安全)
        let r = delete_bulk(&conn, &["ghost".to_string()]).unwrap();
        assert!(r.deleted.is_empty() && r.blobs.is_empty());
        // 空入参短路
        assert!(delete_bulk(&conn, &[]).unwrap().deleted.is_empty());
    }

    #[test]
    fn delete_bulk_is_atomic_on_failure() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "甲", false, 100)).unwrap();
        // 事务性验证:单测里难以注入磁盘错误,退而验证"RETURNING 路径下
        // 删除与 blob 收集同事务"——即删除行数与 blobs 数一致(半途而废
        // 会出现 deleted 多、blobs 少的错位)。
        insert(&conn, &image_entry("i1", "h-1", "blobs/x.png", false, 200)).unwrap();
        insert(&conn, &image_entry("i2", "h-2", "blobs/y.png", false, 300)).unwrap();
        let r = delete_bulk(
            &conn,
            &["i1".to_string(), "i2".to_string(), "a".to_string()],
        )
        .unwrap();
        assert_eq!(r.deleted.len(), 3);
        assert_eq!(r.blobs.len(), 2, "文本无 blob,图片两条都收集到");
        assert_eq!(r.blobs, vec!["blobs/x.png", "blobs/y.png"]);
    }

    #[test]
    fn set_pinned_bulk_updates_only_existing() {
        let conn = crate::store::memory();
        insert(&conn, &entry("a", "h-a", "甲", false, 100)).unwrap();
        insert(&conn, &entry("b", "h-b", "乙", false, 200)).unwrap();
        let ids = vec!["a".to_string(), "ghost".to_string()];
        let n = set_pinned_bulk(&conn, &ids, true).unwrap();
        assert_eq!(n, 1, "ghost 不存在不计入");
        assert!(get(&conn, "a").unwrap().unwrap().pinned);
        assert!(!get(&conn, "b").unwrap().unwrap().pinned, "未选中的不动");
        // 再固定一次(幂等):changed 仍为 1(SQLite UPDATE 命中即计,语义可接受)
        assert_eq!(set_pinned_bulk(&conn, &ids, true).unwrap(), 1);
        // 取消固定
        assert_eq!(set_pinned_bulk(&conn, &["a".to_string()], false).unwrap(), 1);
        assert!(!get(&conn, "a").unwrap().unwrap().pinned);
        // 空入参
        assert_eq!(set_pinned_bulk(&conn, &[], true).unwrap(), 0);
    }
}
