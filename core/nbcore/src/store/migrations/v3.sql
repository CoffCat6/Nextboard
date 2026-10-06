-- v3：文件类型支持（Phase 3）。kind CHECK 增 'file'；SQLite 无法修改 CHECK 约束，
-- 必须重建表并迁移数据；索引随旧表删除，需全部重建。
CREATE TABLE clip_entries_new (
  id           TEXT PRIMARY KEY,              -- UUID v7
  kind         TEXT NOT NULL CHECK (kind IN ('text', 'image', 'file')),
  text_content TEXT,                          -- kind='text' 时非空
  blob_path    TEXT,                          -- kind='image' 时非空，图片文件绝对路径
  file_paths   TEXT,                          -- kind='file' 时非空，JSON 数组（绝对路径）
  width        INTEGER,                       -- 图片像素宽
  height       INTEGER,                       -- 图片像素高
  hash         TEXT NOT NULL,                 -- blake3 hex，去重键
  byte_size    INTEGER NOT NULL DEFAULT 0,    -- 内容字节数（文本 UTF-8 长度；图片 PNG 大小；文件为磁盘大小合计）
  preview      TEXT NOT NULL DEFAULT '',      -- 文本截断 200 字符；图片空串；文件为展示摘要
  pinned       INTEGER NOT NULL DEFAULT 0,    -- 1 = 固定
  created_at   INTEGER NOT NULL,              -- unix 毫秒
  last_used_at INTEGER NOT NULL               -- unix 毫秒
);
INSERT INTO clip_entries_new
  (id, kind, text_content, blob_path, file_paths, width, height, hash, byte_size, preview, pinned, created_at, last_used_at)
SELECT id, kind, text_content, blob_path, NULL, width, height, hash, byte_size, preview, pinned, created_at, last_used_at
FROM clip_entries;
DROP TABLE clip_entries;
ALTER TABLE clip_entries_new RENAME TO clip_entries;
CREATE INDEX IF NOT EXISTS idx_entries_list ON clip_entries (pinned DESC, last_used_at DESC);
CREATE INDEX IF NOT EXISTS idx_entries_hash ON clip_entries (hash);
CREATE INDEX IF NOT EXISTS idx_entries_kind ON clip_entries (kind, last_used_at DESC);
