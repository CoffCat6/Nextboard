-- v1：初始表结构（设计文档 §5.3）
CREATE TABLE IF NOT EXISTS clip_entries (
  id           TEXT PRIMARY KEY,              -- UUID v7
  kind         TEXT NOT NULL CHECK (kind IN ('text', 'image')),
  text_content TEXT,                          -- kind='text' 时非空
  blob_path    TEXT,                          -- kind='image' 时非空，图片文件绝对路径
  width        INTEGER,                       -- 图片像素宽
  height       INTEGER,                       -- 图片像素高
  hash         TEXT NOT NULL,                 -- blake3 hex，去重键
  byte_size    INTEGER NOT NULL DEFAULT 0,    -- 内容字节数（文本为 UTF-8 长度；图片为存储 PNG 大小）
  preview      TEXT NOT NULL DEFAULT '',      -- 文本截断 200 字符；图片条目为空串
  pinned       INTEGER NOT NULL DEFAULT 0,    -- 1 = 固定
  created_at   INTEGER NOT NULL,              -- unix 毫秒
  last_used_at INTEGER NOT NULL               -- unix 毫秒
);
CREATE INDEX IF NOT EXISTS idx_entries_list ON clip_entries (pinned DESC, last_used_at DESC);
CREATE INDEX IF NOT EXISTS idx_entries_hash ON clip_entries (hash);
