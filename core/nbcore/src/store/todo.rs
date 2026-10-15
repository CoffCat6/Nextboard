//! 待办(Todo)数据访问:todo_meta 扩展表 CRUD、列表排序与到点扫描。
//!
//! 设计依据 docs/05-todo.md:
//! - 待办条目本体存 clip_entries(kind='todo'),本模块只管 todo_meta 扩展字段
//!   与"条目+扩展"的联查 —— 两表一致性由本模块的所有写操作维护(同事务)。
//! - 排序:未完成在前(逾期 > 今日到期 > 未来 > 无期限),归档垫底(§5)。
//! - 到点扫描:remind/notify 两个通道各自"未完成、未触发、已到点"(§4.1)。

use rusqlite::{named_params, params, Connection, OptionalExtension};
use serde::Serialize;

use crate::error::{AppError, AppResult};
use crate::now_ms;
use crate::store::entry::{make_preview, ClipEntry, EntryKind, TodoBadge};
/// 单条待办的完整视图:条目本体字段 + todo_meta 扩展字段。
///
/// ⚠️ 序列化形状是**扁平的**(ClipEntry 字段提升到顶层 + `todo` 徽标键),不是
/// `{entry: {...}}` 嵌套 —— 宿主 `ClipEntryModel::applyPage` 对两个端点
/// (list_entries / todo_list)用同一套扁平解析,嵌套形状会把整页解析成空行。
#[derive(Debug, Clone)]
pub struct TodoItem {
    /// 条目本体(序列化时逐字段提升到顶层,见下方 Serialize impl)。
    pub entry: ClipEntry,
    /// 截止时刻(unix 毫秒);None = 无截止。
    pub due_at: Option<i64>,
    /// 面板内提醒时刻;None = 不提醒。
    pub remind_at: Option<i64>,
    /// 系统通知时刻;None = 不通知。
    pub notify_at: Option<i64>,
    /// 完成时刻;None = 未完成。
    pub done_at: Option<i64>,
    /// 是否已过截止且未完成(列表查询按调用时刻计算)。
    pub overdue: bool,
}

impl Serialize for TodoItem {
    /// 手写序列化:顶层 = ClipEntry 全字段(含 tags)+ 三个时刻 + 完成时刻 + `todo` 徽标。
    /// 时刻在顶层是给编辑预填/推迟用的(宿主 `ClipEntryModel::todoMeta` 直接取),
    /// 徽标给卡片正面(与 list_entries 的 todo 键同形状),消费方各取所需。
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::SerializeMap;
        let entry_json = serde_json::to_value(&self.entry).map_err(serde::ser::Error::custom)?;
        let mut map = serializer.serialize_map(None)?;
        if let serde_json::Value::Object(obj) = entry_json {
            for (k, v) in obj {
                map.serialize_entry(&k, &v)?;
            }
        }
        let badge = TodoBadge {
            due_at: self.due_at,
            done: self.done_at.is_some(),
            overdue: self.overdue,
        };
        map.serialize_entry("todo", &badge)?;
        if let Some(ms) = self.due_at {
            map.serialize_entry("dueAt", &ms)?;
        }
        if let Some(ms) = self.remind_at {
            map.serialize_entry("remindAt", &ms)?;
        }
        if let Some(ms) = self.notify_at {
            map.serialize_entry("notifyAt", &ms)?;
        }
        if let Some(ms) = self.done_at {
            map.serialize_entry("doneAt", &ms)?;
        }
        map.end()
    }
}

/// todo_list 的过滤口径(§5:open/done/all)。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TodoFilter {
    /// 未完成。
    Open,
    /// 已完成(归档)。
    Done,
    /// 全部(含归档,宿主待办页用这个:完成是归档不是删除)。
    All,
}

impl TodoFilter {
    pub fn parse(s: Option<&str>) -> AppResult<Self> {
        match s {
            None | Some("open") => Ok(Self::Open),
            Some("done") => Ok(Self::Done),
            Some("all") => Ok(Self::All),
            Some(other) => Err(AppError::InvalidInput(format!(
                "未知待办过滤: {other}(应为 open/done/all)"
            ))),
        }
    }
}

/// 列表查询的公共列:条目本体 + 扩展表字段。
/// `todo_due_at`/`todo_done_at` 是给 `ClipEntry::from_row` 认的徽标列别名。
const TODO_SELECT: &str = "SELECT e.id, e.kind, e.text_content, e.blob_path, e.file_paths, \
     e.width, e.height, e.hash, e.byte_size, e.preview, e.pinned, e.created_at, \
     e.last_used_at, e.source, e.tags, \
     m.due_at, m.remind_at, m.notify_at, m.done_at, \
     m.due_at AS todo_due_at, m.done_at AS todo_done_at \
     FROM clip_entries e JOIN todo_meta m ON m.id = e.id";

/// 待办排序(§5):未完成在前 —— 逾期 > 今日到期 > 未来到期 > 无期限,
/// 组内按 due_at 升序;归档按 done_at 降序垫底。
const TODO_ORDER: &str = "ORDER BY (m.done_at IS NOT NULL) ASC, \
     CASE \
       WHEN m.done_at IS NULL AND m.due_at IS NULL THEN 3 \
       WHEN m.done_at IS NULL AND m.due_at < :now THEN 0 \
       WHEN m.done_at IS NULL AND m.due_at < :end_of_today THEN 1 \
       ELSE 2 \
     END ASC, \
     m.due_at ASC, \
     m.done_at DESC";

