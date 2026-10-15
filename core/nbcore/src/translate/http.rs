//! WinHTTP 安全封装（Phase 7 决策 D7）。
//!
//! 为什么自写：通用 HTTP 客户端在跟随系统代理与证书存储上行为不一致,且 SOCKS5
//! 支持需另建传输层;故采用 `windows` crate 的 `Win32_Networking_WinHttp`——零新增
//! 依赖、跟随系统代理与证书存储、`winhttp.dll` 为系统组件无需分发,SOCKS5 场景
//! 以 ureq 独立补位（见 socks.rs,决策 D17）。
//!
//! 设计约束（第一代 docs/phase7.md §3.1,约束继续生效）：
//! - 全部 `unsafe` 收敛在本文件，上层 provider 只见安全 API；
//! - 句柄经 [`WinHttpHandle`] RAII 包装，`Drop` 中统一关闭，避免泄漏；
//! - 每个 `unsafe` 块附 `SAFETY:` 注释说明不变量（编码规范 §1.1）。
//!
//! 本层只做「发一次请求、取回状态码与响应体」，业务判定（哪些状态码算失败、
//! 如何解析 JSON）一律交给调用方。

use crate::error::{AppError, AppResult};

/// 响应体上限：翻译接口的正常响应远小于此，超过即视为异常并中止（防内存被拖爆）。
/// 两条传输（WinHTTP / ureq）共用，保持行为一致。
pub(crate) const MAX_RESPONSE_BYTES: usize = 4 * 1024 * 1024;

/// SOCKS5 传输（ureq 实现）：WinHTTP 实测不支持 SOCKS5（docs/phase8-proxy.md §2.1），
/// 故该场景独立成模块，由 `send()` 按配置分派。
/// 文件放在 `translate/socks.rs`（与其它传输相关模块同级），用 `path` 指回本模块下。
#[path = "socks.rs"]
pub(crate) mod socks;

/// 单次读取的兜底块大小（`WinHttpQueryDataAvailable` 返回 0 时用于退出循环）。
const READ_CHUNK: usize = 8 * 1024;

/// HTTP 方法（当前只需这两种：翻译接口均为 POST，探活可能用 GET）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HttpMethod {
    Get,
    Post,
}

impl HttpMethod {
    fn as_str(self) -> &'static str {
        match self {
            HttpMethod::Get => "GET",
            HttpMethod::Post => "POST",
        }
    }
}

/// 一次 HTTP 请求的描述（由 provider 构造，本层执行）。
#[derive(Debug, Clone)]
pub struct HttpRequest {
    pub method: HttpMethod,
    pub url: String,
    /// 附加请求头；值中的非 ASCII 会被 UTF-8 编码后按字节传递。
    pub headers: Vec<(String, String)>,
    /// 请求体；`GET` 时须为空。
    pub body: String,
    /// 超时（秒），同时作用于解析 / 连接 / 发送 / 接收四段。
    pub timeout_secs: i64,
    /// 代理配置；`None` = 跟随系统（PAC / IE 设置）。
    pub proxy: Option<ProxyConfig>,
}

/// 代理配置（Phase 8，docs/phase8-proxy.md §3.2）。
///
/// 实测结论（§2.1）：**WinHTTP 不支持 SOCKS5**——`socks5://`、`socks5=`、`socks=` 三种
/// 前缀都会被静默忽略而直连。因此 SOCKS5 由独立的 ureq 传输承担（见 [`send`] 的分派）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProxyConfig {
    /// 直连，不走任何代理（也忽略系统代理）。
    None,
    /// 跟随系统设置（PAC / IE 设置）。由 WinHTTP 的自动发现承担。
    System,
    /// 手动指定代理。
    Manual {
        endpoint: ProxyEndpoint,
        bypass: String,
    },
}

/// 代理类型。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProxyKind {
    /// HTTP 代理（HTTPS 走 CONNECT 隧道，加密强度与 SOCKS5 等价）。
    Http,
    /// SOCKS5 代理（RFC1928）。
    Socks5,
}

/// 手动代理的连接目标与认证。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProxyEndpoint {
    pub kind: ProxyKind,
    pub host: String,
    pub port: u16,
    /// 可选的用户名/密码认证（HTTP 走 WinHttpSetCredentials，SOCKS5 走 RFC1929）。
    pub auth: Option<ProxyAuth>,
}

