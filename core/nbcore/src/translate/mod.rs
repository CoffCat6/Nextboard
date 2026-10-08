//! 翻译提供方的公共契约与响应解析工具。
//!
//! 平移自 clipboard-plus;差异:翻译历史的存储由第一代的「窗口 Tauri State(内存)」
//! 改为本模块进程级内存队列(`HISTORY`),窗口层(C++)经
//! `translate_history` / `clear_translate_history` 语义的 FFI 方法读写。

pub mod custom;
pub mod http;
pub mod json_path;
pub mod lang;
pub mod microsoft;
pub mod openai;
/// 彩云小译（LingoCloud）。
pub mod caiyun;
// 测试专用环回服务端:仅编译进测试,不进入发布产物
#[cfg(test)]
pub mod test_server;

// 供 settings(校验响应路径语法)与 provider(按路径取译文/探测字段)使用。
pub use json_path::{extract_path, validate_response_path, value_as_text};

use std::sync::Mutex;

use serde::{Deserialize, Serialize};

use crate::error::{AppError, AppResult};

/// 提供方显示名(用于状态条与日志,不含密钥)。
pub const PROVIDER_OPENAI: &str = "openai";
pub const PROVIDER_MICROSOFT: &str = "microsoft";
pub const PROVIDER_CAIYUN: &str = "caiyun";
pub const PROVIDER_CUSTOM: &str = "custom";

/// 已配置的提供方枚举。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Provider {
    OpenAi,
    Microsoft,
    Caiyun,
    Custom,
}

impl Provider {
    /// 解析设置里的字符串;未知值**回退 OpenAI 并告警**,不因配置漂移使功能不可用
    /// (与 `ocrBackend` 的回退策略一致,第一代 docs/phase7.md §2.1)。
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "" | PROVIDER_OPENAI => Provider::OpenAi,
            PROVIDER_MICROSOFT => Provider::Microsoft,
            PROVIDER_CAIYUN => Provider::Caiyun,
            PROVIDER_CUSTOM => Provider::Custom,
            other => {
                log::warn!("未知的翻译提供方「{other}」,回退 OpenAI 兼容");
                Provider::OpenAi
            }
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Provider::OpenAi => PROVIDER_OPENAI,
            Provider::Microsoft => PROVIDER_MICROSOFT,
            Provider::Caiyun => PROVIDER_CAIYUN,
            Provider::Custom => PROVIDER_CUSTOM,
        }
    }
}

/// 一次翻译请求的参数(由 FFI 层构造,与设置解耦,便于单测)。
#[derive(Debug, Clone)]
pub struct TranslateRequest {
    pub text: String,
    /// 源语言标签;`auto` 表示让提供方自动检测。
    pub from: String,
    /// 目标语言标签;不允许 `auto`。
    pub to: String,
}

/// 翻译结果(FFI 返回,契约同第一代 docs/phase7.md §6.1)。
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TranslateResult {
    pub text: String,
    /// 提供方探测到的源语言标签;未探测到为 null。
    pub detected_source: Option<String>,
    pub provider: String,
    pub elapsed_ms: i64,
    /// 预留:分段翻译未启用时恒为 false。
    pub truncated: bool,
}

/// 单次请求的**原文**字符上限(取各提供方中最严的一档:微软 v3.0 为 50000 字符)。
const MAX_INPUT_CHARS: usize = 50_000;

/// 翻译历史条目(第一代 translate/window.rs 的 HistoryItem,契约不变)。
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HistoryItem {
    pub id: String,
    pub source_text: String,
    pub target_text: String,
    pub from: String,
    pub to: String,
    pub provider: String,
    pub created_at: i64,
}

/// 进程级翻译历史:仅内存(第一代决策 D15,不落库),上限 50 条,最新在前。
static HISTORY: Mutex<Vec<HistoryItem>> = Mutex::new(Vec::new());
const HISTORY_LIMIT: usize = 50;

