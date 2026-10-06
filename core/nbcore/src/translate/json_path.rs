//! 响应取值路径的轻量解析与文本提取（Phase 7，F23 复用）。
//!
//! 自写解析而非引入 jsonpath 依赖：只需 `a.b[0].c` 这一档能力，而依赖会带来体积与
//! 版本维护成本（决策 D7 的同一取向）。缺失/越界/类型不符一律给出**指出具体路径**的
//! 错误，便于用户对照自己的接口调整 `responsePath`。

use crate::error::{AppError, AppResult};

/// 从带路径的 JSON 中取值（`choices[0].message.content`）。
pub fn extract_path<'a>(
    value: &'a serde_json::Value,
    path: &str,
) -> AppResult<&'a serde_json::Value> {
    let mut current = value;
    for segment in split_path(path)? {
        current = match segment {
            PathSegment::Key(key) => current.get(&key).ok_or_else(|| {
                AppError::Translate(format!("响应中未找到路径「{path}」（缺字段 {key}）"))
            })?,
            PathSegment::Index(index) => current.get(index).ok_or_else(|| {
                AppError::Translate(format!(
                    "响应中未找到路径「{path}」（数组下标 {index} 越界）"
                ))
            })?,
        };
    }
    Ok(current)
}

/// 路径解析结果的一段。
#[derive(Debug, PartialEq, Eq)]
enum PathSegment {
    Key(String),
    Index(usize),
}

/// 把 `a.b[0].c` 拆成 `Key(a) Key(b) Index(0) Key(c)`。
fn split_path(path: &str) -> AppResult<Vec<PathSegment>> {
    let path = path.trim();
    if path.is_empty() {
        return Err(AppError::InvalidInput("响应取值路径不能为空".into()));
    }
    let mut segments = Vec::new();
    for part in path.split('.') {
        if part.is_empty() {
            return Err(AppError::InvalidInput(format!(
                "响应取值路径格式有误: {path}"
            )));
        }
        // 一段内可能带若干 `[n]`，如 `translations[0]`
        let mut rest = part;
        if let Some(bracket) = rest.find('[') {
            let (key, tail) = rest.split_at(bracket);
            if !key.is_empty() {
                segments.push(PathSegment::Key(key.to_owned()));
            }
            rest = tail;
        } else {
            segments.push(PathSegment::Key(rest.to_owned()));
            continue;
        }
        while !rest.is_empty() {
            let close = rest.find(']').ok_or_else(|| {
                AppError::InvalidInput(format!("响应取值路径的方括号不匹配: {path}"))
            })?;
            if !rest.starts_with('[') {
                return Err(AppError::InvalidInput(format!(
                    "响应取值路径格式有误: {path}"
                )));
            }
            let index_text = &rest[1..close];
            let index: usize = index_text.parse().map_err(|_| {
                AppError::InvalidInput(format!("响应取值路径的数组下标无效: {index_text}"))
            })?;
            segments.push(PathSegment::Index(index));
            rest = &rest[close + 1..];
        }
    }
    if segments.is_empty() {
        return Err(AppError::InvalidInput("响应取值路径不能为空".into()));
    }
    Ok(segments)
}

/// 校验响应取值路径的语法（保存设置时调用，避免翻译时才报错）。
pub fn validate_response_path(path: &str) -> AppResult<()> {
    split_path(path).map(|_| ())
}

/// 把提供方返回的 JSON 对象按路径取出文本。
pub fn value_as_text(value: &serde_json::Value, path: &str) -> AppResult<String> {
    let found = extract_path(value, path)?;
    match found {
        serde_json::Value::String(text) => Ok(text.clone()),
        // 少数兼容实现把 content 包成 [{type:"text",text:"…"}]
        serde_json::Value::Array(items) => {
            let joined: String = items
                .iter()
                .filter_map(|item| {
                    item.get("text")
                        .and_then(|t| t.as_str())
                        .or_else(|| item.as_str())
                })
                .collect::<Vec<_>>()
                .join("");
            if joined.is_empty() {
                Err(AppError::Translate(format!(
                    "响应路径「{path}」指向的数组中没有文本内容"
                )))
            } else {
                Ok(joined)
            }
        }
        other => Err(AppError::Translate(format!(
            "响应路径「{path}」的值不是文本（实际类型：{}）",
            json_type_name(other)
        ))),
    }
}

/// JSON 值的中文类型名，用于错误提示。
pub fn json_type_name(value: &serde_json::Value) -> &'static str {
    match value {
        serde_json::Value::Null => "null",
        serde_json::Value::Bool(_) => "布尔",
        serde_json::Value::Number(_) => "数字",
        serde_json::Value::String(_) => "文本",
        serde_json::Value::Array(_) => "数组",
        serde_json::Value::Object(_) => "对象",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extract_path_walks_objects_and_arrays() {
        let json: serde_json::Value = serde_json::json!({
            "choices": [{ "message": { "content": "译文" } }]
        });
        let found = extract_path(&json, "choices[0].message.content").unwrap();
        assert_eq!(found.as_str(), Some("译文"));
    }

    #[test]
    fn extract_path_reports_missing_segment() {
        let value = serde_json::json!({ "data": {} });
        let err = extract_path(&value, "data.translations[0].text").unwrap_err();
        let message = err.to_string();
        assert!(message.contains("未找到路径"), "错误应指出路径: {message}");
        assert!(
            message.contains("translations"),
            "错误应指出缺失字段: {message}"
        );
    }

    #[test]
    fn extract_path_rejects_malformed_path() {
        let json = serde_json::json!({ "a": 1 });
        assert!(extract_path(&json, "").is_err());
        assert!(extract_path(&json, "a..b").is_err(), "连续点号应拒绝");
        assert!(extract_path(&json, "a[x]").is_err(), "下标非数字应拒绝");
        assert!(extract_path(&json, "a[0").is_err(), "括号不匹配应拒绝");
    }

    #[test]
    fn value_as_text_handles_string_and_array_forms() {
        let json = serde_json::json!({ "t": "纯文本" });
        assert_eq!(value_as_text(&json, "t").unwrap(), "纯文本");

        // 兼容 [{type,text}] 形态
        let json = serde_json::json!({ "t": [{ "text": "A" }, { "text": "B" }] });
        assert_eq!(value_as_text(&json, "t").unwrap(), "AB");

        // 类型不符要报可读错误而不是返回空
        let json = serde_json::json!({ "t": 42 });
        let err = value_as_text(&json, "t").unwrap_err().to_string();
        assert!(err.contains("不是文本"), "错误应说明类型: {err}");
    }
}
