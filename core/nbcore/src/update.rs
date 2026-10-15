//! 更新检查（S6.4 v1「检查更新 + 打开下载页」；方案与决策见 docs/07-auto-update.md）。
//!
//! v1 边界：只 GET 一份公开 release JSON 并做语义化比较 —— 不下载、不安装、不常驻定时器。
//!
//! 落点在 core 而不是宿主的 `QNetworkAccessManager`（docs/07 §3 D-a 改判）：传输复用
//! `translate::http`，于是代理三态（直连 / 跟随系统 / 手动含 SOCKS5）、四段超时、4MB
//! 响应上限都是**已实测**的同一条路径；而版本比较与响应解析留在 Rust 侧才能表驱动单测
//! —— 本仓没有 C++ 测试设施。
//!
//! 检查地址由宿主**编译期常量**传入（`NB_UPDATE_CHECK_URL`）。**留空即功能禁用**：
//! 短路返回 `{configured:false}`，既不发请求、也不算错误。

use serde::Serialize;

use crate::core::Core;
use crate::error::{AppError, AppResult};
use crate::translate::http::{self, HttpMethod, HttpRequest, ProxyConfig};

/// 检查超时（秒）。刻意**不**跟随 `translate_timeout_secs`：那是用户为翻译调的长超时，
/// 更新检查失败只该在设置页给一行短提示，不该让人等上一分钟。
const CHECK_TIMEOUT_SECS: i64 = 8;

/// 发布说明截断长度（按**字符**计 —— 汉字 3 字节，按字节切会 panic 在 UTF-8 边界上）。
const NOTES_MAX_CHARS: usize = 400;

/// 一次检查的响应（FFI `data` 形状）。未配置时只有 `configured` 有意义，UI 先看它。
#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct Outcome {
    configured: bool,
    has_update: bool,
    up_to_date: bool,
    current_version: String,
    latest_version: String,
    download_url: String,
    title: String,
    notes: String,
    /// 展示用日期（`YYYY-MM-DD`）；取不到为空（自定义 manifest 通常不带时间）。
    published_at: String,
    checked_at_ms: i64,
}

/// 两种渠道形状归一后的远端发布。
#[derive(Debug)]
struct Release {
    /// 原样 tag（展示时再剥 `v`）。
    version: String,
    /// 已通过 http(s) 白名单；不合规这里是空串（UI 据此决定要不要「打开下载页」）。
    download_url: String,
    title: String,
    notes: String,
    published_at: String,
    /// 预发布：识别但不提示（D-e）。
    prerelease: bool,
}

/// FFI `check_update` 入口（宿主在工作线程调用，UI 线程不碰网络）。
///
/// `current_version` 是宿主的 app 版本（`NB_APP_VERSION`）。nbcore 自己的版本**不参与**
/// 比较 —— 两者节奏不同（实测 app 0.2.0 / core 0.1.0），混进来只会误报。
pub fn check(core: &Core, url: &str, current_version: &str) -> AppResult<serde_json::Value> {
    let settings = core.settings()?;
    // 与 proxy_probe 同一条路径：直连 / 跟随系统 / 手动（HTTP 与 SOCKS5 由 http::send 分派）
    let proxy = ProxyConfig::parse(&settings.proxy.mode, Some(&settings.proxy.manual));
    Ok(serde_json::to_value(outcome(url, current_version, Some(proxy))?)?)
}

/// 完整语义，但不依赖 `Core`：单测可以直接喂环回地址与手工代理配置。
fn outcome(url: &str, current_version: &str, proxy: Option<ProxyConfig>) -> AppResult<Outcome> {
    if url.trim().is_empty() {
        // 空置态：连版本号都不解析 —— "没配置"这件事发生在任何网络与校验之前。
        return Ok(Outcome {
            configured: false,
            has_update: false,
            up_to_date: false,
            current_version: String::new(),
            latest_version: String::new(),
            download_url: String::new(),
            title: String::new(),
            notes: String::new(),
            published_at: String::new(),
            checked_at_ms: crate::now_ms(),
        });
    }
    let current = parse_version(current_version)?;
    let release = fetch_release(url, proxy)?;
    // 远端 tag 同样要能解析：渠道写坏了得当场说清，不能静默成"你已经最新了"
    let latest = parse_version(&release.version).map_err(|_| {
        AppError::Translate(format!("更新源的版本标签无法解析:{}", release.version))
    })?;
    // 预发布标记两处任认其一：渠道给的布尔/后缀判断（release），以及 tag 本身带的 `-`（latest）
    let has_update = is_newer(&current, &latest) && !(release.prerelease || latest.prerelease);
    Ok(Outcome {
        configured: true,
        has_update,
        up_to_date: !has_update,
        current_version: current_version.trim().to_owned(),
        latest_version: display_version(&release.version),
        download_url: release.download_url,
        title: release.title,
        notes: release.notes,
        published_at: release.published_at,
        checked_at_ms: crate::now_ms(),
    })
}

