-- v6:待办(Todo)类型。
--
-- 一、kind CHECK 增 'todo':SQLite 不能修改 CHECK 约束,须重建表并迁移数据;
--     索引随旧表删除,需全部重建。⚠️ 重建必须带全**当时**的全部列 ——
--     v3 重建时只写了 v3 的列,后续 v4(ocr_text)/v5(source、tags)靠 ALTER 追加,
--     所以这里的列清单取 v5 之后的最终形态,顺序与 v5 一致。
--     注意:clip_entries.tags 既是待办的标签来源,也是待办的唯一标签通道
--     (待办不另建标签表,复用 json_each 筛选)。
--
-- 二、todo_meta 扩展表:待办专属时刻字段与触发标志,**与条目 1:1**。
--     待办条目本体仍存 clip_entries(kind='todo'),列表/搜索/标签/固定/删除
--     全部复用既有链路,只有时刻字段进扩展表 —— 避免每行 list_entries 都背
--     时刻载荷,也避免两套序列化分叉。
--     与 v1~v5 一致:**不使用外键**,两表一致性由应用层维护(删除条目时显式
--     删扩展行,见 store::todo::delete_meta)。

CREATE TABLE clip_entries_new (
  id           TEXT PRIMARY KEY,              -- UUID v7
  kind         TEXT NOT NULL CHECK (kind IN ('text', 'image', 'file', 'todo')),
  text_content TEXT,                          -- kind='todo' 时为待办正文(非空)
  blob_path    TEXT,                          -- kind='image' 时非空
  file_paths   TEXT,                          -- kind='file' 时非空,JSON 数组
  width        INTEGER,
  height       INTEGER,
  hash         TEXT NOT NULL,                 -- 待办写占位值 'todo:<id>'(见 §2.4)
  byte_size    INTEGER NOT NULL DEFAULT 0,    -- 待办 = 正文 UTF-8 长度(与 text 同口径)
  preview      TEXT NOT NULL DEFAULT '',      -- 待办 = 正文截断 200 字符(与 text 同口径)
  pinned       INTEGER NOT NULL DEFAULT 0,
  created_at   INTEGER NOT NULL,              -- unix 毫秒
  last_used_at INTEGER NOT NULL,
  source       TEXT NOT NULL DEFAULT 'clipboard',  -- 待办固定 'todo'
  tags         TEXT,                          -- JSON 数组字符串,沿用 v5
  ocr_text     TEXT
);
INSERT INTO clip_entries_new
  (id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size,
   preview, pinned, created_at, last_used_at, source, tags, ocr_text)
SELECT id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size,
       preview, pinned, created_at, last_used_at, source, tags, ocr_text
FROM clip_entries;
DROP TABLE clip_entries;
ALTER TABLE clip_entries_new RENAME TO clip_entries;

-- 索引重建(v1/v2/v3/v5 全量,与迁移前等价)
CREATE INDEX IF NOT EXISTS idx_entries_list   ON clip_entries (pinned DESC, last_used_at DESC);
CREATE INDEX IF NOT EXISTS idx_entries_hash   ON clip_entries (hash);
CREATE INDEX IF NOT EXISTS idx_entries_kind   ON clip_entries (kind, last_used_at DESC);
CREATE INDEX IF NOT EXISTS idx_entries_source ON clip_entries (source, last_used_at DESC);

-- FTS 触发器引用表名,重建后自动指向新表(SQLite 按名字解析)——但**触发器本体
-- 挂在旧表上,DROP TABLE 会连同一起删掉**,所以必须重建(v3 当年没这问题是因为
-- FTS 是 v4 才引入的;漏了这步会让搜索/索引同步全部静默失效,单测会红)。
-- clip_fts 内容行不丢(重建表不触碰它),虚拟表本身也无需重建。
CREATE TRIGGER IF NOT EXISTS clip_fts_insert
AFTER INSERT ON clip_entries BEGIN
  INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text)
  VALUES (new.id, new.text_content, new.file_paths, new.ocr_text);
END;

CREATE TRIGGER IF NOT EXISTS clip_fts_delete
AFTER DELETE ON clip_entries BEGIN
  DELETE FROM clip_fts WHERE entry_id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS clip_fts_update
AFTER UPDATE OF text_content, file_paths, ocr_text ON clip_entries BEGIN
  DELETE FROM clip_fts WHERE entry_id = old.id;
  INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text)
  VALUES (new.id, new.text_content, new.file_paths, new.ocr_text);
END;

-- 待办扩展表:与条目 1:1,随条目删除而删除(应用侧显式 DELETE,无外键)。
CREATE TABLE IF NOT EXISTS todo_meta (
  id           TEXT PRIMARY KEY,              -- = clip_entries.id
  due_at       INTEGER,                       -- 截止时刻,unix 毫秒;NULL = 无截止
  remind_at    INTEGER,                       -- 面板内提醒时刻;NULL = 不提醒
  notify_at    INTEGER,                       -- 系统通知时刻;NULL = 不通知
  done_at      INTEGER,                       -- 完成时刻;NULL = 未完成(归档判定依据)
  reminded     INTEGER NOT NULL DEFAULT 0,    -- remind 已触发过(调度去重)
  notified     INTEGER NOT NULL DEFAULT 0,    -- notify 已触发过
  bubble_shown INTEGER NOT NULL DEFAULT 0     -- 气泡已播报(宿主确认制去重)
);
CREATE INDEX IF NOT EXISTS idx_todo_due    ON todo_meta (due_at);
CREATE INDEX IF NOT EXISTS idx_todo_remind ON todo_meta (remind_at);
CREATE INDEX IF NOT EXISTS idx_todo_notify ON todo_meta (notify_at);
