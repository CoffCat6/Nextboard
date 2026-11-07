//! 核心统一错误类型:nbcore 内部返回 `Result<T, AppError>`,
//! FFI 层经 `to_json()` 序列化为 `{ code, message }` 供宿主提示
//! (沿用 clipboard-plus IPC 错误契约,前端提示逻辑可平移)。

use serde::{ser::SerializeStruct, Serialize, Serializer};

#[derive(Debug, thiserror::Error)]
pub enum AppError {
    #[error("数据库错误:{0}")]
    Db(#[from] rusqlite::Error),
    #[error("序列化错误:{0}")]
    Json(#[from] serde_json::Error),
    #[error("IO 错误:{0}")]
    Io(#[from] std::io::Error),
    // 平台门控:Windows 用 clipboard-win 的 ErrorCode;其他平台用字符串
    // (M1 接入 arboard 后统一为平台无关错误源;错误码保持 "ClipboardWin"
    //  不变,宿主侧提示逻辑无需分平台)。
    #[cfg(windows)]
    #[error("剪贴板错误:{0}")]
    ClipboardWin(#[from] clipboard_win::ErrorCode),
    #[cfg(not(windows))]
    #[error("剪贴板错误:{0}")]
    ClipboardWin(String),
    #[error("图片处理错误:{0}")]
    Image(#[from] image::ImageError),
    #[error("屏幕捕获错误:{0}")]
    Capture(#[from] xcap::XCapError),
    #[error("{0}")]
    Translate(String),
    #[cfg(windows)]
    #[error("系统接口错误:{0}")]
    Windows(#[from] windows::core::Error),
    #[error("无效参数:{0}")]
    InvalidInput(String),
    #[error("内部错误:{0}")]
    Internal(String),
    /// FFI 边界错误:panic 被 catch_unwind 捕获后转换为此变体(02-migration-core.md §5)。
    #[error("核心内部异常:{0}")]
    Panicked(String),
}

impl AppError {
    fn code(&self) -> &'static str {
        match self {
            AppError::Db(_) => "Db",
            AppError::Json(_) => "Json",
            AppError::Io(_) => "Io",
            AppError::ClipboardWin(_) => "ClipboardWin",
            AppError::Image(_) => "Image",
            AppError::Capture(_) => "Capture",
            AppError::Translate(_) => "Translate",
            #[cfg(windows)]
            AppError::Windows(_) => "Windows",
            AppError::InvalidInput(_) => "InvalidInput",
            AppError::Internal(_) => "Internal",
            AppError::Panicked(_) => "Panicked",
        }
    }

    /// FFI 响应体错误部分:`{ "code": "...", "message": "..." }`。
    pub fn to_json(&self) -> serde_json::Value {
        serde_json::json!({ "code": self.code(), "message": self.to_string() })
    }
}

impl Serialize for AppError {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut state = serializer.serialize_struct("AppError", 2)?;
        state.serialize_field("code", self.code())?;
        state.serialize_field("message", &self.to_string())?;
        state.end()
    }
}

pub type AppResult<T> = Result<T, AppError>;