/// 取回并解析远端发布。
///
/// 渠道类型**按响应字段形状**判定（`tag_name` → GitHub Releases；`version` → 自定义
/// manifest），不依赖 URL 后缀 —— 后缀规则会埋一个"忘了写 .json 就当 GitHub 解析"的
/// 隐坑，而两个键名不会互相冒充。
fn fetch_release(url: &str, proxy: Option<ProxyConfig>) -> AppResult<Release> {
    if !is_http_url(url) {
        return Err(AppError::InvalidInput(
            "更新检查地址必须以 http:// 或 https:// 开头".into(),
        ));
    }
    let request = HttpRequest {
        method: HttpMethod::Get,
        url: url.trim().to_owned(),
        headers: vec![("Accept".into(), "application/json".into())],
        body: String::new(),
        timeout_secs: CHECK_TIMEOUT_SECS,
        proxy,
    };
    // 连接层失败（DNS/拒绝/超时/代理）已由 http.rs 带错误码返回可读文案，直接上抛
    let response = http::send(&request)?;
    match response.status {
        200 => {}
        404 => return Err(AppError::Translate("渠道还没有发布版本（404）".into())),
        403 | 429 => {
            return Err(AppError::Translate(
                "检查过于频繁或渠道限流，请稍后再试".into(),
            ))
        }
        other => return Err(AppError::Translate(format!("更新源返回状态码 {other}"))),
    }
    let value: serde_json::Value = serde_json::from_str(&response.body)
        .map_err(|_| AppError::Translate("更新源返回了无法解析的内容".into()))?;
    if value.get("tag_name").is_some() {
        parse_github_latest(&value)
    } else if value.get("version").is_some() {
        parse_manifest(&value)
    } else {
        Err(AppError::Translate(
            "更新源响应缺少 tag_name/version 字段".into(),
        ))
    }
}

/// GitHub `releases/latest`。⚠️ 下载页取 `html_url`：发布渠道只配置一个检查地址，
/// 不存在"检查地址与下载页各写一处、迟早对不上"的问题。
fn parse_github_latest(value: &serde_json::Value) -> AppResult<Release> {
    let tag = field_str(value, "tag_name");
    if tag.is_empty() {
        return Err(AppError::Translate("更新源响应的 tag_name 为空".into()));
    }
    // 预发布两种标记都认：GitHub 的 `prerelease` 布尔字段，以及 tag 里的 `-rc/-beta` 后缀
    let prerelease = value
        .get("prerelease")
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(false)
        || tag.contains('-');
    let name = field_str(value, "name");
    Ok(Release {
        title: if name.is_empty() {
            display_version(&tag)
        } else {
            name
        },
        version: tag,
        download_url: http_url(&field_str(value, "html_url")),
        notes: truncate_chars(&field_str(value, "body")),
        published_at: date_only(&field_str(value, "published_at")),
        prerelease,
    })
}

/// 自定义 manifest：`{"version","url","notes"?, "title"?, "date"?}` 最小集。
fn parse_manifest(value: &serde_json::Value) -> AppResult<Release> {
    let version = field_str(value, "version");
    if version.is_empty() {
        return Err(AppError::Translate("更新源 manifest 的 version 为空".into()));
    }
    let title = field_str(value, "title");
    Ok(Release {
        download_url: http_url(&field_str(value, "url")),
        title: if title.is_empty() {
            display_version(&version)
        } else {
            title
        },
        notes: truncate_chars(&field_str(value, "notes")),
        published_at: date_only(&field_str(value, "date")),
        prerelease: version.contains('-'),
        version,
    })
}

/// 语义化版本：点分**数字**段 + 可选预发布后缀。
#[derive(Debug)]
struct Version {
    numbers: Vec<u64>,
    prerelease: bool,
}