fn row_to_item(row: &rusqlite::Row<'_>, now: i64) -> rusqlite::Result<TodoItem> {
    // 复用 ClipEntry::from_row 的列名解析:它按**列名**取值(不是下标),
    // JOIN 后 e.* 的同名列唯一,m.* 的扩展列不与其冲突,可直接复用。
    let entry = ClipEntry::from_row(row)?;
    let due_at: Option<i64> = row.get("due_at")?;
    let done_at: Option<i64> = row.get("done_at")?;
    Ok(TodoItem {
        entry,
        due_at,
        remind_at: row.get("remind_at")?,
        notify_at: row.get("notify_at")?,
        done_at,
        overdue: done_at.is_none() && due_at.is_some_and(|d| d < now),
    })
}

/// 新建待办:同事务写 clip_entries(kind='todo')与 todo_meta。
///
/// `hash` 写占位值 `todo:<id>`(hash 列 NOT NULL;待办不走 blake3 去重管线,
/// 占位值与剪贴板哈希空间不相交,见 §2.4)。
pub fn create(
    conn: &mut Connection,
    text: &str,
    due_at: Option<i64>,
    remind_at: Option<i64>,
    notify_at: Option<i64>,
    tags: Option<Vec<String>>,
    now: i64,
) -> AppResult<String> {
    let text = text.trim();
    if text.is_empty() {
        return Err(AppError::InvalidInput("待办正文不能为空".into()));
    }
    let id = uuid::Uuid::now_v7().to_string();
    let tx = conn.transaction()?;
    crate::store::entry::insert(
        &tx,
        &ClipEntry {
            id: id.clone(),
            kind: EntryKind::Todo,
            text_content: Some(text.to_owned()),
            blob_path: None,
            file_paths: None,
            width: None,
            height: None,
            hash: format!("todo:{id}"),
            byte_size: text.len() as i64,
            preview: make_preview(text),
            pinned: false,
            created_at: now,
            last_used_at: now,
            // 待办来源固定 'todo':与 clipboard/capture/translate/ocr 并列的第五种来源
            source: "todo".into(),
            // 空数组写 NULL(与 set_tags / update 同一口径:空标签 = 无标签)
            tags: tags
                .as_ref()
                .filter(|t| !t.is_empty())
                .map(|t| serde_json::to_string(t).expect("tags 序列化不会失败(字符串数组)")),
            // 徽标只在联查查询里构造(见 ClipEntry::from_row);入库前恒为 None
            todo: None,
        },
    )?;
    tx.execute(
        "INSERT INTO todo_meta (id, due_at, remind_at, notify_at, done_at) \
         VALUES (?1, ?2, ?3, ?4, NULL)",
        params![id, due_at, remind_at, notify_at],
    )?;
    tx.commit()?;
    Ok(id)
}

/// 更新待办:正文与各时刻(`None` = 不改,`Some(None)` = 清空)。
///
/// ⚠️ 把 remind_at/notify_at 改到未来时复位触发标志(允许再次提醒);
///    改到过去视为"立即提醒",下一轮调度触发一次(设计 §3)。
///    取消完成(done → false)**不复位**标志,避免取消瞬间轰炸历史提醒。
pub fn update(
    conn: &mut Connection,
    id: &str,
    text: Option<&str>,
    due_at: Option<Option<i64>>,
    remind_at: Option<Option<i64>>,
    notify_at: Option<Option<i64>>,
    tags: Option<Vec<String>>,
) -> AppResult<()> {
    let now = now_ms();
    let tx = conn.transaction()?;
    // 条目必须存在且是 todo(防把普通条目当待办改)。
    let kind: String = tx
        .query_row(
            "SELECT kind FROM clip_entries WHERE id = ?1",
            params![id],
            |row| row.get(0),
        )
        .optional()?
        .ok_or_else(|| AppError::InvalidInput("条目不存在".into()))?;
    if kind != EntryKind::Todo.as_str() {
        return Err(AppError::InvalidInput("条目不是待办".into()));
    }
    if let Some(t) = text {
        let t = t.trim();
        if t.is_empty() {
            return Err(AppError::InvalidInput("待办正文不能为空".into()));
        }
        tx.execute(
            "UPDATE clip_entries SET text_content = ?1, byte_size = ?2, preview = ?3, \
             last_used_at = ?4 WHERE id = ?5",
            params![t, t.len() as i64, make_preview(t), now, id],
        )?;
    } else {
        // 只改元数据也算"用过",自然上浮(与剪贴板 touch 同语义)
        tx.execute(
            "UPDATE clip_entries SET last_used_at = ?1 WHERE id = ?2",
            params![now, id],
        )?;
    }
    if let Some(tags) = &tags {
        // ⚠️ 空数组写 NULL 而不是 "[]":与 entry::set_tags 同一口径(空标签 = 无标签),
        //    也让"清空标签"在库里与"从未设过标签"完全一致(list_tags 的
        //    json_each 统计不会多出一个空标签行)。
        let stored: Option<String> = if tags.is_empty() {
            None
        } else {
            Some(
                serde_json::to_string(tags)
                    .map_err(|_| AppError::Internal("tags 序列化失败".into()))?,
            )
        };
        tx.execute(
            "UPDATE clip_entries SET tags = ?1 WHERE id = ?2",
            params![stored, id],
        )?;
    }
    if let Some(v) = due_at {
        tx.execute(
            "UPDATE todo_meta SET due_at = ?1 WHERE id = ?2",
            params![v, id],
        )?;
    }
    if let Some(v) = remind_at {
        tx.execute(
            "UPDATE todo_meta SET remind_at = ?1, reminded = 0 WHERE id = ?2",
            params![v, id],
        )?;
    }
    if let Some(v) = notify_at {
        tx.execute(
            "UPDATE todo_meta SET notify_at = ?1, notified = 0 WHERE id = ?2",
            params![v, id],
        )?;
    }
    tx.commit()?;
    Ok(())
}

