//! 测试专用：极简环回 HTTP 服务端（明文），供 `http.rs` 与各 provider 的端到端测试复用。
//!
//! 为什么自建而非引依赖：本仓零新增 Cargo 依赖（决策 D7），`std::net` 已足够。
//! 为什么需要它：本开发环境的 TLS 被环境策略阻断（无法访问任何真实 HTTPS 接口），
//! 但**环回明文 HTTP 可用**——因此这是唯一能在本环境真实执行 HTTP 全链路（含全部
//! `unsafe` WinHTTP 代码）的途径，比只测纯函数解析更有价值。

use std::io::{Read, Write};
use std::net::TcpListener;
use std::time::{Duration, Instant};

/// 等待客户端连接的时限：超时即 panic（可读失败），而不是让 `join()` 永久挂住。
const ACCEPT_TIMEOUT: Duration = Duration::from_secs(10);
/// 读取单个请求的总时限：**必须存在**——否则"客户端发的字节数少于自己声明的
/// Content-Length"会让服务端线程永久阻塞在 `read` 上，表现为 `cargo test` 挂死而非失败。
const READ_TIMEOUT: Duration = Duration::from_secs(10);

/// 记账结果：服务端实际收到的原始请求文本。
pub struct Recorded {
    pub raw: String,
}

impl Recorded {
    /// 请求行（首行）。
    pub fn request_line(&self) -> &str {
        self.raw.lines().next().unwrap_or_default()
    }

    /// 请求体（`\r\n\r\n` 之后的部分）。
    pub fn body(&self) -> &str {
        match find_subslice(self.raw.as_bytes(), b"\r\n\r\n") {
            Some(index) => &self.raw[index + 4..],
            None => "",
        }
    }

    /// 某请求头的值（大小写不敏感）；不存在返回 None。
    pub fn header(&self, name: &str) -> Option<String> {
        self.raw.lines().find_map(|line| {
            let (key, value) = line.split_once(':')?;
            key.trim()
                .eq_ignore_ascii_case(name)
                .then(|| value.trim().to_owned())
        })
    }
}

/// 子切片查找（避免为测试引入 memchr）。
pub fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

/// 启动单次应答的服务端，返回 `(端口, 句柄)`；`join` 后得到收到的请求。
///
/// `response` 为完整响应文本（含状态行与空行）；用 `Box::leak` 传长响应即可。
pub fn spawn(response: &'static str) -> (u16, std::thread::JoinHandle<Recorded>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("绑定环回端口失败");
    let port = listener.local_addr().expect("取本地地址失败").port();
    listener
        .set_nonblocking(true)
        .expect("设置非阻塞失败（用于 accept 超时）");
    let handle = std::thread::spawn(move || {
        let (mut sock, _) = accept_within(&listener);
        let raw = read_full_request(&mut sock);
        let _ = sock.write_all(response.as_bytes());
        let _ = sock.flush();
        Recorded { raw }
    });
    (port, handle)
}

/// 读尽一个完整请求（请求头 + 按 `Content-Length` 声明的请求体）并返回其原文。
///
/// **必须读尽再应答**：若只读请求行就回响应并关闭连接，而客户端仍在发送请求体，
/// 连接会被 RST 打断、响应丢失，客户端报 `0x80072F78`（ERROR_HTTP_INVALID_SERVER_RESPONSE）。
/// 这个坑在伪造代理上真实踩到过，故两处共用同一实现。
fn read_full_request(sock: &mut std::net::TcpStream) -> String {
    // **必须显式设回阻塞**：`accept` 出的 socket 在 Windows 上会继承监听端的非阻塞模式
    // （accept 超时逻辑把 listener 设成了 nonblocking），此时 `read` 会在数据未到齐时
    // 立即返回 WouldBlock 而触发下面的 panic；且读超时对非阻塞 socket 不生效。
    // 抽成独立步骤而非依赖平台行为差异，否则大 body 用例会随机失败。
    sock.set_nonblocking(false).expect("设置阻塞模式失败");
    // 读超时：把"客户端未按 Content-Length 发满"从挂死变成可读 panic
    sock.set_read_timeout(Some(READ_TIMEOUT))
        .expect("设置读超时失败");
    let mut raw: Vec<u8> = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = match sock.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(err) => panic!(
                "读取请求失败（可能是客户端未发满声明的字节数，或读超时 {READ_TIMEOUT:?}）: {err}"
            ),
        };
        raw.extend_from_slice(&buf[..n]);
        if let Some(head_end) = find_subslice(&raw, b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&raw[..head_end]).into_owned();
            let content_length = head
                .lines()
                .find_map(|line| {
                    let (name, value) = line.split_once(':')?;
                    name.eq_ignore_ascii_case("content-length")
                        .then(|| value.trim().parse::<usize>().ok())?
                })
                .unwrap_or(0);
            if raw.len() >= head_end + 4 + content_length {
                break;
            }
        }
    }
    String::from_utf8_lossy(&raw).into_owned()
}

