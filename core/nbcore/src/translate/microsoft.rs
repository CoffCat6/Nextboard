//! 微软翻译（Azure AI Translator v3.0，Phase 7 F22）。
//!
//! 契约（已按官方文档核实，docs/phase7.md §4.1）：
//! - `POST {endpoint}/translate?api-version=3.0&to=<tag>[&from=<tag>]`
//! - 请求头 `Ocp-Apim-Subscription-Key: <key>`；区域或多服务资源**必须**再带
//!   `Ocp-Apim-Subscription-Region: <region>`
//! - 请求体为 JSON 数组：`[{"Text":"…"}]`
//! - 响应为 JSON 数组：`[{"detectedLanguage":{"language":"en"},"translations":[{"text":"…","to":"zh-Hans"}]}]`
//! - 单请求上限 50000 字符（跨所有目标语言合计），已在 `validate_request` 统一拦截
//!
//! **默认端点为中国版**（21Vianet 运营，用户决策 U1）；国际版改设置里的 `endpoint` 即可。

use crate::error::{AppError, AppResult};
use crate::settings::MicrosoftProvider;
use crate::translate::http::{HttpMethod, HttpRequest, HttpResponse};
use crate::translate::lang;
use crate::translate::{status_error, value_as_text, Provider, TranslateRequest, TranslateResult};

/// API 版本：v3.0 是当前稳定版（官方参考文档）。
const API_VERSION: &str = "3.0";

/// 译文的响应取值路径（首元素为数组，故以 `[0]` 起始）。
pub const RESPONSE_PATH: &str = "[0].translations[0].text";

/// 探测到的源语言路径；服务未返回时为缺失，按 `None` 处理。
pub const DETECTED_PATH: &str = "[0].detectedLanguage.language";

/// 从基础端点构造 `/translate` 地址。
///
/// 端点可由用户改成带路径的自定义域（虚拟网络场景下形如
/// `https://<domain>.cognitiveservices.azure.com/translator/text/v3.0`），
/// 故这里只在末尾追加 `/translate`，不假设端点一定是纯域名。
pub fn endpoint(base: &str, to: &str, from: &str) -> String {
    let base = base.trim().trim_end_matches('/');
    let mut url = format!("{base}/translate?api-version={API_VERSION}&to={to}");
    // `auto` 表示交由服务自动检测：省略 from 参数（带 from=auto 会被服务判为非法语言）
    if !from.trim().is_empty() && !from.trim().eq_ignore_ascii_case(lang::AUTO) {
        url.push_str("&from=");
        url.push_str(from.trim());
    }
    url
}

/// 构造请求体：v3.0 接受元素数组，每个元素含 `Text` 字段。
pub fn build_body(request: &TranslateRequest) -> serde_json::Value {
    serde_json::json!([{ "Text": request.text }])
}

