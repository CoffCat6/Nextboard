//! 微软翻译（**免授权端点**，Phase 7 F22 改造）。
//!
//! 契约（依据 2026-08 起 Edge 内置翻译的端点变更，见 `docs/progress.md`）：
//! - `POST {endpoint}/translatetext?from=<tag>&to=<tag>&isEnterpriseClient=false`
//! - **无需任何鉴权**：不带 `Authorization`、不带订阅密钥
//! - 请求体为 JSON **字符串数组**：`["你好世界","早上好"]`（支持批量；本项目每次一条）
//! - 响应为 JSON 数组：`[{"translations":[{"text":"…","to":"en"}]}]`
//! - `from` 留空 = 由服务自动检测源语言
//!
//! 选它当默认提供方的理由：**零配置、装上就能翻译**（用户不必先申请密钥）。
//!
//! ⚠️ 这是 Edge 前端使用的**非公开端点**，没有 SLA，路径与风控策略可能随时调整。
//! 因此端点保留可配置（设置 → 翻译 → 接口地址），失败时给出可读错误，
//! 用户可随时切到 `openai` / `custom` 通道。

use crate::error::{AppError, AppResult};
use crate::settings::MicrosoftProvider;
use crate::translate::http::{HttpMethod, HttpRequest, HttpResponse};
use crate::translate::lang;
use crate::translate::{status_error, value_as_text, Provider, TranslateRequest, TranslateResult};

/// 译文的响应取值路径（首元素为数组，故以 `[0]` 起始）。
pub const RESPONSE_PATH: &str = "[0].translations[0].text";

/// 探测到的源语言路径。免授权端点通常**不返回**该字段，缺失时按 `None` 降级
/// （保留解析是为了兼容同形态的其它实现）。
pub const DETECTED_PATH: &str = "[0].detectedLanguage.language";

/// 固定查询参数：抓包显示 Edge 前端始终带该值。
const ENTERPRISE_FLAG: &str = "isEnterpriseClient=false";

/// 默认端点（免授权）。设置层与端点构造共用同一个常量，避免两处各写一份而漂移。
pub const DEFAULT_ENDPOINT: &str = "https://edge.microsoft.com/translate";

/// 从基础端点构造 `/translatetext` 地址。
///
/// `from` 留空（或 `auto`）时拼成 `from=`，由服务自动检测源语言 —— 带 `from=auto`
/// 会被服务判为非法语言，故必须真的留空。
///
/// `base` 为空时回退到 [`DEFAULT_ENDPOINT`]：空端点会拼出**相对地址**
/// （`/translatetext?…`），请求必然失败，回退比报错更有用。
pub fn endpoint(base: &str, to: &str, from: &str) -> String {
    let base = base.trim().trim_end_matches('/');
    let base = if base.is_empty() { DEFAULT_ENDPOINT } else { base };
    let from = from.trim();
    let from_param = if from.is_empty() || from.eq_ignore_ascii_case(lang::AUTO) {
        ""
    } else {
        from
    };
    format!("{base}/translatetext?from={from_param}&to={to}&{ENTERPRISE_FLAG}")
}

/// 构造请求体：字符串数组，每项一段待翻译文本。
pub fn build_body(request: &TranslateRequest) -> serde_json::Value {
    serde_json::json!([request.text])
}

/// 发起一次翻译请求。
pub fn translate(
    config: &MicrosoftProvider,
    request: &TranslateRequest,
    timeout_secs: i64,
    proxy: Option<super::http::ProxyConfig>,
) -> AppResult<TranslateResult> {
    let started = std::time::Instant::now();
    let url = endpoint(&config.endpoint, &request.to, &request.from);
    let body = serde_json::to_string(&build_body(request))
        .map_err(|err| AppError::Internal(format!("请求体序列化失败: {err}")))?;
    // 免授权端点只要求 JSON 内容类型，不需要任何鉴权头
    let headers = vec![("Content-Type".to_owned(), "application/json".to_owned())];

    log::info!(
        "已向 {}({}) 发送翻译请求，{} 字符",
        Provider::Microsoft.as_str(),
        host_of(&url),
        request.text.chars().count()
    );
    let response = crate::translate::http::send(&HttpRequest {
        method: HttpMethod::Post,
        url,
        headers,
        body,
        timeout_secs,
        proxy,
    })?;
    parse_response(response, started)
}