/// 在限期内接受一个连接；超时 panic 并说明原因（而非无限阻塞）。
fn accept_within(listener: &TcpListener) -> (std::net::TcpStream, std::net::SocketAddr) {
    let deadline = Instant::now() + ACCEPT_TIMEOUT;
    loop {
        match listener.accept() {
            Ok(pair) => return pair,
            Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
                if Instant::now() >= deadline {
                    panic!("等待客户端连接超时（{ACCEPT_TIMEOUT:?}），测试未按预期发起请求");
                }
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(err) => panic!("接受连接失败: {err}"),
        }
    }
}

/// 把任意长度的响应文本转成 `&'static str`（测试进程内使用，泄漏可忽略）。
pub fn leaked(text: String) -> &'static str {
    Box::leak(text.into_boxed_str())
}

/// 伪造代理：接受一个连接，读一行请求行，回一个合法 HTTP 响应，并把请求行交回。
///
/// 用于**正向**证明"请求确实经过配置的代理"——比"用死代理看是否失败"更强，
/// 且不依赖外网（目标可用任意不可达域名，请求由伪造代理直接应答）。
pub fn spawn_fake_proxy() -> (u16, std::thread::JoinHandle<String>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("绑定代理端口失败");
    let port = listener.local_addr().expect("取地址失败").port();
    listener.set_nonblocking(true).expect("设置非阻塞失败");
    let handle = std::thread::spawn(move || {
        let (mut sock, _) = accept_within(&listener);
        // 读尽请求再应答：若只读请求行就关闭，未读尽的请求体会让连接被 RST，
        // 客户端报 0x80072F78（ERROR_HTTP_INVALID_SERVER_RESPONSE）——已实测踩到过。
        let raw = read_full_request(&mut sock);
        let request_line = raw.lines().next().unwrap_or_default().to_owned();

        // CONNECT 隧道(2026-11-07):ureq 对 http:// 目标经 HTTP 代理也可能走
        // CONNECT(而非 WinHTTP 的正向转发绝对形式)。握手 → 隧道内读真实请求
        // → 记录**隧道内**请求行 → 在隧道里应答。返回值语义不变("客户端真实
        // 请求的请求行"),外层断言按平台分别匹配绝对/原始形式。
        if request_line.starts_with("CONNECT") {
            let _ = sock.write_all(b"HTTP/1.1 200 Connection established\r\n\r\n");
            let _ = sock.flush();
            let inner_raw = read_full_request(&mut sock);
            let inner_line = inner_raw.lines().next().unwrap_or_default().to_owned();
            let body = r#"{"via":"proxy"}"#;
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                body.len()
            );
            let _ = sock.write_all(response.as_bytes());
            let _ = sock.flush();
            return inner_line;
        }

        let body = r#"{"via":"proxy"}"#;
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        let _ = sock.write_all(response.as_bytes());
        let _ = sock.flush();
        request_line
    });
    (port, handle)
}

