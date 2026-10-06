//! OpenAI 兼容提供方（Phase 7 F21，决策 D7/§4.2）。
//!
//! 走 `POST {baseUrl}/chat/completions`，请求体为标准 chat/completions 结构，
//! 译文取自 `choices[0].message.content`。`baseUrl` 可配置，因此同时覆盖
//! OpenAI 官方、Azure OpenAI、各类中转、Ollama / vLLM 等兼容实现。
//!
//! **契约待 PoC 确认**（docs/phase7.md §4.3 P1）：各兼容实现存在差异（路径前缀、
//! 是否支持 `temperature`、返回结构），故响应取值路径做成常量而非硬编码调用点，
//! 便于按实测结果调整。

use crate::error::{AppError, AppResult};
use crate::settings::OpenAiProvider;
use crate::translate::http::{HttpMethod, HttpRequest, HttpResponse};
use crate::translate::lang;
use crate::translate::{status_error, value_as_text, Provider, TranslateRequest, TranslateResult};

/// 响应中译文的取值路径（OpenAI 标准结构）。
pub const RESPONSE_PATH: &str = "choices[0].message.content";

/// 默认提示词：强调"只输出译文"，并保护代码/命令不被翻译。
pub const DEFAULT_SYSTEM_PROMPT: &str =
    "你是专业翻译。只输出译文，不要解释、不要加引号、不要复述原文。\
保持原文的语气与术语；代码、命令、变量名、URL、专有名词一律保持原样不译。";

/// 构造 chat/completions 请求体。
///
/// 单独抽出便于单测：语言标签要翻成自然语言名（模型理解更稳），且 `auto` 源语言
/// 要明确要求模型自行判定而不必回复语言名。
pub fn build_body(config: &OpenAiProvider, request: &TranslateRequest) -> serde_json::Value {
    let system = if config.system_prompt.trim().is_empty() {
        DEFAULT_SYSTEM_PROMPT
    } else {
        config.system_prompt.trim()
    };
    let target = lang::english_name_for_prompt(&request.to);
    let user = if request.from.trim().eq_ignore_ascii_case(lang::AUTO) {
        format!(
            "把下面的内容翻译成{target}（源语言请自行判断）。\n\n{}",
            request.text
        )
    } else {
        let source = lang::english_name_for_prompt(&request.from);
        format!("把下面的内容从{source}翻译成{target}。\n\n{}", request.text)
    };
    serde_json::json!({
        "model": config.model.trim(),
        "messages": [
            { "role": "system", "content": system },
            { "role": "user", "content": user }
        ],
        "temperature": config.temperature,
        "stream": false
    })
}

/// 拼接 chat/completions 端点：`baseUrl` 去掉尾部斜杠后追加固定路径。
pub fn endpoint(base_url: &str) -> String {
    let base = base_url.trim().trim_end_matches('/');
    format!("{base}/chat/completions")
}

/// 发起一次翻译请求。
pub fn translate(
    config: &OpenAiProvider,
    request: &TranslateRequest,
    timeout_secs: i64,
    proxy: Option<super::http::ProxyConfig>,
) -> AppResult<TranslateResult> {
    if config.api_key.trim().is_empty() {
        return Err(AppError::Translate(
            "未配置 OpenAI API Key，请在设置 → 翻译中填写".into(),
        ));
    }
    if config.model.trim().is_empty() {
        return Err(AppError::Translate(
            "未配置模型名（model），请在设置 → 翻译中填写".into(),
        ));
    }
    let started = std::time::Instant::now();
    let url = endpoint(&config.base_url);
    let body = serde_json::to_string(&build_body(config, request))
        .map_err(|err| AppError::Internal(format!("请求体序列化失败: {err}")))?;
    // 日志只记提供方 / 主机 / 字符数，绝不含原文与密钥（决策 D13）
    log::info!(
        "已向 {}({}) 发送翻译请求，{} 字符",
        Provider::OpenAi.as_str(),
        host_of(&url),
        request.text.chars().count()
    );
    let response = crate::translate::http::send(&HttpRequest {
        method: HttpMethod::Post,
        url,
        headers: vec![
            ("Content-Type".into(), "application/json".into()),
            (
                "Authorization".into(),
                format!("Bearer {}", config.api_key.trim()),
            ),
        ],
        body,
        timeout_secs,
        proxy,
    })?;
    parse_response(response, started)
}

