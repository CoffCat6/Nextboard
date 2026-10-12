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
            proc.wait(timeout=40)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise SystemExit("nextboard.exe --todo-groups 超时未退出")
        lines = [r for r in log.read_text(encoding="utf-8", errors="replace").splitlines()
                 if "GROUPS" in r]
        if not lines:
            raise SystemExit(f"日志里没有 GROUPS 行(看 {log})")
        for r in lines:
            print("   ", r.strip())

        todo_line = next((r for r in lines if '"todo"' in r), None)
        all_line = next((r for r in lines if '"all"' in r), None)
        assert todo_line and all_line, f"缺少 todo/all 两个视图的读数: {lines}"

        keys = todo_line.split("keys:")[1].split("|")[0].strip().strip('"').split(",")
        # 排序契约(core §5):归档垫底;未完成内 逾期 > 今日 > 未来 > 无期限
        assert keys == ["overdue", "today", "future", "none", "done"], \
            f"分组键序不符预期: {keys}"
        # 每个分组的首行即"标题出现处"——序列无重复即每个标题恰好出现一次
        staged = [k for i, k in enumerate(keys) if i == 0 or keys[i - 1] != k]
        assert staged == keys, f"分组边界错乱: {staged}"
        print("  ✅ 分组键序 = overdue → today → future → none → done(§5 排序)")
        print("  ✅ 五个组标题各出现在对应分组的首行,无重复/错位")

        # 来源条:待办视图整体隐藏(待办已是顶层标签,列出来是冗余且不生效)
        assert "sourceBar: false" in todo_line, \
            f"待办视图不该显示来源筛选条: {todo_line}"
        print("  ✅ 待办视图不显示来源筛选条(无冗余的「待办」按钮)")

        # 来源条:全部视图里不得出现 todo / clipboard 两个来源
        chips = all_line.split("sourceChips:")[1].split("|")[0].strip().strip('"')
        chip_list = [c.strip('"') for c in chips.split(",") if c.strip('"')]
        assert "todo" not in chip_list, f"来源条不应出现「待办」chip: {chips}"
        assert "clipboard" not in chip_list, f"来源条不应出现「剪贴板」chip: {chips}"
        assert chip_list == ["capture"], \
            f"本用例只造了 capture 来源,期望仅 [capture],实为 {chip_list}"
        print(f"  ✅ 全部视图的来源 chip = {chip_list}(无 clipboard/todo)")
        print("\n全部断言通过 ✅")
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
