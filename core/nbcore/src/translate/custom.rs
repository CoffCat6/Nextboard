//! 自定义 HTTP 提供方（Phase 7 F23）：模板化请求 + 响应取值路径。
//!
//! 设计要点：**同一个占位符在不同上下文必须用不同的转义**，否则会破坏请求结构或引入注入：
//!
//! | 上下文 | 占位符 | 转义方式 | 理由 |
//! | --- | --- | --- | --- |
//! | 请求体（JSON 模板） | `${text}` `${from}` `${to}` `${apiKey}` `${model}` | JSON 字符串转义 | 原文含 `"` `\` 或换行会破坏 JSON 结构 |
//! | 接口地址（查询串） | 同上 | URL percent 编码 | 原文含 `&` `=` `#` 会改变查询语义 |
//! | 请求头 | 同上 | 原样，但剔除 CR/LF | 头值中的换行会造成**响应拆分注入** |
//!
//! 支持 `${name}` 与 `${name:-默认值}` 两种写法，便于给可选字段（如 `${model:-gpt-4o-mini}`）
//! 提供兜底。未知占位符**报错而非静默留空**——静默留空会让用户拿到莫名其妙的服务端报错。

use log::info;

use crate::error::{AppError, AppResult};
use crate::settings::CustomProvider;
use crate::translate::http::{HttpMethod, HttpRequest, HttpResponse};
use crate::translate::{status_error, value_as_text, Provider, TranslateRequest, TranslateResult};

/// 支持的占位符名（用于校验错误提示，与实际取值逻辑 [`Context::value`] 保持一致）。
pub const PLACEHOLDER_NAMES: [&str; 5] = ["text", "from", "to", "apiKey", "model"];

/// 替换值的转义方式：由占位符所处的上下文决定。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Escape {
    /// 请求体（JSON）：按 JSON 字符串内容转义
    Json,
    /// URL 查询串：percent 编码
    Url,
    /// 请求头：剔除 CR/LF（防响应拆分），其余原样
    Header,
}

/// 替换上下文：提供各占位符的取值。
struct Context<'a> {
    request: &'a TranslateRequest,
    api_key: &'a str,
    model: &'a str,
}

impl Context<'_> {
    /// 取占位符对应值；未知名字返回 `None`（由调用方报错）。
    fn value(&self, name: &str) -> Option<&str> {
        match name {
            "text" => Some(&self.request.text),
            "from" => Some(&self.request.from),
            "to" => Some(&self.request.to),
            "apiKey" => Some(self.api_key),
            "model" => Some(self.model),
            _ => None,
        }
    }
}

/// 渲染模板：把 `${name}` / `${name:-默认值}` 替换为按上下文转义后的值。
///
/// 单独抽出并做成纯函数，便于对三种转义与默认值语法做单测。
fn render(template: &str, context: &Context<'_>, escape: Escape) -> AppResult<String> {
    let mut out = String::with_capacity(template.len());
    let mut rest = template;
    while let Some(start) = rest.find("${") {
        out.push_str(&rest[..start]);
        let after = &rest[start + 2..];
        let end = after.find('}').ok_or_else(|| {
            AppError::InvalidInput(format!("模板中的占位符缺少右花括号: {}", truncate(rest)))
        })?;
        let token = &after[..end];
        // `${name:-默认值}` → 名字与默认值
        let (name, fallback) = match token.split_once(":-") {
            Some((name, fallback)) => (name.trim(), Some(fallback)),
            None => (token.trim(), None),
        };
        let raw = match context.value(name) {
            Some(value) if !value.is_empty() => value.to_owned(),
            // 值缺失或为空时用默认值；没写默认值则用空串（如未配置 Key 时的 ${apiKey}）
            _ => fallback.unwrap_or("").to_owned(),
        };
        if context.value(name).is_none() {
            return Err(AppError::InvalidInput(format!(
                "模板中有未知占位符「${{{name}}}」，仅支持 {}",
                PLACEHOLDER_NAMES.map(|n| format!("${{{n}}}")).join(" / ")
            )));
        }
        out.push_str(&apply_escape(&raw, escape));
        rest = &after[end + 1..];
    }
    out.push_str(rest);
    Ok(out)
}