/// 解析响应：非 2xx 先映射为可读错误，再取译文与（若有的）探测源语言。
fn parse_response(
    response: HttpResponse,
    started: std::time::Instant,
) -> AppResult<TranslateResult> {
    if !(200..300).contains(&response.status) {
        return Err(status_error(
            Provider::Microsoft.as_str(),
            response.status,
            &response.body,
        ));
    }
    let json: serde_json::Value = serde_json::from_str(&response.body).map_err(|err| {
        AppError::Translate(format!("响应不是合法 JSON（接口地址可能有误）: {err}"))
    })?;
    let text = value_as_text(&json, RESPONSE_PATH)?;
    if text.trim().is_empty() {
        return Err(AppError::Translate("服务返回了空译文".into()));
    }
    // 探测到的源语言属于附加信息：缺失不影响翻译成功，故失败时静默降级为 None
    let detected_source = crate::translate::extract_path(&json, DETECTED_PATH)
        .ok()
        .and_then(|value| value.as_str())
        .map(str::to_owned);
    Ok(TranslateResult {
        text,
        detected_source,
        provider: Provider::Microsoft.as_str().into(),
        elapsed_ms: started.elapsed().as_millis() as i64,
        truncated: false,
    })
}

/// 取 URL 主机名用于日志（不打印完整 URL，避免把查询参数记进日志）。
fn host_of(url: &str) -> &str {
    url.split("://")
        .nth(1)
        .and_then(|rest| rest.split('/').next())
        .unwrap_or(url)
}

#[cfg(test)]
mod tests {
    use super::*;

    const BASE: &str = "https://edge.microsoft.com/translate";

    fn request(from: &str, to: &str) -> TranslateRequest {
        TranslateRequest {
            text: "你好".into(),
            from: from.into(),
            to: to.into(),
        }
    }

    #[test]
    fn endpoint_adds_path_target_and_flag() {
        assert_eq!(
            endpoint(BASE, "en", "zh-Hans"),
            "https://edge.microsoft.com/translate/translatetext\
             ?from=zh-Hans&to=en&isEnterpriseClient=false"
        );
        // 尾部斜杠应归一，不能拼出双斜杠
        assert_eq!(
            endpoint("https://edge.microsoft.com/translate/", "en", "zh-Hans"),
            "https://edge.microsoft.com/translate/translatetext\
             ?from=zh-Hans&to=en&isEnterpriseClient=false"
        );
    }

    #[test]
    fn endpoint_leaves_from_empty_when_auto() {
        // 自动检测必须拼成 `from=`（空值），带 `from=auto` 会被判为非法语言
        let url = endpoint(BASE, "zh-Hans", "auto");
        assert!(url.contains("?from=&to=zh-Hans"), "{url}");
        let url = endpoint(BASE, "zh-Hans", "AUTO");
        assert!(url.contains("?from=&to=zh-Hans"), "auto 大小写不敏感: {url}");
        let url = endpoint(BASE, "zh-Hans", "");
        assert!(url.contains("?from=&to=zh-Hans"), "留空等同自动检测: {url}");
    }

    #[test]
    fn endpoint_falls_back_to_default_when_base_blank() {
        // 设置里端点留空时不得拼出相对地址（那必然失败），应回退到默认端点
        let expected = format!(
            "{DEFAULT_ENDPOINT}/translatetext?from=&to=zh-Hans&isEnterpriseClient=false"
        );
        assert_eq!(endpoint("", "zh-Hans", "auto"), expected);
        assert_eq!(endpoint("   ", "zh-Hans", "auto"), expected);
    }

    #[test]
    fn endpoint_supports_custom_domain_with_path() {
        let url = endpoint("https://example.com/api/translate", "en", "zh-Hans");
        assert_eq!(
            url,
            "https://example.com/api/translate/translatetext\
             ?from=zh-Hans&to=en&isEnterpriseClient=false"
        );
    }

    #[test]
    fn body_is_string_array() {
        let body = build_body(&request("auto", "en"));
        assert!(body.is_array(), "端点要求字符串数组形式的请求体");
        assert_eq!(body[0], "你好");
        assert_eq!(body.as_array().map(Vec::len), Some(1));
    }

    #[test]
    fn parse_response_reads_edge_shape() {
        // 免授权端点的响应形态（无 detectedLanguage）
        let body = r#"[{ "translations": [ { "text": "Hello", "to": "en" } ] }]"#;
        let result = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap();
        assert_eq!(result.text, "Hello");
        assert_eq!(result.provider, "microsoft");
        assert!(
            result.detected_source.is_none(),
            "该端点不返回源语言，应降级为 None 而不是报错"
        );
    }

