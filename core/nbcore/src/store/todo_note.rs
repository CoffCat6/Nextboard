//! 待办贴屏(桌面便签)的窗口状态:todo_note 扩展表 CRUD(docs/20-todo-sticky-note.md §3)。
//!
//! 职责边界与 `store::todo` 同构:条目本体仍存 clip_entries(kind='todo'),
//! 待办时刻在 todo_meta,本模块只管**窗口状态**(几何/吸附边/半隐藏/透明度/置顶)。
//!
//! ⚠️ 三表一致性由应用层维护(**无外键**,与 v1~v7 一致):条目被删/清空/批量删除/
//!    导入替换时必须显式删本表,否则留永久孤儿行。

use rusqlite::{params, Connection, OptionalExtension};
use serde::Serialize;

use crate::error::{AppError, AppResult};
use crate::now_ms;
use crate::store::entry::ClipEntry;
use crate::store::todo::TodoItem;

// ---- 数值边界(宿主写入前在这里**夹紧**,不报错)----
//
// 为什么夹紧而不是拒绝:拖窗口时宿主每次松手/吸附都会写一次几何,屏幕刚被拔掉的
// 那一瞬间 x 可能是 3200 —— 报错会让便签卡在"位置永远存不进去"的状态。夹紧则总能
// 落到一个可见位置。`edge` 是枚举,非法值仍然报错(枚举不存在"差一点"的合法近似,
// 静默回退会掩盖宿主 bug)。
pub const MIN_W: i64 = 180;
pub const MAX_W: i64 = 1200;
pub const MIN_H: i64 = 120;
pub const MAX_H: i64 = 900;
pub const MIN_OPACITY: i64 = 30;
pub const MAX_OPACITY: i64 = 100;
/// 0 = 跟随主题字号(不是"字号为 0")。
pub const MAX_FONT_SIZE: i64 = 96;

/// 吸附边。`None` = 未吸附(自由摆放)。
///
/// 线上/库里的表示都是小写字符串(as_str/parse),`Serialize` 只为让 NoteState
/// 能直接下发 —— 两者的字符串必须一致,故用 `rename_all = "lowercase"` 而不是
/// 手写 Serialize(手写会多一份容易漂移的映射)。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum NoteEdge {
    None,
    Left,
    Right,
    Top,
    /// 下边(任务栏那一侧)。允许吸附,**不允许 autoHide** —— 见 docs/20 §6.2 第 4 条。
    Bottom,
}

impl NoteEdge {
    pub fn as_str(self) -> &'static str {
        match self {
            NoteEdge::None => "none",
            NoteEdge::Left => "left",
            NoteEdge::Right => "right",
            NoteEdge::Top => "top",
            NoteEdge::Bottom => "bottom",
        }
    }

    /// 解析宿主传来的边;**非法值显式报错**(见模块头注释的取舍)。
    pub fn parse(s: &str) -> AppResult<Self> {
        match s {
            "none" => Ok(NoteEdge::None),
            "left" => Ok(NoteEdge::Left),
            "right" => Ok(NoteEdge::Right),
            "top" => Ok(NoteEdge::Top),
            "bottom" => Ok(NoteEdge::Bottom),
            other => Err(AppError::InvalidInput(format!(
                "未知吸附边: {other}(应为 none/left/right/top/bottom)"
            ))),
        }
    }

    /// 下边吸附不支持自动半隐藏(会与任务栏自动隐藏抢鼠标)。
    pub fn allows_auto_hide(self) -> bool {
        !matches!(self, NoteEdge::Bottom)
    }
}

/// 一行的窗口状态(不含条目本体)。
///
/// 直接 `Serialize`(驼峰键):`todo_note_save` 的返回值就是它,宿主拿来就地更新
/// 自己的状态对象,**不需要**再回查一次列表。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NoteState {
    pub x: i64,
    pub y: i64,
    pub w: i64,
    pub h: i64,
    pub edge: NoteEdge,
    pub auto_hide: bool,
    pub collapsed: bool,
    pub top_most: bool,
    pub visible: bool,
    pub opacity: i64,
    /// 0 = 跟随主题字号。
    pub font_size: i64,
    pub updated_at: i64,
}

