"""待办智能分组(§6.3)验证:播种各分组 → 读回分组键 → 断言分界点。

用法(前置:cargo build --release;并用同一 --data-dir 先跑过应用或本脚本自建):
    python tools/verify_todo_groups.py

思路:
  1. 用 nbcore.dll 直接在**独立数据目录**造五种分组的待办
     (逾期 / 今天 / 以后 / 无期限 / 已完成);
  2. 启动 nextboard.exe --todo-groups --data-dir <同目录>,它读回各行分组键;
  3. 断言键序符合 core §5 排序:overdue → today → future → none → done,
     且每个分组的**首行**即"标题出现处"。
"""
import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DLL = REPO_ROOT / "core" / "target" / "release" / "nbcore.dll"
EXE = REPO_ROOT / "app" / "build" / "nextboard.exe"

DAY = 24 * 60 * 60 * 1000


def end_of_today_ms():
    """今天 23:59:59.999 本地毫秒(与 core end_of_today_ms 同口径)。"""
    import datetime
    now = datetime.datetime.now()
    end = now.replace(hour=23, minute=59, second=59, microsecond=999000)
    return int(end.timestamp() * 1000)


class Core:
    def __init__(self, data_dir):
        if not DLL.is_file():
            raise SystemExit(f"找不到 {DLL}\n请先在 core/ 执行 cargo build --release")
        self.dll = ctypes.CDLL(str(DLL))
        self.dll.nb_init.restype = ctypes.c_int
        self.dll.nb_init.argtypes = [ctypes.c_char_p]
        self.dll.nb_call.restype = ctypes.c_int
        self.dll.nb_call.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                     ctypes.POINTER(ctypes.c_void_p)]
        self.dll.nb_free_string.restype = None
        self.dll.nb_free_string.argtypes = [ctypes.c_void_p]
        # ⚠️ 配置键是 snake_case 的 data_dir(与 verify_todo.py 同口径)。
        cfg = json.dumps({"data_dir": data_dir, "notify_hwnd": 0}).encode()
        rc = self.dll.nb_init(cfg)
        if rc != 0:
            raise SystemExit(f"nb_init 失败: {rc}")

    def call(self, method, req=None):
        out = ctypes.c_void_p()
        arg = json.dumps(req or {}).encode("utf-8")
        rc = self.dll.nb_call(method.encode("utf-8"), arg, ctypes.byref(out))
        if not out.value:
            raise SystemExit(f"nb_call {method} 无响应(rc={rc})")
        try:
            return json.loads(ctypes.string_at(out.value).decode("utf-8"))
        finally:
            self.dll.nb_free_string(out)


