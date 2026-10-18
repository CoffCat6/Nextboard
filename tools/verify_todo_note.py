"""待办贴屏(桌面便签)契约验证:经 nbcore.dll 的 nb_call 走真实 FFI。

用法(仓库根或任意位置):
    python tools/verify_todo_note.py
前置:`cargo build --release` 已产出 nbcore.dll(core/target/release/)。

覆盖(全部在**独立临时数据目录**,不碰用户库):
  - v7 建库:todo_note 表 + idx_todo_note_screen,user_version 跟 core 一致
  - todo_note_save 的 upsert / 三态(键缺失=不改)/ 首次缺几何报错
  - 数值夹紧(w/h/opacity/fontSize 越界不报错);edge 非法值报错
  - 下边吸附禁止 autoHide(落库即纠正)
  - todo_note_list 形状:扁平条目 + todo 徽标 + note 对象
  - visible 语义:关窗保留几何(重启不建窗、重贴回原位)
  - 级联:todo_delete / delete_entry / delete_entries_bulk / clear_history /
          todo_clear_done 之后 todo_note 无孤儿行
  - 备份导入:便签窗口状态随新 id 一起搬(换 id 也不丢)
"""
import ctypes
import json
import re
import shutil
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

# windows 控制台默认 GBK,输出里的 ✅/中文会抛 UnicodeEncodeError 把脚本打断
# (实测:断言都过了,却在 print 上崩)。这里强制 UTF-8,失败就退回可替换模式。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover - 老 Python / 非 TTY
    pass

REPO_ROOT = Path(__file__).resolve().parent.parent
DLL = REPO_ROOT / "core" / "target" / "release" / "nbcore.dll"
MOD_RS = REPO_ROOT / "core" / "nbcore" / "src" / "store" / "mod.rs"


def latest_version():
    text = MOD_RS.read_text(encoding="utf-8")
    return int(re.search(r"const\s+LATEST_VERSION\s*:\s*i64\s*=\s*(\d+)", text).group(1))


class Core:
    def __init__(self, data_dir):
        if not DLL.is_file():
            raise SystemExit(
                f"找不到 {DLL}\n请先在 core/ 目录执行:cargo build --release")
        self.dll = ctypes.CDLL(str(DLL))
        # 出参用 c_void_p 保留原始指针,再 string_at 取内容:用 c_char_p 会把指针
        # 换成 Python bytes,回传给 nb_free_string 就是野指针(堆损坏)。
        self.dll.nb_init.restype = ctypes.c_int
        self.dll.nb_init.argtypes = [ctypes.c_char_p]
        self.dll.nb_call.restype = ctypes.c_int
        self.dll.nb_call.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                     ctypes.POINTER(ctypes.c_void_p)]
        self.dll.nb_free_string.restype = None
        self.dll.nb_free_string.argtypes = [ctypes.c_void_p]
        cfg = json.dumps({"data_dir": data_dir, "notify_hwnd": 0}).encode()
        rc = self.dll.nb_init(cfg)
        assert rc == 0, f"nb_init 失败 rc={rc}"

    def call(self, method, payload=None):
        out = ctypes.c_void_p()
        code = self.dll.nb_call(method.encode(),
                                json.dumps(payload or {}).encode(),
                                ctypes.byref(out))
        assert out.value, f"{method} 无响应(rc={code})"
        try:
            return json.loads(ctypes.string_at(out.value).decode("utf-8"))
        finally:
            self.dll.nb_free_string(out)


def note_count(db, where=""):
    con = sqlite3.connect(db)
    try:
        sql = "SELECT COUNT(*) FROM todo_note" + (" WHERE " + where if where else "")
        return con.execute(sql).fetchone()[0]
    finally:
        con.close()


