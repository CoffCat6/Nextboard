"""完整性检查:迁移重建表之后,既有资产(数据/索引/触发器)是否完好。

用法:
    python tools/check_db_integrity.py [数据库路径]
不给路径时默认检查 app/build/data/clipboard.db(便携式布局的运行时库)。

重点看:
  1. user_version 与 core 的最新迁移版本一致(从 store/mod.rs 读,不写死)
  2. FTS 三个触发器仍在(重建表会连触发器一起 DROP,必须显式重建)
  3. FTS 索引行数与主表一致(触发器真在工作,不是空壳)
  4. 重建表列未丢、索引齐全、integrity_check ok
  5. 触发器探针:插一行看 FTS 是否同步,再删掉(不动用户数据)
"""
import re
import sqlite3
import sys

# windows 控制台默认 GBK,输出里的 ✅/❌/中文会抛 UnicodeEncodeError —— 实测断言
# 全过却在最后一行 print 上崩,退出码非 0 被当成"检查失败"。这里强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover - 老 Python / 非 TTY
    pass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIGRATION_RS = REPO_ROOT / "core" / "nbcore" / "src" / "store" / "mod.rs"


def latest_version():
    """从 core 源码读 LATEST_VERSION —— 避免在脚本里写死版本号,
    加 v7 迁移时这里自动跟随,不会误报"版本不符"。"""
    text = MIGRATION_RS.read_text(encoding="utf-8")
    m = re.search(r"const\s+LATEST_VERSION\s*:\s*i64\s*=\s*(\d+)", text)
    if not m:
        raise SystemExit(f"无法从 {MIGRATION_RS} 解析 LATEST_VERSION")
    return int(m.group(1))


EXPECTED_VERSION = latest_version()

if len(sys.argv) > 1:
    db_path = Path(sys.argv[1])
else:
    db_path = REPO_ROOT / "app" / "build" / "data" / "clipboard.db"
if not db_path.is_file():
    raise SystemExit(f"数据库不存在:{db_path}\n(先运行一次应用,或显式传入路径)")
print("检查数据库:", db_path)
print("期望版本(来自 core):", EXPECTED_VERSION)

db = db_path
con = sqlite3.connect(db)
fail = []

ver = con.execute("PRAGMA user_version").fetchone()[0]
print("user_version:", ver)
if ver != EXPECTED_VERSION:
    fail.append(f"user_version 应为 {EXPECTED_VERSION},实为 {ver}")

triggers = sorted(r[0] for r in con.execute(
    "SELECT name FROM sqlite_master WHERE type='trigger'"))
print("triggers:", triggers)
for t in ("clip_fts_insert", "clip_fts_delete", "clip_fts_update",
          "todo_meta_fts_insert", "todo_meta_fts_update"):
    if t not in triggers:
        fail.append(f"缺少 FTS 触发器 {t}(搜索/索引同步会静默失效)")

indexes = sorted(r[0] for r in con.execute(
    "SELECT name FROM sqlite_master WHERE type='index' AND name LIKE 'idx_%'"))
print("indexes:", indexes)
for idx in ("idx_entries_list", "idx_entries_hash", "idx_entries_kind", "idx_entries_source"):
    if idx not in indexes:
        fail.append(f"缺少索引 {idx}")

entries = con.execute("SELECT COUNT(*) FROM clip_entries").fetchone()[0]
fts = con.execute("SELECT COUNT(*) FROM clip_fts").fetchone()[0]
print(f"clip_entries={entries} clip_fts={fts}")
if entries != fts:
    fail.append(f"FTS 与主表行数不一致(entries={entries} fts={fts})")

tables = sorted(r[0] for r in con.execute(
    "SELECT name FROM sqlite_master WHERE type='table'"))
if "todo_meta" not in tables:
    fail.append("缺少 todo_meta 表")
# v7 便签窗口状态(docs/20 §3.2):表或索引缺失 → 贴屏功能整块失效(便签不落库)
if "todo_note" not in tables:
    fail.append("缺少 todo_note 表(v7 迁移未执行)")
if "idx_todo_note_screen" not in indexes:
    fail.append("缺少索引 idx_todo_note_screen")
# 便签扩展表的孤儿行体检:指向已删条目的行(级联漏了就靠这里暴露)
if "todo_note" in tables:
    orphan = con.execute(
        "SELECT COUNT(*) FROM todo_note n "
        "LEFT JOIN clip_entries e ON e.id = n.id "
        "WHERE e.id IS NULL OR e.kind != 'todo'").fetchone()[0]
    print("todo_note 孤儿行:", orphan)
    if orphan:
        fail.append(f"todo_note 有 {orphan} 行孤儿(条目已删/非待办,级联漏了)")
# 备注列(v8):丢了这一列,待办只剩标题,备注静默消失
meta_cols = [r[1] for r in con.execute("PRAGMA table_info(todo_meta)")]
if "note_text" not in meta_cols:
    fail.append("todo_meta 缺少 note_text 列(v8 迁移未执行)")
print("todo_meta 列:", meta_cols)

cols = [r[1] for r in con.execute("PRAGMA table_info(clip_entries)")]
print("clip_entries 列数:", len(cols), cols)
for c in ("source", "tags", "ocr_text", "file_paths"):
    if c not in cols:
        fail.append(f"重建表丢了列 {c}")

# 触发器真在工作:插一行看 FTS 是否同步,再删掉(不动用户数据)
con.execute(
    "INSERT INTO clip_entries (id, kind, text_content, hash, byte_size, preview, "
    "pinned, created_at, last_used_at) VALUES "
    "('__probe__','text','触发器探针 probe-token','h-probe',4,'probe',0,1,1)")
synced = con.execute("SELECT COUNT(*) FROM clip_fts WHERE entry_id='__probe__'").fetchone()[0]
con.execute("DELETE FROM clip_entries WHERE id='__probe__'")
left = con.execute("SELECT COUNT(*) FROM clip_fts WHERE entry_id='__probe__'").fetchone()[0]
con.rollback()  # 探针不留痕
print(f"触发器探针: 插入后同步={synced} 删除后残留={left}")
if synced != 1:
    fail.append("INSERT 触发器未同步 FTS")
if left != 0:
    fail.append("DELETE 触发器未清理 FTS")

integrity = con.execute("PRAGMA integrity_check").fetchone()[0]
print("integrity_check:", integrity)
if integrity != "ok":
    fail.append(f"integrity_check={integrity}")

con.close()
if fail:
    print("\n❌ 发现问题:")
    for f in fail:
        print("  -", f)
    sys.exit(1)
print("\n✅ 库结构完好")