/// 追加一条历史;同"原文+目标语言+提供方"去重(命中则删除旧条目,新条目置顶)。
pub fn push_history(item: HistoryItem) {
    let mut guard = match HISTORY.lock() {
        Ok(g) => g,
        Err(_) => {
            log::warn!("翻译历史锁中毒,跳过记账(不影响翻译结果)");
            return;
        }
    };
    guard.retain(|h| {
        !(h.source_text == item.source_text
            && h.to == item.to
            && h.provider == item.provider)
    });
    guard.insert(0, item);
    guard.truncate(HISTORY_LIMIT);
}

/// 读取全部历史(最新在前)。
pub fn history() -> Vec<HistoryItem> {
    HISTORY.lock().map(|g| g.clone()).unwrap_or_default()
}

/// 清空历史。
pub fn clear_history() {
    if let Ok(mut guard) = HISTORY.lock() {
        guard.clear();
    }
}

/// 按当前设置执行一次翻译(FFI 层唯一入口)。
///
/// 提供方由 `settings.translate_provider` 决定;`from` / `to` 允许调用方覆盖
/// (翻译抽屉内改语言不落库,同第一代 docs/phase7.md §2.1)。
///
/// **本函数不记录翻译历史**:它是无副作用的纯函数且被单测直接调用。
/// 需要记账的调用方请改用 [`translate_and_record`]。
pub fn translate_with_settings(
    settings: &crate::settings::Settings,
    text: &str,
    from: Option<&str>,
    to: Option<&str>,
) -> AppResult<TranslateResult> {
    let request = TranslateRequest {
        text: text.to_owned(),
        from: from
            .map(str::to_owned)
            .unwrap_or_else(|| settings.translate_source_lang.clone()),
        to: to
            .map(str::to_owned)
            .unwrap_or_else(|| settings.translate_target_lang.clone()),
    };
    validate_request(&request)?;
    let proxy = Some(http::ProxyConfig::parse(
        &settings.proxy.mode,
        Some(&settings.proxy.manual),
    ));
    let provider = Provider::parse(&settings.translate_provider);
    let timeout = settings.translate_timeout_secs.clamp(5, 120);
    match provider {
        Provider::OpenAi => openai::translate(&settings.translate_openai, &request, timeout, proxy),
        Provider::Microsoft => {
            microsoft::translate(&settings.translate_microsoft, &request, timeout, proxy)
        }
        Provider::Caiyun => caiyun::translate(&settings.translate_caiyun, &request, timeout, proxy),
        // S3 已落地:模板化请求 + 用户配置的响应取值路径
        Provider::Custom => custom::translate(&settings.translate_custom, &request, timeout, proxy),
    }
}

/// 翻译并记入历史(所有面向用户的翻译入口都应走这里)。
///
/// 存在的意义:翻译有三条入口(翻译抽屉手动、截图翻译、历史条目右键),
/// 若各自调 [`translate_with_settings`] 再各自记账,很容易漏掉其中一条
/// (第一代实际就漏过)。统一在此包装,并把"记账"与"纯翻译"分开——后者保持无副作用、便于单测。
pub fn translate_and_record(
    settings: &crate::settings::Settings,
    text: &str,
    from: Option<&str>,
    to: Option<&str>,
) -> AppResult<TranslateResult> {
    let result = translate_with_settings(settings, text, from, to)?;
    record_history(settings, text, from, to, &result);
    Ok(result)
}

/// 把一次成功翻译写入内存历史;失败仅告警,绝不影响翻译结果本身。
fn record_history(
    settings: &crate::settings::Settings,
    text: &str,
    from: Option<&str>,
    to: Option<&str>,
    result: &TranslateResult,
) {
    let item = HistoryItem {
        id: uuid::Uuid::now_v7().to_string(),
        source_text: text.to_owned(),
        target_text: result.text.clone(),
        // 与 translate_with_settings 的取值口径一致:缺省时用设置里的当前语言
        from: from
            .map(str::to_owned)
            .unwrap_or_else(|| settings.translate_source_lang.clone()),
        to: to
            .map(str::to_owned)
            .unwrap_or_else(|| settings.translate_target_lang.clone()),
        provider: result.provider.clone(),
        created_at: crate::now_ms(),
    };
    push_history(item);
}

