//! Windows 内置 OCR 引擎(`Windows.Media.Ocr`)。
//!
//! 原样迁自 `ocr.rs` 的 `mod platform` 与 `recognize_with` 的 Windows 分支,
//! **零行为变更**(纯搬迁)。

use crate::error::AppResult;

use super::{LanguageChoice, Preprocess};

/// 优先尝试的 OCR 语言(简中覆盖中英混排最常见场景)。
pub(super) const PREFERRED_LANGUAGES: [&str; 2] = ["zh-Hans-CN", "en-US"];

/// 按指定语言与预处理做 OCR(内置引擎;供命令层与对比基准使用)。
pub fn recognize_with(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    language: &LanguageChoice,
    preprocess: Preprocess,
) -> AppResult<String> {
    #[cfg(windows)]
    {
        platform::recognize(rgba, width, height, language, preprocess)
    }
    #[cfg(not(windows))]
    {
        let _ = (rgba, width, height, language, preprocess);
        Err(AppError::Internal("当前平台不支持 OCR".into()))
    }
}

/// 列出可用的 OCR 语言标签(内置引擎)。
pub fn available_languages() -> Vec<String> {
    #[cfg(windows)]
    {
        platform::available_languages()
    }
    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

#[cfg(windows)]
mod platform {
    use super::super::{LanguageChoice, Preprocess};
    use super::PREFERRED_LANGUAGES;
    use crate::error::{AppError, AppResult};

    use log::{info, warn};
    use windows::core::HSTRING;
    use windows::Globalization::Language;
    use windows::Graphics::Imaging::{BitmapPixelFormat, SoftwareBitmap};
    use windows::Media::Ocr::OcrEngine;
    use windows::Security::Cryptography::CryptographicBuffer;

    /// 线程公寓:OCR 的异步操作在 MTA 下才能安全等待,故先 `CoInitializeEx(MTA)`。
    /// 同一线程重复初始化会返回 `RPC_E_CHANGED_MODE`(已由其他租户初始化),可忽略。
    fn ensure_apartment() {
        use windows::Win32::System::Com::{CoInitializeEx, COINIT_MULTITHREADED};
        // SAFETY:仅初始化本线程的 COM 公寓,无其他副作用;失败不影响后续调用。
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        }
    }

    /// 构造 OCR 引擎:按偏好语言逐个尝试,最后回退用户配置语言。
    fn create_engine(choice: &LanguageChoice) -> AppResult<OcrEngine> {
        // 显式指定语言时先试它,失败继续走自动链(避免配错语言直接不可用)
        let mut candidates: Vec<String> = Vec::new();
        if let LanguageChoice::Fixed(tag) = choice {
            candidates.push(tag.clone());
        }
        candidates.extend(PREFERRED_LANGUAGES.iter().map(|s| (*s).to_owned()));
        for tag in candidates {
            let language = Language::CreateLanguage(&HSTRING::from(&tag))?;
            if let Ok(engine) = OcrEngine::TryCreateFromLanguage(&language) {
                info!("OCR 引擎已就绪({tag})");
                return Ok(engine);
            }
        }
        let available = available_languages();
        OcrEngine::TryCreateFromUserProfileLanguages().map_err(|_| {
            AppError::Internal(format!(
                "系统未安装可用的 OCR 语言包(可在“设置 → 时间和语言 → 语言和区域”里为语言添加“光学字符识别”功能;当前可用: {})",
                if available.is_empty() {
                    "无".to_owned()
                } else {
                    available.join(", ")
                }
            ))
        })
    }

    /// 列出本机 OCR 可用语言。
    pub(super) fn available_languages() -> Vec<String> {
        ensure_apartment();
        match OcrEngine::AvailableRecognizerLanguages() {
            Ok(list) => list
                .into_iter()
                .filter_map(|lang| lang.LanguageTag().ok())
                .map(|tag| tag.to_string_lossy())
                .collect(),
            Err(err) => {
                warn!("枚举 OCR 语言失败: {err}");
                Vec::new()
            }
        }
    }

    /// 预处理:复用共享实现(放大 + 可选二值化)。
    fn preprocess(
        rgba: Vec<u8>,
        width: u32,
        height: u32,
        options: Preprocess,
    ) -> AppResult<(Vec<u8>, u32, u32)> {
        super::super::preprocess::apply_preprocess(rgba, width, height, options)
    }

    pub(super) fn recognize(
        rgba: Vec<u8>,
        width: u32,
        height: u32,
        language: &LanguageChoice,
        preprocess_options: Preprocess,
    ) -> AppResult<String> {
        if width == 0 || height == 0 {
            return Err(AppError::InvalidInput("图片尺寸为空,无法识别".into()));
        }
        // 先做纯参数校验,避免为此启动 OCR 引擎或初始化 COM 公寓
        let expected = width as usize * height as usize * 4;
        if rgba.len() != expected {
            return Err(AppError::Internal("RGBA 长度与尺寸不匹配".into()));
        }
        let (pixels, pw, ph) = preprocess(rgba, width, height, preprocess_options)?;
        let max = OcrEngine::MaxImageDimension()?;
        if pw > max || ph > max {
            return Err(AppError::InvalidInput(format!(
                "图片尺寸 {pw}x{ph} 超出 OCR 上限 {max} 像素,请先裁剪或缩小"
            )));
        }
        ensure_apartment();
        let engine = create_engine(language)?;
        let bitmap = to_bitmap(pixels, pw, ph)?;
        // join() 阻塞等待 WinRT 异步操作(本函数运行在线程池线程上,见 ocr_entry 命令)
        let result = engine.RecognizeAsync(&bitmap)?.join()?;
        let mut lines = Vec::new();
        for line in result.Lines()? {
            lines.push(line.Text()?.to_string_lossy());
        }
        let text = lines.join("\n");
        info!(
            "OCR 完成:{width}x{height}(预处理后 {pw}x{ph},scale={},binarize={}),{} 行",
            preprocess_options.scale,
            preprocess_options.binarize,
            lines.len()
        );
        Ok(text)
    }

    /// RGBA → SoftwareBitmap(BGRA8,OCR 要求的输入格式)。
    ///
    /// 收 owned 缓冲并**原地**换 R/B:`pixels` 在预处理后已独占,OCR 也不再需要
    /// 原始 RGBA,故无需再复制一份 BGRA(原先此处 `to_vec()` 会多占一份整图)。
    fn to_bitmap(mut rgba: Vec<u8>, width: u32, height: u32) -> AppResult<SoftwareBitmap> {
        let expected = width as usize * height as usize * 4;
        if rgba.len() != expected {
            return Err(AppError::Internal("RGBA 长度与尺寸不匹配".into()));
        } // OCR 只接受 BGRA8:逐像素换 R/B 通道
        for i in (0..rgba.len()).step_by(4) {
            rgba.swap(i, i + 2);
        }
        let buffer = CryptographicBuffer::CreateFromByteArray(&rgba)?;
        SoftwareBitmap::CreateCopyFromBuffer(
            &buffer,
            BitmapPixelFormat::Bgra8,
            width as i32,
            height as i32,
        )
        .map_err(AppError::from)
    }
}