    #[test]
    fn parse_response_uses_detected_language_when_present() {
        // 同形态实现若返回 detectedLanguage，也应被取用
        let body = r#"[{
            "detectedLanguage": { "language": "zh-Hans" },
            "translations": [{ "text": "Hello", "to": "en" }]
        }]"#;
        let result = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap();
        assert_eq!(result.detected_source.as_deref(), Some("zh-Hans"));
    }

    #[test]
    fn parse_response_rejects_non_2xx() {
        let body = r#"{"error":{"code":429,"message":"Too many requests"}}"#;
        let err = parse_response(
            HttpResponse {
                status: 429,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("过于频繁") || err.contains("429"), "{err}");
        assert!(err.contains("Too many requests"), "应带出服务原文: {err}");
    }

    #[test]
    fn parse_response_rejects_empty_translation() {
        let body = r#"[{ "translations": [{ "text": "   ", "to": "en" }] }]"#;
        let err = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("空译文"), "{err}");
    }

    #[test]
    fn host_of_ignores_path_and_query() {
        assert_eq!(
            host_of("https://edge.microsoft.com/translate/translatetext?to=en"),
            "edge.microsoft.com"
        );
    }

    /// **端到端（环回明文）**：走完整 `translate()` 链路 —— 端点构造 → WinHTTP 发送 →
    /// 状态码判定 → 按响应路径取译文。
    ///
    /// 这条测试真实执行了除 TLS 外的全部生产代码；同时钉住"**不带任何鉴权头**"这一关键契约
    /// （免授权端点最容易被误加回密钥头）。
    #[test]
    fn translate_end_to_end_over_loopback() {
        let body = r#"[{"translations":[{"text":"Hello","to":"en"}]}]"#;
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", body),
        );

        let config = MicrosoftProvider {
            // 指向环回服务端（明文），其余与真实配置一致
            endpoint: format!("http://127.0.0.1:{port}"),
        };
        let result = translate(&config, &request("zh-Hans", "en"), 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(result.text, "Hello");
        assert_eq!(result.provider, "microsoft");

        // 请求行：/translatetext + from/to + 固定 flag
        assert!(
            received.request_line().starts_with(
                "POST /translatetext?from=zh-Hans&to=en&isEnterpriseClient=false"
            ),
            "请求行不符: {}",
            received.request_line()
        );
        // 免授权：不得出现任何鉴权头
        assert!(
            received.header("Ocp-Apim-Subscription-Key").is_none(),
            "免授权端点不应带订阅密钥头: {}",
            received.raw
        );
        assert!(
            received.header("Authorization").is_none(),
            "免授权端点不应带 Authorization 头: {}",
            received.raw
        );
        assert_eq!(
            received.header("Content-Type").as_deref(),
            Some("application/json")
        );
        // 请求体为字符串数组
        let sent: serde_json::Value =
            serde_json::from_str(received.body()).expect("请求体应是 JSON");
        assert_eq!(sent[0], "你好");
    }

    /// 自动检测源语言时，请求里 `from` 必须是**空值**。
    #[test]
    fn translate_sends_empty_from_when_auto() {
        let body = r#"[{"translations":[{"text":"Hello","to":"en"}]}]"#;
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", body),
        );
        let config = MicrosoftProvider {
            endpoint: format!("http://127.0.0.1:{port}"),
        };
        translate(&config, &request("auto", "en"), 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");
        assert!(
            received
                .request_line()
                .starts_with("POST /translatetext?from=&to=en"),
            "自动检测应发空 from: {}",
            received.request_line()
        );
    }

    /// 服务端返回错误时，错误码与消息应经 `status_error` 映射为可读文案。
    #[test]
    fn translate_maps_server_error_over_loopback() {
        let (port, _server) =
            crate::translate::test_server::spawn(crate::translate::test_server::json_response(
                "HTTP/1.1 429 Too Many Requests",
                r#"{"error":{"code":429,"message":"Too many requests"}}"#,
            ));
        let config = MicrosoftProvider {
            endpoint: format!("http://127.0.0.1:{port}"),
        };
        let err = translate(&config, &request("zh-Hans", "en"), 10, None)
            .expect_err("应报错")
            .to_string();
        assert!(err.contains("过于频繁") || err.contains("429"), "{err}");
        assert!(err.contains("Too many requests"), "应带出服务原文: {err}");
    }
}