/// 待办列表(§5 排序)。`query` 按正文模糊过滤,`tag` 精确匹配单标签;
/// `due_max` 给定时的口径 = **有截止 且 due_at ≤ due_max** 的未完成/按 filter
/// 条件的条目(托盘「今日待办」用:传"今天 23:59:59.999 本地毫秒",
/// 未完成时"已逾期"是该集合的子集,一条 WHERE 等价"逾期 或 今日到期")。
///
/// ⚠️ 过滤条件必须与 `entry::list` 同语义:宿主切到"待办"页时下发同一组筛选参数,
///    这里若忽略就会与主列表给出不一致的结果集(实测表现为"筛选后条目变了")。
pub fn list(
    conn: &Connection,
    filter: TodoFilter,
    query: Option<&str>,
    tag: Option<&str>,
    due_max: Option<i64>,
    limit: i64,
    offset: i64,
) -> AppResult<(Vec<TodoItem>, i64)> {
    let now = now_ms();
    // "今日到期"的界:今天 23:59:59.999(本地时区;DB 存绝对毫秒,边界按本地日历算)
    let end_of_today = end_of_today_ms(now);
    // ⚠️ filter_clause 恒以 WHERE 开头(All 用 1=1):后面还要叠加 like/tag 两个
    //    AND 条件,空 WHERE 会让它们并进 JOIN 的 ON 子句,语法与语义双错。
    let filter_clause = match filter {
        TodoFilter::Open => "WHERE m.done_at IS NULL",
        TodoFilter::Done => "WHERE m.done_at IS NOT NULL",
        TodoFilter::All => "WHERE 1=1",
    };
    let like = query.map(str::trim).filter(|q| !q.is_empty()).map(|q| {
        let mut p = String::with_capacity(q.len() + 2);
        p.push('%');
        for c in q.chars() {
            if matches!(c, '%' | '_' | '\\') {
                p.push('\\');
            }
            p.push(c);
        }
        p.push('%');
        p
    });
    let tag = tag.map(str::trim).filter(|t| !t.is_empty());
    let sql = format!(
        "{TODO_SELECT} {filter_clause} \
         AND (:like IS NULL OR e.text_content LIKE :like ESCAPE '\\') \
         AND (:tag IS NULL OR EXISTS (SELECT 1 FROM json_each(e.tags) \
                                      WHERE json_each.value = :tag)) \
         AND (:due_max IS NULL OR (m.due_at IS NOT NULL AND m.due_at <= :due_max)) \
         {TODO_ORDER} LIMIT :limit OFFSET :offset"
    );
    // ⚠️ 命名参数统一 `:name` 形式:sqlite 没有 `?limit` 这种写法(实测报
    //    "near \"limit\": syntax error")—— `?` 只能匿名按序绑定,与命名参数不可混用。
    let items = conn
        .prepare(&sql)?
        .query_map(
            named_params! {
                ":now": now,
                ":end_of_today": end_of_today,
                ":like": like,
                ":tag": tag,
                ":due_max": due_max,
                ":limit": limit,
                ":offset": offset,
            },
            |row| row_to_item(row, now),
        )?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let count_sql = format!(
        "SELECT COUNT(*) FROM todo_meta m JOIN clip_entries e ON e.id = m.id {filter_clause} \
         AND (:like IS NULL OR e.text_content LIKE :like ESCAPE '\\') \
         AND (:tag IS NULL OR EXISTS (SELECT 1 FROM json_each(e.tags) \
                                      WHERE json_each.value = :tag)) \
         AND (:due_max IS NULL OR (m.due_at IS NOT NULL AND m.due_at <= :due_max))"
    );
    let total: i64 = conn.query_row(
        &count_sql,
        named_params! {":like": like, ":tag": tag, ":due_max": due_max},
        |row| row.get(0),
    )?;
    Ok((items, total))
}

/// 按 id 取单条(含扩展);id 不存在或非 todo 返回 None。
pub fn get(conn: &Connection, id: &str) -> AppResult<Option<TodoItem>> {
    let now = now_ms();
    let sql = format!("{TODO_SELECT} WHERE e.id = ?1");
    let item = conn
        .prepare(&sql)?
        .query_row(params![id], |row| row_to_item(row, now))
        .optional()?;
    Ok(item)
}

/// 完成态切换(设计 §3):done = true 写 done_at = now;false 清空。
/// ⚠️ 取消完成不复位 reminded/notified(避免取消瞬间轰炸历史提醒)。
pub fn set_done(conn: &Connection, id: &str, done: bool) -> AppResult<()> {
    let n = conn.execute(
        "UPDATE todo_meta SET done_at = ?1 WHERE id = ?2",
        params![done.then(|| Some(now_ms())), id],
    )?;
    if n == 0 {
        return Err(AppError::InvalidInput("待办不存在".into()));
    }
    Ok(())
}