/// 代理认证凭据。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProxyAuth {
    pub username: String,
    pub password: String,
}

impl ProxyConfig {
    /// 从设置的字符串字段解析（`mode` + 手动配置）。非法值回落为跟随系统。
    pub fn parse(mode: &str, manual: Option<&crate::settings::ManualProxy>) -> Self {
        match mode.trim() {
            "none" => Self::None,
            "manual" => {
                let Some(manual) = manual else {
                    return Self::System;
                };
                match crate::settings::validate_proxy_manual(manual) {
                    Ok(endpoint) => Self::Manual {
                        endpoint,
                        bypass: manual.bypass.trim().to_owned(),
                    },
                    // 配置不合法时回落系统代理并让校验层报错，不在传输层静默兜底
                    Err(_) => Self::System,
                }
            }
            _ => Self::System,
        }
    }
}

/// 一次 HTTP 响应的结果（进程内部类型，不跨 IPC，故无需 Serialize）。
#[derive(Debug, Clone)]
pub struct HttpResponse {
    pub status: u16,
    pub body: String,
}

/// 解析后的 URL 组成部分。
#[derive(Debug, Clone, PartialEq, Eq)]
struct Url {
    /// `https` 为 true。
    secure: bool,
    host: String,
    port: u16,
    /// 含查询串的路径，如 `/v1/chat/completions?x=1`；空路径归一为 `/`。
    target: String,
}

/// 解析 `http(s)://host[:port]/path?query`。
///
/// 只用最小编译单元自解析，不调 `WinHttpCrackUrl`（后者需要构造并维护
/// `URL_COMPONENTS` 的多个裸指针字段，unsafe 面积更大且难测）。
fn parse_url(url: &str) -> AppResult<Url> {
    let url = url.trim();
    let (secure, rest) = if let Some(rest) = url.strip_prefix("https://") {
        (true, rest)
    } else if let Some(rest) = url.strip_prefix("http://") {
        (false, rest)
    } else {
        return Err(AppError::InvalidInput(
            "接口地址必须以 http:// 或 https:// 开头".into(),
        ));
    };
    // 主机部分到第一个 '/' 为止；无路径时用默认路径
    let (authority, target) = match rest.find('/') {
        Some(index) => (&rest[..index], &rest[index..]),
        None => (rest, "/"),
    };
    if authority.is_empty() {
        return Err(AppError::InvalidInput("接口地址缺少主机名".into()));
    }
    // 主机可带端口；注意 IPv6 字面量不在支持范围（翻译服务不会用到）
    let (host, port) = match authority.rsplit_once(':') {
        Some((host, port_text)) => {
            let port: u16 = port_text
                .parse()
                .map_err(|_| AppError::InvalidInput(format!("端口号无效: {port_text}")))?;
            (host, port)
        }
        None => (authority, if secure { 443 } else { 80 }),
    };
    if host.is_empty() {
        return Err(AppError::InvalidInput("接口地址缺少主机名".into()));
    }
    let target = if target.is_empty() { "/" } else { target };
    Ok(Url {
        secure,
        host: host.to_owned(),
        port,
        target: target.to_owned(),
    })
}

/// WinHTTP 句柄的 RAII 包装：非空即有效，`Drop` 时关闭。
struct WinHttpHandle(*mut core::ffi::c_void);

impl WinHttpHandle {
    /// 包住一个 API 返回的句柄；为 NULL 时返回携带 `GetLastError` 的错误。
    fn new(raw: *mut core::ffi::c_void, context: &str) -> AppResult<Self> {
        if raw.is_null() {
            Err(last_error(context))
        } else {
            Ok(Self(raw))
        }
    }

    fn raw(&self) -> *mut core::ffi::c_void {
        self.0
    }
}

impl Drop for WinHttpHandle {
    fn drop(&mut self) {
        if self.0.is_null() {
            return;
        }
        // SAFETY：句柄由 WinHttpOpen/Connect/OpenRequest 返回且经非空校验，本包装独占其
        // 所有权（不 Clone），Drop 只执行一次，故不会重复关闭同一句柄。
        unsafe {
            let _ = windows::Win32::Networking::WinHttp::WinHttpCloseHandle(self.0);
        }
    }
}