/// 发起一次翻译请求。
pub fn translate(
    config: &MicrosoftProvider,
    request: &TranslateRequest,
    timeout_secs: i64,
    proxy: Option<super::http::ProxyConfig>,
) -> AppResult<TranslateResult> {
    if config.api_key.trim().is_empty() {
        return Err(AppError::Translate(
            "未配置微软翻译 API Key，请在设置 → 翻译中填写".into(),
        ));
    }
    let started = std::time::Instant::now();
    let url = endpoint(&config.endpoint, &request.to, &request.from);
    let body = serde_json::to_string(&build_body(request))
        .map_err(|err| AppError::Internal(format!("请求体序列化失败: {err}")))?;

    let mut headers = vec![
        ("Content-Type".into(), "application/json".into()),
        (
            "Ocp-Apim-Subscription-Key".into(),
            config.api_key.trim().to_owned(),
        ),
    ];
    // 区域头按需附带：区域资源与多服务资源必填，全局资源带了也合法
    let region = config.region.trim();
    if !region.is_empty() {
        headers.push(("Ocp-Apim-Subscription-Region".into(), region.to_owned()));
    }

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

/// 解析响应：非 2xx 先映射为可读错误，再取译文与探测到的源语言。
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

    fn config() -> MicrosoftProvider {
        MicrosoftProvider {
            endpoint: "https://api.translator.azure.cn".into(),
            region: "chinaeast2".into(),
            api_key: "test-key".into(),
        }
    }

    fn request(from: &str, to: &str) -> TranslateRequest {
        TranslateRequest {
            text: "你好".into(),
            from: from.into(),
            to: to.into(),
        }
    }

    #[test]
    fn endpoint_adds_version_and_target() {
        assert_eq!(
            endpoint("https://api.translator.azure.cn", "en", "auto"),
            "https://api.translator.azure.cn/translate?api-version=3.0&to=en"
        );
        // 尾部斜杠应归一
        assert_eq!(
            endpoint("https://api.translator.azure.cn/", "en", "auto"),
            "https://api.translator.azure.cn/translate?api-version=3.0&to=en"
        );
    }

    #[test]
    fn endpoint_omits_from_when_auto() {
        let url = endpoint("https://api.translator.azure.cn", "zh-Hans", "auto");
        assert!(!url.contains("from="), "auto 不应拼出 from 参数: {url}");
        let url = endpoint("https://api.translator.azure.cn", "zh-Hans", "AUTO");
        assert!(!url.contains("from="), "auto 大小写不敏感: {url}");
    }

    #[test]
    fn endpoint_includes_from_when_specified() {
        let url = endpoint("https://api.translator.azure.cn", "zh-Hans", "en");
        assert!(url.contains("&from=en"), "指定源语言应拼出 from: {url}");
    }

    #[test]
    fn endpoint_supports_path_style_custom_domain() {
        // 虚拟网络场景：端点自带路径
        let url = endpoint(
            "https://example.cognitiveservices.azure.com/translator/text/v3.0",
            "en",
            "auto",
        );
        assert_eq!(
            url,
            "https://example.cognitiveservices.azure.com/translator/text/v3.0/translate?api-version=3.0&to=en"
        );
    }

    #[test]
    fn body_is_single_element_array_with_text_field() {
        let body = build_body(&request("auto", "en"));
        assert!(body.is_array(), "v3.0 要求数组形式的请求体");
        assert_eq!(body[0]["Text"], "你好");
        assert_eq!(body.as_array().map(Vec::len), Some(1));
    }

    #[test]
    fn parse_response_reads_v3_shape() {
        let body = r#"[{
            "detectedLanguage": { "language": "zh-Hans", "score": 1.0 },
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
        assert_eq!(result.text, "Hello");
        assert_eq!(result.detected_source.as_deref(), Some("zh-Hans"));
        assert_eq!(result.provider, "microsoft");
    }

    #[test]
    fn parse_response_tolerates_missing_detected_language() {
        // 指定了 from 时服务可能不回 detectedLanguage：译文照常返回
        let body = r#"[{ "translations": [{ "text": "你好", "to": "zh-Hans" }] }]"#;
        let result = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap();
        assert_eq!(result.text, "你好");
        assert!(result.detected_source.is_none(), "缺失源语言不应导致失败");
    }

    #[test]
    fn parse_response_rejects_non_2xx() {
        // Azure 错误体形态
        let body = r#"{"error":{"code":401000,"message":"The request is not authorized because credentials are missing or invalid."}}"#;
        let err = parse_response(
            HttpResponse {
                status: 401,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("鉴权失败"), "{err}");
        assert!(err.contains("credentials"), "应带出服务原文: {err}");
    }

    #[test]
    fn parse_response_rejects_invalid_language_error() {
        // 目标语言不受支持时 Azure 返回 400
        let body = r#"{"error":{"code":400036,"message":"The target language is not valid."}}"#;
        let err = parse_response(
            HttpResponse {
                status: 400,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("400"), "{err}");
        assert!(err.contains("target language"), "应带出服务原文: {err}");
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
    fn translate_rejects_missing_api_key() {
        // 未填 Key 时应在发请求前拦下，而不是拿到 401 才知道
        let mut broken = config();
        broken.api_key = "  ".into();
        let err = translate(&broken, &request("auto", "en"), 30, None)
            .unwrap_err()
            .to_string();
        assert!(err.contains("未配置"), "{err}");
    }

    #[test]
    fn host_of_ignores_path_and_query() {
        assert_eq!(
            host_of("https://api.translator.azure.cn/translate?api-version=3.0&to=en"),
            "api.translator.azure.cn"
        );
    }

    /// **端到端（环回明文）**：走完整 `translate()` 链路——端点构造 → WinHTTP 发送 →
    /// 状态码判定 → 按响应路径取译文与探测源语言。
    ///
    /// 真实 Key 的请求在本环境无法执行（TLS 被策略阻断），但这条测试真实执行了除 TLS 外的
    /// 全部生产代码，包括请求头拼装与响应解析的接线。
    #[test]
    fn translate_end_to_end_over_loopback() {
        let body = r#"[{"detectedLanguage":{"language":"zh-Hans"},"translations":[{"text":"Hello","to":"en"}]}]"#;
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", body),
        );

        let config = MicrosoftProvider {
            // 指向环回服务端（明文），其余字段与真实配置一致
            endpoint: format!("http://127.0.0.1:{port}"),
            region: "chinaeast2".into(),
            api_key: "test-key".into(),
        };
        let result = translate(&config, &request("zh-Hans", "en"), 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        // 结果：译文与探测源语言都按 v3 结构取到
        assert_eq!(result.text, "Hello");
        assert_eq!(result.detected_source.as_deref(), Some("zh-Hans"));
        assert_eq!(result.provider, "microsoft");

        // 请求：v3 端点形状、鉴权头、区域头
        assert!(
            received
                .request_line()
                .starts_with("POST /translate?api-version=3.0&to=en&from=zh-Hans"),
            "请求行不符: {}",
            received.request_line()
        );
        assert_eq!(
            received.header("Ocp-Apim-Subscription-Key").as_deref(),
            Some("test-key")
        );
        assert_eq!(
            received.header("Ocp-Apim-Subscription-Region").as_deref(),
            Some("chinaeast2"),
            "区域头应在配置了区域时附带"
        );
        // 请求体为 v3 要求的单元素数组
        let sent: serde_json::Value =
            serde_json::from_str(received.body()).expect("请求体应是 JSON");
        assert_eq!(sent[0]["Text"], "你好");
    }

    /// 未配置区域时**不应**发送区域头（全局资源场景）。
    #[test]
    fn translate_omits_region_header_when_unset() {
        let body = r#"[{"translations":[{"text":"Hello","to":"en"}]}]"#;
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", body),
        );

        let config = MicrosoftProvider {
            endpoint: format!("http://127.0.0.1:{port}"),
            region: String::new(),
            api_key: "test-key".into(),
        };
        let result = translate(&config, &request("en", "en"), 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(result.text, "Hello");
        assert!(
            received.header("Ocp-Apim-Subscription-Region").is_none(),
            "未配置区域时不应带区域头: {}",
            received.raw
        );
    }

    /// 服务端返回错误时，错误码与消息应经 `status_error` 映射为可读文案。
    #[test]
    fn translate_maps_server_error_over_loopback() {
        let (port, _server) =
            crate::translate::test_server::spawn(crate::translate::test_server::json_response(
                "HTTP/1.1 401 Unauthorized",
                r#"{"error":{"code":401000,"message":"credentials are missing or invalid."}}"#,
            ));
        let config = MicrosoftProvider {
            endpoint: format!("http://127.0.0.1:{port}"),
            region: "chinaeast2".into(),
            api_key: "wrong".into(),
        };
        let err = translate(&config, &request("zh-Hans", "en"), 10, None)
            .expect_err("应报错")
            .to_string();
        assert!(err.contains("鉴权失败"), "{err}");
        assert!(err.contains("credentials"), "应带出服务原文: {err}");
    }
}
