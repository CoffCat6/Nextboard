//! 彩云小译（LingoCloud）文本翻译。
//!
//! 契约（依据官方文档 `docs.caiyunapp.com/lingocloud-api` + 本机实测，见 `docs/progress.md`）：
//! - `POST {endpoint}/v1/translator`，端点默认 `https://api.interpreter.caiyunai.com`
//! - 请求头 `X-Authorization: token <TOKEN>`（**注意前缀 `token ` 是协议的一部分**）
//! - 请求体：`{"source":["…"],"trans_type":"auto2zh","detect":true,"media":"text"}`
//! - 响应：`{"rc":0,"target":["译文"]}` → 取值路径 `target[0]`
//! - 失败形态：401 `{"message":"Invalid token"}`、409 `{"message":"Unsupported trans_type"}`
//!
//! **语言代码要转换**：本项目用 `zh-Hans`，彩云用 `zh` —— 实测直接发 `auto2zh-Hans`
//! 会被拒（HTTP 409 Unsupported trans_type）。方向串由「源2目标」拼接，源为
//! 空/`auto` 时用 `auto` 并把 `detect` 置真（官方要求 auto2xx 必须 detect）。

use crate::error::{AppError, AppResult};
use crate::settings::CaiyunProvider;
use crate::translate::http::{HttpMethod, HttpRequest, HttpResponse};
use crate::translate::lang;
use crate::translate::{status_error, value_as_text, Provider, TranslateRequest, TranslateResult};

/// 默认端点。用户可改（自建代理或官方域名调整时）。
pub const DEFAULT_ENDPOINT: &str = "https://api.interpreter.caiyunai.com";

/// 官方文档提供的**测试 Token**。
///
/// 官方明确「不保证该 Token 的可用性」，故它只用于"装上就能试"；
/// 设置页会提示持续使用需自行申请正式 Token。
pub const DEFAULT_TOKEN: &str = "3975l6lr5pcbvidl6jl2";

/// 译文的响应取值路径（`source` 传数组时 `target` 也是数组）。
pub const RESPONSE_PATH: &str = "target[0]";

/// 把本项目的语言代码换成彩云认的写法。
///
/// 只有简体中文不同（本项目 `zh-Hans` / 彩云 `zh`）；繁体彩云自己就叫 `zh-Hant`，
/// 其余（`en`/`ja`/`ko`/`fr`…）一致。空值与 `auto` 统一成 `auto`（自动识别）。
fn to_caiyun_lang(code: &str) -> String {
    let code = code.trim();
    if code.is_empty() || code.eq_ignore_ascii_case(lang::AUTO) {
        return "auto".to_owned();
    }
    match code.to_ascii_lowercase().as_str() {
        "zh-hans" | "zh-cn" | "zh-sg" | "zh" => "zh".to_owned(),
        "zh-hant" | "zh-tw" | "zh-hk" => "zh-Hant".to_owned(),
        _ => code.to_owned(),
    }
}

/// 翻译方向串：`{源}2{目标}`（彩云用合并写法，不是两个独立参数）。
pub fn trans_type(from: &str, to: &str) -> String {
    format!("{}2{}", to_caiyun_lang(from), to_caiyun_lang(to))
}

/// 构造请求体。
pub fn build_body(request: &TranslateRequest) -> serde_json::Value {
    let source = to_caiyun_lang(&request.from);
    serde_json::json!({
        "source": [request.text],
        "trans_type": trans_type(&request.from, &request.to),
        // 官方要求 auto2xx 时必须为 true；显式源语言时带上也无害（实测通过）
        "detect": source == "auto",
        "media": "text",
        "request_id": "nextboard",
    })
}

/// 从基础端点构造 `/v1/translator` 地址。
pub fn endpoint(base: &str) -> String {
    let base = base.trim().trim_end_matches('/');
    let base = if base.is_empty() {
        DEFAULT_ENDPOINT
    } else {
        base
    };
    format!("{base}/v1/translator")
}