/// 请求级校验:空输入与超长都在发请求前拦下(不静默截断)。
fn validate_request(request: &TranslateRequest) -> AppResult<()> {
    if request.text.trim().is_empty() {
        return Err(AppError::InvalidInput("原文为空,无法翻译".into()));
    }
    let chars = request.text.chars().count();
    if chars > MAX_INPUT_CHARS {
        return Err(AppError::InvalidInput(format!(
            "原文 {chars} 字符,超过单次请求上限 {MAX_INPUT_CHARS} 字符,请分段翻译"
        )));
    }
    if !lang::is_valid_tag(&request.from) {
        return Err(AppError::InvalidInput("源语言标签无效".into()));
    }
    if !lang::is_valid_target(&request.to) {
        return Err(AppError::InvalidInput("目标语言标签无效".into()));
    }
    Ok(())
}

/// 探测提供方可用性(设置页「测试」)。
///
/// 只做"配置是否齐备 + 发一次极短请求"两件事:前者能立刻发现漏填,
/// 后者能验证 Key / 地址 / 网络是否真的通。任何失败都返回 `available: false` 与原因,
/// **不抛错**——设置页需要的是展示结论而不是中断操作。
pub fn probe(settings: &crate::settings::Settings, provider: &str) -> TranslateProbe {
    let parsed = Provider::parse(provider);
    let target = settings.translate_target_lang.clone();
    let sample = TranslateRequest {
        text: "hi".into(),
        from: "en".into(),
        to: target,
    };
    let timeout = settings.translate_timeout_secs.clamp(5, 120);
    let proxy = Some(http::ProxyConfig::parse(
        &settings.proxy.mode,
        Some(&settings.proxy.manual),
    ));
    let outcome = match parsed {
        Provider::OpenAi => openai::translate(&settings.translate_openai, &sample, timeout, proxy)
            .map(|_| {
                format!(
                    "可用({} · {})",
                    openai::endpoint(&settings.translate_openai.base_url),
                    settings.translate_openai.model.trim()
                )
            }),
        Provider::Microsoft => {
            microsoft::translate(&settings.translate_microsoft, &sample, timeout, proxy).map(|_| {
                format!(
                    "可用({} · 免密钥)",
                    settings.translate_microsoft.endpoint.trim()
                )
            })
        }
        Provider::Caiyun => caiyun::translate(&settings.translate_caiyun, &sample, timeout, proxy)
            .map(|_| {
                format!(
                    "可用({})",
                    caiyun::endpoint(&settings.translate_caiyun.endpoint)
                )
            }),
        Provider::Custom => custom::translate(&settings.translate_custom, &sample, timeout, proxy)
            .map(|_| {
                format!(
                    "可用({} · 响应路径 {})",
                    settings.translate_custom.url.trim(),
                    settings.translate_custom.response_path.trim()
                )
            }),
    };
    match outcome {
        Ok(message) => TranslateProbe {
            provider: parsed.as_str().into(),
            available: true,
            message,
        },
        Err(err) => TranslateProbe {
            provider: parsed.as_str().into(),
            available: false,
            message: err.to_string(),
        },
    }
}