/// 解析响应：状态码非 2xx 先按错误映射，再取译文并校验非空。
fn parse_response(
    response: HttpResponse,
    started: std::time::Instant,
) -> AppResult<TranslateResult> {
    if !(200..300).contains(&response.status) {
        return Err(status_error(
            Provider::OpenAi.as_str(),
            response.status,
            &response.body,
        ));
    }
    let json: serde_json::Value = serde_json::from_str(&response.body).map_err(|err| {
        AppError::Translate(format!(
            "响应不是合法 JSON（接口地址可能不是 chat/completions）: {err}"
        ))
    })?;
    let text = value_as_text(&json, RESPONSE_PATH)?;
    if text.trim().is_empty() {
        return Err(AppError::Translate("模型返回了空译文".into()));
    }
    Ok(TranslateResult {
        text,
        // chat/completions 不回传探测到的源语言；保留 null 让前端不显示
        detected_source: None,
        provider: Provider::OpenAi.as_str().into(),
        elapsed_ms: started.elapsed().as_millis() as i64,
        truncated: false,
    })
}

/// 取 URL 的主机名用于日志（不打印完整 URL，避免把查询参数里的敏感内容记进日志）。
fn host_of(url: &str) -> &str {
    url.split("://")
        .nth(1)
        .and_then(|rest| rest.split('/').next())
        .unwrap_or(url)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> OpenAiProvider {
        OpenAiProvider {
            base_url: "https://api.openai.com/v1".into(),
            api_key: "sk-test".into(),
            model: "gpt-4o-mini".into(),
            system_prompt: String::new(),
            temperature: 0.2,
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
    fn endpoint_normalizes_trailing_slash() {
        assert_eq!(
            endpoint("https://api.openai.com/v1"),
            "https://api.openai.com/v1/chat/completions"
        );
        assert_eq!(
            endpoint("https://api.openai.com/v1/"),
            "https://api.openai.com/v1/chat/completions"
        );
        assert_eq!(
            endpoint("  http://127.0.0.1:11434/v1  "),
            "http://127.0.0.1:11434/v1/chat/completions"
        );
    }

    #[test]
    fn body_uses_default_prompt_when_blank() {
        let body = build_body(&config(), &request("auto", "en"));
        assert_eq!(body["model"], "gpt-4o-mini");
        assert_eq!(body["stream"], false);
        assert_eq!(body["messages"][0]["content"], DEFAULT_SYSTEM_PROMPT);
    }

    #[test]
    fn body_uses_custom_prompt_when_set() {
        let mut config = config();
        config.system_prompt = "只译术语表内的词".into();
        let body = build_body(&config, &request("zh-Hans", "en"));
        assert_eq!(body["messages"][0]["content"], "只译术语表内的词");
    }

    #[test]
    fn body_states_target_language_in_prompt() {
        let body = build_body(&config(), &request("zh-Hans", "en"));
        let user = body["messages"][1]["content"].as_str().unwrap();
        assert!(user.contains("English"), "目标语言应出现在提示词: {user}");
        assert!(
            user.contains("Simplified Chinese"),
            "源语言应出现在提示词: {user}"
        );
        assert!(user.contains("你好"), "原文应包含在提示词中");
    }

    #[test]
    fn body_asks_model_to_detect_source_when_auto() {
        let body = build_body(&config(), &request("auto", "en"));
        let user = body["messages"][1]["content"].as_str().unwrap();
        assert!(
            user.contains("自行判断"),
            "auto 源语言应交由模型判断: {user}"
        );
        assert!(!user.contains("从"), "不应声称已知源语言: {user}");
    }

    #[test]
    fn parse_response_reads_standard_shape() {
        let body = r#"{"choices":[{"message":{"role":"assistant","content":"Hello"}}]}"#;
        let result = parse_response(
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap();
        assert_eq!(result.text, "Hello");
        assert_eq!(result.provider, "openai");
        assert!(result.detected_source.is_none());
    }

    #[test]
    fn parse_response_rejects_non_2xx() {
        let response = HttpResponse {
            status: 401,
            body: r#"{"error":{"message":"Incorrect API key"}}"#.into(),
        };
        let err = parse_response(response, std::time::Instant::now())
            .unwrap_err()
            .to_string();
        assert!(err.contains("鉴权失败"), "{err}");
    }

    #[test]
    fn parse_response_rejects_empty_translation() {
        let body = r#"{"choices":[{"message":{"content":"   "}}]}"#;
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
            host_of("https://api.openai.com/v1/chat/completions"),
            "api.openai.com"
        );
        assert_eq!(host_of("http://127.0.0.1:11434/x"), "127.0.0.1:11434");
    }

    /// **端到端（环回明文）**：走完整 `translate()` 链路，核对实际报文与响应解析。
    #[test]
    fn translate_end_to_end_over_loopback() {
        let body = r#"{"choices":[{"message":{"role":"assistant","content":"Hello there"}}]}"#;
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", body),
        );

        let config = OpenAiProvider {
            base_url: format!("http://127.0.0.1:{port}/v1"),
            ..config()
        };
        let result = translate(&config, &request("zh-Hans", "en"), 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(result.text, "Hello there");
        // 端点：baseUrl 归一后追加 /chat/completions
        assert!(
            received
                .request_line()
                .starts_with("POST /v1/chat/completions HTTP/1."),
            "请求行不符: {}",
            received.request_line()
        );
        // 鉴权：Bearer 头
        assert_eq!(
            received.header("Authorization").as_deref(),
            Some("Bearer sk-test")
        );
        // 请求体：标准 chat/completions 结构，含 system + user 两条消息
        let sent: serde_json::Value =
            serde_json::from_str(received.body()).expect("请求体应是 JSON");
        assert_eq!(sent["model"], "gpt-4o-mini");
        assert_eq!(sent["stream"], false);
        assert_eq!(sent["messages"][0]["role"], "system");
        assert_eq!(sent["messages"][1]["role"], "user");
        // 原文经 JSON 转义后仍可解析，且内容保留
        assert!(
            sent["messages"][1]["content"]
                .as_str()
                .unwrap_or_default()
                .contains("你好"),
            "原文应出现在 user 消息中: {}",
            sent["messages"][1]["content"]
        );
    }

    /// 401 应映射为「鉴权失败」并带出服务端说明。
    #[test]
    fn translate_maps_auth_error_over_loopback() {
        let (port, _server) =
            crate::translate::test_server::spawn(crate::translate::test_server::json_response(
                "HTTP/1.1 401 Unauthorized",
                r#"{"error":{"message":"Incorrect API key provided"}}"#,
            ));
        let config = OpenAiProvider {
            base_url: format!("http://127.0.0.1:{port}/v1"),
            ..config()
        };
        let err = translate(&config, &request("zh-Hans", "en"), 10, None)
            .expect_err("应报错")
            .to_string();
        assert!(err.contains("鉴权失败"), "{err}");
        assert!(err.contains("Incorrect API key"), "应带出服务原文: {err}");
    }

    /// 响应不是 chat/completions 结构时，错误应指出路径而非静默返回空。
    #[test]
    fn translate_reports_unexpected_shape_over_loopback() {
        let (port, _server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", r#"{"ok":true}"#),
        );
        let config = OpenAiProvider {
            base_url: format!("http://127.0.0.1:{port}/v1"),
            ..config()
        };
        let err = translate(&config, &request("zh-Hans", "en"), 10, None)
            .expect_err("应报错")
            .to_string();
        assert!(err.contains("未找到路径"), "{err}");
    }
}