/// 批量完成态切换(条目多选用):返回**真实**完成/取消的条数。
///
/// 语义:
/// - 非 todo 条目或已处于目标态的条目不计入 —— 宿主多选可能混选文本与待办,
///   "完成 3 条"必须是真实生效数,不能把混入的文本条目也算进去
///   (靠 JOIN clip_entries 的 kind='todo' 过滤,而不是先查再改);
///   已处于目标态的也不计入:SQLite 的 UPDATE **命中即计**、不做"值未变则
///   跳过",不加状态过滤会把"重复完成"虚报成 changed(单测踩到);
/// - reminded/notified 不复位(与单条 set_done 同一语义:批量完成是把
///   "已提醒过"的任务归档,复位会让调度线程把它们再报一遍);
/// - 无事务包裹:单条 UPDATE 原子,批量语义就是"改到多少算多少",
///   部分失效不回滚(与 delete_bulk 的整体事务取舍不同 —— 固定/完成态
///   是幂等操作,重试无副作用,不需要原子性)。
pub fn set_done_bulk(conn: &Connection, ids: &[String], done: bool) -> AppResult<i64> {
    if ids.is_empty() {
        return Ok(0);
    }
    let placeholders = vec!["?"; ids.len()].join(",");
    // 状态过滤:done=true 只改"未完成"的;false 只改"已完成"的
    // (两侧互补,等价于"目标态 != 当前态")
    let state_filter = if done {
        "todo_meta.done_at IS NULL"
    } else {
        "todo_meta.done_at IS NOT NULL"
    };
    let sql = format!(
        "UPDATE todo_meta SET done_at = ?1 \
         WHERE id IN ({placeholders}) \
           AND {state_filter} \
           AND EXISTS (SELECT 1 FROM clip_entries e \
                       WHERE e.id = todo_meta.id AND e.kind = 'todo')"
    );
    // ⚠️ 临时值先落变量再取引用:`&done.then(...)` 的临时 Option 活不过当前
    //    语句,塞进 Vec<&dyn ToSql> 后调用时已悬垂(E0716,实测编译错)。
    let done_at = done.then(|| Some(now_ms()));
    let mut params_vec: Vec<&dyn rusqlite::ToSql> = vec![&done_at];
    for id in ids {
        params_vec.push(id);
    }
    let changed = conn.execute(&sql, params_vec.as_slice())?;
    Ok(changed as i64)
}

/// 删除待办:条目与扩展表一起删(调用方负责清理 blobs —— 待办无 blob,此处无需)。
pub fn delete(conn: &Connection, id: &str) -> AppResult<()> {
    let n = conn.execute("DELETE FROM todo_meta WHERE id = ?1", params![id])?;
    conn.execute("DELETE FROM clip_entries WHERE id = ?1", params![id])?;
    if n == 0 {
        return Err(AppError::InvalidInput("待办不存在".into()));
    }
    Ok(())
}

/// 只清扩展表(供通用的 delete_entry 接线:不管删什么类型条目,meta 都要跟着走,
/// 否则 todo_meta 会留孤儿行)。条目不存在时静默通过(调用方另行报错)。
pub fn delete_meta(conn: &Connection, id: &str) -> AppResult<()> {
    conn.execute("DELETE FROM todo_meta WHERE id = ?1", params![id])?;
    Ok(())
}

/// 清空**已完成**的待办(归档清理;"清空历史"不含待办,见 entry::clear)。
pub fn clear_done(conn: &Connection) -> AppResult<()> {
    conn.execute(
        "DELETE FROM clip_entries WHERE kind = 'todo' AND id IN \
         (SELECT id FROM todo_meta WHERE done_at IS NOT NULL)",
        [],
    )?;
    conn.execute("DELETE FROM todo_meta WHERE done_at IS NOT NULL", [])?;
    Ok(())
}

/// 到点扫描的命中(调度线程每轮调用,§4.1):
/// remind/notify 各取一批"未完成、未触发、时刻 <= now"的待办。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DueHit {
    pub id: String,
    pub text: String,
    /// 本次命中的通道(remind / notify)。
    pub channel: &'static str,
    /// 设定时刻(气泡文案用)。
    pub at: i64,
}

pub fn due_reminds(conn: &Connection, now: i64, limit: i64) -> AppResult<Vec<DueHit>> {
    due_scan(conn, now, limit, "remind_at", "reminded", "remind")
}

pub fn due_notifies(conn: &Connection, now: i64, limit: i64) -> AppResult<Vec<DueHit>> {
    due_scan(conn, now, limit, "notify_at", "notified", "notify")
}

