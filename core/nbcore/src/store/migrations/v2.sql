-- v2：分类过滤索引（F3 内容分类：kind 高选择性过滤，避免切换文本/图像时全表扫描）
CREATE INDEX IF NOT EXISTS idx_entries_kind ON clip_entries (kind, last_used_at DESC);