/// 构造 JSON 响应（自动补 `Content-Length`，连接随即关闭）。
pub fn json_response(status_line: &str, body: &str) -> &'static str {
    leaked(format!(
        "{status_line}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    ))
}

/// SOCKS5 测试服务端的句柄：取端口、断言收到握手。
pub struct Socks5Server {
    port: u16,
    /// 服务端线程收到的首段字节（空 = 客户端从未连接）。
    seen: std::sync::mpsc::Receiver<Vec<u8>>,
}

impl Socks5Server {
    pub fn port(&self) -> u16 {
        self.port
    }

    /// 断言客户端发来的是合法 SOCKS5 无认证握手（`05 01 00`）。
    pub fn assert_handshake_seen(&self) -> bool {
        let seen = self
            .seen
            .recv_timeout(Duration::from_secs(10))
            .unwrap_or_default();
        seen.starts_with(&[0x05, 0x01, 0x00])
    }
}

/// 假 SOCKS5 服务端（RFC1928 无认证）。
///
/// 行为：收到客户端的 SOCKS5 握手后按协议应答（选 no-auth），读取 CONNECT 请求并确认
/// 地址类型合法，回"隧道已建立"，然后**直接把一段预置的 HTTP 200 响应写给客户端**——
/// 不做真实转发。足以让 `socks.rs` 的全链路（握手 → 请求 → 读响应）真实执行。
///
/// **为什么不做真实转发**：转发需要本测试端再作为客户端连向"目标"，而目标是测试自己
/// 起的第二个服务端——那会让两个 accept 互相等待（实测死锁 10s 后连锁失败）。
/// 握手判据（`assert_handshake_seen`）已足以证明"请求走了 SOCKS5 而非直连"，
/// 隧道内的 HTTP 行为已由 `http.rs` 的直连端到端测试覆盖，无需在本层重复。
///
/// **正向判据**：`assert_handshake_seen()` 证明客户端发来的是合法 SOCKS5 握手。
pub fn spawn_socks5_proxy() -> Socks5Server {
    use std::io::Write;

    let listener = TcpListener::bind("127.0.0.1:0").expect("绑定 SOCKS5 端口失败");
    let port = listener.local_addr().expect("取地址失败").port();
    listener.set_nonblocking(true).expect("设置非阻塞失败");
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let (mut sock, _) = accept_within(&listener);
        sock.set_nonblocking(false).expect("设回阻塞失败");
        sock.set_read_timeout(Some(READ_TIMEOUT))
            .expect("设置读超时失败");

        // 1) 握手：客户端应为 05 <nmethods> <methods...>；无认证方法为 0x00
        let mut greeting = [0u8; 32];
        let n = sock.read(&mut greeting).unwrap_or(0);
        let _ = tx.send(greeting[..n].to_vec());
        if n < 3 || greeting[0] != 0x05 {
            return; // 非法握手：让测试断言失败，不再继续
        }
        let _ = sock.write_all(&[0x05, 0x00]); // 选 no-auth

        // 2) CONNECT 请求：VER CMD RSV ATYP <addr> <port>；只校验地址类型合法
        //    （IPv4/域名/IPv6），不解析地址内容——本服务端不做真实转发
        let mut request = [0u8; 262];
        let _ = sock.read(&mut request);
        if request.len() < 5 || request[1] != 0x01 {
            return; // 只支持 CONNECT
        }
        let valid_atyp = matches!(request.get(3), Some(0x01) | Some(0x03) | Some(0x04));
        if !valid_atyp {
            return;
        }
        let _ = sock.write_all(&[
            0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 80, // 成功应答（绑定地址占位）
        ]);

        // 3) 隧道内应答：预置的 HTTP 200 响应（代表目标服务端经隧道返回的内容）。
        //    客户端的 HTTP 请求由**独立线程**读取：请求与响应谁先到不确定，
        //    若在同一循环里"先读尽再答"会互相等待（实测死锁 10s）。
        //    读取线程只消费字节（防 RST），内容不参与断言——握手才是判据。
        let mut tunnel = sock.try_clone().expect("克隆隧道 socket 失败");
        std::thread::spawn(move || {
            let _ = tunnel.set_read_timeout(Some(Duration::from_millis(500)));
            let mut drain = [0u8; 4096];
            loop {
                match tunnel.read(&mut drain) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {}
                }
            }
        });

        let body = r#"{"via":"socks5"}"#;
        let reply = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        let _ = sock.write_all(reply.as_bytes());
        let _ = sock.flush();
        // 给客户端留出读取时间再关闭，避免响应在途中被 RST 吞掉
        std::thread::sleep(Duration::from_millis(200));
    });
    Socks5Server { port, seen: rx }
}
