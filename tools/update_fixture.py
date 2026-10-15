"""更新检查验收的环回 fixture 服务(docs/07-auto-update.md §11.2)。

为什么要有它:`--update-flow` 要断言的是"真的发了一次 HTTP 请求并把结果解析回来"。
打真实渠道(GitHub)会让验收依赖外网、限流以及"仓库到底发布过没有";环回明文 HTTP
是本环境唯一能稳定跑通 WinHTTP 全链路的途径(见 `core/nbcore/src/translate/test_server.rs`
头注释同样写明这一点)。

用法:
    # 终端 A:起服务(打印 "PORT <n>",一直服务到被结束)
    python tools/update_fixture.py

    # 终端 B:把检查地址指到某个模式路径,跑验收流(数据目录必须隔离)
    app/build/nextboard.exe --update-flow \
        --data-dir <隔离目录> --update-url http://127.0.0.1:<n>/new

模式(按**路径**给不同响应;一次起服务可连跑多种模式,每种一个 exe 进程):
    /new         GitHub releases/latest 形状,99.0.0
                 → 断言「发现新版本 99.0.0（2026-10-20）」且出现「打开下载页」
    /same        tag = 0.2.0(当前 app 版本)→ 断言「已是最新版本 0.2.0」
                 ⚠️ 版本上调后该模式**仍成立**(远端不会比本地更新),断言不随版本漂移翻转
    /prerelease  tag = 99.0.0-rc.1 且 prerelease=true → 更新的预发布**不该**提示(docs/07 D-e)
    /manifest    自建 manifest 形状 {"version","url","notes"} → 走另一条解析分支
    /notfound    404 → 断言可读错误「渠道还没有发布版本（404）」
    /garbage     200 + 非 JSON → 断言「更新源返回了无法解析的内容」

每收到一个请求会打印一行 `REQ <path>` —— 那正是"客户端确实发出了请求"的证据
(空置态那一支 core 根本不发请求,日志里就不会出现对应行)。
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 下载页链接故意用 .invalid(永远解析不到):验收只断言"URL 被原样取回并交给 openUrl",
# 而 openUrl 在验收模式已降级为日志,不会真去访问它。
RELEASE_PAGE = "https://example.invalid/releases/tag/v99.0.0"

# (状态码, reason, JSON 或原始文本) 按路径给响应;`raw` 非 None 时直接给非 JSON 内容
ROUTES = {
    "/new": (200, "OK", {
        "tag_name": "v99.0.0",
        "name": "Nextboard 99.0.0",
        "html_url": RELEASE_PAGE,
        "published_at": "2026-10-20T09:00:00Z",
        "body": "本次更新:新增检查更新入口。",
        "prerelease": False,
    }),
    "/same": (200, "OK", {
        "tag_name": "v0.2.0",
        "html_url": "https://example.invalid/releases/tag/v0.2.0",
        "published_at": "2026-09-01T00:00:00Z",
        "prerelease": False,
    }),
    "/prerelease": (200, "OK", {
        "tag_name": "v99.0.0-rc.1",
        "html_url": RELEASE_PAGE,
        "published_at": "2026-10-20T09:00:00Z",
        "prerelease": True,
    }),
    "/manifest": (200, "OK", {
        "version": "99.1.0",
        "url": "https://example.invalid/download",
        "notes": "自建渠道的最小 manifest。",
    }),
    "/notfound": (404, "Not Found", {"message": "Not Found"}),
    "/garbage": (200, "OK", None),   # 见 RAW_BODIES
}

RAW_BODIES = {"/garbage": "<html>这不是 JSON</html>"}


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.1 必须给准 Content-Length(否则客户端会等下去)
    protocol_version = "HTTP/1.1"

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler 的命名约定
        route = ROUTES.get(self.path)
        if route is None:
            body = json.dumps({"error": "unknown fixture path", "path": self.path}).encode()
            self._reply(404, "Not Found", body)
            print(f"REQ {self.path} -> 404(未预置的路径)", flush=True)
            return
        status, reason, payload = route
        if payload is None:
            body = RAW_BODIES[self.path].encode()
            content_type = "text/html; charset=utf-8"
        else:
            body = json.dumps(payload, ensure_ascii=False).encode()
            content_type = "application/json"
        self._reply(status, reason, body, content_type)
        print(f"REQ {self.path} -> {status}", flush=True)

    def _reply(self, status, reason, body, content_type="application/json"):
        self.send_response(status, reason)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):   # 静音默认 stderr 访问日志(自己打 REQ 行)
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    # 只解析一次端口,再交给 server 的 sigint 处理(便于反复跑)
    actual = server.server_address[1]
    print(f"PORT {actual}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