impl NoteState {
    /// 新行的默认值(仅在首次 `save` 且请求没给该字段时使用)。
    fn defaults(x: i64, y: i64, w: i64, h: i64, now: i64) -> Self {
        Self {
            x,
            y,
            w,
            h,
            edge: NoteEdge::None,
            auto_hide: false,
            collapsed: false,
            top_most: true,
            visible: true,
            opacity: MAX_OPACITY,
            font_size: 0,
            updated_at: now,
        }
    }
}

/// `save` 的补丁:**None = 不改**(与 todo_update 的三态同语义,见 docs/05 §5)。
#[derive(Debug, Clone, Default)]
pub struct NotePatch {
    pub x: Option<i64>,
    pub y: Option<i64>,
    pub w: Option<i64>,
    pub h: Option<i64>,
    pub screen: Option<String>,
    pub edge: Option<NoteEdge>,
    pub auto_hide: Option<bool>,
    pub collapsed: Option<bool>,
    pub top_most: Option<bool>,
    pub visible: Option<bool>,
    pub opacity: Option<i64>,
    pub font_size: Option<i64>,
}

/// 一条便签的完整视图:待办(TodoItem,含扁平条目字段)+ 窗口状态。
///
/// ⚠️ 序列化形状必须是**扁平**的(todo 的字段提升到顶层 + `note` 键),
/// 不是 `{todo: {...}, note: {...}}` 嵌套 —— 宿主对 list_entries / todo_list /
/// todo_note_list 三个端点走同一套扁平解析,嵌套形状会把整页解析成空行
/// (docs/05 §2.3 的实测记录)。
#[derive(Debug, Clone)]
pub struct NoteItem {
    pub todo: TodoItem,
    pub note: NoteState,
    /// QScreen::name();空串 = 未记录(恢复时按坐标找屏)。
    pub screen: String,
}

impl Serialize for NoteItem {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::SerializeMap;
        let todo_json = serde_json::to_value(&self.todo).map_err(serde::ser::Error::custom)?;
        let mut map = serializer.serialize_map(None)?;
        if let serde_json::Value::Object(obj) = todo_json {
            for (k, v) in obj {
                map.serialize_entry(&k, &v)?;
            }
        }
        // note 对象 = NoteState 全字段 + screen(空串省略:未记录屏名时宿主按坐标找屏)。
        let mut note =
            serde_json::to_value(self.note).map_err(serde::ser::Error::custom)?;
        if !self.screen.is_empty() {
            if let serde_json::Value::Object(obj) = &mut note {
                obj.insert(
                    "screen".into(),
                    serde_json::Value::String(self.screen.clone()),
                );
            }
        }
        map.serialize_entry("note", &note)?;
        map.end()
    }
}

/// 联查三表:条目本体 + 待办时刻 + 窗口状态。
///
/// 列名与 `todo::TODO_SELECT` 保持一致(`ClipEntry::from_row` 按**列名**取值,
/// 复用同一套解析);`todo_due_at`/`todo_done_at` 是它认的徽标列别名。
const NOTE_SELECT: &str = "SELECT e.id, e.kind, e.text_content, e.blob_path, e.file_paths,
     e.width, e.height, e.hash, e.byte_size, e.preview, e.pinned, e.created_at,
     e.last_used_at, e.source, e.tags,
     m.due_at, m.remind_at, m.notify_at, m.done_at, m.note_text,
     m.due_at AS todo_due_at, m.done_at AS todo_done_at,
     n.x, n.y, n.w, n.h, n.screen, n.edge, n.auto_hide, n.collapsed, n.top_most,
     n.visible, n.opacity, n.font_size, n.updated_at
     FROM clip_entries e
     JOIN todo_meta m ON m.id = e.id
     JOIN todo_note n ON n.id = e.id";