/// 代理连通性测试(第一代 Phase 8,docs/phase8-proxy.md §7)。
///
/// 按**当前设置里的代理配置**发一次极短请求。目标优先取**当前翻译提供方的真实端点**
/// ——探测的正是用户实际要走的链路(既测代理、也测目标可达性),避免
/// "公开站点被墙/限流导致误报代理故障";拿不到端点时回退公开明文地址。
///
/// **不抛错**:设置页要的是展示结论(连通/耗时/原因),中断操作没有意义。
/// `detail` 不含凭据(docs/phase8-proxy.md §5)。
pub fn proxy_probe(settings: &crate::settings::Settings) -> ProxyProbe {
    use std::time::Instant;

    let started = Instant::now();
    let config = http::ProxyConfig::parse(&settings.proxy.mode, Some(&settings.proxy.manual));
    let detail = match &config {
        http::ProxyConfig::None => "直连(不使用代理)".into(),
        http::ProxyConfig::System => "跟随系统代理".into(),
        http::ProxyConfig::Manual { endpoint, .. } => {
            let kind = match endpoint.kind {
                http::ProxyKind::Http => "HTTP",
                http::ProxyKind::Socks5 => "SOCKS5",
            };
            let auth_mark = if endpoint.auth.is_some() {
                " · 带认证"
            } else {
                ""
            };
            format!("{kind} {}:{}{auth_mark}", endpoint.host, endpoint.port)
        }
    };
    // 探测目标:**当前翻译提供方的真实端点**(探测的正是用户实际要走的链路,
    // 避免"公开站点被墙导致误报代理故障");取不到端点时回退公开明文站
    let target = provider_endpoint(settings).unwrap_or_else(|| "http://neverssl.com/".into());
    let request = http::HttpRequest {
        method: http::HttpMethod::Get,
        url: target,
        headers: Vec::new(),
        body: String::new(),
        timeout_secs: settings.translate_timeout_secs.clamp(5, 120),
        proxy: Some(config),
    };
    let (ok, error) = match http::send(&request) {
        // 4xx 也算"链路通":探测只关心代理与目标之间是否可达,
        // 401/404 说明已打到目标服务(缺 Key 是翻译配置问题,不是代理问题)
        Ok(response) if (200..500).contains(&response.status) => (true, None),
        Ok(response) => (
            false,
            Some(format!("代理连通,但目标返回状态码 {}", response.status)),
        ),
        Err(err) => (false, Some(err.to_string())),
    };
    ProxyProbe {
        ok,
        detail,
        error,
        elapsed_ms: started.elapsed().as_millis() as i64,
    }
}

/// 当前翻译提供方的接口根地址(校验过 scheme 才返回;拿不到返回 `None`)。
fn provider_endpoint(settings: &crate::settings::Settings) -> Option<String> {
    let url = match Provider::parse(&settings.translate_provider) {
        Provider::OpenAi => Some(settings.translate_openai.base_url.trim().to_owned()),
        Provider::Microsoft => Some(settings.translate_microsoft.endpoint.trim().to_owned()),
        Provider::Caiyun => Some(settings.translate_caiyun.endpoint.trim().to_owned()),
        Provider::Custom => {
            let url = settings.translate_custom.url.trim().to_owned();
            (!url.is_empty()).then_some(url)
        }
    }?;
    (url.starts_with("http://") || url.starts_with("https://")).then_some(url)
}

/// `translate_probe` 的返回(镜像第一代 `src/api/types.ts`)。
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TranslateProbe {
    pub provider: String,
    pub available: bool,
    /// 可用时的端点/模型摘要,或失败原因(可直接展示给用户)。
    pub message: String,
}

/// `proxy_probe` 的返回(第一代 Phase 8,镜像 `src/api/types.ts`)。
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProxyProbe {
    /// 代理链路是否可用。
    pub ok: bool,
    /// 实际使用的代理摘要(**不含凭据**),如「SOCKS5 127.0.0.1:1080 · 带认证」。
    pub detail: String,
    /// 失败原因(可读文案,不含凭据);成功为 `null`。
    pub error: Option<String>,
    /// 实测耗时(毫秒)。
    pub elapsed_ms: i64,
}

/// 把 HTTP 状态码与响应体映射为可读错误(错误文案不含请求内容与密钥)。
///
/// 4xx 不重试:鉴权与参数问题重试无意义;具体文案按状态码分派,给出可操作建议。
pub fn status_error(provider: &str, status: u16, body: &str) -> AppError {
    let detail = summarize_error_body(body);
    let message = match status {
        401 | 403 => format!("{provider} 鉴权失败(HTTP {status}),请检查 API Key{detail}"),
        404 => format!("{provider} 接口地址可能有误(HTTP 404){detail}"),
        429 => format!("{provider} 请求过于频繁或额度已用尽(HTTP 429){detail}"),
        _ if status >= 500 => format!("{provider} 服务端错误(HTTP {status}){detail}"),
        _ => format!("{provider} 请求失败(HTTP {status}){detail}"),
    };
    AppError::Translate(message)
}