fn parse_version(raw: &str) -> AppResult<Version> {
    let body = display_version(raw);
    if body.is_empty() {
        return Err(AppError::InvalidInput(format!(
            "版本标签无法解析:{raw}(空)"
        )));
    }
    let (numeric, prerelease) = match body.split_once('-') {
        // "1.0.0-" 这种空后缀是写坏的 tag：不当作合法预发布，整串交给数字段解析当场报错
        Some((numeric, pre)) if !pre.is_empty() => (numeric, true),
        Some(_) | None => (body.as_str(), false),
    };
    let mut numbers = Vec::with_capacity(4);
    for segment in numeric.split('.') {
        let parsed = segment.parse::<u64>().map_err(|_| {
            AppError::InvalidInput(format!("版本标签无法解析:{raw}"))
        })?;
        numbers.push(parsed);
    }
    Ok(Version { numbers, prerelease })
}

/// `candidate` 是否比 `current` 更新（段数不足按 0 补齐：`0.2` == `0.2.0`）。
fn is_newer(current: &Version, candidate: &Version) -> bool {
    let len = current.numbers.len().max(candidate.numbers.len());
    for index in 0..len {
        let mine = current.numbers.get(index).copied().unwrap_or(0);
        let theirs = candidate.numbers.get(index).copied().unwrap_or(0);
        if theirs != mine {
            return theirs > mine;
        }
    }
    false
}

/// 展示用版本号：剥掉前导 `v`/`V`。**仅当后跟数字**才剥 —— 否则 `version-1.2` 会被啃掉首字母。
fn display_version(raw: &str) -> String {
    let text = raw.trim();
    match text.strip_prefix(['v', 'V']) {
        Some(rest) if rest.starts_with(|c: char| c.is_ascii_digit()) => rest.to_owned(),
        _ => text.to_owned(),
    }
}

/// 只接受 http(s)。⚠️ 这个白名单是安全约束：响应里的链接最终交给
/// `QDesktopServices::openUrl`，`file:`/`javascript:` 之类必须挡掉。
fn is_http_url(text: &str) -> bool {
    let lower = text.trim().to_ascii_lowercase();
    lower.starts_with("http://") || lower.starts_with("https://")
}

fn http_url(raw: &str) -> String {
    if is_http_url(raw) {
        raw.trim().to_owned()
    } else {
        String::new()
    }
}

/// 从时间串里取展示日期：`2026-10-20T09:00:00Z` → `2026-10-20`；不像日期就返回空。
fn date_only(raw: &str) -> String {
    let text = raw.trim();
    let head = text.split(['T', ' ']).next().unwrap_or("");
    let dated = head.len() == 10
        && head.as_bytes()[4] == b'-'
        && head.as_bytes()[7] == b'-'
        && head.bytes().enumerate().all(|(i, b)| match i {
            4 | 7 => true,
            _ => b.is_ascii_digit(),
        });
    if dated { head.to_owned() } else { String::new() }
}

/// 按字符截断（超长补省略号），保证不切在 UTF-8 中间。
fn truncate_chars(text: &str) -> String {
    let text = text.trim();
    if text.chars().count() <= NOTES_MAX_CHARS {
        return text.to_owned();
    }
    let end = text
        .char_indices()
        .nth(NOTES_MAX_CHARS)
        .map_or(text.len(), |(index, _)| index);
    format!("{}…", &text[..end])
}

fn field_str(value: &serde_json::Value, key: &str) -> String {
    value
        .get(key)
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_owned()
}