fn row_to_item(row: &rusqlite::Row<'_>, now: i64) -> rusqlite::Result<NoteItem> {
    let entry = ClipEntry::from_row(row)?;
    let due_at: Option<i64> = row.get("due_at")?;
    let done_at: Option<i64> = row.get("done_at")?;
    let edge_text: String = row.get("edge")?;
    // 库里的值只有本模块写入,理论恒合法;损坏数据按 none 处理而不是让整页查询失败
    // (与 ClipEntry::from_row 对脏 todo_meta 的宽容口径一致)。
    let edge = NoteEdge::parse(&edge_text).unwrap_or(NoteEdge::None);
    let screen: String = row.get("screen")?;
    Ok(NoteItem {
        todo: TodoItem {
            entry,
            due_at,
            remind_at: row.get("remind_at")?,
            notify_at: row.get("notify_at")?,
            done_at,
            overdue: done_at.is_none() && due_at.is_some_and(|d| d < now),
            note_text: row.get("note_text")?,
        },
        note: NoteState {
            x: row.get("x")?,
            y: row.get("y")?,
            w: row.get("w")?,
            h: row.get("h")?,
            edge,
            auto_hide: row.get::<_, i64>("auto_hide")? != 0,
            collapsed: row.get::<_, i64>("collapsed")? != 0,
            top_most: row.get::<_, i64>("top_most")? != 0,
            visible: row.get::<_, i64>("visible")? != 0,
            opacity: row.get("opacity")?,
            font_size: row.get("font_size")?,
            updated_at: row.get("updated_at")?,
        },
        screen,
    })
}

fn clamp(v: i64, lo: i64, hi: i64) -> i64 {
    v.clamp(lo, hi)
}

/// 夹紧后的窗口状态(所有写入路径共用,避免"某处忘了夹")。
fn normalized(mut state: NoteState) -> NoteState {
    state.w = clamp(state.w, MIN_W, MAX_W);
    state.h = clamp(state.h, MIN_H, MAX_H);
    state.opacity = clamp(state.opacity, MIN_OPACITY, MAX_OPACITY);
    state.font_size = clamp(state.font_size, 0, MAX_FONT_SIZE);
    // 下边吸附不允许自动隐藏(docs/20 §6.2 第 4 条):把非法组合在落库时纠正,
    // 而不是留给宿主每次读取时再判断一遍。
    if !state.edge.allows_auto_hide() {
        state.auto_hide = false;
        state.collapsed = false;
    }
    if !state.auto_hide {
        state.collapsed = false;
    }
    // 未吸附就没有"收起方向":`edge=none` 却 `collapsed=1` 是非法组合 ——
    // 收起位置的几何是按边算的(见 docs/20 §6.3),没有边就等于把窗口丢到屏外。
    // 实测踩到:收起态点「取消贴边」,库里 collapsed 被 auto_hide 分支之外的路径
    // 留在 1,窗口停在屏幕外、鼠标永远进不来 = 便签消失。
    if state.edge == NoteEdge::None {
        state.collapsed = false;
    }
    state
}

/// 保存窗口状态(**upsert**):行在首次 `save` 时创建(docs/20 §4)。
///
/// 首次 `save` **必须**给出 x/y/w/h —— 宿主建窗即有几何,拿不到说明调用方写错了。
/// 其余字段取 [`NoteState::defaults`]。
pub fn save(conn: &Connection, id: &str, patch: &NotePatch) -> AppResult<NoteState> {
    // 只能给 todo 条目建便签(防把普通条目当待办贴屏)。
    let kind: Option<String> = conn
        .query_row(
            "SELECT kind FROM clip_entries WHERE id = ?1",
            params![id],
            |row| row.get(0),
        )
        .optional()?;
    let kind = kind.ok_or_else(|| AppError::InvalidInput("条目不存在".into()))?;
    if kind != crate::store::entry::EntryKind::Todo.as_str() {
        return Err(AppError::InvalidInput("条目不是待办,不能贴屏".into()));
    }

    let now = now_ms();
    let existing = get_state(conn, id)?;
    let mut state = match existing {
        Some(prev) => prev,
        None => match (patch.x, patch.y, patch.w, patch.h) {
            (Some(x), Some(y), Some(w), Some(h)) => NoteState::defaults(x, y, w, h, now),
            _ => {
                return Err(AppError::InvalidInput(
                    "首次保存便签必须带齐 x/y/w/h".into(),
                ))
            }
        },
    };
    if let Some(v) = patch.x {
        state.x = v;
    }
    if let Some(v) = patch.y {
        state.y = v;
    }
    if let Some(v) = patch.w {
        state.w = v;
    }
    if let Some(v) = patch.h {
        state.h = v;
    }
    if let Some(v) = patch.edge {
        state.edge = v;
    }
    if let Some(v) = patch.auto_hide {
        state.auto_hide = v;
    }
    if let Some(v) = patch.collapsed {
        state.collapsed = v;
    }
    if let Some(v) = patch.top_most {
        state.top_most = v;
    }
    if let Some(v) = patch.visible {
        state.visible = v;
    }
    if let Some(v) = patch.opacity {
        state.opacity = v;
    }
    if let Some(v) = patch.font_size {
        state.font_size = v;
    }
    state.updated_at = now;
    let state = normalized(state);

    // screen 是纯文本,不做 clamp;补丁没给就沿用库里的旧值。
    let screen = match &patch.screen {
        Some(s) => s.clone(),
        None => get_screen(conn, id)?.unwrap_or_default(),
    };

    conn.execute(
        "INSERT OR REPLACE INTO todo_note
           (id, x, y, w, h, screen, edge, auto_hide, collapsed, top_most, visible,
            opacity, font_size, updated_at)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)",
        params![
            id,
            state.x,
            state.y,
            state.w,
            state.h,
            screen,
            state.edge.as_str(),
            i64::from(state.auto_hide),
            i64::from(state.collapsed),
            i64::from(state.top_most),
            i64::from(state.visible),
            state.opacity,
            state.font_size,
            state.updated_at,
        ],
    )?;
    Ok(state)
}

