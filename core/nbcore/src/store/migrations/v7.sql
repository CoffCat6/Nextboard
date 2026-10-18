-- v7:待办贴屏(桌面便签)窗口状态(docs/20-todo-sticky-note.md §3)。
--
-- 与 todo_meta 同一手法:条目本体仍存 clip_entries(kind='todo'),只把**窗口状态**
-- 放进扩展表 —— 列表/搜索/标签/计数/删除全部沿用既有链路。
-- ⚠️ 本迁移**只加表,不重建 clip_entries**(v6 那次重建是为了改 CHECK 约束;
--    这次 kind 无需新增),故迁移风险比 v6 低一个数量级 —— 但仍须牢记:
--    **无外键**(与 v1~v6 一致),两表一致性由应用层维护(删条目/清空/批量删除/
--    导入时显式删本表,见 store::todo_note 与 entry::{clear,delete_bulk})。
--
-- 坐标与尺寸一律是**逻辑像素**(Qt 窗口坐标):跨屏时 Qt 按新屏 DPR 缩放窗口物理
-- 尺寸,存物理像素会在"125% 屏摆好、换 100% 屏"时错位。screen 存 QScreen::name()
-- (形如 \\.\\DISPLAY1),恢复时优先落回原屏,找不到再夹回可用区。
CREATE TABLE IF NOT EXISTS todo_note (
  id         TEXT PRIMARY KEY,               -- = clip_entries.id(kind 必须为 'todo')
  x          INTEGER NOT NULL,               -- 逻辑像素(窗口左上,不含阴影留白)
  y          INTEGER NOT NULL,
  w          INTEGER NOT NULL,               -- 逻辑像素(内容区,180..1200)
  h          INTEGER NOT NULL,               -- 逻辑像素(内容区,120..900)
  screen     TEXT NOT NULL DEFAULT '',       -- QScreen::name(),恢复时优先匹配
  edge       TEXT NOT NULL DEFAULT 'none',   -- none|left|right|top|bottom(吸附边)
  auto_hide  INTEGER NOT NULL DEFAULT 0,     -- 贴边后是否自动半隐藏
  collapsed  INTEGER NOT NULL DEFAULT 0,     -- 当前是否处于半隐藏(滑出)态
  top_most   INTEGER NOT NULL DEFAULT 1,     -- 是否置顶
  visible    INTEGER NOT NULL DEFAULT 1,     -- 0 = 用户已关闭(保留几何,重贴回原位)
  opacity    INTEGER NOT NULL DEFAULT 100,   -- 30..100
  font_size  INTEGER NOT NULL DEFAULT 0,     -- 0 = 用全局默认(主题字号)
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_todo_note_screen ON todo_note (screen);