fn due_scan(
    conn: &Connection,
    now: i64,
    limit: i64,
    at_col: &str,
    flag_col: &str,
    channel: &'static str,
) -> AppResult<Vec<DueHit>> {
    // 列名来自本文件内的两个调用点(常量),非用户输入 —— 无注入面。
    let sql = format!(
        "SELECT m.id, e.text_content, m.{at_col} AS at \
         FROM todo_meta m JOIN clip_entries e ON e.id = m.id \
         WHERE m.done_at IS NULL AND m.{flag_col} = 0 \
           AND m.{at_col} IS NOT NULL AND m.{at_col} <= ?1 \
         ORDER BY m.{at_col} ASC LIMIT ?2"
    );
    let hits = conn
        .prepare(&sql)?
        .query_map(params![now, limit], |row| {
            Ok(DueHit {
                id: row.get(0)?,
                text: row.get::<_, Option<String>>(1)?.unwrap_or_default(),
                channel,
                at: row.get(2)?,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(hits)
}

pub fn mark_reminded(conn: &Connection, id: &str) -> AppResult<()> {
    conn.execute(
        "UPDATE todo_meta SET reminded = 1 WHERE id = ?1",
        params![id],
    )?;
    Ok(())
}

pub fn mark_notified(conn: &Connection, id: &str) -> AppResult<()> {
    conn.execute(
        "UPDATE todo_meta SET notified = 1 WHERE id = ?1",
        params![id],
    )?;
    Ok(())
}

/// 待播报的气泡(宿主拉取,§4.2 宿主确认制去重):已触发 notify 但尚未播报的。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PendingBubble {
    pub id: String,
    pub text: String,
    /// 设定时刻。
    pub at: i64,
    /// 是否是"启动补发"(到点时刻早于本次进程启动):气泡文案加前缀(§3)。
    pub missed: bool,
}

pub fn pending_bubbles(conn: &Connection, limit: i64) -> AppResult<Vec<PendingBubble>> {
    let startup = crate::todo::startup_ms();
    let rows = conn
        .prepare(
            "SELECT m.id, e.text_content, m.notify_at \
             FROM todo_meta m JOIN clip_entries e ON e.id = m.id \
             WHERE m.notified = 1 AND m.bubble_shown = 0 AND m.notify_at IS NOT NULL \
             ORDER BY m.notify_at ASC LIMIT ?1",
        )?
        .query_map(params![limit], |row| {
            let at: i64 = row.get(2)?;
            Ok(PendingBubble {
                id: row.get(0)?,
                text: row.get::<_, Option<String>>(1)?.unwrap_or_default(),
                at,
                missed: at < startup,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(rows)
}

/// 气泡播报确认(幂等):宿主弹完气泡后回写,避免下次重复弹。
pub fn mark_bubble_shown(conn: &Connection, ids: &[String]) -> AppResult<()> {
    for id in ids {
        conn.execute(
            "UPDATE todo_meta SET bubble_shown = 1 WHERE id = ?1",
            params![id],
        )?;
    }
    Ok(())
}

/// 今天 23:59:59.999 的本地毫秒("今日到期"排序界;ffi 的 `due=today` 过滤同源)。
/// 用 chrono 的本地时区 —— DB 存绝对毫秒,日历边界按本地时区解释(设计 §3)。
pub(crate) fn end_of_today_ms(now_ms: i64) -> i64 {
    use chrono::TimeZone;
    let now = chrono::Local
        .timestamp_millis_opt(now_ms)
        .single()
        .unwrap_or_else(chrono::Local::now);
    let tomorrow = (now.date_naive() + chrono::Duration::days(1))
        .and_hms_opt(0, 0, 0)
        .expect("00:00:00 恒合法");
    chrono::Local
        .from_local_datetime(&tomorrow)
        .single()
        .map(|t| t.timestamp_millis())
        .unwrap_or_else(|| now_ms + 86_400_000)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::entry;
    use crate::store::entry::EntryCounts;

    const DAY: i64 = 86_400_000;

    fn open_count(conn: &rusqlite::Connection) -> i64 {
        entry::counts(conn).expect("counts").todo_open
    }

    fn create_at(
        conn: &mut rusqlite::Connection,
        text: &str,
        due: Option<i64>,
        remind: Option<i64>,
        notify: Option<i64>,
        now: i64,
    ) -> String {
        create(conn, text, due, remind, notify, None, now).expect("create")
    }

    fn plain_entry(id: &str, text: &str, now: i64) -> ClipEntry {
        ClipEntry {
            id: id.into(),
            kind: EntryKind::Text,
            text_content: Some(text.into()),
            blob_path: None,
            file_paths: None,
            width: None,
            height: None,
            hash: format!("h-{id}"),
            byte_size: text.len() as i64,
            preview: text.into(),
            pinned: false,
            created_at: now,
            last_used_at: now,
            source: "clipboard".into(),
            tags: None,
            todo: None,
        }
    }

    // ---- 排序(§5:逾期 > 今日 > 未来 > 无期限;归档垫底) ----

    #[test]
    fn list_orders_overdue_today_future_none_done() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let today_end = end_of_today_ms(now);

        let overdue = create_at(&mut conn, "逾期", Some(now - DAY), None, None, now);
        let today = create_at(&mut conn, "今日", Some(today_end - 1000), None, None, now);
        let future = create_at(&mut conn, "未来", Some(now + DAY), None, None, now);
        let none = create_at(&mut conn, "无期限", None, None, None, now);
        let done = create_at(&mut conn, "已完成", None, None, None, now);
        set_done(&conn, &done, true).expect("set done");

        let (items, total) = list(&conn, TodoFilter::Open, None, None, None, 50, 0).expect("open");
        assert_eq!(total, 4);
        let ids: Vec<&str> = items.iter().map(|i| i.entry.id.as_str()).collect();
        assert_eq!(
            ids,
            [overdue.as_str(), today.as_str(), future.as_str(), none.as_str()]
        );

        // 归档视图:只有已完成,按完成时间倒序
        let (done_items, done_total) =
            list(&conn, TodoFilter::Done, None, None, None, 50, 0).expect("done");
        assert_eq!(done_total, 1);
        assert_eq!(done_items[0].entry.id, done);
        assert!(!done_items[0].overdue, "完成条目不再标逾期");

        // 全部:归档垫底,但仍可见(宿主待办页口径)
        let (all, all_total) = list(&conn, TodoFilter::All, None, None, None, 50, 0).expect("all");
        assert_eq!(all_total, 5);
        assert_eq!(all.last().unwrap().entry.id, done);
    }

    #[test]
    fn list_marks_overdue_only_for_unfinished() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let overdue = create_at(&mut conn, "过期的", Some(now - DAY), None, None, now);
        let (items, _) = list(&conn, TodoFilter::All, None, None, None, 50, 0).expect("list");
        assert!(items[0].overdue, "未完成且过截止应标逾期");
        set_done(&conn, &overdue, true).expect("done");
        let (items, _) = list(&conn, TodoFilter::All, None, None, None, 50, 0).expect("list2");
        assert!(!items[0].overdue, "完成后不再逾期");
    }

    // ---- 序列化形状(§2.3:扁平 + todo 徽标;宿主 applyPage 依赖此形状) ----

    #[test]
    fn todo_item_serializes_flat_with_badge() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let id = create_at(&mut conn, "带标签的任务", Some(now - DAY), None, None, now);
        crate::store::entry::set_tags(&conn, &id, &["工作".to_string()]).expect("tags");

        let (items, _) = list(&conn, TodoFilter::All, None, Some("工作"), None, 50, 0).expect("tag");
        assert_eq!(items.len(), 1, "标签筛选应命中待办");
        let json = serde_json::to_value(&items[0]).expect("serialize");
        assert_eq!(json["id"], serde_json::json!(id), "顶层 id(不是 entry.id 嵌套)");
        assert_eq!(json["kind"], serde_json::json!("todo"));
        assert_eq!(json["textContent"], serde_json::json!("带标签的任务"));
        assert_eq!(json["tags"], serde_json::json!("[\"工作\"]"));
        assert_eq!(json["dueAt"].as_i64(), Some(now - DAY));
        assert_eq!(json["todo"]["overdue"], serde_json::json!(true));
        assert!(json.get("entry").is_none(), "不得是嵌套形状");
        assert!(json.get("hash").is_none(), "hash 不下发");
    }

    #[test]
    fn list_entries_carries_todo_badge_for_todo_rows_only() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let _todo = create_at(&mut conn, "待办", Some(now - DAY), None, None, now);
        entry::insert(&conn, &plain_entry("plain", "普通文本", now)).expect("insert");

        let (items, _) = entry::list(&conn, None, None, None, None, None, 50, 0).expect("all");
        let todo_row = items.iter().find(|e| e.kind == EntryKind::Todo).expect("todo row");
        let badge = todo_row.todo.expect("todo 条目应带徽标");
        assert!(badge.overdue, "逾期应在 list_entries 上就算出来");
        assert!(!badge.done);
        let plain_row = items.iter().find(|e| e.id == "plain").expect("plain row");
        assert!(plain_row.todo.is_none(), "非 todo 条目不得带徽标");
    }

    // ---- 过滤(query / tag,与 entry::list 同语义) ----

    #[test]
    fn list_filters_by_query_and_tag() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let milk = create_at(&mut conn, "买牛奶", None, None, None, now);
        create_at(&mut conn, "写周报", None, None, None, now);
        crate::store::entry::set_tags(&conn, &milk, &["生活".to_string()]).expect("tags");

        let (items, total) = list(&conn, TodoFilter::Open, Some("牛奶"), None, None, 50, 0).expect("query");
        assert_eq!(total, 1);
        assert_eq!(items[0].entry.text_content.as_deref(), Some("买牛奶"));

        let (_, total) = list(&conn, TodoFilter::Open, None, Some("生活"), None, 50, 0).expect("tag");
        assert_eq!(total, 1);
        let (_, total) = list(&conn, TodoFilter::Open, None, Some("工作"), None, 50, 0).expect("tag2");
        assert_eq!(total, 0);

        // LIKE 通配符须转义:纯 "_" 不应全量命中
        let (_, total) = list(&conn, TodoFilter::Open, Some("_"), None, None, 50, 0).expect("escape");
        assert_eq!(total, 0, "通配符未转义");
    }

    // ---- due_max 过滤(托盘「今日待办」= 逾期 + 今日到期) ----

    #[test]
    fn list_filters_by_due_max_for_today() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let today_end = end_of_today_ms(now);
        let overdue = create_at(&mut conn, "逾期的", Some(now - DAY), None, None, now);
        let today = create_at(&mut conn, "今天到期", Some(today_end - 1000), None, None, now);
        // "未来的"只用于证明 due_max 过滤会排除它(断言走 total/ids,不引变量)
        create_at(&mut conn, "未来的", Some(now + DAY), None, None, now);
        create_at(&mut conn, "无截止", None, None, None, now);
        let done_today = create_at(&mut conn, "今天但已完成", Some(today_end - 2000), None, None, now);
        set_done(&conn, &done_today, true).expect("done");

        // 口径:未完成 且 有截止 且 due ≤ 今天末尾 —— "已逾期"是该集合的子集
        let (items, total) = list(&conn, TodoFilter::Open, None, None, Some(today_end), 50, 0)
            .expect("today filter");
        assert_eq!(total, 2, "应为 逾期+今天到期,排除 未来/无截止/已完成");
        let ids: Vec<&str> = items.iter().map(|i| i.entry.id.as_str()).collect();
        assert_eq!(ids, [overdue.as_str(), today.as_str()], "沿用 §5 排序:逾期在前");

        // due_max = None 时行为与旧契约完全一致(全部可见)
        let (_, total) = list(&conn, TodoFilter::Open, None, None, None, 50, 0).expect("no filter");
        assert_eq!(total, 4);

        // 精确 COUNT 是托盘"…共 N 条"的前提:limit 1 时 total 仍为 2
        let (_, total) = list(&conn, TodoFilter::Open, None, None, Some(today_end), 1, 0)
            .expect("count with limit");
        assert_eq!(total, 2);
    }

    // ---- 完成态与 counts ----

    #[test]
    fn set_done_roundtrip_and_counts() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        entry::insert(&conn, &plain_entry("plain", "普通条目", now)).expect("insert");
        let a = create_at(&mut conn, "待办A", None, None, None, now);
        create_at(&mut conn, "待办B", None, None, None, now);

        let c: EntryCounts = entry::counts(&conn).expect("counts");
        assert_eq!(c.all, 3);
        assert_eq!(c.todo_open, 2);
        assert_eq!(c.todo_done, 0);

        set_done(&conn, &a, true).expect("done");
        let c: EntryCounts = entry::counts(&conn).expect("counts2");
        assert_eq!(c.todo_open, 1);
        assert_eq!(c.todo_done, 1);

        set_done(&conn, &a, false).expect("undone");
        let c: EntryCounts = entry::counts(&conn).expect("counts3");
        assert_eq!(c.todo_open, 2);
    }

    // ---- 到点扫描与去重(§4.1) ----

    #[test]
    fn due_scan_hits_only_due_and_unfired() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        // ⚠️ "已到期"的三条给**不同时刻**——扫描按 at ASC 排序,同刻顺序不定,
        //    断言若依赖顺序就会偶发红。
        let oldest = create_at(&mut conn, "该提醒了", None, Some(now - 3000), None, now);
        create_at(&mut conn, "还没到", None, Some(now + DAY), None, now);
        let mid = create_at(&mut conn, "已触发", None, Some(now - 2000), None, now);
        let newest = create_at(&mut conn, "完成了", None, Some(now - 1000), None, now);
        mark_reminded(&conn, &mid).expect("mark");
        set_done(&conn, &newest, true).expect("done");

        let hits = due_reminds(&conn, now, 8).expect("scan");
        assert_eq!(hits.len(), 1, "只应命中未触发且未完成的");
        assert_eq!(hits[0].id, oldest);
        assert_eq!(hits[0].channel, "remind");

        // 触发后标记 → 再扫不重发
        mark_reminded(&conn, &oldest).expect("mark2");
        assert!(due_reminds(&conn, now, 8).expect("scan2").is_empty());

        // notify 通道独立
        let n = create_at(&mut conn, "通知", None, None, Some(now - 1000), now);
        let hits = due_notifies(&conn, now, 8).expect("scan3");
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].id, n);
        mark_notified(&conn, &n).expect("mark3");
        assert!(due_notifies(&conn, now, 8).expect("scan4").is_empty());
    }

    #[test]
    fn bubble_pending_and_mark_shown() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let id = create_at(&mut conn, "要弹气泡", None, None, Some(now - 1000), now);
        assert!(pending_bubbles(&conn, 8).expect("p0").is_empty(), "未触发前无待播报");
        mark_notified(&conn, &id).expect("mark");
        let pending = pending_bubbles(&conn, 8).expect("p1");
        assert_eq!(pending.len(), 1);
        assert_eq!(pending[0].id, id);
        mark_bubble_shown(&conn, &[id.clone()]).expect("shown");
        assert!(pending_bubbles(&conn, 8).expect("p2").is_empty(), "播报确认后不再弹");
    }

    // ---- 淘汰排除与全量清空 ----

    #[test]
    fn evict_spares_todo_and_counts_only_clips() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let mut old = plain_entry("old", "最旧", now);
        old.last_used_at = now - 3 * DAY;
        let mut newer = plain_entry("newer", "较新", now);
        newer.last_used_at = now - DAY;
        entry::insert(&conn, &old).expect("old");
        entry::insert(&conn, &newer).expect("newer");
        create_at(&mut conn, "待办1", None, None, None, now);
        create_at(&mut conn, "待办2", None, None, None, now);

        let blobs = entry::evict(&conn, 1).expect("evict");
        assert!(blobs.is_empty());
        assert!(entry::get(&conn, "old").expect("get").is_none(), "最旧普通条目应被淘汰");
        assert!(entry::get(&conn, "newer").expect("get").is_some(), "较新普通条目应保留");
        let c: EntryCounts = entry::counts(&conn).expect("counts");
        assert_eq!(c.todo_open, 2, "待办两条都保留");
        assert_eq!(c.all, 3, "淘汰只删了最旧的普通条目");

        // 上限收到 0:普通条目全没,待办仍保留
        entry::evict(&conn, 0).expect("evict0");
        assert!(entry::get(&conn, "newer").expect("get").is_none());
        assert_eq!(open_count(&conn), 2, "上限 0 也不得动待办");
    }

    #[test]
    fn clear_history_removes_everything() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        entry::insert(&conn, &plain_entry("clip", "剪贴板", now)).expect("insert");
        let open = create_at(&mut conn, "未完成待办", None, None, None, now);
        let done = create_at(&mut conn, "已完成待办", None, None, None, now);
        set_done(&conn, &done, true).expect("done");

        // 全量清空:待办(含归档)随剪贴板历史一并删除,meta 无孤儿 —— 2026-10-15
        // 语义调整,原"清空不动待办"的排除废除(危险区计数本来就把待办算进 N)。
        entry::clear(&conn).expect("clear");
        assert!(entry::get(&conn, "clip").expect("get").is_none());
        assert!(entry::get(&conn, &open).expect("get").is_none(), "未完成待办应被清空");
        assert!(entry::get(&conn, &done).expect("get").is_none(), "已完成待办应被清空");
        let left: i64 = conn
            .query_row("SELECT COUNT(*) FROM todo_meta", [], |r| r.get(0))
            .expect("meta count");
        assert_eq!(left, 0, "todo_meta 不得留孤儿行");
        let c: EntryCounts = entry::counts(&conn).expect("counts");
        assert_eq!(c.all, 0, "清空后计数应全口径归零");

        // clear_done 语义不变:只删已完成,未完成不受影响
        let keep = create_at(&mut conn, "保留的未完成", None, None, None, now);
        let gone = create_at(&mut conn, "要清的已完成", None, None, None, now);
        set_done(&conn, &gone, true).expect("done2");
        clear_done(&conn).expect("clear done");
        let (items, _) = list(&conn, TodoFilter::All, None, None, None, 50, 0).expect("list");
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].entry.id, keep);
    }

    #[test]
    fn delete_entry_removes_meta_row() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let id = create_at(&mut conn, "要删的", None, None, None, now);
        delete_meta(&conn, &id).expect("meta");
        entry::delete(&conn, &id).expect("entry");
        let left: i64 = conn
            .query_row("SELECT COUNT(*) FROM todo_meta", [], |r| r.get(0))
            .expect("count");
        assert_eq!(left, 0, "不应留孤儿 meta 行");
    }

    // ---- 校验与边界 ----

    #[test]
    fn create_rejects_blank_text() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        assert!(create(&mut conn, "   ", None, None, None, None, now).is_err());
        assert!(create(&mut conn, "ok", None, None, None, None, now).is_ok());
    }

    #[test]
    fn update_rewrites_remind_flag_but_undone_keeps_flags() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let id = create_at(&mut conn, "任务", None, Some(now - 1000), None, now);
        mark_reminded(&conn, &id).expect("mark");

        // 改到未来 → 标志复位,可再次提醒
        update(&mut conn, &id, None, None, Some(Some(now + DAY)), None, None).expect("future");
        assert_eq!(due_reminds(&conn, now, 8).expect("s1").len(), 0);

        // 改到过去 → 视为立即提醒(下一轮命中)
        update(&mut conn, &id, None, None, Some(Some(now - 1000)), None, None).expect("past");
        assert_eq!(due_reminds(&conn, now, 8).expect("s2").len(), 1);
        // 模拟调度线程本轮已消费:扫描是纯查询,触发标记由调度侧显式写(§4.1)
        mark_reminded(&conn, &id).expect("mark after scan");

        // 完成后再取消:标志不复位
        set_done(&conn, &id, true).expect("done");
        assert_eq!(due_reminds(&conn, now, 8).expect("s3").len(), 0, "完成不触发");
        set_done(&conn, &id, false).expect("undone");
        assert_eq!(
            due_reminds(&conn, now, 8).expect("s4").len(),
            0,
            "取消完成不补发(标志位保留)"
        );
    }

    #[test]
    fn update_clears_times_and_rejects_non_todo() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let id = create_at(&mut conn, "任务", Some(now + DAY), Some(now + 3600_000), None, now);
        update(&mut conn, &id, None, Some(None), Some(None), None, None).expect("clear");
        let item = get(&conn, &id).expect("get").expect("exists");
        assert!(item.due_at.is_none() && item.remind_at.is_none(), "清空时刻未生效");

        entry::insert(&conn, &plain_entry("clip", "普通", now)).expect("insert");
        assert!(update(&mut conn, "clip", Some("改"), None, None, None, None).is_err());
        assert!(set_done(&conn, "clip", true).is_err());
    }

    // ---- FTS 搜索命中待办正文(§2.5) ----

    #[test]
    fn fts_and_like_find_todo_text() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        create_at(&mut conn, "记住要买牛奶", None, None, None, now);
        // FTS 路径(中文短语)
        let (items, _) = entry::list(&conn, Some("买牛奶"), Some(EntryKind::Todo), None, None, None, 50, 0)
            .expect("fts");
        assert_eq!(items.len(), 1);
        // LIKE 回退路径(拆词后仍应命中)
        let (items, _) =
            entry::list(&conn, Some("牛奶"), None, None, None, None, 50, 0).expect("like");
        assert_eq!(items.len(), 1);
    }

    #[test]
    fn get_returns_extended_fields() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let id = create_at(&mut conn, "任务", Some(now + DAY), Some(now + 3600_000),
                           Some(now + 7200_000), now);
        let item = get(&conn, &id).expect("get").expect("exists");
        assert_eq!(item.due_at, Some(now + DAY));
        assert_eq!(item.remind_at, Some(now + 3600_000));
        assert_eq!(item.notify_at, Some(now + 7200_000));
        assert!(item.done_at.is_none());
        assert_eq!(item.entry.text_content.as_deref(), Some("任务"));
        assert!(get(&conn, "nope").expect("get2").is_none());
    }

    // ---------- 批量完成态(条目多选用) ----------

    /// **混选语义**是本函数的核心回归:多选可能混选文本与待办,
    /// 返回值必须是真实生效数(只数 todo),不能把混入的文本算进去。
    #[test]
    fn set_done_bulk_counts_only_todo_entries() {
        let mut conn = crate::store::memory();
        let now = now_ms();
        let t1 = create_at(&mut conn, "待办一", Some(now - DAY), None, None, now);
        let t2 = create_at(&mut conn, "待办二", None, None, None, now);
        // 普通文本条目:多选时会被混进来,但完成态只对 todo 生效
        entry::insert(&conn, &plain_entry("plain", "普通文本", now)).expect("plain");

        let ids = vec![t1.clone(), t2.clone(), "plain".to_string(), "ghost".to_string()];
        let n = set_done_bulk(&conn, &ids, true).unwrap();
        assert_eq!(n, 2, "混选时只数真实完成的 todo,plain/ghost 不计");

        let done = |id: &str| {
            conn.query_row(
                "SELECT done_at IS NOT NULL FROM todo_meta WHERE id = ?1",
                params![id],
                |r| r.get::<_, bool>(0),
            )
            .unwrap()
        };
        assert!(done(&t1) && done(&t2));

        // 批量取消完成:同样只数 todo
        assert_eq!(set_done_bulk(&conn, &ids, false).unwrap(), 2);
        assert!(!done(&t1));

        // 幂等:已完成再完成 → 0(宿主 toast 不虚报)
        set_done_bulk(&conn, &[t1.clone()], true).unwrap();
        assert_eq!(set_done_bulk(&conn, &[t1.clone()], true).unwrap(), 0);

        // 空入参
        assert_eq!(set_done_bulk(&conn, &[], true).unwrap(), 0);
    }
}