/// 按上下文转义替换值。
fn apply_escape(value: &str, escape: Escape) -> String {
    match escape {
        Escape::Json => {
            // 用 serde 序列化一个字符串并去掉两端引号，即得到 JSON 字符串内容的安全形式
            let quoted = serde_json::Value::String(value.to_owned()).to_string();
            quoted[1..quoted.len() - 1].to_owned()
        }
        Escape::Url => url_encode(value),
        Escape::Header => value.replace(['\r', '\n'], " "),
    }
}

/// 最小 percent 编码：保留 RFC 3986 的 unreserved 字符，其余按 UTF-8 字节编码。
fn url_encode(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for byte in value.as_bytes() {
        let c = *byte as char;
        if c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.' | '~') {
            out.push(c);
        } else {
            out.push('%');
            out.push_str(&format!("{byte:02X}"));
        }
    }
    out
}

/// 解析多行请求头（每行 `Name: Value`）；空行与 `#` 注释行忽略。
fn parse_headers(raw: &str, context: &Context<'_>) -> AppResult<Vec<(String, String)>> {
    let mut headers = Vec::new();
    for (index, line) in raw.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (name, value) = line.split_once(':').ok_or_else(|| {
            AppError::InvalidInput(format!(
                "请求头第 {} 行缺少冒号（应为 Name: Value）: {line}",
                index + 1
            ))
        })?;
        let name = name.trim();
        if name.is_empty() {
            return Err(AppError::InvalidInput(format!(
                "请求头第 {} 行的名称不能为空",
                index + 1
            )));
        }
        headers.push((
            name.to_owned(),
            render(value.trim(), context, Escape::Header)?,
        ));
    }
    Ok(headers)
}

/// 短的模板片段，仅用于错误提示（避免把整段含密钥的模板回显）。
fn truncate(text: &str) -> String {
    text.chars().take(60).collect()
}

/// 发起一次翻译请求。
pub fn translate(
    config: &CustomProvider,
    request: &TranslateRequest,
    timeout_secs: i64,
    proxy: Option<super::http::ProxyConfig>,
) -> AppResult<TranslateResult> {
    let method = parse_method(&config.method)?;
    let context = Context {
        request,
        api_key: config.api_key.trim(),
        model: config.model.trim(),
    };

    let mut headers = parse_headers(&config.headers, &context)?;
    // 仅在用户没自己写 Content-Type 时补默认值，不覆盖用户的显式设置
    if method == HttpMethod::Post
        && !headers
            .iter()
            .any(|(name, _)| name.eq_ignore_ascii_case("content-type"))
    {
        headers.push(("Content-Type".into(), "application/json".into()));
    }

    // URL 上的占位符用 URL 编码替换（GET 场景把参数写进查询串就靠这个）
    let url = render(&config.url, &context, Escape::Url)?;
    let body = if method == HttpMethod::Post {
        render(&config.body_template, &context, Escape::Json)?
    } else {
        String::new()
    };

    let started = std::time::Instant::now();
    info!(
        "已向 {}({}) 发送翻译请求，{} 字符",
        Provider::Custom.as_str(),
        host_of(&url),
        request.text.chars().count()
    );
    let response = crate::translate::http::send(&HttpRequest {
        method,
        url,
        headers,
        body,
        timeout_secs,
        proxy,
    })?;
    parse_response(config, response, started)
}

/// 解析并校验请求方法。
fn parse_method(raw: &str) -> AppResult<HttpMethod> {
    match raw.trim().to_ascii_uppercase().as_str() {
        "" | "POST" => Ok(HttpMethod::Post),
        "GET" => Ok(HttpMethod::Get),
        other => Err(AppError::InvalidInput(format!(
            "自定义提供方只支持 GET 或 POST（当前：{other}）"
        ))),
    }
}

/// 解析响应：非 2xx 映射为可读错误，再按用户给的路径取译文。
fn parse_response(
    config: &CustomProvider,
    response: HttpResponse,
    started: std::time::Instant,
) -> AppResult<TranslateResult> {
    if !(200..300).contains(&response.status) {
        return Err(status_error(
            Provider::Custom.as_str(),
            response.status,
            &response.body,
        ));
    }
    let json: serde_json::Value = serde_json::from_str(&response.body).map_err(|err| {
        AppError::Translate(format!(
            "响应不是合法 JSON，无法按路径取值（若服务返回的是纯文本，请改用支持 JSON 的接口）: {err}"
        ))
    })?;
    let path = config.response_path.trim();
    let text = value_as_text(&json, path)?;
    if text.trim().is_empty() {
        return Err(AppError::Translate("接口返回了空译文".into()));
    }
    Ok(TranslateResult {
        text,
        // 自定义接口没有约定的探测字段，留 null
        detected_source: None,
        provider: Provider::Custom.as_str().into(),
        elapsed_ms: started.elapsed().as_millis() as i64,
        truncated: false,
    })
}

