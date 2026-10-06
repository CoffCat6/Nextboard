//! 语言标签的统一表示与提供方映射（Phase 7 决策 D11）。
//!
//! 内部统一用一套标签（`auto` / `zh-Hans` / `en` / …），各提供方自行映射：
//! - 微软 Translator v3.0 使用 BCP-47 风格标签，内部标签**原样可用**；
//! - OpenAI 类把标签翻成自然语言名写进提示词（`zh-Hans` → `简体中文`），模型理解更稳；
//! - 自定义提供方按 `${from}` / `${to}` 原样传入，由用户对齐服务方要求。

/// 源语言为「自动检测」时使用的标签。
pub const AUTO: &str = "auto";

/// 语言标签是否合法：字母数字加 `-` / `_`，或 `auto`（仅源语言允许）。
///
/// 与 `settings.rs` 的 OCR 语言校验同一套字符白名单，避免把非法串拼进 URL 查询参数。
pub fn is_valid_tag(tag: &str) -> bool {
    let tag = tag.trim();
    if tag.is_empty() {
        return false;
    }
    if tag.eq_ignore_ascii_case(AUTO) {
        return true;
    }
    tag.chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

/// 目标语言是否合法：不允许 `auto`（译成"自动"没有意义）。
pub fn is_valid_target(tag: &str) -> bool {
    let tag = tag.trim();
    !tag.is_empty() && !tag.eq_ignore_ascii_case(AUTO) && is_valid_tag(tag)
}

/// 语言标签 → 写进提示词的自然语言名；未知标签回退为标签本身。
///
/// 只覆盖常用项 + 少数常见变体，其余交给模型按标签自行理解。
/// （界面可选语言列表在前端 `src/translate/languages.ts`，此处不做展示用映射。）
pub fn english_name_for_prompt(tag: &str) -> String {
    let normalized = tag.trim();
    let name = match normalized.to_ascii_lowercase().as_str() {
        "zh-hans" | "zh-cn" | "zh" => "Simplified Chinese",
        "zh-hant" | "zh-tw" | "zh-hk" => "Traditional Chinese",
        "en" | "en-us" | "en-gb" => "English",
        "ja" | "ja-jp" => "Japanese",
        "ko" | "ko-kr" => "Korean",
        "fr" => "French",
        "de" => "German",
        "es" => "Spanish",
        "ru" => "Russian",
        "pt" => "Portuguese",
        "it" => "Italian",
        "ar" => "Arabic",
        "th" => "Thai",
        "vi" => "Vietnamese",
        other => return other.to_owned(),
    };
    name.to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tag_validation_accepts_language_labels() {
        assert!(is_valid_tag("zh-Hans"));
        assert!(is_valid_tag("en"));
        assert!(is_valid_tag("auto"));
        assert!(is_valid_tag("zh_Hant_TW"), "下划线也允许");
    }

    #[test]
    fn tag_validation_rejects_junk() {
        assert!(!is_valid_tag(""));
        assert!(!is_valid_tag("   "));
        assert!(!is_valid_tag("zh Hans"), "空格非法");
        assert!(!is_valid_tag("zh;rm -rf"), "注入字符非法");
        assert!(!is_valid_tag("a/b"), "斜杠非法（会破坏 URL 参数）");
    }

    #[test]
    fn target_rejects_auto() {
        assert!(!is_valid_target("auto"));
        assert!(!is_valid_target("AUTO"));
        assert!(!is_valid_target(""));
        assert!(is_valid_target("zh-Hans"));
    }

    #[test]
    fn prompt_name_maps_known_tags() {
        assert_eq!(english_name_for_prompt("zh-Hans"), "Simplified Chinese");
        assert_eq!(english_name_for_prompt("EN"), "English");
        assert_eq!(english_name_for_prompt("unknown-tag"), "unknown-tag");
    }
}
