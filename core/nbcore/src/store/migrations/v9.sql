-- v9:应用来源追踪(docs/25-source-owner-plan.md)。
--
-- source_owner = 复制发生时持有剪贴板的应用 exe 文件名(小写,如 "chrome.exe")。
-- NULL = 未知:既有条目(v9 前入库)、采集失败(属主进程退出/权限不足)、
-- 应用内部入库(截图/翻译等,来源已由 source 列表达)。
--
-- ⚠️ 只 ALTER 加列,不重建表(与 v8 同手法):本列不进 FTS、无索引需求,
--    重建的高风险完全不必要。
ALTER TABLE clip_entries ADD COLUMN source_owner TEXT;