/// 发起一次翻译请求。
pub fn translate(
    config: &CaiyunProvider,
    request: &TranslateRequest,
    timeout_secs: i64,
    proxy: Option<super::http::ProxyConfig>,
) -> AppResult<TranslateResult> {
    let token = config.token.trim();
    if token.is_empty() {
        return Err(AppError::Translate(
            "未配置彩云小译 Token，请在设置 → 翻译中填写（也可先用官方测试 Token）".into(),
        ));
    }
    let started = std::time::Instant::now();
    let url = endpoint(&config.endpoint);
    let body = serde_json::to_string(&build_body(request))
        .map_err(|err| AppError::Internal(format!("请求体序列化失败: {err}")))?;
    // `token ` 前缀是协议的一部分，不能省
    let headers = vec![
        ("Content-Type".to_owned(), "application/json".to_owned()),
        ("X-Authorization".to_owned(), format!("token {token}")),
    ];

    log::info!(
        "已向 {}({}) 发送翻译请求，{} 字符",
        Provider::Caiyun.as_str(),
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

/// 解析响应：非 2xx 先映射为可读错误；再按 `rc` 判成败，最后取译文。
fn parse_response(
    response: HttpResponse,
    started: std::time::Instant,
) -> AppResult<TranslateResult> {
    if !(200..300).contains(&response.status) {
        return Err(status_error(
            Provider::Caiyun.as_str(),
            response.status,
            &response.body,
        ));
    }
    let json: serde_json::Value = serde_json::from_str(&response.body).map_err(|err| {
        AppError::Translate(format!("响应不是合法 JSON（接口地址可能有误）: {err}"))
    })?;
    // 200 也可能带业务错误（`rc != 0`）：优先报出服务给的原因
    if let Some(rc) = json.get("rc").and_then(|value| value.as_i64()) {
        if rc != 0 {
            let message = json
                .get("message")
                .or_else(|| json.get("error"))
                .and_then(|value| value.as_str())
                .unwrap_or("未知错误");
            return Err(AppError::Translate(format!("彩云小译返回错误: {message}")));
        }
    }
    let text = value_as_text(&json, RESPONSE_PATH)?;
    if text.trim().is_empty() {
        return Err(AppError::Translate("服务返回了空译文".into()));
    }
    Ok(TranslateResult {
        text,
        // 该接口不回传源语言；自动识别时可从 `trans_type`（如 `en2zh`）反推
        detected_source: detected_source(&json),
        provider: Provider::Caiyun.as_str().into(),
        elapsed_ms: started.elapsed().as_millis() as i64,
        truncated: false,
    })
}

/// 从响应的 `trans_type` 反推源语言（形如 `en2zh`；识别不出则 `None`）。
fn detected_source(json: &serde_json::Value) -> Option<String> {
    let full = json.get("trans_type")?.as_str()?;
    let (from, _) = full.split_once('2')?;
    (!from.is_empty()).then(|| from.to_owned())
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

    fn request(from: &str, to: &str) -> TranslateRequest {
        TranslateRequest {
            text: "你好世界".into(),
            from: from.into(),
            to: to.into(),
        }
    }

    #[test]
    fn simplified_chinese_maps_to_zh() {
        // 本项目 zh-Hans → 彩云 zh（实测直接发 zh-Hans 会 409）
        assert_eq!(to_caiyun_lang("zh-Hans"), "zh");
        assert_eq!(to_caiyun_lang("zh-CN"), "zh");
        assert_eq!(to_caiyun_lang("zh-Hant"), "zh-Hant");
        assert_eq!(to_caiyun_lang("en"), "en");
        assert_eq!(to_caiyun_lang(""), "auto", "留空等同自动识别");
        assert_eq!(to_caiyun_lang("AUTO"), "auto");
    }

    #[test]
    fn trans_type_joins_direction() {
        assert_eq!(trans_type("", "zh-Hans"), "auto2zh");
        assert_eq!(trans_type("auto", "en"), "auto2en");
        assert_eq!(trans_type("en", "zh-Hans"), "en2zh");
        assert_eq!(trans_type("zh-Hant", "en"), "zh-Hant2en");
    }

    #[test]
    fn body_has_source_array_direction_and_detect() {
        let body = build_body(&request("", "zh-Hans"));
        assert_eq!(body["source"][0], "你好世界", "source 必须是数组");
        assert_eq!(body["trans_type"], "auto2zh");
        assert_eq!(body["detect"], true, "auto2xx 必须 detect=true");
        assert_eq!(body["media"], "text");

        // 显式源语言时 detect 应为 false
        let body = build_body(&request("en", "zh-Hans"));
        assert_eq!(body["trans_type"], "en2zh");
        assert_eq!(body["detect"], false);
    }

    #[test]
    fn endpoint_appends_path_and_tolerates_blank() {
        assert_eq!(
            endpoint("https://api.interpreter.caiyunai.com"),
            "https://api.interpreter.caiyunai.com/v1/translator"
        );
        assert_eq!(
            endpoint("https://api.interpreter.caiyunai.com/"),
            "https://api.interpreter.caiyunai.com/v1/translator"
        );
        assert_eq!(
            endpoint(""),
            format!("{DEFAULT_ENDPOINT}/v1/translator"),
            "端点留空应回退默认，不能拼出相对地址"
        );
    }

    #[test]
    fn parse_response_reads_target_array() {
        let body = r#"{"rc":0,"confidence":1,"trans_type":"en2zh","target":["你好，世界"]}"#;
        let result = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap();
        assert_eq!(result.text, "你好，世界");
        assert_eq!(result.provider, "caiyun");
        assert_eq!(result.detected_source.as_deref(), Some("en"));
    }

    #[test]
    fn parse_response_reports_business_error_on_200() {
        let body = r#"{"rc":-1,"error_code":491000,"message":"Unsupported trans_type"}"#;
        let err = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("Unsupported trans_type"), "{err}");
    }

    #[test]
    fn parse_response_rejects_non_2xx() {
        let err = parse_response(
            HttpResponse {
                status: 401,
                body: r#"{"message":"Invalid token"}"#.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("鉴权失败") || err.contains("401"), "{err}");
        assert!(err.contains("Invalid token"), "应带出服务原文: {err}");
    }

    #[test]
    fn translate_rejects_missing_token() {
        let config = CaiyunProvider {
            endpoint: String::new(),
            token: "  ".into(),
        };
        let err = translate(&config, &request("", "zh-Hans"), 10, None)
            .unwrap_err()
            .to_string();
        assert!(err.contains("未配置"), "{err}");
    }

    /// **端到端（环回明文）**：端点构造 → WinHTTP 发送 → 状态码判定 → 取译文。
    /// 同时钉住两条关键契约：`X-Authorization` 必须带 `token ` 前缀；请求体形状。
    #[test]
    fn translate_end_to_end_over_loopback() {
        let body = r#"{"rc":0,"trans_type":"en2zh","target":["你好，世界"]}"#;
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", body),
        );
        let config = CaiyunProvider {
            endpoint: format!("http://127.0.0.1:{port}"),
            token: "test-token".into(),
        };
        let result = translate(&config, &request("", "zh-Hans"), 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(result.text, "你好，世界");
        assert!(
            received.request_line().starts_with("POST /v1/translator"),
            "请求行不符: {}",
            received.request_line()
        );
        assert_eq!(
            received.header("X-Authorization").as_deref(),
            Some("token test-token"),
            "`token ` 前缀是协议的一部分"
        );
        let sent: serde_json::Value =
            serde_json::from_str(received.body()).expect("请求体应是 JSON");
        assert_eq!(sent["source"][0], "你好世界");
        assert_eq!(sent["trans_type"], "auto2zh");
    }
}