/// 把 Win32 线程错误码转成可读的 `AppError`（不含请求内容，避免泄露密钥/原文）。
fn last_error(context: &str) -> AppError {
    let err = windows::core::Error::from_thread();
    AppError::Translate(format!("{context}失败：{err}"))
}

/// 转为以 NUL 结尾的宽字符串（WinHTTP 的 `PCWSTR` 参数要求）。
fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

/// 发送请求并读回完整响应体。
///
/// 失败情形统一映射为 `AppError::Translate`（含可读原因），由命令层转给前端提示。
pub fn send(req: &HttpRequest) -> AppResult<HttpResponse> {
    // SOCKS5 由 ureq 传输承担（WinHTTP 实测不支持，docs/phase8-proxy.md §2.1）；
    // 其余（直连 / 系统代理 / HTTP 代理）走既有 WinHTTP 路径。
    match req.proxy.as_ref() {
        Some(ProxyConfig::Manual { endpoint, .. }) if endpoint.kind == ProxyKind::Socks5 => {
            socks::send(req, endpoint)
        }
        _ => winhttp::send(req),
    }
}

/// WinHTTP 实现：直连 / 系统代理 / HTTP 代理（含认证与绕过列表）。
mod winhttp {
    use windows::core::PCWSTR;
    use windows::Win32::Networking::WinHttp::{
        WinHttpConnect, WinHttpOpen, WinHttpOpenRequest, WinHttpQueryDataAvailable,
        WinHttpQueryHeaders, WinHttpReadData, WinHttpReceiveResponse, WinHttpSendRequest,
        WinHttpSetCredentials, WinHttpSetTimeouts, INTERNET_DEFAULT_HTTPS_PORT,
        INTERNET_DEFAULT_HTTP_PORT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_ACCESS_TYPE_NAMED_PROXY, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_AUTH_SCHEME_BASIC,
        WINHTTP_AUTH_TARGET_PROXY, WINHTTP_FLAG_SECURE, WINHTTP_OPEN_REQUEST_FLAGS,
        WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_QUERY_STATUS_CODE,
    };

    use super::*;