/// 从错误响应体里提取一段简短说明,便于用户定位(截断且去掉换行)。
fn summarize_error_body(body: &str) -> String {
    let trimmed = body.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    // 优先取 OpenAI 风格的 {"error":{"message":"…"}}
    if let Ok(json) = serde_json::from_str::<serde_json::Value>(trimmed) {
        let candidate = json
            .pointer("/error/message")
            .or_else(|| json.pointer("/error/code"))
            .or_else(|| json.pointer("/message"))
            .and_then(|v| v.as_str());
        if let Some(text) = candidate {
            return format!(":{}", truncate(text, 200));
        }
    }
    format!(":{}", truncate(trimmed, 200))
}

/// 按字符数截断(不切坏 UTF-8),并压掉换行避免撑破提示条。
fn truncate(text: &str, limit: usize) -> String {
    let cleaned: String = text
        .chars()
        .map(|c| if c == '\n' || c == '\r' { ' ' } else { c })
        .collect();
    let mut out: String = cleaned.chars().take(limit).collect();
    if cleaned.chars().count() > limit {
        out.push('…');
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::Settings;

    /// 一个提供方的端到端用例:(名称, 服务端响应体, 按端口构造设置的函数)。
    type ProviderCase = (&'static str, &'static str, fn(u16) -> Settings);

    /// 端到端(环回明文):验证 `translate_with_settings` 的**分派**把三种提供方
    /// 各自接到了正确实现——单测能覆盖 provider 内部,但只有这里能覆盖"设置 → 分派 → 实现"
    /// 这条接线(第一代 openai/microsoft/custom 的分支各写各的,换错分支不会被发现)。
    #[test]
    fn dispatch_routes_each_provider_to_its_implementation() {
        use crate::translate::test_server::{json_response, spawn};

        // 每个提供方一套"能用"的环回配置与各自的响应结构
        let cases: Vec<ProviderCase> = vec![
            (
                "openai",
                r#"{"choices":[{"message":{"content":"openai 译文"}}]}"#,
                |port| Settings {
                    translate_provider: "openai".into(),
                    translate_openai: crate::settings::OpenAiProvider {
                        base_url: format!("http://127.0.0.1:{port}/v1"),
                        api_key: "k".into(),
                        ..Default::default()
                    },
                    ..Default::default()
                },
            ),
            (
                "microsoft",
                r#"[{"translations":[{"text":"microsoft 译文"}]}]"#,
                |port| Settings {
                    translate_provider: "microsoft".into(),
                    translate_microsoft: crate::settings::MicrosoftProvider {
                        endpoint: format!("http://127.0.0.1:{port}"),
                    },
                    ..Default::default()
                },
            ),
            (
                "custom",
                r#"{"data":{"translations":[{"text":"custom 译文"}]}}"#,
                |port| Settings {
                    translate_provider: "custom".into(),
                    translate_custom: crate::settings::CustomProvider {
                        url: format!("http://127.0.0.1:{port}/t"),
                        body_template: r#"{"q":"${text}"}"#.into(),
                        response_path: "data.translations[0].text".into(),
                        ..Default::default()
                    },
                    ..Default::default()
                },
            ),
        ];

        for (name, body, build) in cases {
            let (port, server) = spawn(json_response("HTTP/1.1 200 OK", body));
            let settings = build(port);
            // 配置自身必须能通过校验,否则用户根本存不进去
            settings
                .validate()
                .unwrap_or_else(|err| panic!("{name} 的设置应通过校验: {err}"));

            let result = translate_with_settings(&settings, "你好", Some("zh-Hans"), Some("en"))
                .unwrap_or_else(|err| panic!("{name} 应翻译成功: {err}"));
            let _ = server.join();

            assert_eq!(result.provider, name, "分派到了错误的提供方");
            assert_eq!(result.text, format!("{name} 译文"), "{name} 译文不符");
        }
    }

    /// 需要密钥的通道应由各自实现给出可读错误(而非 panic 或空译文)。
    ///
    /// microsoft 已改为**免授权端点**（默认提供方），不再要求密钥 ——
    /// 它"无密钥也能翻译"由 `translate::microsoft` 的环回端到端用例覆盖。
    #[test]
    fn dispatch_reports_missing_key_for_keyed_provider() {
        let settings = Settings {
            translate_provider: "openai".into(),
            ..Default::default()
        };
        let err = translate_with_settings(&settings, "你好", None, None)
            .expect_err("未配置 Key 应报错")
            .to_string();
        assert!(err.contains("未配置"), "openai 应提示未配置 Key: {err}");
    }

    #[test]
    fn validate_request_rejects_empty_and_overlong_input() {
        let base = Settings::default();
        // 空输入
        let err = translate_with_settings(&base, "   ", None, None)
            .expect_err("空输入应拒绝")
            .to_string();
        assert!(err.contains("原文为空"), "{err}");
        // 超长输入(>50000 字符)应在发请求前拦下
        let long = "a".repeat(50_001);
        let err = translate_with_settings(&base, &long, None, None)
            .expect_err("超长输入应拒绝")
            .to_string();
        assert!(err.contains("超过单次请求上限"), "{err}");
    }

    #[test]
    fn provider_parse_falls_back_to_openai() {
        assert_eq!(Provider::parse("openai"), Provider::OpenAi);
        assert_eq!(Provider::parse("MICROSOFT"), Provider::Microsoft);
        assert_eq!(Provider::parse("custom"), Provider::Custom);
        assert_eq!(Provider::parse(""), Provider::OpenAi);
        // 未知值回退,不 panic
        assert_eq!(Provider::parse("deepl"), Provider::OpenAi);
    }

    #[test]
    fn status_error_maps_common_codes() {
        let auth = status_error("OpenAI", 401, "").to_string();
        assert!(auth.contains("鉴权失败"), "{auth}");
        let missing = status_error("OpenAI", 404, "").to_string();
        assert!(missing.contains("接口地址"), "{missing}");
        let quota = status_error("OpenAI", 429, "").to_string();
        assert!(quota.contains("额度"), "{quota}");
    }

    #[test]
    fn status_error_extracts_api_message() {
        let body = r#"{"error":{"message":"Incorrect API key provided"}}"#;
        let err = status_error("OpenAI", 401, body).to_string();
        assert!(err.contains("Incorrect API key provided"), "{err}");
    }

    #[test]
    fn summarize_truncates_and_flattens() {
        let long = "x".repeat(500);
        let text = summarize_error_body(&long);
        assert!(
            text.chars().count() <= 202,
            "应截断: {}",
            text.chars().count()
        );
        let multiline = summarize_error_body("line1\nline2");
        assert!(!multiline.contains('\n'), "应压掉换行");
    }

    /// 历史去重与置顶(本模块新增行为的位置迁移验证:第一代在窗口 State)。
    #[test]
    fn history_dedups_and_keeps_latest_first() {
        clear_history();
        let mk = |text: &str| HistoryItem {
            id: uuid::Uuid::now_v7().to_string(),
            source_text: text.into(),
            target_text: "t".into(),
            from: "en".into(),
            to: "zh-Hans".into(),
            provider: "openai".into(),
            created_at: crate::now_ms(),
        };
        push_history(mk("a"));
        push_history(mk("b"));
        push_history(mk("a")); // 与第一条同原文+语言+提供方:去重置顶
        let list = history();
        assert_eq!(list.len(), 2);
        assert_eq!(list[0].source_text, "a");
        clear_history();
        assert!(history().is_empty());
    }
}