/// 取 URL 主机名用于日志。
fn host_of(url: &str) -> &str {
    url.split("://")
        .nth(1)
        .and_then(|rest| rest.split('/').next())
        .unwrap_or(url)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> TranslateRequest {
        TranslateRequest {
            text: "你好".into(),
            from: "zh-Hans".into(),
            to: "en".into(),
        }
    }

    fn context<'a>(request: &'a TranslateRequest, api_key: &'a str) -> Context<'a> {
        Context {
            request,
            api_key,
            model: "test-model",
        }
    }

    /// 端到端（不含网络）：模板渲染 + 请求头解析的组合路径。
    /// 覆盖"用户按文档填一套 JSON 模板"的真实场景，确认可被服务端解析。
    #[test]
    fn renders_a_realistic_request_template() {
        let req = TranslateRequest {
            text: "他说\"你好\"\n谢谢".into(),
            from: "zh-Hans".into(),
            to: "en".into(),
        };
        let ctx = context(&req, "sk-abc");
        let headers = parse_headers("Authorization: Bearer ${apiKey}", &ctx).unwrap();
        assert_eq!(headers[0].1, "Bearer sk-abc");

        let body = render(
            r#"{"q":"${text}","source":"${from}","target":"${to}"}"#,
            &ctx,
            Escape::Json,
        )
        .unwrap();
        // 关键：原文里的引号与换行不得破坏 JSON 结构
        let parsed: serde_json::Value = serde_json::from_str(&body).unwrap();
        assert_eq!(parsed["q"], "他说\"你好\"\n谢谢");
        assert_eq!(parsed["source"], "zh-Hans");
        assert_eq!(parsed["target"], "en");
    }

    /// GET 场景：参数写在 URL 查询串里，必须做 URL 编码而非 JSON 编码。
    #[test]
    fn renders_get_url_with_url_encoding() {
        let req = TranslateRequest {
            text: "a&b=c".into(),
            from: "auto".into(),
            to: "en".into(),
        };
        let ctx = context(&req, "k");
        let url = render(
            "https://api.example.com/t?q=${text}&to=${to}",
            &ctx,
            Escape::Url,
        )
        .unwrap();
        // `&` 与 `=` 必须被编码，否则查询串语义被改写
        assert!(url.contains("q=a%26b%3Dc"), "{url}");
        assert!(url.ends_with("&to=en"), "{url}");
    }

    #[test]
    fn render_replaces_known_placeholders() {
        let req = request();
        let ctx = context(&req, "k");
        let out = render("q=${text}&from=${from}&to=${to}", &ctx, Escape::Url).unwrap();
        assert_eq!(out, "q=%E4%BD%A0%E5%A5%BD&from=zh-Hans&to=en");
    }

    #[test]
    fn render_escapes_json_special_chars() {
        let req = TranslateRequest {
            text: "he said \"hi\"\nnext\\line".into(),
            from: "en".into(),
            to: "zh-Hans".into(),
        };
        let ctx = context(&req, "k");
        let out = render(r#"{"q":"${text}"}"#, &ctx, Escape::Json).unwrap();
        // 结果必须仍是合法 JSON，且内容与原文一致
        let parsed: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(parsed["q"], "he said \"hi\"\nnext\\line");
    }

    #[test]
    fn render_strips_newlines_in_headers() {
        // 头值中的换行若不剔除，可造成响应拆分注入
        let req = TranslateRequest {
            text: "value\r\nX-Injected: evil".into(),
            from: "auto".into(),
            to: "en".into(),
        };
        let ctx = context(&req, "k");
        let out = render("Bearer ${text}", &ctx, Escape::Header).unwrap();
        assert!(!out.contains('\r'), "不应残留 CR: {out:?}");
        assert!(!out.contains('\n'), "不应残留 LF: {out:?}");
    }

    #[test]
    fn render_supports_default_values() {
        let req = request();
        let ctx = context(&req, "k");
        // model 为空 → 用默认值；apiKey 为空 → 默认值同样生效
        let empty = Context {
            request: &req,
            api_key: "",
            model: "",
        };
        let out = render("${apiKey:-none}/${model:-m1}", &empty, Escape::Json).unwrap();
        assert_eq!(out, "none/m1");
        // 有值时优先取值
        let out = render("${model:-m1}", &ctx, Escape::Json).unwrap();
        assert_eq!(out, "test-model");
    }

    #[test]
    fn context_reads_model_from_settings() {
        // ${model} 必须真的能取到设置里的值（曾被写死为空串导致该占位符失效）
        let req = request();
        let ctx = Context {
            request: &req,
            api_key: "k",
            model: "my-deployment",
        };
        let out = render(r#"{"model":"${model}"}"#, &ctx, Escape::Json).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(parsed["model"], "my-deployment");
    }

    #[test]
    fn render_rejects_unknown_placeholder() {
        let req = request();
        let ctx = context(&req, "k");
        let err = render("${unknown}", &ctx, Escape::Json)
            .unwrap_err()
            .to_string();
        assert!(err.contains("未知占位符"), "{err}");
        assert!(err.contains("unknown"), "应指出具体名字: {err}");
    }

    #[test]
    fn render_rejects_unclosed_placeholder() {
        let req = request();
        let ctx = context(&req, "k");
        assert!(render("${text", &ctx, Escape::Json).is_err());
    }

    #[test]
    fn render_leaves_plain_text_untouched() {
        let req = request();
        let ctx = context(&req, "k");
        let out = render("no placeholders here", &ctx, Escape::Json).unwrap();
        assert_eq!(out, "no placeholders here");
    }

    #[test]
    fn url_encode_keeps_unreserved_and_encodes_rest() {
        assert_eq!(url_encode("abc-_.~"), "abc-_.~");
        assert_eq!(url_encode("a b"), "a%20b");
        assert_eq!(url_encode("&="), "%26%3D");
        // 多字节按 UTF-8 逐字节编码
        assert_eq!(url_encode("中"), "%E4%B8%AD");
    }

    #[test]
    fn parse_headers_reads_lines_and_skips_blanks() {
        let req = request();
        let ctx = context(&req, "secret");
        let headers = parse_headers(
            "# 注释\nAuthorization: Bearer ${apiKey}\n\n  X-Extra : 1  ",
            &ctx,
        )
        .unwrap();
        assert_eq!(headers.len(), 2);
        assert_eq!(headers[0].0, "Authorization");
        assert_eq!(headers[0].1, "Bearer secret");
        assert_eq!(headers[1].0, "X-Extra");
        assert_eq!(headers[1].1, "1");
    }

    #[test]
    fn parse_headers_rejects_missing_colon() {
        let req = request();
        let ctx = context(&req, "k");
        let err = parse_headers("Authorization Bearer x", &ctx)
            .unwrap_err()
            .to_string();
        assert!(err.contains("冒号"), "{err}");
    }

    #[test]
    fn parse_method_defaults_to_post() {
        assert_eq!(parse_method("").unwrap(), HttpMethod::Post);
        assert_eq!(parse_method("post").unwrap(), HttpMethod::Post);
        assert_eq!(parse_method(" GET ").unwrap(), HttpMethod::Get);
        assert!(parse_method("PUT").is_err(), "不支持的动词应拒绝");
    }

    #[test]
    fn parse_response_reads_configured_path() {
        let config = CustomProvider {
            response_path: "data.translations[0].text".into(),
            ..CustomProvider::default()
        };
        let body = r#"{"data":{"translations":[{"text":"Hello"}]}}"#;
        let result = parse_response(
            &config,
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap();
        assert_eq!(result.text, "Hello");
        assert_eq!(result.provider, "custom");
    }

    #[test]
    fn parse_response_rejects_wrong_path() {
        let config = CustomProvider {
            response_path: "data.missing.text".into(),
            ..CustomProvider::default()
        };
        let body = r#"{"data":{}}"#;
        let err = parse_response(
            &config,
            HttpResponse {
                status: 200,
                body: body.into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("未找到路径"), "{err}");
    }

    #[test]
    fn parse_response_rejects_non_json_body() {
        let config = CustomProvider {
            response_path: "text".into(),
            ..CustomProvider::default()
        };
        let err = parse_response(
            &config,
            HttpResponse {
                status: 200,
                body: "plain text answer".into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("不是合法 JSON"), "{err}");
    }

    #[test]
    fn parse_response_rejects_non_2xx() {
        let config = CustomProvider {
            response_path: "text".into(),
            ..CustomProvider::default()
        };
        let err = parse_response(
            &config,
            HttpResponse {
                status: 500,
                body: "boom".into(),
            },
            std::time::Instant::now(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("服务端错误"), "{err}");
    }

    /// **端到端（环回明文）**：走完整 `translate()` 链路，并核对**实际发出的报文**。
    ///
    /// 这是转义设计的最终验证点：单测能确认 `render()` 的输出，但只有真实报文能确认
    /// 它会原样落在请求行/请求头/请求体里。原文特意包含 `"`、`\`、换行、`&`、`=`、
    /// 非 ASCII，任何一种转义用错都会在此暴露（JSON 解析失败 / 查询串被截断 / 注入头）。
    #[test]
    fn translate_end_to_end_over_loopback() {
        // 原文含各类危险字符：引号、反斜杠、换行、查询串分隔符、多字节
        let text = "a\"b\\c\nd&e=f 中文";
        let config = CustomProvider {
            method: "POST".into(),
            headers: "Authorization: Bearer ${apiKey}\nX-Model: ${model}".into(),
            body_template: r#"{"q":"${text}","source":"${from}","target":"${to}"}"#.into(),
            response_path: "data.translations[0].text".into(),
            api_key: "sk-secret".into(),
            model: "m1".into(),
            // 端口稍后替换（先构造配置，再拼 URL——避免默认 url 为空时校验失败）
            url: String::new(),
        };
        let (port, server) =
            crate::translate::test_server::spawn(crate::translate::test_server::json_response(
                "HTTP/1.1 200 OK",
                r#"{"data":{"translations":[{"text":"译文 OK"}]}}"#,
            ));
        let config = CustomProvider {
            url: format!("http://127.0.0.1:{port}/api?from=${{from}}&to=${{to}}"),
            ..config
        };

        let request = TranslateRequest {
            text: text.into(),
            from: "zh-Hans".into(),
            to: "en".into(),
        };
        let result = translate(&config, &request, 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(result.text, "译文 OK");
        assert_eq!(result.provider, "custom");

        // 1) URL 上的占位符走 percent 编码（`&` 不得截断查询串）
        assert!(
            received
                .request_line()
                .starts_with("POST /api?from=zh-Hans&to=en HTTP/1."),
            "请求行不符: {}",
            received.request_line()
        );
        // 2) 请求头：${apiKey} / ${model} 替换且无 CR-LF 注入
        assert_eq!(
            received.header("Authorization").as_deref(),
            Some("Bearer sk-secret")
        );
        assert_eq!(received.header("X-Model").as_deref(), Some("m1"));
        // 3) 请求体：JSON 转义后仍可解析，且原文逐字保留
        let sent: serde_json::Value =
            serde_json::from_str(received.body()).expect("请求体应是合法 JSON（转义正确）");
        assert_eq!(sent["q"], text, "原文应逐字保留");
        assert_eq!(sent["source"], "zh-Hans");
        assert_eq!(sent["target"], "en");
    }

    /// GET 场景端到端：原文进查询串，必须 URL 编码而非 JSON 编码。
    #[test]
    fn translate_get_encodes_query_over_loopback() {
        let config = CustomProvider {
            method: "GET".into(),
            headers: String::new(),
            body_template: String::new(),
            response_path: "text".into(),
            api_key: String::new(),
            model: String::new(),
            url: String::new(),
        };
        let (port, server) = crate::translate::test_server::spawn(
            crate::translate::test_server::json_response("HTTP/1.1 200 OK", r#"{"text":"ok"}"#),
        );
        let config = CustomProvider {
            url: format!("http://127.0.0.1:{port}/t?q=${{text}}"),
            ..config
        };

        let request = TranslateRequest {
            text: "a&b=c d".into(),
            from: "auto".into(),
            to: "en".into(),
        };
        let result = translate(&config, &request, 10, None).expect("应翻译成功");
        let received = server.join().expect("服务端线程应正常结束");

        assert_eq!(result.text, "ok");
        // `&`/`=`/空格必须被编码，否则服务端只会收到 `q=a` 且多出 b=c 参数
        assert!(
            received.request_line().contains("q=a%26b%3Dc%20d"),
            "查询串编码不符: {}",
            received.request_line()
        );
        assert!(
            received.header("Content-Length").is_none(),
            "GET 不应带请求体"
        );
    }
}