    pub(super) fn send(req: &HttpRequest) -> AppResult<HttpResponse> {
        let url = parse_url(&req.url)?;
        if req.method == HttpMethod::Get && !req.body.is_empty() {
            return Err(AppError::Internal("GET 请求不应携带请求体".into()));
        }
        let timeout = req.timeout_secs.clamp(5, 120) as i32 * 1000;

        // 会话三态（docs/phase8-proxy.md §3.3）：
        //   None   → NO_PROXY（显式直连，忽略系统代理）
        //   System → AUTOMATIC_PROXY（PAC / IE 设置）
        //   Manual → NAMED_PROXY（HTTP 代理；SOCKS5 在 send() 已分派给 ureq）
        let (manual, bypass) = match req.proxy.as_ref() {
            Some(ProxyConfig::None) => (None, None),
            Some(ProxyConfig::Manual { endpoint, bypass }) => {
                (Some(endpoint), Some(bypass.as_str()))
            }
            _ => (None, None),
        };
        // UA 会出现在代理与服务端日志里,产品名必须对得上(第一代叫 clipboard-plus,
        // 本仓已是 Nextboard;docs/07-auto-update.md D-g)。
        let agent = wide(concat!("nextboard/", env!("CARGO_PKG_VERSION")));
        let proxy_w = manual.map(|endpoint| wide(&proxy_string(endpoint)));
        let bypass_w = bypass.filter(|_| manual.is_some()).map(wide);
        let access_type = match (&manual, req.proxy.as_ref()) {
            (Some(_), _) => WINHTTP_ACCESS_TYPE_NAMED_PROXY,
            (_, Some(ProxyConfig::None)) => WINHTTP_ACCESS_TYPE_NO_PROXY,
            _ => WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        };
        let session = {
            // SAFETY：agent/proxy/bypass 的宽字符串在本作用域内存活且以 NUL 结尾；
            // bypass 只在 NAMED_PROXY 下才有意义，其余访问类型传空指针。
            let raw = unsafe {
                WinHttpOpen(
                    PCWSTR(agent.as_ptr()),
                    access_type,
                    proxy_w
                        .as_ref()
                        .map_or(PCWSTR::null(), |text| PCWSTR(text.as_ptr())),
                    bypass_w
                        .as_ref()
                        .map_or(PCWSTR::null(), |text| PCWSTR(text.as_ptr())),
                    0,
                )
            };
            WinHttpHandle::new(raw, "创建 HTTP 会话")?
        };

        // 四段超时统一取设置值（解析 / 连接 / 发送 / 接收）
        // SAFETY：session 为有效会话句柄，超时参数为纯数值。
        unsafe {
            WinHttpSetTimeouts(session.raw(), timeout, timeout, timeout, timeout)
                .map_err(|err| AppError::Translate(format!("设置请求超时失败：{err}")))?;
        }

        let host = wide(&url.host);
        let port = if url.port == 0 {
            if url.secure {
                INTERNET_DEFAULT_HTTPS_PORT
            } else {
                INTERNET_DEFAULT_HTTP_PORT
            }
        } else {
            url.port
        };
        // SAFETY：session 有效；host 宽字符串以 NUL 结尾且在本作用域内存活。
        let connect = {
            let raw = unsafe { WinHttpConnect(session.raw(), PCWSTR(host.as_ptr()), port, 0) };
            WinHttpHandle::new(raw, "连接服务器")?
        };

        let verb = wide(req.method.as_str());
        let target = wide(&url.target);
        let flags = WINHTTP_OPEN_REQUEST_FLAGS(if url.secure { WINHTTP_FLAG_SECURE.0 } else { 0 });
        // SAFETY：connect 有效；verb/target 宽字符串以 NUL 结尾且在本作用域内存活；
        // 接受类型传空指针表示"任意类型"，与 WinHTTP 文档一致。
        let request = {
            let raw = unsafe {
                WinHttpOpenRequest(
                    connect.raw(),
                    PCWSTR(verb.as_ptr()),
                    PCWSTR(target.as_ptr()),
                    PCWSTR::null(),
                    PCWSTR::null(),
                    std::ptr::null(),
                    flags,
                )
            };
            WinHttpHandle::new(raw, "创建请求")?
        };

        // 请求头：每行以 CRLF 结尾，整块也须以 CRLF 收尾。
        // 末尾额外补一个 NUL 以满足"长度显式给出时字符串仍须以 NUL 结束"的要求，
        // 但传给 API 的切片长度**不含**该 NUL（避免把 NUL 计入头部字符数）。
        let header_text: String = req
            .headers
            .iter()
            .map(|(name, value)| format!("{name}: {value}\r\n"))
            .collect();
        let mut header_wide = wide(&header_text);
        if req.headers.is_empty() {
            header_wide.clear();
        }
        let header_slice = if header_wide.is_empty() {
            None
        } else {
            // 去掉末尾 NUL 后再传；底层指针仍指向以 NUL 结尾的缓冲区
            Some(&header_wide[..header_wide.len() - 1])
        };
        let body_bytes = req.body.as_bytes();
        let (body_ptr, body_len) = if body_bytes.is_empty() {
            (std::ptr::null(), 0u32)
        } else {
            (
                body_bytes.as_ptr() as *const core::ffi::c_void,
                body_bytes.len() as u32,
            )
        };
        // SAFETY：request 有效；header_slice 指向以 NUL 结尾的宽字符缓冲区；body_ptr 指向
        // body_bytes 的起始处（为空时传空指针且长度为 0）；dwcontext 传 0 表示不使用。
        unsafe {
            WinHttpSendRequest(
                request.raw(),
                header_slice,
                if body_len == 0 { None } else { Some(body_ptr) },
                body_len,
                body_len,
                0,
            )
            .map_err(|err| AppError::Translate(format!("发送请求失败：{err}")))?;
            // SAFETY：request 有效，第二个参数按文档要求传空。
            WinHttpReceiveResponse(request.raw(), std::ptr::null_mut())
                .map_err(|err| AppError::Translate(format!("等待响应失败：{err}")))?;
        }

        // 代理认证：HTTP 代理要求凭据时返回 407。用 WinHttpSetCredentials 提供凭据后重试，
        // 而不是把用户名/密码拼进代理串——后者更容易出现在日志与错误文案里（docs/phase8-proxy.md §5）。
        let auth = manual.and_then(|endpoint| endpoint.auth.as_ref());
        if let Some(auth) = auth {
            let status = query_status(request.raw())?;
            if status == 407 {
                let user = wide(&auth.username);
                let pass = wide(&auth.password);
                // SAFETY：request 有效；user/pass 宽字符串以 NUL 结尾且在本作用域内存活。
                unsafe {
                    WinHttpSetCredentials(
                        request.raw(),
                        WINHTTP_AUTH_TARGET_PROXY,
                        WINHTTP_AUTH_SCHEME_BASIC.0,
                        PCWSTR(user.as_ptr()),
                        PCWSTR(pass.as_ptr()),
                        std::ptr::null_mut(),
                    )
                    .map_err(|err| AppError::Translate(format!("设置代理凭据失败：{err}")))?;
                    WinHttpSendRequest(
                        request.raw(),
                        header_slice,
                        if body_len == 0 { None } else { Some(body_ptr) },
                        body_len,
                        body_len,
                        0,
                    )
                    .map_err(|err| AppError::Translate(format!("带凭据重发请求失败：{err}")))?;
                    WinHttpReceiveResponse(request.raw(), std::ptr::null_mut())
                        .map_err(|err| AppError::Translate(format!("等待响应失败：{err}")))?;
                }
                // 重试后复查：仍 407 说明凭据被拒，或代理要求 Basic 之外的方案
                // （本项目仅支持 Basic；NTLM/Negotiate 常见于企业代理）。
                // 必须区分这两种情况——否则用户会以为只是密码填错。
                if query_status(request.raw())? == 407 {
                    return Err(AppError::Translate(
                        "代理拒绝了提供的凭据（认证后仍 407）。请核对用户名密码；\
                         若代理要求 NTLM/Negotiate 认证，当前暂不支持"
                            .into(),
                    ));
                }
            }
        }

        // 状态码：WINHTTP_QUERY_FLAG_NUMBER 要求以整数缓冲区接收
        let status = query_status(request.raw())?;

        let mut body: Vec<u8> = Vec::new();
        loop {
            let mut available: u32 = 0;
            // SAFETY：request 有效；available 为可写的 u32。
            unsafe {
                WinHttpQueryDataAvailable(request.raw(), &mut available)
                    .map_err(|err| AppError::Translate(format!("读取响应长度失败：{err}")))?;
            }
            let to_read = if available == 0 {
                READ_CHUNK
            } else {
                available as usize
            };
            let mut chunk = vec![0u8; to_read];
            let mut read: u32 = 0;
            // SAFETY：request 有效；chunk 为可写缓冲区，其容量即 dwnumberofbytestoread。
            unsafe {
                WinHttpReadData(
                    request.raw(),
                    chunk.as_mut_ptr() as *mut core::ffi::c_void,
                    to_read as u32,
                    &mut read,
                )
                .map_err(|err| AppError::Translate(format!("读取响应内容失败：{err}")))?;
            }
            if read == 0 {
                break;
            }
            chunk.truncate(read as usize);
            body.extend_from_slice(&chunk);
            if body.len() > MAX_RESPONSE_BYTES {
                return Err(AppError::Translate(
                    "响应内容过大，已中止（接口可能返回了非预期内容）".into(),
                ));
            }
        }

        Ok(HttpResponse {
            status: status as u16,
            // 翻译接口均为 UTF-8 JSON；非法字节做替换而不是整体报错，便于把原文错误体回显给用户
            body: String::from_utf8_lossy(&body).into_owned(),
        })
    }

