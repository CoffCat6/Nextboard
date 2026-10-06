-- v4：全文检索（FTS5）+ OCR 自动入库（Phase 9）。
--
-- 一、全文检索：**普通 FTS5 表**（非外部内容表）。
--   为什么不用外部内容表（content='clip_entries'）：外部内容模式要求 content_rowid
--   指向 INTEGER rowid，而 clip_entries 主键是 TEXT UUID（前端契约到处传 id，不能改）。
--   强行使用会报 "datatype mismatch"（实测）。故本表自存分词文本 + 一个 UNINDEXED
--   的 entry_id 列指回主表。
--   空间代价：每条多一份可分词文本的副本。可接受的理由：FTS 存的是分词后倒排索引
--   + 原文列，文本条目占比不高且历史上限 5000 条；相比 LIKE 全表扫描的收益值得。
--
--   三个触发器（INSERT/DELETE/UPDATE）自动维护索引——insert/delete/touch 等既有
--   代码路径无需任何改动。
--
-- 二、OCR 自动入库：clip_entries 补一列 ocr_text（NULL = 未识别/未开启），
--   一并纳入 FTS，使"截图里的字"可被搜索。存量行为 NULL，由用户开启
--   ocrEnabled 后按需回填，不在迁移里批量跑 OCR。

-- OCR 文本列（Phase 9 F28）。必须在虚拟表之前：触发器引用 new.ocr_text。
ALTER TABLE clip_entries ADD COLUMN ocr_text TEXT;

CREATE VIRTUAL TABLE IF NOT EXISTS clip_fts USING fts5(
  entry_id UNINDEXED,
  text_content,
  file_paths,
  ocr_text
);

-- 存量数据一次性建索引（普通 FTS 表直接 INSERT...SELECT）
INSERT INTO clip_fts (entry_id, text_content, file_paths, ocr_text)
SELECT id, text_content, file_paths, ocr_text FROM clip_entries;

-- 同步触发器：虚拟表列顺序须与声明一致
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
