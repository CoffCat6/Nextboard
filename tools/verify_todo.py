"""待办链路端到端验证:经 nbcore.dll 的 nb_call 走真实契约。

用法(在仓库根目录或任意位置):
    python tools/verify_todo.py
前置:`cargo build --release` 已产出 nbcore.dll(core/target/release/)。

覆盖(全部在**独立临时数据目录**,不碰用户库):
  - v6 建库:todo_meta 存在、user_version=6
  - todo_create / todo_get / todo_list / todo_set_done / todo_update / todo_delete
  - 形状契约:todo_list 是**扁平**的(顶层 id/kind/textContent + todo 徽标)
  - list_entries 对 todo 条目联查下发 todo 徽标(主列表复选框/截止的前提)
  - 时刻三态:null = 清空;传入毫秒 = 设置
  - tag / query / due=today 过滤在两个端点同语义
  - 排序:逾期 > 今日 > 未来 > 无期限,归档垫底
  - 到点扫描与气泡待播报 / 播报确认去重
"""
import ctypes
import json
import os
import shutil
import sqlite3
import tempfile
import time
from pathlib import Path

# 路径按**脚本位置**推导(仓库根 = tools/ 的上一级):此前写死绝对路径,
# 换机器/换目录即失效 —— 入库后必须可移植。
REPO_ROOT = Path(__file__).resolve().parent.parent
DLL = REPO_ROOT / "core" / "target" / "release" / "nbcore.dll"


class Core:
    def __init__(self, data_dir):
        if not DLL.is_file():
            raise SystemExit(
                f"找不到 {DLL}\n请先在 core/ 目录执行:cargo build --release")
        self.dll = ctypes.CDLL(str(DLL))
        # ⚠️ 出参必须用 c_void_p 保留原始指针再 string_at:用 c_char_p 会把指针
        #    换成 Python bytes,回传给 nb_free_string 就成了野指针(堆损坏)。
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


