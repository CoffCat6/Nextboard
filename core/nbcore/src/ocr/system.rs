//! Windows 内置 OCR 引擎(`Windows.Media.Ocr`)。
//!
//! 原样迁自 `ocr.rs` 的 `mod platform` 与 `recognize_with` 的 Windows 分支,
//! **零行为变更**(纯搬迁)。

use crate::error::AppResult;

use super::{LanguageChoice, Preprocess};

/// 优先尝试的 OCR 语言(简中覆盖中英混排最常见场景)。
pub(super) const PREFERRED_LANGUAGES: [&str; 2] = ["zh-Hans-CN", "en-US"];

/// 一行识别结果的包围盒(预处理前原图坐标系)。
///
/// 系统引擎没有整行框,只有逐词的 BoundingRect;行框 = 该行全部词框的并集。
#[derive(Debug, Default, Clone)]
pub struct LineRect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl LineRect {
    /// 并入一个词框(取外接矩形)。
    ///
    /// 两个防坑,都是单测钉住的:
    ///   - 零尺寸词框(引擎对空词可能给出)**直接忽略** —— 它不含像素,
    ///     并入只会把矩形拖向 (0,0);
    ///   - self 为空(尚未并入任何词)时,首个有效词框**成为初值** ——
    ///     否则 default 的 (0,0,0,0) 会被当成一个真词框,把并集拖向原点
    ///     (实测:首个词框 (100,50) 并入后得到 (0,0,140,70))。
    fn union(&mut self, x: i32, y: i32, w: u32, h: u32) {
        if w == 0 || h == 0 {
            return;
        }
        if self.width == 0 && self.height == 0 {
            self.x = x;
            self.y = y;
            self.width = w;
            self.height = h;
            return;
        }
        let left = self.x.min(x);
        let top = self.y.min(y);
        let right = (self.x + self.width as i32).max(x + w as i32);
        let bottom = (self.y + self.height as i32).max(y + h as i32);
        self.x = left;
        self.y = top;
        self.width = (right - left).max(0) as u32;
        self.height = (bottom - top).max(0) as u32;
    }
}

/// 按指定语言与预处理做 OCR,并给出**每行在原图中的包围盒**(「图中取字」用)。
///
/// 与 [`recognize_with`] 同一实现路径;纯文本版本只是丢弃行框的薄包装,
/// 两条路径不会漂移。行框已从预处理坐标系**还原到原图物理像素**
/// (除以放大倍数;系统引擎的词框在放大图上)。
pub fn recognize_with_lines(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    language: &LanguageChoice,
    preprocess: Preprocess,
) -> AppResult<(String, Vec<super::OcrLineBox>)> {
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
        platform::recognize(rgba, width, height, language, preprocess).map(|(text, _)| text)
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


#[cfg(test)]
mod tests {
    use super::LineRect;

    /// 行框 = 词框并集:「图中取字」的高亮矩形直接由它决定,并错了框就画歪。
    /// 三个方向各测一词 + 无序输入,钉住"取外接矩形"而不是"取首个词框"。
    #[test]
    fn line_rect_union_takes_bounding_box() {
        let mut r = LineRect::default();
        r.union(100, 50, 40, 20);
        assert_eq!((r.x, r.y, r.width, r.height), (100, 50, 40, 20), "首个词框即初值");

        // 左侧扩展(标点/行首符号常在第一个词框左边);底边 60+18=78 也随之扩展
        r.union(80, 60, 15, 18);
        assert_eq!((r.x, r.y, r.width, r.height), (80, 50, 60, 28));

        // 右下方扩展(词间距大时 Windows 常给出离散词框)
        r.union(160, 75, 30, 22);
        assert_eq!((r.x, r.y, r.width, r.height), (80, 50, 110, 47));
    }

    #[test]
    fn line_rect_union_handles_zero_and_single_pixel() {
        let mut r = LineRect::default();
        r.union(5, 5, 1, 1);
        assert_eq!((r.x, r.y, r.width, r.height), (5, 5, 1, 1));
        // 零尺寸词框(引擎对空词可能给出)被忽略,不得破坏外接矩形
        r.union(0, 0, 0, 0);
        assert_eq!((r.x, r.y, r.width, r.height), (5, 5, 1, 1));
        // 空矩形 + 零尺寸词框 = 仍是空(没有有效词,行框无从谈起)
        let mut empty = LineRect::default();
        empty.union(9, 9, 0, 3);
        assert_eq!((empty.x, empty.y, empty.width, empty.height), (0, 0, 0, 0));
    }
}

#[cfg(windows)]
mod platform {
    use super::super::{LanguageChoice, Preprocess};
    use super::{LineRect, PREFERRED_LANGUAGES};
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
    ) -> AppResult<(String, Vec<super::super::OcrLineBox>)> {
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
        // 放大倍率按**原图尺寸**决定(preprocess::apply_preprocess 内部同式),
        // 这里重算一次,把"放大图上的词框"除回原图坐标(见 OcrLineBox 的坐标系契约)。
        let scale = super::super::preprocess::effective_scale(width, height, preprocess_options.scale) as i32;
        let mut lines = Vec::new();
        let mut boxes = Vec::new();
        for line in result.Lines()? {
            let line_text = line.Text()?.to_string_lossy();
            // 行框 = 该行全部词框的外接矩形(系统引擎没有整行框,只有词级 BoundingRect)。
            let mut rect = LineRect::default();
            for word in line.Words()? {
                let b = word.BoundingRect()?;
                rect.union(b.X as i32, b.Y as i32, b.Width.max(0.0) as u32, b.Height.max(0.0) as u32);
            }
            // 词框在预处理放大后的图上:除以倍率还原到原图;末边向上取整,保住最后一个像素。
            let (bx, by, bw, bh) = if scale > 1 {
                let x0 = rect.x / scale;
                let y0 = rect.y / scale;
                let x1 = (rect.x + rect.width as i32 + scale - 1) / scale;
                let y1 = (rect.y + rect.height as i32 + scale - 1) / scale;
                (x0, y0, (x1 - x0).max(0) as u32, (y1 - y0).max(0) as u32)
            } else {
                (rect.x, rect.y, rect.width, rect.height)
            };
            boxes.push(super::super::OcrLineBox {
                text: line_text.clone(),
                x: bx,
                y: by,
                width: bw,
                height: bh,
            });
            lines.push(line_text);
        }
        let text = lines.join("\n");
        info!(
            "OCR 完成:{width}x{height}(预处理后 {pw}x{ph},scale={},binarize={}),{} 行",
            preprocess_options.scale,
            preprocess_options.binarize,
            lines.len()
        );
        Ok((text, boxes))
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
