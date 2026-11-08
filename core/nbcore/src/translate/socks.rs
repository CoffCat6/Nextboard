//! SOCKS5 传输（Phase 8，docs/phase8-proxy.md §3.3）。
//!
//! **为什么存在**：实测 WinHTTP 完全不支持 SOCKS5（`socks5://`、`socks5=`、`socks=` 三种
//! 前缀都会被静默忽略而直连，见 phase8-proxy.md §2.1）。故 SOCKS5 由本模块经 `ureq` 承担，
//! 这是引入该依赖的唯一原因（决策 D17）。
//!
//! **范围**：只处理 `ProxyConfig::Manual { kind: Socks5 }`；直连 / 系统代理 / HTTP 代理
//! 仍走 WinHTTP 路径（`super::winhttp`），已实现且经端到端测试验证，不迁移。

use std::time::Duration;

use super::{HttpMethod, HttpRequest, HttpResponse, ProxyAuth, ProxyEndpoint, MAX_RESPONSE_BYTES};
use crate::error::{AppError, AppResult};

/// 平台对应的 TLS provider(两条 ureq 传输——SOCKS5/HTTP——共用)。
///
/// ⚠️ **必须显式设置**:ureq 3.4 的默认 provider **永远是 Rustls**(源码
/// `#[default]` 标注),而 `NativeTls` 注明 "The setting is never picked up
/// automatically" —— 只开 native-tls feature 不在 Agent 里显式指定时,
/// 任何 https 请求都会 panic("provider is Rustls but feature is not enabled:
/// rustls")。2026-11-08 Linux 翻译实测踩中;Windows 的 SOCKS5+https 同样埋着
/// 此雷(此前 E2E 全是 http:// fixture 才没爆)。feature 与此处的对应关系见
/// Cargo.toml 两个 target 段:Windows=native-tls(schannel,免 NASM),
/// 非 Windows=rustls(纯 Rust,免 openssl)。
pub(super) fn tls_provider() -> ureq::tls::TlsProvider {
    if cfg!(windows) {
        ureq::tls::TlsProvider::NativeTls
    } else {
        ureq::tls::TlsProvider::Rustls
    }
}

/// 经 SOCKS5 代理发送请求（ureq 3.x 的阻塞 API）。
pub(super) fn send(req: &HttpRequest, endpoint: &ProxyEndpoint) -> AppResult<HttpResponse> {
    let proxy_uri = build_proxy_uri(endpoint);
    let proxy =
        ureq::Proxy::new(&proxy_uri).map_err(|_| AppError::Translate("代理配置无效".into()))?;
    let agent = ureq::Agent::config_builder()
        .tls_config(ureq::tls::TlsConfig::builder().provider(tls_provider()).build())
        .proxy(Some(proxy))
        .timeout_global(Some(Duration::from_secs(
            req.timeout_secs.clamp(5, 120) as u64
        )))
        .build()
        .new_agent();

    // ureq 3.x 用 typestate 区分"带体/不带体"的构建器，GET 与 POST 类型不同，
    // 不能放进同一个 match 表达式——分支内各自完成发送。
    let response = match req.method {
        HttpMethod::Get => {
            let mut request = agent.get(&req.url);
            for (name, value) in &req.headers {
                request = request.header(name, value);
            }
            request.call()
        }
        HttpMethod::Post => {
            let mut request = agent.post(&req.url);
            for (name, value) in &req.headers {
                request = request.header(name, value);
            }
            if req.body.is_empty() {
                // POST 空体：显式给空 body（post() 已进入 WithBody 状态机）
                request.send(Vec::new())
            } else {
                request.send(req.body.as_bytes())
            }
        }
    }
    .map_err(map_ureq_error)?;

    let status = response.status().as_u16();
    // 响应体上限沿用 WinHTTP 路径的常量，避免两条传输行为不一致
    // ⚠️ 必须**增量**读(2026-11-03,外部审查 #36):原实现用 read_to_string() 一次性
    //    把整个响应体读进内存,然后才比 MAX_RESPONSE_BYTES —— 上限变成"读完才查",
    //    恶意服务端返回几个 GB 时进程先被撑爆。改为按块读、逐块累计,越界立刻停手
    //    (与 WinHTTP 路径的语义拉齐)。
    let mut body = String::new();
    {
        use std::io::Read;
        let mut reader = response.into_body().into_reader();
        let mut buf = [0u8; 64 * 1024];
        loop {
            let n = reader
                .read(&mut buf)
                .map_err(|err| AppError::Translate(format!("读取响应内容失败：{err}")))?;
            if n == 0 {
                break;
            }
            if body.len() + n > MAX_RESPONSE_BYTES {
                return Err(AppError::Translate(
                    "响应内容过大，已中止（接口可能返回了非预期内容）".into(),
                ));
            }
            // 只接受 UTF-8:这是流式读,无法回头做"非法字节替换",遇到非法字节
            // 整体报错比写出乱码诚实。
            let piece = std::str::from_utf8(&buf[..n])
                .map_err(|_| AppError::Translate("响应内容不是合法 UTF-8".into()))?;
            body.push_str(piece);
        }
    }
    Ok(HttpResponse { status, body })
}