    /// 读取响应状态码（407 探测与正常路径共用）。
    fn query_status(request: *mut core::ffi::c_void) -> AppResult<u32> {
        let mut status: u32 = 0;
        let mut status_len = std::mem::size_of::<u32>() as u32;
        // SAFETY：request 有效；&mut status 为可写的 u32，长度与之匹配；索引传空表示第 0 个。
        unsafe {
            WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                PCWSTR::null(),
                Some(&mut status as *mut u32 as *mut core::ffi::c_void),
                &mut status_len,
                std::ptr::null_mut(),
            )
            .map_err(|err| AppError::Translate(format!("读取响应状态码失败：{err}")))?;
        }
        Ok(status)
    }

    /// WinHTTP 代理串：`host:port`。不含凭据（凭据走 `WinHttpSetCredentials`，
    /// 避免出现在日志/错误文案里），不含 scheme 前缀（`http=` 才是按协议指定）。
    fn proxy_string(endpoint: &ProxyEndpoint) -> String {
        format!("{}:{}", endpoint.host.trim(), endpoint.port)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::translate::test_server::{json_response, leaked, spawn};

    /// 构造一次指向环回服务端的明文请求描述。
    fn loopback_request(port: u16, method: HttpMethod, body: &str) -> HttpRequest {
        HttpRequest {
            method,
            url: format!("http://127.0.0.1:{port}/translate?api-version=3.0&to=en"),
            headers: vec![
                ("Content-Type".into(), "application/json".into()),
                ("Ocp-Apim-Subscription-Key".into(), "test-key-123".into()),
            ],
            body: body.into(),
            timeout_secs: 10,
            proxy: None,
        }
    }

    /// **端到端**：真实发起 WinHTTP 请求，验证请求行、请求头、查询串与请求体都按预期送达。
    ///
    /// 这是本文件真正执行 `unsafe` WinHTTP 代码路径的测试——覆盖句柄 RAII、请求头拼装
    /// （含尾部 CRLF 与长度语义）、请求体发送、状态码读取与响应体分块读取。
    #[test]
    fn send_delivers_request_and_reads_response() {
        let (port, server) = spawn(json_response("HTTP/1.1 200 OK", "{\"ok\":true}"));
        let body = r#"{"Text":"你好"}"#;
        let response = send(&loopback_request(port, HttpMethod::Post, body)).expect("请求应成功");
        let received = server.join().expect("服务端线程应正常结束");

        // 响应：状态码与响应体
        assert_eq!(response.status, 200);
        assert_eq!(response.body, "{\"ok\":true}");

        // 请求：方法、路径与查询串
        assert!(
            received
                .request_line()
                .starts_with("POST /translate?api-version=3.0&to=en HTTP/1."),
            "请求行不符: {}",
            received.request_line()
        );
        // 请求头（大小写不敏感，值原样）
        assert_eq!(
            received.header("Ocp-Apim-Subscription-Key").as_deref(),
            Some("test-key-123")
        );
        assert_eq!(
            received.header("Content-Type").as_deref(),
            Some("application/json")
        );
        // 请求体必须完整送达（含非 ASCII，按 UTF-8 字节计长度）
        assert_eq!(received.body(), body, "请求体应原样送达");
        assert_eq!(
            received.header("Content-Length").as_deref(),
            Some(body.len().to_string().as_str()),
            "Content-Length 应是 UTF-8 字节数"
        );
    }

    /// 非 2xx 的状态码与错误体必须原样带回（由调用方映射为可读错误）。
    #[test]
    fn send_returns_non_2xx_status_and_error_body() {
        let (port, server) = spawn(json_response(
            "HTTP/1.1 401 Unauthorized",
            "{\"error\":{\"message\":\"bad key\"}}",
        ));
        let response = send(&loopback_request(port, HttpMethod::Post, "{}")).expect("请求应成功");
        let _ = server.join();

        assert_eq!(response.status, 401, "应回报真实状态码而不是报错");
        assert!(
            response.body.contains("bad key"),
            "错误体应可读: {}",
            response.body
        );
    }

    /// GET 无请求体：服务端不应收到请求体。
    #[test]
    fn send_get_without_body() {
        let (port, server) = spawn(json_response("HTTP/1.1 200 OK", "hi"));
        let response = send(&loopback_request(port, HttpMethod::Get, "")).expect("请求应成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(response.status, 200);
        assert_eq!(response.body, "hi");
        assert!(
            received.request_line().starts_with("GET /translate"),
            "{}",
            received.request_line()
        );
        assert!(received.body().is_empty(), "GET 不应带请求体");
    }

    /// 空响应体（如 204）不应把读取循环卡住。
    #[test]
    fn send_handles_empty_response_body() {
        let (port, server) = spawn(leaked(
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".to_owned(),
        ));
        let response = send(&loopback_request(port, HttpMethod::Post, "{}")).expect("请求应成功");
        let _ = server.join();

        assert_eq!(response.status, 204);
        assert!(
            response.body.is_empty(),
            "空响应体应读为空串: {:?}",
            response.body
        );
    }

    /// 较大响应体应被完整读回（覆盖分块读取路径，而非只读第一块）。
    #[test]
    fn send_reads_large_body_across_chunks() {
        // 构造 >64KB 的 JSON 响应，超出单次读取缓冲
        let body = format!("{{\"t\":\"{}\"}}", "x".repeat(80 * 1024));
        let (port, server) = spawn(json_response("HTTP/1.1 200 OK", &body));

        let response = send(&loopback_request(port, HttpMethod::Post, "{}")).expect("请求应成功");
        let _ = server.join();

        assert_eq!(response.status, 200);
        assert_eq!(response.body.len(), body.len(), "大响应体应完整读回");
        assert_eq!(response.body, body);
    }

    /// 超长请求体（>64KB）应完整送达（覆盖 WinHttpSendRequest 的全量发送语义）。
    #[test]
    fn send_delivers_large_request_body() {
        let (port, server) = spawn(json_response("HTTP/1.1 200 OK", "ok"));
        let big = format!("{{\"Text\":\"{}\"}}", "翻".repeat(30_000));
        let response = send(&loopback_request(port, HttpMethod::Post, &big)).expect("请求应成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(response.status, 200);
        assert_eq!(
            received.body().len(),
            big.len(),
            "大请求体应完整送达（UTF-8 字节数一致）"
        );
        assert_eq!(received.body(), big);
    }

    /// 连不上的端口应给出可读错误而非 panic。
    #[test]
    fn send_reports_unreachable_server() {
        // 绑一个端口后立即释放，得到"几乎必然无人监听"的端口号
        let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("绑定失败");
        let port = listener.local_addr().expect("取地址失败").port();
        drop(listener);

        let err =
            send(&loopback_request(port, HttpMethod::Post, "{}")).expect_err("无服务端时应报错");
        let message = err.to_string();
        // 连接阶段的失败应带 WinHTTP 错误码，且**不得**被误报成 HTTP 状态类问题
        // （状态码文案由 provider 层的 status_error 产生，send 只回报状态码）
        assert!(message.contains("0x"), "应带 WinHTTP 错误码: {message}");
        assert!(
            !message.contains("鉴权失败") && !message.contains("服务端错误"),
            "连接失败不应被误判为状态类错误: {message}"
        );
    }

    /// 请求必须**真的经过**配置的代理——用伪造代理正向证明。
    ///
    /// 为什么不能用"死代理 + 环回目标"：实测 WinHTTP 对本地地址默认绕过代理，
    /// 该组合无法判定代理是否被采用。故这里用伪造代理直接应答，并断言
    /// ①客户端拿到了代理的响应；②代理观察到的是**绝对形式**请求行
    /// （`POST http://…` 而非 `/path`）——两者同时成立才证明流量确实走代理。
    #[test]
    fn send_routes_through_configured_proxy() {
        let (proxy_port, proxy) = crate::translate::test_server::spawn_fake_proxy();

        // 目标用不可达域名：若请求绕过代理直连，必然失败，不会被伪造代理应答
        let request = HttpRequest {
            method: HttpMethod::Post,
            url: "http://proxy-test.invalid/v1/chat/completions".into(),
            headers: vec![("Content-Type".into(), "application/json".into())],
            body: "{}".into(),
            timeout_secs: 10,
            proxy: Some(ProxyConfig::Manual {
                endpoint: ProxyEndpoint {
                    kind: ProxyKind::Http,
                    host: "127.0.0.1".into(),
                    port: proxy_port,
                    auth: None,
                },
                bypass: String::new(),
            }),
        };
        let response = send(&request).expect("经伪造代理应成功（证明代理被采用）");
        let seen_request_line = proxy.join().expect("伪造代理线程应正常结束");

        assert_eq!(response.status, 200);
        assert_eq!(
            response.body, r#"{"via":"proxy"}"#,
            "响应应来自伪造代理，而非目标服务器"
        );
        // 代理收到的是绝对形式请求行（HTTP 代理的约定形式）
        assert!(
            seen_request_line.starts_with("POST http://proxy-test.invalid/v1/chat/completions"),
            "代理应看到绝对形式请求行: {seen_request_line}"
        );
    }

    /// **端到端**（环回明文）：SOCKS5 走**完整分派层**的验证。
    ///
    /// 为什么放在 `http.rs` 而不是 `socks.rs`：生产路径是 `send()` 按 `endpoint.kind` 分派，
    /// `socks.rs` 内部测试直接调 `socks::send` 会**绕过分派**——变异验证表明那样防不住
    /// "分派条件写反、SOCKS5 被静默交给 WinHTTP 直连"（恰好是 Phase 7 R7 要防的隐私事故）。
    /// 这里从公开入口 `send` 进入并断言握手，分派被写坏时必然失败。
    #[test]
    fn send_dispatches_socks5_through_ureq() {
        let server = crate::translate::test_server::spawn_socks5_proxy();
        // 目标域名用 .invalid（不可解析）：若 SOCKS5 被错误地忽略而直连，必然失败——
        // 因此"拿到响应"本身就证明流量经过了 SOCKS5
        let request = HttpRequest {
            method: HttpMethod::Get,
            url: "http://socks-test.invalid/".into(),
            headers: Vec::new(),
            body: String::new(),
            timeout_secs: 10,
            proxy: Some(ProxyConfig::Manual {
                endpoint: ProxyEndpoint {
                    kind: ProxyKind::Socks5,
                    host: "127.0.0.1".into(),
                    port: server.port(),
                    auth: None,
                },
                bypass: String::new(),
            }),
        };
        let response = send(&request).expect("经 SOCKS5 隧道的请求应成功");
        assert_eq!(response.status, 200);
        assert!(
            response.body.contains("\"via\":\"socks5\""),
            "响应应来自隧道内预置的应答"
        );
        assert!(
            server.assert_handshake_seen(),
            "SOCKS5 服务端应收到 0x05 握手"
        );
    }

    /// 诊断（显式运行）：打印直连与经代理的错误差异，供人工核对代理行为。
    ///
    /// 断言已在 `send_routes_through_configured_proxy`（正向、不依赖外网）里覆盖；
    /// 本用例只做输出，便于排查本机网络环境。
    #[test]
    #[ignore = "需真实外网明文 HTTP；仅打印诊断信息"]
    fn send_proxy_usage() {
        let mut request = HttpRequest {
            method: HttpMethod::Get,
            url: "http://neverssl.com/".into(),
            headers: Vec::new(),
            body: String::new(),
            timeout_secs: 8,
            proxy: None,
        };
        println!("无代理直连 : {:?}", send(&request));
        request.proxy = Some(ProxyConfig::Manual {
            endpoint: ProxyEndpoint {
                kind: ProxyKind::Http,
                host: "127.0.0.1".into(),
                port: 1,
                auth: None,
            },
            bypass: String::new(),
        });
        println!("经死代理   : {:?}", send(&request));
        println!(
            "（直连失败码 0x80072EE2=超时 vs 经代理 0x80072EFD=连不上代理，差异即证明代理被采用）"
        );
    }

    #[test]
    fn parse_url_extracts_host_port_and_target() {
        let url = parse_url("https://api.openai.com/v1/chat/completions").unwrap();
        assert!(url.secure);
        assert_eq!(url.host, "api.openai.com");
        assert_eq!(url.port, 443);
        assert_eq!(url.target, "/v1/chat/completions");

        let url = parse_url("http://127.0.0.1:8080/translate").unwrap();
        assert!(!url.secure);
        assert_eq!(url.host, "127.0.0.1");
        assert_eq!(url.port, 8080);
        assert_eq!(url.target, "/translate");
    }

    #[test]
    fn parse_url_keeps_query_and_defaults_path() {
        let url =
            parse_url("https://api.translator.azure.cn/translate?api-version=3.0&to=en").unwrap();
        assert_eq!(url.target, "/translate?api-version=3.0&to=en");
        // 无路径时补 '/'
        let url = parse_url("https://example.com").unwrap();
        assert_eq!(url.target, "/");
        assert_eq!(url.port, 443);
        // http 的默认端口是 80
        let url = parse_url("http://example.com").unwrap();
        assert_eq!(url.port, 80);
    }

    #[test]
    fn parse_url_rejects_bad_input() {
        assert!(
            parse_url("ftp://example.com").is_err(),
            "非 http(s) 协议应拒绝"
        );
        assert!(parse_url("example.com").is_err(), "缺少协议应拒绝");
        assert!(parse_url("https://").is_err(), "缺少主机应拒绝");
        assert!(parse_url("https://host:abc/").is_err(), "端口非数字应拒绝");
    }

    #[test]
    fn parse_url_trims_whitespace() {
        let url = parse_url("  https://example.com/x  ").unwrap();
        assert_eq!(url.host, "example.com");
        assert_eq!(url.target, "/x");
    }
}