/// 只读窗口状态(不含条目本体);不存在返回 None。
pub fn get_state(conn: &Connection, id: &str) -> AppResult<Option<NoteState>> {
    let row = conn
        .query_row(
            "SELECT x, y, w, h, edge, auto_hide, collapsed, top_most, visible,
                    opacity, font_size, updated_at
             FROM todo_note WHERE id = ?1",
            params![id],
            |row| {
                let edge_text: String = row.get("edge")?;
                Ok(NoteState {
                    x: row.get("x")?,
                    y: row.get("y")?,
                    w: row.get("w")?,
                    h: row.get("h")?,
                    edge: NoteEdge::parse(&edge_text).unwrap_or(NoteEdge::None),
                    auto_hide: row.get::<_, i64>("auto_hide")? != 0,
                    collapsed: row.get::<_, i64>("collapsed")? != 0,
                    top_most: row.get::<_, i64>("top_most")? != 0,
                    visible: row.get::<_, i64>("visible")? != 0,
                    opacity: row.get("opacity")?,
                    font_size: row.get("font_size")?,
                    updated_at: row.get("updated_at")?,
                })
            },
        )
        .optional()?;
    Ok(row)
}

fn get_screen(conn: &Connection, id: &str) -> AppResult<Option<String>> {
    let screen: Option<String> = conn
        .query_row(
            "SELECT screen FROM todo_note WHERE id = ?1",
            params![id],
            |row| row.get(0),
        )
        .optional()?;
    Ok(screen)
}