def main():
    data_dir = tempfile.mkdtemp(prefix="nb_todo_verify_")
    print("临时数据目录:", data_dir)
    core = Core(data_dir)
    now = int(time.time() * 1000)
    DAY = 86_400_000

    # ---- v6 建库 ----
    db = os.path.join(data_dir, "clipboard.db")
    con = sqlite3.connect(db)
    ver = con.execute("PRAGMA user_version").fetchone()[0]
    tabs = sorted(r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type='table'"))
    cols = [r[1] for r in con.execute("PRAGMA table_info(todo_meta)")]
    con.close()
    print("user_version:", ver, "| todo_meta 列:", cols)
    assert ver == 6, ver
    assert "todo_meta" in tabs, tabs
    assert "bubble_shown" in cols and "reminded" in cols, cols
    print("  ✅ v6 迁移:todo_meta 建好、版本号正确")

    # ---- create ----
    overdue = core.call("todo_create", {"text": "过期的任务", "dueAt": now - DAY,
                                        "tags": ["工作"]})["data"]["id"]
    future = core.call("todo_create", {"text": "买牛奶", "dueAt": now + DAY,
                                       "remindAt": now + 3600_000,
                                       "tags": ["生活"]})["data"]["id"]
    plain = core.call("todo_create", {"text": "无期限的任务"})["data"]["id"]
    core.call("todo_set_done", {"id": plain, "done": True})
    print("create ×3 + set_done ✅")

    # ---- 形状契约(宿主 applyPage 依赖扁平) ----
    res = core.call("todo_list", {"done": "all", "limit": 50, "offset": 0})
    items = res["data"]["items"]
    assert len(items) == 3, items
    for it in items:
        print("  ", it["textContent"][:8], "| kind:", it["kind"],
              "| todo:", it.get("todo"), "| tags:", it.get("tags"))
    assert all(it.get("kind") == "todo" and "id" in it for it in items), "未扁平化"
    assert all("entry" not in it for it in items), "仍是嵌套形状"
    assert all(it.get("todo") is not None for it in items), "缺 todo 徽标"
    assert all("hash" not in it for it in items), "hash 不应下发"
    print("  ✅ todo_list 扁平形状 + 徽标齐全")

    # ---- 排序 ----
    assert items[0]["id"] == overdue, "逾期未完成应排最前"
    assert items[-1]["id"] == plain, "已完成应垫底"
    assert items[0]["todo"]["overdue"] is True
    assert items[0]["todo"]["done"] is False
    print("  ✅ 排序(逾期>未来>无期限,归档垫底)+ overdue 徽标")

    # ---- list_entries 联查 ----
    entries = core.call("list_entries", {"limit": 50, "offset": 0})["data"]["items"]
    todos = [e for e in entries if e.get("kind") == "todo"]
    assert len(todos) == 3, todos
    assert all(e.get("todo") is not None for e in todos), "主列表 todo 条目缺徽标"
    done_row = [e for e in todos if e["id"] == plain][0]
    assert done_row["todo"]["done"] is True, "主列表完成态不同步"
    print("  ✅ list_entries 对待办条目联查下发徽标")

    # ---- tag / query 过滤(两端口径一致) ----
    tags = core.call("list_tags")["data"]
    assert sorted(t["tag"] for t in tags) == ["工作", "生活"], tags
    a = core.call("todo_list", {"done": "all", "tag": "工作", "limit": 50, "offset": 0})["data"]
    b = core.call("list_entries", {"tag": "工作", "limit": 50, "offset": 0})["data"]
    assert a["total"] == 1 and a["items"][0]["id"] == overdue, a
    assert b["total"] == 1, b
    q = core.call("todo_list", {"done": "all", "query": "牛奶", "limit": 50, "offset": 0})["data"]
    assert q["total"] == 1 and q["items"][0]["id"] == future, q
    esc = core.call("todo_list", {"done": "all", "query": "_", "limit": 50, "offset": 0})["data"]
    assert esc["total"] == 0, f"LIKE 通配符未转义: {esc}"
    print("  ✅ 标签筛选 / 搜索两端口径一致,通配符已转义")

    # ---- due=today 过滤(托盘「今日待办」口径:逾期 + 今日到期,未完成) ----
    t = core.call("todo_list", {"done": "open", "due": "today", "limit": 50, "offset": 0})["data"]
    t_ids = sorted(i["id"] for i in t["items"])
    print("todo_list(due=today) total:", t["total"], "| ids:", [i["textContent"] for i in t["items"]])
    # 未完成:overdue(逾期)+ future(明天)= 应只含逾期;future 是明天不含;
    # plain 已完成不含。此刻 future 因 due=tomorrow 被排除,只剩 overdue
    assert t_ids == [overdue], t
    # due=today 但 done=all:已完成的无截止条目也不该出现(due IS NOT NULL 前提)
    t2 = core.call("todo_list", {"done": "all", "due": "today", "limit": 50, "offset": 0})["data"]
    assert t2["total"] == 1, t2
    # 非法 due 值必须报错,不能静默忽略
    bad = core.call("todo_list", {"done": "open", "due": "tomorrow"})
    assert not bad.get("ok"), bad
    print("  ✅ due=today 过滤(逾期+今日,未完成)+ 非法值报错")

    # ---- 编辑预填(todo_get)与时刻三态 ----
    got = core.call("todo_get", {"id": future})["data"]
    print("todo_get 顶层键:", sorted(got.keys()))
    assert got["dueAt"] == now + DAY and got["remindAt"] == now + 3600_000, got
    assert got["textContent"] == "买牛奶"
    assert json.loads(got["tags"]) == ["生活"]
    assert got["todo"]["done"] is False
    print("  ✅ 编辑预填字段齐全(正文/截止/提醒/标签)")

    core.call("todo_update", {"id": future, "dueAt": None, "remindAt": None, "notifyAt": None})
    got = core.call("todo_get", {"id": future})["data"]
    assert got.get("dueAt") is None and got.get("remindAt") is None, got
    print("  ✅ 时刻清空(null 三态)生效")

    # ---- 编辑正文/标签 + 清空标签 ----
    core.call("todo_update", {"id": future, "text": "买牛奶和鸡蛋", "tags": []})
    got = core.call("todo_get", {"id": future})["data"]
    assert got["textContent"] == "买牛奶和鸡蛋", got
    assert not got.get("tags"), f"空数组应清空标签: {got.get('tags')}"
    print("  ✅ 编辑正文 + 空数组清空标签")

    # ---- 推迟(只动时刻,不动正文/标签) ----
    core.call("todo_update", {"id": overdue, "dueAt": now + 2 * DAY,
                              "remindAt": now + 2 * DAY, "notifyAt": now + 2 * DAY})
    got = core.call("todo_get", {"id": overdue})["data"]
    assert got["textContent"] == "过期的任务", got
    assert json.loads(got["tags"]) == ["工作"], got
    assert got["dueAt"] == now + 2 * DAY
    print("  ✅ 推迟后正文与标签未变")

    # ---- 完成/取消完成 ----
    core.call("todo_set_done", {"id": overdue, "done": True})
    items = core.call("todo_list", {"done": "all", "limit": 50, "offset": 0})["data"]["items"]
    row = [i for i in items if i["id"] == overdue][0]
    assert row["todo"]["done"] is True and row["todo"]["overdue"] is False, row
    core.call("todo_set_done", {"id": overdue, "done": False})
    items = core.call("todo_list", {"done": "all", "limit": 50, "offset": 0})["data"]["items"]
    row = [i for i in items if i["id"] == overdue][0]
    assert row["todo"]["done"] is False, row
    print("  ✅ 完成/取消完成往返 + 徽标同步")

    # ---- 计数 ----
    counts = core.call("entry_counts")["data"]
    print("counts:", {k: counts[k] for k in ("all", "todoOpen", "todoDone")})
    # 未完成:overdue(取消完成)+ future = 2;已完成:plain = 1
    assert counts["todoOpen"] == 2 and counts["todoDone"] == 1, counts
    print("  ✅ todoOpen / todoDone 计数正确")

    # ---- 到点扫描与气泡待播报(宿主确认制) ----
    soon = core.call("todo_create", {"text": "马上要响", "remindAt": now - 1000,
                                     "notifyAt": now - 1000})["data"]["id"]
    time.sleep(17)   # 调度线程 15s 一轮
    pending = core.call("todo_due_pending")["data"]
    print("待播报:", pending)
    assert any(p["id"] == soon for p in pending), "到点后应有待播报气泡"
    core.call("todo_due_mark_shown", {"ids": [soon]})
    pending2 = core.call("todo_due_pending")["data"]
    assert not any(p["id"] == soon for p in pending2), "播报确认后不应再弹"
    print("  ✅ 到点扫描触发 + 气泡播报去重")

    # ---- 清空已完成 / 删除 ----
    core.call("todo_clear_done")
    ids = [i["id"] for i in core.call("todo_list", {"done": "all", "limit": 50, "offset": 0})["data"]["items"]]
    assert plain not in ids, "已完成应被清掉"
    for tid in list(ids):
        assert core.call("todo_delete", {"id": tid})["ok"]
    total = core.call("todo_list", {"done": "all", "limit": 50, "offset": 0})["data"]["total"]
    assert total == 0, total
    con = sqlite3.connect(db)
    orphan = con.execute("SELECT COUNT(*) FROM todo_meta").fetchone()[0]
    con.close()
    assert orphan == 0, f"todo_meta 残留 {orphan} 行孤儿"
    print("  ✅ 清空已完成 / 删除,无孤儿 meta 行")

    shutil.rmtree(data_dir, ignore_errors=True)
    print("\n全部断言通过 ✅")


if __name__ == "__main__":
    main()
