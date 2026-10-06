-- v5:一体化素材库扩展(Nextboard 架构 §6)。
--
-- 一、source 来源标记:board 化统一素材库的基础,区分条目来自系统剪贴板、
--    本应用截图还是翻译保存。默认 'clipboard' 保证老数据与存量导入零成本兼容。
ALTER TABLE clip_entries ADD COLUMN source TEXT NOT NULL DEFAULT 'clipboard';

-- 二、tags 标签:JSON 字符串数组,预留分组/收藏功能(v1 仅存储与过滤)。
ALTER TABLE clip_entries ADD COLUMN tags TEXT;

-- FTS 索引不纳入 source/tags(不参与全文检索),既有触发器无需变更。
CREATE INDEX IF NOT EXISTS idx_entries_source ON clip_entries (source, last_used_at DESC);