def main():
    data_dir = tempfile.mkdtemp(prefix="nb_groups_")
    print("数据目录:", data_dir)
    try:
        core = Core(data_dir)
        now = int(time.time() * 1000)
        eot = end_of_today_ms()

        def mk(text, due):
            resp = core.call("todo_create", {"text": text, "dueAt": due})
            assert resp.get("ok"), resp
            return resp["data"]["id"]

        # 有意**乱序**创建:验证分组键来自排序而不是插入顺序
        mk("无期限的", None)
        mk("今天到期的", eot - 1000)
        mk("已逾期的", now - DAY)
        mk("以后的", now + 3 * DAY)
        done_id = mk("已完成的", now - 2 * DAY)
        core.call("todo_set_done", {"id": done_id, "done": True})

        # 再造一条 capture 来源的条目:用来验证"来源条应出现 capture、
        # 不出现 clipboard/todo"。save_text_entry 可指定 source。
        resp = core.call("save_text_entry",
                         {"text": "截图识别结果", "source": "capture"})
        assert resp.get("ok"), resp

        # 让独立进程用同一数据目录读回分组键
        log = Path(data_dir) / "nextboard.log"
        proc = subprocess.Popen([str(EXE), "--todo-groups", "--data-dir", data_dir])
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise SystemExit("nextboard.exe --todo-groups 超时未退出")
        # ⚠️ 日志是追加写的,进程刚退出时最后一两行可能还没落盘 ——
        #    轮询等 FILTER 行出现再解析,否则会误报"缺少读数"(实测踩到)。
        deadline = time.time() + 10
        text = ""
        while time.time() < deadline:
            text = log.read_text(encoding="utf-8", errors="replace")
            if "FILTER" in text:
                break
            time.sleep(0.3)
        lines = [r for r in text.splitlines() if "GROUPS" in r or "FILTER" in r]
        if not lines:
            raise SystemExit(f"日志里没有 GROUPS/FILTER 行(看 {log})")
        for r in lines:
            print("   ", r.strip())

        todo_line = next((r for r in lines if '"todo"' in r), None)
        all_line = next((r for r in lines if '"all"' in r), None)
        filter_line = next((r for r in lines if "FILTER" in r), None)
        assert todo_line and all_line and filter_line, \
            f"缺少 todo/all/FILTER 读数: {lines}"

        keys = todo_line.split("keys:")[1].split("|")[0].strip().strip('"').split(",")
        # 排序契约(core §5):归档垫底;未完成内 逾期 > 今日 > 未来 > 无期限
        assert keys == ["overdue", "today", "future", "none", "done"], \
            f"分组键序不符预期: {keys}"
        # 每个分组的首行即"标题出现处"——序列无重复即每个标题恰好出现一次
        staged = [k for i, k in enumerate(keys) if i == 0 or keys[i - 1] != k]
        assert staged == keys, f"分组边界错乱: {staged}"
        print("  ✅ 分组键序 = overdue → today → future → none → done(§5 排序)")
        print("  ✅ 五个组标题各出现在对应分组的首行,无重复/错位")

        # 来源筛选已并入筛选浮层(方案 A):校验"待办视图不列来源项 +
        # 全部视图只列有数据的来源(不含 clipboard/todo/零计数)"。
        assert "sourceChips:" in todo_line, f"待办视图缺 sourceChips 读数: {todo_line}"
        todo_chips = todo_line.split("sourceChips:")[1].strip().strip('"')
        assert todo_chips == "", f"待办视图不该列来源项: {todo_chips!r}"
        print("  ✅ 待办视图不列来源项(无冗余的「待办」按钮)")

        chips = all_line.split("sourceChips:")[1].split("|")[0].strip().strip('"')
        chip_list = [c.strip('"') for c in chips.split(",") if c.strip('"')]
        assert "todo" not in chip_list, f"来源项不应出现「待办」: {chips}"
        assert "clipboard" not in chip_list, f"来源项不应出现「剪贴板」: {chips}"
        assert chip_list == ["capture"], \
            f"本用例只造了 capture 来源,期望仅 [capture],实为 {chip_list}"
        print(f"  ✅ 全部视图的来源项 = {chip_list}(无 clipboard/todo)")

        # 筛选浮层:能打开,且初始无生效筛选(角标口径)
        assert "FILTER popupOpen: true" in filter_line, \
            f"筛选浮层未能打开: {filter_line}"
        assert "active: false" in filter_line and "count: 0" in filter_line, \
            f"初始不该有生效筛选: {filter_line}"
        print("  ✅ 筛选浮层可打开,初始无生效条件(角标 0)")
        # 程序化打开(鼠标从未进入浮层)后 ~1s 再读,**不应**被自动收回:
        # 自动收回要求"鼠标曾进入过浮层",纯点开没进去不触发 —— 否则用户
        # 点开浮层去看选项、手在别处,浮层立刻被关,选项都来不及看。
        assert "FILTER autoHideKept popupOpen: true" in text, \
            "未进入浮层时不该自动收回(鼠标没进去过,谈何'移出')"
        print("  ✅ 仅打开未进入时不自动收回(需先进入过浮层)")

        # 设「截图」来源后:角标 1、activeSource=capture、列表收窄到 capture 总数
        set_line = next((r for r in lines if "FILTER afterSet" in r), None)
        clear_line = next((r for r in lines if "FILTER afterClear" in r), None)
        assert set_line and clear_line, f"缺少筛选生效/清除读数: {lines}"
        print("   ", set_line.strip())
        print("   ", clear_line.strip())
        assert "active: true" in set_line and "count: 1" in set_line, \
            f"设来源后应 active 且角标=1: {set_line}"
        assert "activeSource: \"capture\"" in set_line, \
            f"生效的来源应为 capture: {set_line}"
        set_total = int(set_line.split("listTotal:")[1].split("|")[0])
        # capture 条目 = 截图 1(5 个 todolist 来源的),list_entries(source=capture) 返回其总数
        assert set_total == 1, f"来源筛选后列表应有 1 条 capture: {set_total}"
        print("  ✅ 设「截图」来源 → 角标 1 + 列表收窄到 capture 总数")

        # 清除后回到未筛选态
        assert "active: false" in clear_line and "count: 0" in clear_line, \
            f"清除后应回到未筛选态: {clear_line}"
        print("  ✅ 清除来源 → 回到未筛选态")
        print("\n全部断言通过 ✅")
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