// ---------------------------------------------------------------------------
// 测试（纯逻辑表驱动 + 环回端到端；环回是本环境唯一能真实跑完 HTTP 全链路的途径，
// 见 translate/test_server.rs 头注释）
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::translate::test_server;

    #[test]
    fn version_compare_is_numeric_not_lexicographic() {
        assert!(is_newer(
            &parse_version("0.2.0").unwrap(),
            &parse_version("v0.3.0").unwrap()
        ));
        // 字典序会把这条判反 —— 数字段比较存在的理由
        assert!(is_newer(
            &parse_version("0.9.0").unwrap(),
            &parse_version("0.10.0").unwrap()
        ));
        assert!(is_newer(
            &parse_version("1.2.3").unwrap(),
            &parse_version("2.0.0").unwrap()
        ));
        // 段数不足补 0：相等、以及 0.2.1 仍比 0.2 新
        assert!(!is_newer(
            &parse_version("0.2").unwrap(),
            &parse_version("0.2.0").unwrap()
        ));
        assert!(is_newer(
            &parse_version("0.2").unwrap(),
            &parse_version("0.2.1").unwrap()
        ));
        assert!(!is_newer(
            &parse_version("0.3.0").unwrap(),
            &parse_version("0.2.9").unwrap()
        ));
        for bad in ["", "v", "0..1", "0.2.0a", "beta", "1.0.0-"] {
            assert!(parse_version(bad).is_err(), "{bad} 应判为无法解析");
        }
    }

    #[test]
    fn display_version_strips_v_only_before_a_digit() {
        assert_eq!(display_version("v0.3.0"), "0.3.0");
        assert_eq!(display_version("V0.3.0"), "0.3.0");
        assert_eq!(display_version("0.3.0"), "0.3.0");
        // 不是版本号时不能啃掉首字母
        assert_eq!(display_version("version-1.2"), "version-1.2");
        assert_eq!(display_version(""), "");
    }

    #[test]
    fn prerelease_detected_by_suffix() {
        assert!(parse_version("1.0.0-rc.1").unwrap().prerelease);
        assert!(!parse_version("1.0.0").unwrap().prerelease);
    }

    #[test]
    fn empty_check_url_short_circuits_before_parse_and_network() {
        // current_version 故意写坏：若短路没生效，会先报"版本无法解析"而不是 Ok ——
        // 这条断言同时证明"未配置"判定早于解析，也早于任何网络请求。
        let got = outcome("", "不是版本号", None).expect("空置不该是错误");
        assert!(!got.configured);
        assert!(!got.has_update);
        assert!(got.download_url.is_empty());
    }

    #[test]
    fn non_http_check_url_is_rejected() {
        assert!(outcome("file:///c:/windows/win.ini", "0.2.0", None).is_err());
        assert!(outcome("example.com/latest", "0.2.0", None).is_err());
    }

    #[test]
    fn github_latest_shape_parses_fields() {
        let value = serde_json::json!({
            "tag_name": "v0.3.0",
            "name": "Nextboard 0.3.0",
            "html_url": "https://github.com/o/r/releases/tag/v0.3.0",
            "published_at": "2026-10-20T09:00:00Z",
            "body": "新增更新检查",
            "prerelease": false
        });
        let release = parse_github_latest(&value).expect("latest 形状");
        assert_eq!(release.version, "v0.3.0");
        assert_eq!(release.title, "Nextboard 0.3.0");
        assert_eq!(release.download_url, "https://github.com/o/r/releases/tag/v0.3.0");
        assert_eq!(release.published_at, "2026-10-20");
        assert!(!release.prerelease);

        // 布尔字段与后缀两种预发布标记都要认
        let by_flag = serde_json::json!({"tag_name": "v1.0.0", "prerelease": true});
        assert!(parse_github_latest(&by_flag).unwrap().prerelease);
        let by_suffix = serde_json::json!({"tag_name": "v1.0.0-rc.1"});
        assert!(parse_github_latest(&by_suffix).unwrap().prerelease);

        // name 缺失用版本号兜底；tag 空要当场报错（不是"没有更新"）
        let no_name = serde_json::json!({"tag_name": "v0.4.0"});
        assert_eq!(parse_github_latest(&no_name).unwrap().title, "0.4.0");
        assert!(parse_github_latest(&serde_json::json!({"tag_name": ""})).is_err());
    }

    #[test]
    fn manifest_shape_parses_and_drops_unsafe_links() {
        let ok = serde_json::json!({"version": "0.3.0", "url": "https://host/dl", "notes": "n"});
        let release = parse_manifest(&ok).expect("manifest 形状");
        assert_eq!(release.download_url, "https://host/dl");
        assert_eq!(release.title, "0.3.0", "无 title 时用版本号兜底");
        assert_eq!(release.notes, "n");

        // ⚠️ 非 http(s) 的链接必须丢掉：它最终会进 openUrl
        for evil in ["javascript:alert(1)", "file:///c:/", "相对/路径"] {
            let value = serde_json::json!({"version": "0.3.0", "url": evil});
            assert_eq!(
                parse_manifest(&value).unwrap().download_url,
                "",
                "{evil} 应被拒绝"
            );
        }
        assert!(parse_manifest(&serde_json::json!({"url": "https://host"})).is_err());
    }

    #[test]
    fn notes_truncation_is_char_safe() {
        // 汉字 3 字节：按字节切会 panic 在字符边界上
        let long = "汉".repeat(NOTES_MAX_CHARS + 10);
        let cut = truncate_chars(&long);
        assert_eq!(cut.chars().count(), NOTES_MAX_CHARS + 1, "含省略号");
        assert!(cut.ends_with('…'));
        assert_eq!(truncate_chars("短说明"), "短说明");
    }

    #[test]
    fn date_only_accepts_iso_and_rejects_junk() {
        assert_eq!(date_only("2026-10-20T09:00:00Z"), "2026-10-20");
        assert_eq!(date_only("2026-10-20 09:00"), "2026-10-20");
        assert_eq!(date_only(""), "");
        assert_eq!(date_only("最新"), "");
        assert_eq!(date_only("2026-1-20T09:00:00Z"), "");
    }

    #[test]
    fn outcome_fetches_over_loopback_and_reports_update() {
        let body = r#"{"tag_name":"v0.3.0","html_url":"https://example.com/releases/v0.3.0","prerelease":false}"#;
        let (port, server) = test_server::spawn(test_server::json_response("HTTP/1.1 200 OK", body));
        let url = format!("http://127.0.0.1:{port}/latest");

        let got = outcome(&url, "0.2.0", None).expect("环回检查应成功");
        let recorded = server.join().expect("服务端线程应结束");

        assert!(got.configured);
        assert!(got.has_update, "0.3.0 > 0.2.0 应提示更新");
        assert!(!got.up_to_date);
        assert_eq!(got.latest_version, "0.3.0");
        assert_eq!(got.download_url, "https://example.com/releases/v0.3.0");
        // 真发了请求（而不是短路）：服务端收到的请求行就是证据
        assert!(
            recorded.request_line().starts_with("GET /latest"),
            "请求行不符: {}",
            recorded.request_line()
        );
        assert_eq!(recorded.header("Accept").as_deref(), Some("application/json"));
    }

    #[test]
    fn newer_prerelease_does_not_prompt_update() {
        let body = r#"{"tag_name":"v1.0.0-rc.1","html_url":"https://example.com/rc","prerelease":true}"#;
        let (port, server) = test_server::spawn(test_server::json_response("HTTP/1.1 200 OK", body));
        let url = format!("http://127.0.0.1:{port}/latest");

        let got = outcome(&url, "0.2.0", None).expect("环回检查应成功");
        let _ = server.join();

        assert!(!got.has_update, "预发布不该推给用户");
        assert!(got.up_to_date);
        assert_eq!(got.latest_version, "1.0.0-rc.1");
    }

    #[test]
    fn status_and_shape_failures_are_readable() {
        // 404：渠道还没发布
        let (port, server) = test_server::spawn(test_server::json_response(
            "HTTP/1.1 404 Not Found",
            r#"{"message":"Not Found"}"#,
        ));
        let url = format!("http://127.0.0.1:{port}/latest");
        let err = outcome(&url, "0.2.0", None).expect_err("404 应报错");
        assert!(err.to_string().contains("404"), "文案应带状态码: {err}");
        let _ = server.join();

        // 200 但不是 JSON：问题在更新源，要说清
        let (port2, server2) = test_server::spawn(test_server::leaked(
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"
                .to_owned(),
        ));
        let url2 = format!("http://127.0.0.1:{port2}/latest");
        let err = outcome(&url2, "0.2.0", None).expect_err("非 JSON 应报错");
        assert!(err.to_string().contains("无法解析"), "文案应可读: {err}");
        let _ = server2.join();

        // 200 + 两种形状都不认：明确指出缺哪个字段
        let (port3, server3) = test_server::spawn(test_server::json_response(
            "HTTP/1.1 200 OK",
            r#"{"unexpected":true}"#,
        ));
        let url3 = format!("http://127.0.0.1:{port3}/latest");
        let err = outcome(&url3, "0.2.0", None).expect_err("缺字段应报错");
        assert!(err.to_string().contains("tag_name"), "文案应指向字段: {err}");
        let _ = server3.join();
    }

    #[test]
    fn broken_remote_tag_is_reported_not_swallowed() {
        // 渠道写坏了不能说成"你已经最新了"
        let body = r#"{"tag_name":"nightly-build","html_url":"https://example.com/x"}"#;
        let (port, server) = test_server::spawn(test_server::json_response("HTTP/1.1 200 OK", body));
        let url = format!("http://127.0.0.1:{port}/latest");
        let err = outcome(&url, "0.2.0", None).expect_err("坏 tag 应报错");
        assert!(err.to_string().contains("nightly-build"), "文案应带上坏 tag: {err}");
        let _ = server.join();
    }
}