/// 全部便签行(含 `visible = 0`)。宿主据 `visible` 决定是否建窗(docs/20 §7.5)。
///
/// 顺带**自愈孤儿行**:JOIN 不上的行(条目已删但级联漏了)直接删除 ——
/// 与 `TodoItem` 的 JOIN 口径一致,读路径不该把脏数据抛给用户。
pub fn list(conn: &Connection) -> AppResult<Vec<NoteItem>> {
    let now = now_ms();
    let orphans: Vec<String> = conn
        .prepare(
            "SELECT n.id FROM todo_note n
             LEFT JOIN clip_entries e ON e.id = n.id
             WHERE e.id IS NULL OR e.kind != 'todo'",
        )?
        .query_map([], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    for id in &orphans {
        conn.execute("DELETE FROM todo_note WHERE id = ?1", params![id])?;
    }

    let sql = format!("{NOTE_SELECT} ORDER BY n.updated_at DESC, e.last_used_at DESC");
    let items = conn
        .prepare(&sql)?
        .query_map([], |row| row_to_item(row, now))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(items)
}

/// 删除某条目的便签行(级联用;不存在时静默通过)。
///
/// ⚠️ 与 `todo::delete_meta` 同一取舍:调用方(通用删条目路径)不管条目什么类型
/// 都调一次,非待办条目自然是空操作。
pub fn delete_for_entry(conn: &Connection, id: &str) -> AppResult<()> {
    conn.execute("DELETE FROM todo_note WHERE id = ?1", params![id])?;
    Ok(())
}

/// 清空全部(全量重置路径用;与 `entry::clear` 同事务调用)。
pub fn clear_all(conn: &Connection) -> AppResult<()> {
    conn.execute("DELETE FROM todo_note", [])?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::entry::{self, EntryKind};
    use crate::store::todo;

    fn todo_at(conn: &mut Connection, text: &str) -> String {
        todo::create(conn, text, None, None, None, None, None, now_ms()).expect("todo")
    }

    fn patch_full(x: i64, y: i64, w: i64, h: i64) -> NotePatch {
        NotePatch {
            x: Some(x),
            y: Some(y),
            w: Some(w),
            h: Some(h),
            ..Default::default()
        }
    }

    fn note_count(conn: &Connection) -> i64 {
        conn.query_row("SELECT COUNT(*) FROM todo_note", [], |r| r.get(0))
            .expect("count")
    }

    // ---- 迁移 ----

    #[test]
    fn v7_creates_table_and_index_and_keeps_v6_data() {
        let conn = crate::store::memory();
        let tables: Vec<String> = conn
            .prepare("SELECT name FROM sqlite_master WHERE type='table'")
            .unwrap()
            .query_map([], |r| r.get(0))
            .unwrap()
            .collect::<rusqlite::Result<Vec<_>>>()
            .unwrap();
        assert!(tables.iter().any(|t| t == "todo_note"), "v7 应建 todo_note 表");
        let idx: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master \
                 WHERE type='index' AND name='idx_todo_note_screen'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(idx, 1, "v7 应建 idx_todo_note_screen");
        // v6 的 todo_meta 仍在(只加表、不重建)
        let meta: i64 = conn
            .query_row("SELECT COUNT(*) FROM todo_meta", [], |r| r.get(0))
            .unwrap();
        assert_eq!(meta, 0);
        // 完整性
        let ok: String = conn.query_row("PRAGMA integrity_check", [], |r| r.get(0)).unwrap();
        assert_eq!(ok, "ok");
    }

    // ---- CRUD 与三态 ----

    #[test]
    fn save_requires_geometry_on_first_call_then_patches() {
        let mut conn = crate::store::memory();
        let id = todo_at(&mut conn, "买牛奶");

        // 首次缺几何 → 明确报错(不是静默落一个默认位置)
        let err = save(&conn, &id, &NotePatch::default()).unwrap_err();
        assert!(err.to_string().contains("x/y/w/h"), "错误信息应指出缺几何: {err}");

        let state = save(&conn, &id, &patch_full(100, 200, 300, 240)).expect("first save");
        assert_eq!((state.x, state.y, state.w, state.h), (100, 200, 300, 240));
        assert_eq!(state.edge, NoteEdge::None);
        assert!(state.visible, "新便签默认可见");
        assert!(state.top_most, "新便签默认置顶");

        // 键缺失 = 不改
        let patched = save(
            &conn,
            &id,
            &NotePatch {
                x: Some(150),
                ..Default::default()
            },
        )
        .expect("patch x");
        assert_eq!(patched.x, 150);
        assert_eq!(patched.y, 200, "未提及的字段必须保留");
        assert_eq!(patched.w, 300);

        // 仍是同一行(upsert 不重复建)
        assert_eq!(note_count(&conn), 1);
    }

    #[test]
    fn save_rejects_non_todo_and_clamps_numbers() {
        let mut conn = crate::store::memory();
        // 普通文本条目不能贴屏
        let clip = ClipEntry {
            id: "t1".into(),
            kind: EntryKind::Text,
            text_content: Some("x".into()),
            blob_path: None,
            file_paths: None,
            width: None,
            height: None,
            hash: "h1".into(),
            byte_size: 1,
            preview: "x".into(),
            pinned: false,
            created_at: 1,
            last_used_at: 1,
            source: "clipboard".into(),
            tags: None,
            todo: None,
        };
        entry::insert(&conn, &clip).unwrap();
        assert!(save(&conn, "t1", &patch_full(0, 0, 200, 200)).is_err());
        assert!(save(&conn, "missing", &patch_full(0, 0, 200, 200)).is_err());

        // 数值越界被夹紧(不报错):拖窗口时不该因为一次越界就存不进去
        let id = todo_at(&mut conn, "夹紧");
        let state = save(
            &conn,
            &id,
            &NotePatch {
                x: Some(-999_999),
                y: Some(999_999),
                w: Some(10),
                h: Some(100_000),
                opacity: Some(5),
                font_size: Some(500),
                ..Default::default()
            },
        )
        .expect("clamped");
        assert_eq!(state.x, -999_999, "坐标不夹(允许负坐标多屏布局)");
        assert_eq!(state.w, MIN_W);
        assert_eq!(state.h, MAX_H);
        assert_eq!(state.opacity, MIN_OPACITY);
        assert_eq!(state.font_size, MAX_FONT_SIZE);
    }

    #[test]
    fn bottom_edge_forbids_auto_hide() {
        let mut conn = crate::store::memory();
        let id = todo_at(&mut conn, "任务栏侧");
        let state = save(
            &conn,
            &id,
            &NotePatch {
                edge: Some(NoteEdge::Bottom),
                auto_hide: Some(true),
                collapsed: Some(true),
                ..patch_full(0, 0, 240, 200)
            },
        )
        .expect("bottom");
        // 下边吸附与任务栏自动隐藏抢鼠标(docs/20 §6.2 第 4 条):落库时就纠正
        assert!(!state.auto_hide);
        assert!(!state.collapsed);

        // 其它边允许
        let left = save(
            &conn,
            &id,
            &NotePatch {
                edge: Some(NoteEdge::Left),
                auto_hide: Some(true),
                collapsed: Some(true),
                ..Default::default()
            },
        )
        .expect("left");
        assert!(left.auto_hide);
        assert!(left.collapsed);

        // 关掉 autoHide 时 collapsed 必须一起落(否则会出现"没开自动隐藏却处于收起态")
        let off = save(
            &conn,
            &id,
            &NotePatch {
                auto_hide: Some(false),
                ..Default::default()
            },
        )
        .expect("off");
        assert!(!off.auto_hide);
        assert!(!off.collapsed);
    }

    #[test]
    fn edge_rejects_unknown_value() {
        assert!(NoteEdge::parse("diagonal").is_err());
        assert_eq!(NoteEdge::parse("bottom").unwrap(), NoteEdge::Bottom);
    }

    #[test]
    fn detaching_edge_clears_collapsed() {
        // 回归:收起态点「取消贴边」→ collapsed 若留在 1,窗口会停在收起几何
        // (按边算出来的屏外位置),鼠标永远进不来 = 便签"凭空消失"。
        let mut conn = crate::store::memory();
        let id = todo_at(&mut conn, "取消贴边");
        save(
            &conn,
            &id,
            &NotePatch {
                edge: Some(NoteEdge::Left),
                auto_hide: Some(true),
                collapsed: Some(true),
                ..patch_full(0, 100, 240, 200)
            },
        )
        .expect("collapsed");
        assert!(get_state(&conn, &id).unwrap().unwrap().collapsed);

        let detached = save(
            &conn,
            &id,
            &NotePatch {
                edge: Some(NoteEdge::None),
                ..Default::default()
            },
        )
        .expect("detach");
        assert_eq!(detached.edge, NoteEdge::None);
        assert!(!detached.collapsed, "没有边就不该处于收起态");
        // autoHide 本身可以留着:用户下次再贴边时它仍然生效(不强制清)
        assert!(detached.auto_hide);
    }

    // ---- 形状(宿主单一解析路径) ----

    #[test]
    fn note_item_serializes_flat_with_note_object() {
        let mut conn = crate::store::memory();
        let id = todo_at(&mut conn, "写周报");
        save(
            &conn,
            &id,
            &NotePatch {
                screen: Some(r"\\.\DISPLAY1".into()),
                edge: Some(NoteEdge::Right),
                auto_hide: Some(true),
                ..patch_full(1200, 160, 280, 240)
            },
        )
        .expect("save");

        let items = list(&conn).expect("list");
        assert_eq!(items.len(), 1);
        let json = serde_json::to_value(&items[0]).expect("serialize");
        // 扁平:条目字段在顶层,不是 {entry:{...}}
        assert_eq!(json["id"], serde_json::json!(id));
        assert_eq!(json["kind"], serde_json::json!("todo"));
        assert_eq!(json["textContent"], serde_json::json!("写周报"));
        assert_eq!(json["todo"]["done"], serde_json::json!(false));
        assert!(json.get("entry").is_none(), "不得是嵌套形状");
        assert!(json.get("hash").is_none(), "hash 不下发");
        // note 对象
        assert_eq!(json["note"]["x"], serde_json::json!(1200));
        assert_eq!(json["note"]["edge"], serde_json::json!("right"));
        assert_eq!(json["note"]["autoHide"], serde_json::json!(true));
        assert_eq!(json["note"]["topMost"], serde_json::json!(true));
        assert_eq!(json["note"]["screen"], serde_json::json!(r"\\.\DISPLAY1"));
    }

    #[test]
    fn list_skips_rows_of_other_kinds_via_join() {
        let mut conn = crate::store::memory();
        let a = todo_at(&mut conn, "有便签");
        let _b = todo_at(&mut conn, "无便签");
        save(&conn, &a, &patch_full(0, 0, 240, 200)).expect("save");
        let items = list(&conn).expect("list");
        assert_eq!(items.len(), 1, "没贴屏的待办不出现在便签列表里");
        assert_eq!(items[0].todo.entry.id, a);
    }

    // ---- 级联:绝不留下孤儿行 ----

    #[test]
    fn cascade_on_delete_clear_and_clear_done() {
        let mut conn = crate::store::memory();
        let one = todo_at(&mut conn, "一");
        let two = todo_at(&mut conn, "二");
        let done = todo_at(&mut conn, "三");
        for id in [&one, &two, &done] {
            save(&conn, id, &patch_full(0, 0, 240, 200)).expect("save");
        }
        assert_eq!(note_count(&conn), 3);

        // 单条删除(走 todo::delete)
        todo::delete(&conn, &one).expect("delete");
        assert_eq!(note_count(&conn), 2);

        // 通用删条目路径(delete_entry 的接线:先 todo::delete_meta 再 entry::delete)
        todo::delete_meta(&conn, &two).expect("meta");
        entry::delete(&conn, &two).expect("entry");
        assert_eq!(note_count(&conn), 1);

        // clear_done 只清已完成
        todo::set_done(&conn, &done, true).expect("done");
        todo::clear_done(&conn).expect("clear done");
        assert_eq!(note_count(&conn), 0, "清已完成应连带清便签行");

        // clear 全量
        let last = todo_at(&mut conn, "最后");
        save(&conn, &last, &patch_full(0, 0, 240, 200)).expect("save last");
        entry::clear(&conn).expect("clear");
        assert_eq!(note_count(&conn), 0, "全量清空不得留便签孤儿行");
    }

    #[test]
    fn delete_bulk_cascades_note_rows() {
        let mut conn = crate::store::memory();
        let a = todo_at(&mut conn, "a");
        let b = todo_at(&mut conn, "b");
        for id in [&a, &b] {
            save(&conn, id, &patch_full(0, 0, 240, 200)).expect("save");
        }
        let result = entry::delete_bulk(&conn, &[a.clone()]).expect("bulk");
        assert_eq!(result.deleted, vec![a]);
        assert_eq!(note_count(&conn), 1, "批量删除要在同一事务里清便签行");
    }

    // ---- 读路径自愈(写路径漏了也不该把脏数据抛给用户) ----

    #[test]
    fn list_self_heals_orphan_rows() {
        let mut conn = crate::store::memory();
        let id = todo_at(&mut conn, "会被偷删");
        save(&conn, &id, &patch_full(0, 0, 240, 200)).expect("save");
        // 模拟"绕过级联"的删除(旧版本 exe、外部脚本)
        conn.execute("DELETE FROM todo_meta WHERE id = ?1", params![id])
            .unwrap();
        conn.execute("DELETE FROM clip_entries WHERE id = ?1", params![id])
            .unwrap();
        assert_eq!(note_count(&conn), 1, "此刻确有一条孤儿行");

        let items = list(&conn).expect("list");
        assert!(items.is_empty());
        assert_eq!(note_count(&conn), 0, "读路径应把孤儿行清掉");
    }
}