/// 组装代理串：`socks5h://[user:pass@]host:port`。
///
/// ⚠️ **必须用 socks5h 而非 socks5**(2026-11-07,M1/R2 实测):ureq 对 `socks5://`
/// 会把目标主机名先做**本地 DNS 解析**再交给代理 —— 主机名泄漏给本地解析器,
/// 违反 phase8 R7 的隐私契约(目标域名只应让代理解析)。`socks5h://`(h = hostname
/// 由代理端解析)ureq 原生支持,端到端测试(send_dispatches_socks5_through_ureq
/// 用 .invalid 域名)已验证。此前该测试在 Windows 上的"环境性失败"实为同一根因。
///
/// 主机与用户名的合法性由设置校验层保证（`validate_proxy_manual` 的字符白名单），
/// 这里不再重复校验——但**不打印**该串（含凭据）。
fn build_proxy_uri(endpoint: &ProxyEndpoint) -> String {
    let host = endpoint.host.trim();
    let credentials = endpoint
        .auth
        .as_ref()
        .map(|ProxyAuth { username, password }| format!("{username}:{password}@"))
        .unwrap_or_default();
    format!("socks5h://{credentials}{host}:{}", endpoint.port)
}

/// 把 ureq 的错误转成用户可读的文案（不回显代理串与凭据）。
/// 非 Windows 的直连/HTTP 代理传输（`http.rs` 的 `ureq_http` 模块）复用同一映射,
/// 保持两条 ureq 路径的错误文案一致。
pub(super) fn map_ureq_error(err: ureq::Error) -> AppError {
    let message = match err {
        // 4xx/5xx 被 ureq 默认转为错误；这里仍按状态码语义交给上层统一处理
        ureq::Error::StatusCode(code) => {
            return AppError::Translate(format!("接口返回状态码 {code}"));
        }
        ureq::Error::BadUri(_) => "请求地址格式无效".to_owned(),
        ureq::Error::InvalidProxyUrl => "代理地址格式无效".to_owned(),
        ureq::Error::HostNotFound => "代理主机名无法解析".to_owned(),
        ureq::Error::Timeout(_) => "请求超时".to_owned(),
        ureq::Error::Io(io) => format!("网络读写失败（{io}）"),
        ureq::Error::Tls(reason) => format!("TLS 握手失败（{reason}）"),
        ureq::Error::Http(e) => format!("请求构造失败（{e}）"),
        ureq::Error::Protocol(e) => format!("HTTP 协议错误（{e}）"),
        ureq::Error::RedirectFailed => "重定向失败".to_owned(),
        other => format!("网络请求失败（{other}）"),
    };
    AppError::Translate(message)
}

#[cfg(test)]
mod tests {
    use super::{build_proxy_uri, ProxyAuth, ProxyEndpoint};
    use crate::translate::http::ProxyKind;

    fn endpoint(
        kind: ProxyKind,
        host: &str,
        port: u16,
        auth: Option<(&str, &str)>,
    ) -> ProxyEndpoint {
        ProxyEndpoint {
            kind,
            host: host.into(),
            port,
            auth: auth.map(|(u, p)| ProxyAuth {
                username: u.into(),
                password: p.into(),
            }),
        }
    }

    #[test]
    fn proxy_uri_without_auth() {
        let e = endpoint(ProxyKind::Socks5, "127.0.0.1", 1080, None);
        // socks5h = 主机名由代理端解析(隐私契约,见 build_proxy_uri 注释)
        assert_eq!(build_proxy_uri(&e), "socks5h://127.0.0.1:1080");
    }

    #[test]
    fn proxy_uri_with_auth() {
        let e = endpoint(
            ProxyKind::Socks5,
            "proxy.local",
            1080,
            Some(("user", "pass")),
        );
        assert_eq!(build_proxy_uri(&e), "socks5h://user:pass@proxy.local:1080");
    }

    #[test]
    fn proxy_uri_never_changes_scheme() {
        // 防御：无论 host 被污染成什么，都不得改变 scheme（依赖校验层白名单 + 此处只拼 socks5h://）
        let e = endpoint(ProxyKind::Socks5, "http://evil", 80, None);
        assert!(build_proxy_uri(&e).starts_with("socks5h://"));
    }
}
