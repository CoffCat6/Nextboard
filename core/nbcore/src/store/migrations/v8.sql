-- v8:待办增加「备注」字段(docs/21-todo-title-note.md)。
--
-- 背景:此前待办只有一个 text_content,它同时充当**标题**与**正文** ——
-- 用户想要"一句话标题 + 展开的细节"。现在明确:
--   clip_entries.text_content  → **标题**(单行,列表/便签署名用)
--   todo_meta.note_text        → **备注**(可多行,默认折叠)
--
-- ⚠️ 只 ALTER 加列,**不重建 clip_entries**(与 v5 同一手法):kind 无需新增,
--    重建表的高风险(漏列/漏索引,见 v6 的注释)完全不必要。
--
-- ⚠️ 备注**纳入全文检索**:中文用户普遍把细节写在备注里,不索引等于搜不到。
--    但 clip_fts 是普通 FTS5 表(非外部内容表,见 v4 注释),SQLite 不能给已存在的
--    虚拟表加列 —— 只能重建。
--
-- ⚠️⚠️ 语句顺序**不能换**(实测踩到):
--    触发器挂在 clip_fts 上,DROP TABLE 之后它们就指向不存在的表,此时
--    ALTER TABLE clip_fts_new RENAME TO clip_fts 会因为**校验现有触发器**而报
--    "error in trigger clip_fts_insert: no such table: main.clip_fts"。
--    所以必须先 DROP TRIGGER、再动表 —— 与 v6 那次正好相反(v6 是重建被触发器
--    引用的主表,所以"重建后必须补回触发器";这里是"卸掉引用后才能动表")。
ALTER TABLE todo_meta ADD COLUMN note_text TEXT;

DROP TRIGGER IF EXISTS clip_fts_insert;
DROP TRIGGER IF EXISTS clip_fts_delete;
DROP TRIGGER IF EXISTS clip_fts_update;

CREATE VIRTUAL TABLE clip_fts_new USING fts5(
  entry_id UNINDEXED,
  text_content,
  file_paths,
  ocr_text,
  note_text
);
INSERT INTO clip_fts_new (entry_id, text_content, file_paths, ocr_text, note_text)
SELECT entry_id, text_content, file_paths, ocr_text, NULL FROM clip_fts;
DROP TABLE clip_fts;
ALTER TABLE clip_fts_new RENAME TO clip_fts;

-- 触发器重建:列集合变了,旧触发器只写 4 列,note_text 永远进不了索引。
CREATE TRIGGER clip_fts_insert
AFTER INSERT ON clip_entries BEGIN
  INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text, note_text)
  VALUES (new.id, new.text_content, new.file_paths, new.ocr_text,
          (SELECT note_text FROM todo_meta WHERE id = new.id));
END;

CREATE TRIGGER clip_fts_delete
AFTER DELETE ON clip_entries BEGIN
  DELETE FROM clip_fts WHERE entry_id = old.id;
END;

CREATE TRIGGER clip_fts_update
AFTER UPDATE OF text_content, file_paths, ocr_text ON clip_entries BEGIN
  DELETE FROM clip_fts WHERE entry_id = old.id;
  INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text, note_text)
  VALUES (new.id, new.text_content, new.file_paths, new.ocr_text,
          (SELECT note_text FROM todo_meta WHERE id = new.id));
END;

-- 备注在 todo_meta 上、不在 clip_entries 上,故它要单独两条触发器。
-- FTS 行整行重建(删后插):只改 note_text 时若走 UPDATE,text_content 列会丢。
CREATE TRIGGER todo_meta_fts_update
AFTER UPDATE OF note_text ON todo_meta BEGIN
  DELETE FROM clip_fts WHERE entry_id = old.id;
  INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text, note_text)
  SELECT e.id, e.text_content, e.file_paths, e.ocr_text, new.note_text
  FROM clip_entries e WHERE e.id = new.id;
END;

-- 新建待办时 todo_meta 行在 clip_entries **之后**插入:clip_fts_insert 执行那一刻
-- meta 行还不存在,子查询取到 NULL。这条补一次同步(否则新待办的备注搜不到,
-- 要等下次编辑正文才偶然进索引)。
CREATE TRIGGER todo_meta_fts_insert
AFTER INSERT ON todo_meta BEGIN
  DELETE FROM clip_fts WHERE entry_id = new.id;
  INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text, note_text)
  SELECT e.id, e.text_content, e.file_paths, e.ocr_text, new.note_text
  FROM clip_entries e WHERE e.id = new.id;
END;