def main():
    data_dir = tempfile.mkdtemp(prefix="nb_note_verify_")
    print("临时数据目录:", data_dir)
    core = Core(data_dir)
    db = str(Path(data_dir) / "clipboard.db")

    # ---- v7 建库 ----
    con = sqlite3.connect(db)
    ver = con.execute("PRAGMA user_version").fetchone()[0]
    tabs = sorted(r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type='table'"))
    cols = [r[1] for r in con.execute("PRAGMA table_info(todo_note)")]
    con.close()
    print("user_version:", ver, "| todo_note 列:", cols)
    assert ver == latest_version(), f"user_version={ver} 应为 {latest_version()}"
    assert "todo_note" in tabs, tabs
    for c in ("screen", "edge", "auto_hide", "collapsed", "top_most", "visible",
              "opacity", "font_size", "updated_at"):
        assert c in cols, f"todo_note 缺列 {c}: {cols}"
    print("  ✅ v7 迁移:todo_note 表与列齐全,版本号跟随 core")

    # ---- 首次 save 缺几何 → 报错 ----
    tid = core.call("todo_create", {"text": "写周报"})["data"]["id"]
    bad = core.call("todo_note_save", {"id": tid})
    assert not bad.get("ok"), f"首次缺几何应报错: {bad}"
    assert "x/y/w/h" in bad["error"]["message"], bad
    print("  ✅ 首次 save 缺几何明确报错(不是静默落默认位置)")

    # ---- 建窗 ----
    r = core.call("todo_note_save",
                  {"id": tid, "x": 1200, "y": 160, "w": 280, "h": 240,
                   "screen": "\\\\.\\DISPLAY1", "edge": "right", "autoHide": True})
    assert r["ok"], r
    st = r["data"]
    assert (st["x"], st["y"], st["w"], st["h"]) == (1200, 160, 280, 240), st
    assert st["edge"] == "right" and st["autoHide"] is True, st
    assert st["topMost"] is True and st["visible"] is True, st
    print("  ✅ 首次 save 建行,几何/吸附边/置顶默认值正确")

    # ---- 三态:键缺失 = 不改 ----
    r = core.call("todo_note_save", {"id": tid, "x": 1180})["data"]
    assert r["x"] == 1180 and r["y"] == 160 and r["w"] == 280, r
    print("  ✅ 键缺失 = 不改(只动了 x)")

    # ---- 夹紧(不报错) ----
    r = core.call("todo_note_save",
                  {"id": tid, "w": 5, "h": 99999,
                   "opacity": 1, "fontSize": 999})
    assert r["ok"], r
    st = r["data"]
    assert st["w"] == 180 and st["h"] == 900, st
    assert st["opacity"] == 30 and st["fontSize"] == 96, st
    print("  ✅ 数值越界被夹紧(拖窗越界不该让位置存不进去)")

    # ---- edge 非法值 → 报错(枚举不夹) ----
    bad = core.call("todo_note_save", {"id": tid, "edge": "diagonal"})
    assert not bad.get("ok"), bad
    print("  ✅ edge 非法值显式报错")

    # ---- 下边吸附禁止 autoHide ----
    r = core.call("todo_note_save",
                  {"id": tid, "edge": "bottom", "autoHide": True, "collapsed": True})["data"]
    assert r["autoHide"] is False and r["collapsed"] is False, r
    print("  ✅ 下边吸附自动纠正 autoHide(不与任务栏抢鼠标)")

    # ---- 非待办条目不能贴屏 ----
    txt_id = core.call("save_text_entry", {"text": "普通文本"})["data"]["entry"]["id"]
    bad = core.call("todo_note_save", {"id": txt_id, "x": 0, "y": 0, "w": 240, "h": 200})
    assert not bad.get("ok"), bad
    print("  ✅ 非待办条目拒绝贴屏")

    # ---- 形状:扁平 + todo 徽标 + note ----
    r = core.call("todo_note_save",
                  {"id": tid, "edge": "left", "autoHide": True, "collapsed": False})["data"]
    assert r["edge"] == "left" and r["autoHide"] is True, r
    items = core.call("todo_note_list")["data"]["items"]
    assert len(items) == 1, items
    it = items[0]
    print("  条目键:", sorted(it.keys()))
    assert it["id"] == tid and it["kind"] == "todo", it
    assert it["textContent"] == "写周报", it
    assert it.get("todo") is not None, "缺 todo 徽标"
    assert "entry" not in it, "仍是嵌套形状(宿主会解析成空行)"
    assert "hash" not in it, "hash 不应下发"
    note = it["note"]
    assert note["edge"] == "left" and note["autoHide"] is True, note
    assert note.get("screen") == "\\\\.\\DISPLAY1", note
    print("  ✅ todo_note_list 扁平条目 + todo 徽标 + note 对象")

    # ---- visible = 0:关窗保留几何 ----
    core.call("todo_note_save", {"id": tid, "visible": False})
    items = core.call("todo_note_list")["data"]["items"]
    assert len(items) == 1 and items[0]["note"]["visible"] is False, items
    assert items[0]["note"]["x"] == 1180, "关窗不得丢几何(重贴要回原位)"
    # 重贴:只翻 visible,几何仍在
    core.call("todo_note_save", {"id": tid, "visible": True})
    items = core.call("todo_note_list")["data"]["items"]
    assert items[0]["note"]["visible"] is True
    assert items[0]["note"]["x"] == 1180, "重贴应回到上次几何"
    print("  ✅ visible 往返:关窗保留几何,重贴回原位")

    # ---- 多行 + 排序(updated_at 降序) ----
    t2 = core.call("todo_create", {"text": "第二条"})["data"]["id"]
    core.call("todo_note_save", {"id": t2, "x": 40, "y": 40, "w": 240, "h": 200})
    items = core.call("todo_note_list")["data"]["items"]
    assert len(items) == 2, items
    assert items[0]["id"] == t2, "最近更新的应排最前"
    print("  ✅ 多行列表 + updated_at 降序")

    # ---- 标题 / 备注(docs/21)----
    # todo_create 带备注 → todo_get / todo_note_list 都要下发 noteText,且与标题分开
    nt = core.call("todo_create", {"text": "有备注的待办",
                                   "noteText": "第一行\n第二行"})["data"]["id"]
    core.call("todo_note_save", {"id": nt, "x": 10, "y": 10, "w": 240, "h": 200})
    got = core.call("todo_get", {"id": nt})["data"]
    assert got["textContent"] == "有备注的待办", got
    assert got["noteText"] == "第一行\n第二行", f"备注应原样保留: {got.get('noteText')}"
    listed = [i for i in core.call("todo_note_list")["data"]["items"] if i["id"] == nt][0]
    assert listed.get("noteText") == "第一行\n第二行", listed.get("noteText")
    # 便签端点的形状:noteText 在**顶层**,不在 note 对象里
    assert "noteText" not in listed["note"], listed["note"]
    print("  ✅ 标题与备注分离:todo_get / todo_note_list 都下发顶层 noteText")

    # 三态:缺键 = 不改;空串 = 清空
    core.call("todo_update", {"id": nt, "noteText": "改过的备注"})
    assert core.call("todo_get", {"id": nt})["data"]["noteText"] == "改过的备注"
    core.call("todo_update", {"id": nt, "text": "只改标题"})
    assert core.call("todo_get", {"id": nt})["data"]["noteText"] == "改过的备注", \
        "只改标题时备注必须保留(缺键 = 不改)"
    core.call("todo_update", {"id": nt, "noteText": None})
    assert not core.call("todo_get", {"id": nt})["data"].get("noteText"), \
        "null 应清空备注"
    print("  ✅ 备注三态:缺键不改 / null 清空")

    # 备注可被搜索(FTS + LIKE 两条路都要覆盖)
    core.call("todo_update", {"id": nt, "noteText": "季度 OKR 复盘材料"})
    hit = core.call("list_entries", {"query": "OKR", "limit": 50, "offset": 0})["data"]
    assert any(i["id"] == nt for i in hit["items"]), f"备注应可被 FTS 检索: {hit['total']}"
    hit2 = core.call("list_entries", {"query": "复盘", "limit": 50, "offset": 0})["data"]
    assert any(i["id"] == nt for i in hit2["items"]), \
        f"中文整词要靠 LIKE 回退命中备注: {hit2['total']}"
    core.call("todo_delete", {"id": nt})
    print("  ✅ 备注可被检索(FTS 命中 + 中文 LIKE 回退命中)")

    # ---- 级联:todo_delete ----
    core.call("todo_note_delete", {"id": t2})
    assert note_count(db) == 1
    # 幂等:再删一次也成功
    assert core.call("todo_note_delete", {"id": t2})["ok"]
    print("  ✅ todo_note_delete 幂等")

    core.call("todo_note_save", {"id": t2, "x": 40, "y": 40, "w": 240, "h": 200})
    core.call("todo_delete", {"id": t2})
    assert note_count(db) == 1, "todo_delete 应连带删便签行"
    print("  ✅ todo_delete 级联清便签行")

    # ---- 级联:delete_entry(通用路径) ----
    t3 = core.call("todo_create", {"text": "第三条"})["data"]["id"]
    core.call("todo_note_save", {"id": t3, "x": 10, "y": 10, "w": 240, "h": 200})
    core.call("delete_entry", {"id": t3})
    assert note_count(db) == 1, "delete_entry 应连带删便签行"
    print("  ✅ delete_entry 级联清便签行")

    # ---- 级联:delete_entries_bulk ----
    t4 = core.call("todo_create", {"text": "第四条"})["data"]["id"]
    core.call("todo_note_save", {"id": t4, "x": 10, "y": 10, "w": 240, "h": 200})
    core.call("delete_entries_bulk", {"ids": [t4]})
    assert note_count(db) == 1, "批量删除应连带删便签行"
    print("  ✅ delete_entries_bulk 级联清便签行")

    # ---- 级联:todo_clear_done(只清已完成的) ----
    t5 = core.call("todo_create", {"text": "已完成的"})["data"]["id"]
    t6 = core.call("todo_create", {"text": "未完成的"})["data"]["id"]
    for i in (t5, t6):
        core.call("todo_note_save", {"id": i, "x": 0, "y": 0, "w": 240, "h": 200})
    core.call("todo_set_done", {"id": t5, "done": True})
    core.call("todo_clear_done")
    left = core.call("todo_note_list")["data"]["items"]
    left_ids = [i["id"] for i in left]
    assert t5 not in left_ids, "清已完成应删掉它的便签行"
    assert t6 in left_ids and tid in left_ids, left_ids
    print("  ✅ todo_clear_done 只清已完成的便签行")

    # ---- 备份导入:便签状态随新 id 搬迁 ----
    # 备份前先造一条带备注的待办(导入后要断言备注跟回来)
    noted = core.call("todo_create", {"text": "带备注备份", "noteText": "备份里的备注"})["data"]["id"]
    core.call("todo_note_save", {"id": noted, "x": 0, "y": 0, "w": 240, "h": 200})

    backup = str(Path(data_dir) / "backup.zip")
    r = core.call("backup_create", {"path": backup})
    assert r["ok"], r
    # 先清空当前便签行,再导入(原 id 冲突会被重铸,便签行必须跟着新 id 走)
    core.call("clear_history")
    assert note_count(db) == 0, "全量清空后不得留便签孤儿行"
    r = core.call("import_backup", {"path": backup})
    assert r["ok"], r
    print("  import 结果:", {k: r["data"][k] for k in ("imported", "skipped")})
    items = core.call("todo_note_list")["data"]["items"]
    assert items, "导入后便签行应随待办一起回来"
    # 备注也必须随导入回来(docs/21):只搬 todo_meta 的时刻字段会静默丢备注
    notes_present = [i.get("noteText") for i in items if i.get("noteText")]
    print("  导入后的备注:", notes_present)
    assert any(notes_present), f"导入后备注不该全丢: {[i.get('noteText') for i in items]}"
    imported_note = items[0]["note"]
    print("  导入后的便签:", items[0]["textContent"], imported_note)
    assert imported_note["w"] >= 180, imported_note
    print("  ✅ 备份导入:便签窗口状态随新 id 搬迁,clear_history 无孤儿")

    # ---- 级联:clear_history 已经验证;再确认 todo_note 行数与待办行数一致 ----
    con = sqlite3.connect(db)
    todos = con.execute("SELECT COUNT(*) FROM clip_entries WHERE kind='todo'").fetchone()[0]
    notes = con.execute("SELECT COUNT(*) FROM todo_note").fetchone()[0]
    con.close()
    assert notes <= todos, f"便签行({notes}) 不得多于待办({todos})"
    print(f"  便签行 {notes} ≤ 待办 {todos} ✅")

    shutil.rmtree(data_dir, ignore_errors=True)
    print("\n全部断言通过 ✅")


if __name__ == "__main__":
    main()
