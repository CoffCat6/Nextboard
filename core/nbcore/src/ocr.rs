//! 截图文字识别（OCR）：调用 Windows 内置 OCR 引擎（`Windows.Media.Ocr`）。
//!
//! 完全离线、无额外运行时依赖，但需要系统装有对应语言的 OCR 语言包
//! （`C:\Windows\OCR\<tag>`；本机实测有 `zh-Hans-CN` 与 `en-US`）。
//!
//! 设计取舍（与用户确认）：**仅提供手动入口**——图片条目右键「识别文字」，
//! 识别结果写入系统剪贴板；不做自动识别入库，因此不涉及数据库迁移与新设置项。

use log::{info, warn};

use crate::error::{AppError, AppResult};

/// 优先尝试的 OCR 语言（简中覆盖中英混排最常见场景）。
const PREFERRED_LANGUAGES: [&str; 2] = ["zh-Hans-CN", "en-US"];

/// OCR 引擎语言选择策略（Phase 6 增强，可自定义）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LanguageChoice {
    /// 自动：按 `zh-Hans-CN` → `en-US` → 用户配置语言 依次尝试。
    Auto,
    /// 指定语言标签（如 `en-US`），失败时回退自动链。
    Fixed(String),
}

impl LanguageChoice {
    /// 解析设置项字符串：空/`auto` 走自动链，其余视为语言标签。
    pub fn parse(value: &str) -> Self {
        let trimmed = value.trim();
        if trimmed.is_empty() || trimmed.eq_ignore_ascii_case("auto") {
            LanguageChoice::Auto
        } else {
            LanguageChoice::Fixed(trimmed.to_owned())
        }
    }
}

/// 识别前的图像预处理（Phase 6 增强）。
///
/// 实测依据（`docs/phase6.md` §3.6）：同一张 12px 代码截图直接识别时 `l/I/1` 与 `0/9`
/// 大量混淆、`MAX_LIMIT` 被读成 `卜1厶X`；**放大 2~3 倍后明显改善**——OCR 对小字号
/// 文本的识别率对有效像素高度非常敏感，放大比换引擎的收益更高，且零外部依赖。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Preprocess {
    /// 放大倍数（1 = 不放大，2/3 = 先插值放大再识别）。
    pub scale: u32,
    /// 是否二值化（按 Otsu 阈值转黑白），对低对比截图/带底纹截图有效。
    pub binarize: bool,
}

impl Default for Preprocess {
    fn default() -> Self {
        // 默认放大 2 倍：对常规截图提升明显，耗时与内存可接受
        Self {
            scale: 2,
            binarize: false,
        }
    }
}

impl Preprocess {
    /// 关闭预处理（对照基准用）。
    #[cfg(test)]
    pub const NONE: Self = Self {
        scale: 1,
        binarize: false,
    };
}

/// OCR 后端选择（Phase 6 增强，可自定义）。
///
/// 系统内置引擎开箱即用但小字号/等宽字体存在固有混淆（见 `docs/phase6.md` §3.6 实测）；
/// 需要更高精度时可切到 Tesseract（需用户自行安装并把 `tesseract` 放进 PATH）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    /// Windows 内置 OCR（离线、零依赖）。
    System,
    /// 外部 Tesseract 进程（需自行安装）。
    Tesseract,
}

impl Backend {
    /// 解析设置项；未知值回退系统引擎并告警。
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "" | "system" | "windows" => Backend::System,
            "tesseract" => Backend::Tesseract,
            other => {
                warn!("未知 OCR 后端「{other}」，回退系统内置引擎");
                Backend::System
            }
        }
    }
}

/// 识别请求参数（设置项 → 引擎）。
#[derive(Debug, Clone)]
pub struct OcrOptions {
    pub backend: Backend,
    pub language: LanguageChoice,
    pub preprocess: Preprocess,
}

impl Default for OcrOptions {
    fn default() -> Self {
        Self {
            backend: Backend::System,
            language: LanguageChoice::Auto,
            preprocess: Preprocess::default(),
        }
    }
}

impl OcrOptions {
    /// 从设置构造（命令层、截图翻译、条目翻译共用，避免三处各写一遍导致漂移）。
    pub fn from_settings(settings: &crate::settings::Settings) -> Self {
        Self {
            backend: Backend::parse(&settings.ocr_backend),
            language: LanguageChoice::parse(&settings.ocr_language),
            preprocess: Preprocess {
                scale: settings.ocr_scale.clamp(1, 4) as u32,
                // 二值化实测有害（丢行），保持关闭（docs/phase6.md §3.6）
                binarize: false,
            },
        }
    }
}

/// 按指定后端／语言／预处理做 OCR（命令层与对比基准共用入口）。
///
/// 收 **owned** RGBA：内部预处理/转换会原地复用这份缓冲（见 [`apply_preprocess`] 与
/// `to_bitmap`），避免整图的多余拷贝；调用方传参后即不再需要该像素。
pub fn recognize_configured(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    options: &OcrOptions,
) -> AppResult<String> {
    match options.backend {
        Backend::System => {
            recognize_with(rgba, width, height, &options.language, options.preprocess)
        }
        Backend::Tesseract => {
            tesseract::recognize(rgba, width, height, &options.language, options.preprocess)
        }
    }
}

/// 后端探测结果（序列化为 camelCase 供设置页展示）。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OcrProbe {
    /// 探测的后端标识（`system` / `tesseract`）。
    pub backend: String,
    /// 该后端是否可用（Tesseract 未安装时为 false）。
    pub available: bool,
    /// 该后端可用的语言标签列表。
    pub languages: Vec<String>,
}

/// 探测指定后端的可用性与语言。
pub fn probe(backend: Backend) -> OcrProbe {
    let (name, available) = match backend {
        Backend::System => ("system", !languages(Backend::System).is_empty()),
        Backend::Tesseract => ("tesseract", tesseract::available()),
    };
    OcrProbe {
        backend: name.to_owned(),
        available,
        languages: languages(backend),
    }
}

/// 列出可用的 OCR 语言标签（当前后端）。
pub fn languages(backend: Backend) -> Vec<String> {
    match backend {
        Backend::System => {
            #[cfg(windows)]
            {
                platform::available_languages()
            }
            #[cfg(not(windows))]
            {
                Vec::new()
            }
        }
        Backend::Tesseract => tesseract::languages(),
    }
}

/// Tesseract 外部进程后端：约定 `tesseract` 在 PATH 中（`tesseract --version` 可用）。
mod tesseract {
    use super::*;

    /// 是否检测到 tesseract 可执行文件。
    pub fn available() -> bool {
        std::process::Command::new("tesseract")
            .arg("--version")
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }

    /// 已安装的语言包（`tesseract --list-langs` 的第二行起）。
    pub fn languages() -> Vec<String> {
        let Ok(output) = std::process::Command::new("tesseract")
            .arg("--list-langs")
            .output()
        else {
            return Vec::new();
        };
        String::from_utf8_lossy(&output.stdout)
            .lines()
            .skip(1)
            .map(str::trim)
            .filter(|line| !line.is_empty())
            .map(str::to_owned)
            .collect()
    }

    /// 语言标签映射：`zh-Hans-CN` → `chi_sim`、`en-US` → `eng`（Tesseract 命名不同）。
    fn to_tesseract_lang(choice: &LanguageChoice) -> Option<String> {
        let tag = match choice {
            LanguageChoice::Auto => return Some("chi_sim+eng".to_owned()),
            LanguageChoice::Fixed(tag) => tag.as_str(),
        };
        let lower = tag.to_ascii_lowercase();
        let mapped = if lower.starts_with("zh") {
            "chi_sim"
        } else if lower.starts_with("en") {
            "eng"
        } else if lower.starts_with("ja") {
            "jpn"
        } else if lower.starts_with("ko") {
            "kor"
        } else {
            return Some(tag.to_owned());
        };
        Some(mapped.to_owned())
    }

    pub(super) fn recognize(
        rgba: Vec<u8>,
        width: u32,
        height: u32,
        language: &LanguageChoice,
        options: Preprocess,
    ) -> AppResult<String> {
        if !available() {
            return Err(AppError::InvalidInput(
                "未找到 tesseract 可执行文件：请安装 Tesseract OCR 并将其加入 PATH（或在设置中切回系统内置引擎）".into(),
            ));
        }
        if width == 0 || height == 0 {
            return Err(AppError::InvalidInput("图片尺寸为空，无法识别".into()));
        }
        // Tesseract 接受 PNG；复用统一的放大/二值预处理
        let (pixels, pw, ph) = apply_preprocess(rgba, width, height, options)?;
        let image = image::RgbaImage::from_raw(pw, ph, pixels)
            .ok_or_else(|| AppError::Internal("预处理后图像重建失败".into()))?;
        let mut png = Vec::new();
        image::DynamicImage::ImageRgba8(image)
            .write_to(&mut std::io::Cursor::new(&mut png), image::ImageFormat::Png)?;

        let dir = std::env::temp_dir();
        let stem = format!("cp-ocr-{}", uuid::Uuid::now_v7());
        let input = dir.join(format!("{stem}.png"));
        let output_base = dir.join(&stem);
        std::fs::write(&input, &png)?;

        let lang = to_tesseract_lang(language).unwrap_or_else(|| "eng".to_owned());
        let result = std::process::Command::new("tesseract")
            .arg(&input)
            .arg(&output_base)
            .arg("-l")
            .arg(&lang)
            .output();
        let _ = std::fs::remove_file(&input);

        let output = match result {
            Ok(output) => output,
            Err(err) => {
                let _ = std::fs::remove_file(output_base.with_extension("txt"));
                return Err(AppError::Internal(format!("调用 tesseract 失败: {err}")));
            }
        };
        let text_path = output_base.with_extension("txt");
        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr).trim().to_owned();
            let _ = std::fs::remove_file(&text_path);
            return Err(AppError::Internal(format!(
                "tesseract 识别失败（语言 {lang}）：{stderr}"
            )));
        }
        let text = std::fs::read_to_string(&text_path).unwrap_or_default();
        let _ = std::fs::remove_file(&text_path);
        info!("OCR(tesseract) 完成：{pw}x{ph}，语言 {lang}");
        Ok(text.trim_end().to_owned())
    }
}

/// 预处理后允许的最大边长（物理像素）：超过则**不再放大**。
///
/// 放大（`ocr_scale`，默认 2）是为**小字号截图**设的：12px 代码截图放大后 OCR 明显
/// 更准（docs/phase6.md §3.6）。但它对大图是净损失：
///   1. 内存：3072x1920 全屏 ×2 → 6144x3840，一份 RGBA 就是 90MB（原图仅 22.5MB），
///      且其后每一步（BGRA 转换 / WinRT buffer / SoftwareBitmap / 引擎灰度）都按 90MB 计；
///   2. 正确性：Windows OCR 的 `MaxImageDimension` 为 10000，5120x2880（5K）×2 = 10240
///      会**直接失败**（见 `platform::recognize` 的尺寸闸），而不是多占内存。
///
/// 故按"预处理后最长边不超过本值"钳制实际倍数：大图退到 1（不放大），
/// 1080p 及以下行为**完全不变**（仍是 ×2）。
pub const MAX_OCR_EDGE: u32 = 4096;

/// 把请求的放大倍数钳到"预处理后最长边 ≤ [`MAX_OCR_EDGE`]"。返回值至少为 1。
fn effective_scale(width: u32, height: u32, requested: u32) -> u32 {
    let longest = width.max(height).max(1);
    let max_scale = (MAX_OCR_EDGE / longest).max(1);
    requested.min(max_scale)
}

/// 统一预处理（两个后端共用）：放大 + 可选 Otsu 二值化。返回 (RGBA, w, h)。
///
/// 收 **owned** 缓冲：不处理时直接沿用（零拷贝），需要处理时也只在必要时新建 ——
/// 调用方（`ocr_entry`）本就不再需要原始像素。
pub(crate) fn apply_preprocess(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    options: Preprocess,
) -> AppResult<(Vec<u8>, u32, u32)> {
    let expected = width as usize * height as usize * 4;
    if rgba.len() != expected {
        return Err(AppError::Internal("RGBA 长度与尺寸不匹配".into()));
    }
    let scale = effective_scale(width, height, options.scale);
    if scale <= 1 && !options.binarize {
        return Ok((rgba, width, height));
    }
    let source = image::RgbaImage::from_raw(width, height, rgba)
        .ok_or_else(|| AppError::Internal("图像重建失败".into()))?;
    let scaled = if scale > 1 {
        // CatmullRom 在放大文本截图时比最近邻/双线性更利于笔画连续性
        image::imageops::resize(
            &source,
            width * scale,
            height * scale,
            image::imageops::FilterType::CatmullRom,
        )
    } else {
        source
    };
    let (sw, sh) = scaled.dimensions();
    let mut raw = scaled.into_raw();
    if options.binarize {
        otsu_binarize(&mut raw);
    }
    Ok((raw, sw, sh))
}

/// Otsu 阈值二值化（原地修改 RGBA）。
fn otsu_binarize(rgba: &mut [u8]) {
    const LEVELS: usize = 256;
    let mut histogram = [0u32; LEVELS];
    let pixels = rgba.len() / 4;
    for chunk in rgba.as_chunks::<4>().0 {
        let gray = luma(chunk);
        histogram[gray as usize] += 1;
    }
    let total_sum: f64 = histogram
        .iter()
        .enumerate()
        .map(|(level, count)| level as f64 * *count as f64)
        .sum();
    let (mut sum_b, mut weight_b, mut best_var, mut threshold) = (0.0f64, 0u32, -1.0f64, 128u8);
    for (level, count) in histogram.iter().enumerate() {
        weight_b += count;
        if weight_b == 0 {
            continue;
        }
        let weight_f = pixels as u32 - weight_b;
        if weight_f == 0 {
            break;
        }
        sum_b += level as f64 * *count as f64;
        let mean_b = sum_b / weight_b as f64;
        let mean_f = (total_sum - sum_b) / weight_f as f64;
        let variance = weight_b as f64 * weight_f as f64 * (mean_b - mean_f) * (mean_b - mean_f);
        if variance > best_var {
            best_var = variance;
            threshold = level as u8;
        }
    }
    for chunk in rgba.as_chunks_mut::<4>().0 {
        let value = if luma(chunk) > threshold as u32 {
            255
        } else {
            0
        };
        chunk[0] = value;
        chunk[1] = value;
        chunk[2] = value;
        chunk[3] = 255;
    }
}

/// BT.601 灰度（整数近似，避免浮点开销）。
fn luma(pixel: &[u8]) -> u32 {
    (299 * pixel[0] as u32 + 587 * pixel[1] as u32 + 114 * pixel[2] as u32) / 1000
}

/// 按指定语言与预处理做 OCR（内置引擎；供命令层与对比基准使用）。
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

/// 对 RGBA 像素做 OCR（自动语言 + 默认预处理），返回按行拼接的文本。
///
/// 阻塞调用（内部等待 WinRT 异步操作完成），须在非主线程执行。
/// 生产代码走 `recognize_configured`（带设置项）；本入口供测试与内置引擎直连使用。
#[cfg_attr(not(test), allow(dead_code))]
pub fn recognize_rgba(rgba: Vec<u8>, width: u32, height: u32) -> AppResult<String> {
    recognize_with(rgba, width, height, &LanguageChoice::Auto, Preprocess::default())
}

/// 折叠 OCR 输出里**中文字符之间**的空白。
///
/// 为什么要做（真机缺陷，Phase 9）：Windows OCR 对中文常输出字间空格
/// （"会 议 纪 要"），原样入库后 FTS 与 LIKE 双通道都搜不到连续词
/// （用户实测"只能搜单字"）。归一化后中文词恢复连续，可正常搜索。
///
/// **只折叠 CJK 邻接的空白**——英文词间空格必须保留，否则 "meeting notes"
/// 会连成 "meetingnotes"，把英文搜索破坏掉。换行同样按此规则处理
/// （行尾中文接行首中文时合并，视觉上属同一段文字）。
pub fn normalize_cjk_spaces(text: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len());
    for (i, &c) in chars.iter().enumerate() {
        if c.is_whitespace() {
            let prev_cjk = i > 0 && is_cjk(chars[i - 1]);
            let next_cjk = chars.get(i + 1).copied().is_some_and(is_cjk);
            if prev_cjk && next_cjk {
                continue; // 中文…中文之间的空白：折叠
            }
        }
        out.push(c);
    }
    out
}

/// 是否为 CJK 统一表意文字（含扩展 A）与兼容表意文字。
fn is_cjk(ch: char) -> bool {
    matches!(ch as u32,
        0x4E00..=0x9FFF        // CJK 统一表意文字
        | 0x3400..=0x4DBF      // 扩展 A
        | 0xF900..=0xFAFF      // 兼容表意文字
    )
}

#[cfg(windows)]
mod platform {
    use super::{PREFERRED_LANGUAGES, *};

    use windows::core::HSTRING;
    use windows::Globalization::Language;
    use windows::Graphics::Imaging::{BitmapPixelFormat, SoftwareBitmap};
    use windows::Media::Ocr::OcrEngine;
    use windows::Security::Cryptography::CryptographicBuffer;

    /// 线程公寓：OCR 的异步操作在 MTA 下才能安全等待，故先 `CoInitializeEx(MTA)`。
    /// 同一线程重复初始化会返回 `RPC_E_CHANGED_MODE`（已由其他租户初始化），可忽略。
    fn ensure_apartment() {
        use windows::Win32::System::Com::{CoInitializeEx, COINIT_MULTITHREADED};
        // SAFETY：仅初始化本线程的 COM 公寓，无其他副作用；失败不影响后续调用。
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        }
    }

    /// 构造 OCR 引擎：按偏好语言逐个尝试，最后回退用户配置语言。
    fn create_engine(choice: &LanguageChoice) -> AppResult<OcrEngine> {
        // 显式指定语言时先试它，失败继续走自动链（避免配错语言直接不可用）
        let mut candidates: Vec<String> = Vec::new();
        if let LanguageChoice::Fixed(tag) = choice {
            candidates.push(tag.clone());
        }
        candidates.extend(PREFERRED_LANGUAGES.iter().map(|s| (*s).to_owned()));
        for tag in candidates {
            let language = Language::CreateLanguage(&HSTRING::from(&tag))?;
            if let Ok(engine) = OcrEngine::TryCreateFromLanguage(&language) {
                info!("OCR 引擎已就绪（{tag}）");
                return Ok(engine);
            }
        }
        let available = available_languages();
        OcrEngine::TryCreateFromUserProfileLanguages().map_err(|_| {
            AppError::Internal(format!(
                "系统未安装可用的 OCR 语言包（可在“设置 → 时间和语言 → 语言和区域”里为语言添加“光学字符识别”功能；当前可用: {}）",
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

    /// 预处理：复用共享实现（放大 + 可选二值化）。
    fn preprocess(
        rgba: Vec<u8>,
        width: u32,
        height: u32,
        options: Preprocess,
    ) -> AppResult<(Vec<u8>, u32, u32)> {
        super::apply_preprocess(rgba, width, height, options)
    }

    pub(super) fn recognize(
        rgba: Vec<u8>,
        width: u32,
        height: u32,
        language: &LanguageChoice,
        preprocess_options: Preprocess,
    ) -> AppResult<String> {
        if width == 0 || height == 0 {
            return Err(AppError::InvalidInput("图片尺寸为空，无法识别".into()));
        }
        // 先做纯参数校验，避免为此启动 OCR 引擎或初始化 COM 公寓
        let expected = width as usize * height as usize * 4;
        if rgba.len() != expected {
            return Err(AppError::Internal("RGBA 长度与尺寸不匹配".into()));
        }
        let (pixels, pw, ph) = preprocess(rgba, width, height, preprocess_options)?;
        let max = OcrEngine::MaxImageDimension()?;
        if pw > max || ph > max {
            return Err(AppError::InvalidInput(format!(
                "图片尺寸 {pw}x{ph} 超出 OCR 上限 {max} 像素，请先裁剪或缩小"
            )));
        }
        ensure_apartment();
        let engine = create_engine(language)?;
        let bitmap = to_bitmap(pixels, pw, ph)?;
        // join() 阻塞等待 WinRT 异步操作（本函数运行在线程池线程上，见 ocr_entry 命令）
        let result = engine.RecognizeAsync(&bitmap)?.join()?;
        let mut lines = Vec::new();
        for line in result.Lines()? {
            lines.push(line.Text()?.to_string_lossy());
        }
        let text = lines.join("\n");
        info!(
            "OCR 完成：{width}x{height}（预处理后 {pw}x{ph}，scale={}，binarize={}），{} 行",
            preprocess_options.scale,
            preprocess_options.binarize,
            lines.len()
        );
        Ok(text)
    }

    /// RGBA → SoftwareBitmap（BGRA8，OCR 要求的输入格式）。
    ///
    /// 收 owned 缓冲并**原地**换 R/B：`pixels` 在预处理后已独占，OCR 也不再需要
    /// 原始 RGBA，故无需再复制一份 BGRA（原先此处 `to_vec()` 会多占一份整图）。
    fn to_bitmap(mut rgba: Vec<u8>, width: u32, height: u32) -> AppResult<SoftwareBitmap> {
        let expected = width as usize * height as usize * 4;
        if rgba.len() != expected {
            return Err(AppError::Internal("RGBA 长度与尺寸不匹配".into()));
        } // OCR 只接受 BGRA8：逐像素换 R/B 通道
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reject_empty_dimensions() {
        assert!(recognize_rgba(vec![], 0, 0).is_err());
    }

    // ---------- CJK 空白归一化（Phase 9：中文 OCR 文本可搜索） ----------

    #[test]
    fn normalize_collapses_spaces_between_cjk() {
        assert_eq!(
            normalize_cjk_spaces("会 议 纪 要"),
            "会议纪要",
            "Windows OCR 的中文字间空格应被折叠"
        );
        assert_eq!(
            normalize_cjk_spaces("项目进度：三项 已完成"),
            "项目进度：三项已完成",
            "标点邻接的中文空白也折叠"
        );
    }

    #[test]
    fn normalize_keeps_spaces_between_latin_words() {
        assert_eq!(
            normalize_cjk_spaces("meeting notes"),
            "meeting notes",
            "英文词间空格必须保留，否则英文搜索被破坏"
        );
        assert_eq!(
            normalize_cjk_spaces("OpenAI GPT-4"),
            "OpenAI GPT-4",
            "纯西文不变"
        );
    }

    #[test]
    fn normalize_mixed_cjk_and_latin() {
        assert_eq!(
            normalize_cjk_spaces("使用 OpenAI 翻译"),
            "使用 OpenAI 翻译",
            "CJK 与西文交界处的空格保留（两侧不同类）"
        );
        assert_eq!(
            normalize_cjk_spaces("会议纪要\n项目进度"),
            "会议纪要项目进度",
            "换行两侧都是中文时折叠（OCR 按行输出，视觉上同段）"
        );
        assert_eq!(
            normalize_cjk_spaces("会议纪要\nmeeting notes"),
            "会议纪要\nmeeting notes",
            "换行一侧是英文时保留"
        );
    }

    #[test]
    fn normalize_edge_cases() {
        assert_eq!(normalize_cjk_spaces(""), "");
        assert_eq!(
            normalize_cjk_spaces("   "),
            "   ",
            "无 CJK 邻接的空白原样保留"
        );
        assert_eq!(normalize_cjk_spaces("会议纪要"), "会议纪要", "无空白则不变");
        assert_eq!(normalize_cjk_spaces("会\n议"), "会议", "中文间的换行也折叠");
    }

    #[test]
    fn reject_size_mismatch() {
        // 长度与尺寸不符：应在进入引擎前就被拒绝
        let err = recognize_rgba(vec![0u8; 16], 8, 8).unwrap_err();
        assert!(matches!(err, AppError::Internal(_)));
    }

    #[test]
    fn backend_and_language_parsing() {
        // 后端：大小写/别名容错，未知值回退系统引擎
        assert_eq!(Backend::parse("system"), Backend::System);
        assert_eq!(Backend::parse("WINDOWS"), Backend::System);
        assert_eq!(Backend::parse("tesseract"), Backend::Tesseract);
        assert_eq!(Backend::parse("  Tesseract "), Backend::Tesseract);
        assert_eq!(Backend::parse("paddle"), Backend::System);
        // 语言：空/auto 走自动链，其余为固定标签
        assert_eq!(LanguageChoice::parse(""), LanguageChoice::Auto);
        assert_eq!(LanguageChoice::parse("AUTO"), LanguageChoice::Auto);
        assert_eq!(
            LanguageChoice::parse("en-US"),
            LanguageChoice::Fixed("en-US".into())
        );
    }

    #[test]
    fn preprocess_default_scales_up_for_small_text() {
        // 默认放大 2 倍（实测 12px 代码截图 2x 可修复 l/1 与下划线混淆）
        let default = Preprocess::default();
        assert_eq!(default.scale, 2);
        assert!(!default.binarize, "二值化实测会丢行，默认关闭");
    }

    #[test]
    fn apply_preprocess_scales_pixels_and_reports_size() {
        // 2x2 图放大 2 倍 → 4x4，且不改变像素数以外的语义
        let rgba = vec![
            10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255,
        ];
        let (out, w, h) = apply_preprocess(rgba.clone(), 2, 2, Preprocess::default()).unwrap();
        assert_eq!((w, h), (4, 4));
        assert_eq!(out.len(), 4 * 4 * 4);

        // scale=1 且不二值化：原样返回
        let (same, w, h) =
            apply_preprocess(rgba.clone(), 2, 2, super::Preprocess::NONE).unwrap();
        assert_eq!((w, h), (2, 2));
        assert_eq!(same, rgba);

        // 长度不符 → 明确报错，不进入引擎
        assert!(apply_preprocess(vec![0u8; 8], 2, 2, Preprocess::default()).is_err());
    }

    // ---------- 自适应放大（内存收口：大图不放大） ----------

    #[test]
    fn effective_scale_leaves_small_images_at_requested() {
        // 1080p 及以下：2x 后最长边仍 ≤ MAX_OCR_EDGE，行为**完全不变**
        assert_eq!(effective_scale(1920, 1080, 2), 2, "1080p 仍放大 2 倍");
        assert_eq!(effective_scale(1280, 720, 2), 2, "720p 仍放大 2 倍");
        // 边界：最长边 2048 × 2 = 4096 = MAX_OCR_EDGE，正好放得下
        assert_eq!(effective_scale(2048, 1152, 2), 2, "2048×2 == 4096，仍在限内");
        // 2049 × 2 = 4098 > 4096 → 退到 1
        assert_eq!(effective_scale(2049, 1152, 2), 1, "超过 4096 则不再放大");
    }

    #[test]
    fn effective_scale_disables_upscale_for_large_screens() {
        // 3072x1920 全屏（本机屏）：×2 会到 6144 > 4096 → 退到 1
        assert_eq!(effective_scale(3072, 1920, 2), 1, "4K 级全屏不放大");
        // 5K：×2 = 10240 会超过 Windows OCR 的 MaxImageDimension(10000) 而直接失败，
        // 退到 1 后 5120 < 10000，可正常识别（修掉现存溢出）
        assert_eq!(effective_scale(5120, 2880, 2), 1, "5K 不放大，避免超出 OCR 上限");
        // 用户请求 3x 时同样受钳制
        assert_eq!(effective_scale(3072, 1920, 3), 1, "请求 3x 的大图同样退到 1");
    }

    #[test]
    fn effective_scale_never_below_one() {
        // 超大图：MAX_OCR_EDGE / longest 下取整为 0，必须夹到 1（不可 0，否则尺寸为 0）
        assert_eq!(effective_scale(10000, 5000, 2), 1);
        assert_eq!(effective_scale(20_000, 20_000, 2), 1, "超过上限也不得退到 0");
        assert_eq!(effective_scale(0, 0, 2), 2, "非法尺寸不触发除零路径");
    }

    #[test]
    fn apply_preprocess_does_not_upscale_large_image() {
        // 3072x1920（> MAX_OCR_EDGE）× 2：应保持原尺寸，而不是 6144x3840
        let (w, h) = (3072u32, 1920u32);
        let rgba = vec![0u8; (w * h * 4) as usize];
        let (out, ow, oh) = apply_preprocess(rgba, w, h, Preprocess::default()).unwrap();
        assert_eq!((ow, oh), (3072, 1920), "大图不放大（内存从 90MB 降到 22.5MB）");
        assert_eq!(out.len(), (w * h * 4) as usize);
    }

    /// OCR 后端探测（诊断用）：打印两种后端在本机的可用性与语言列表。
    ///
    /// `cargo test -- --ignored --nocapture ocr_probe_backends`
    #[test]
    #[ignore = "依赖本机环境，按需显式运行以诊断 OCR 后端可用性"]
    fn ocr_probe_backends() {
        for backend in [Backend::System, Backend::Tesseract] {
            let result = probe(backend);
            println!(
                "后端 {} 可用={} 语言=[{}]",
                result.backend,
                result.available,
                result.languages.join(", ")
            );
        }
    }

    /// 真机质量回归：`OCR_CODE_SAMPLE` 指向等宽代码截图时，断言默认预处理（放大 2 倍）
    /// 能识别出 `build_entry` 的下划线（原图直识别会丢成 `build entry`）。
    ///
    /// `set OCR_CODE_SAMPLE=<png> && cargo test -- --ignored --nocapture ocr_reads_code_underscore`
    #[test]
    #[ignore = "依赖本机 OCR 语言包与样例图，按需显式运行"]
    fn ocr_reads_code_underscore() {
        let path = std::env::var("OCR_CODE_SAMPLE").expect("需设置 OCR_CODE_SAMPLE");
        let img = image::open(&path).expect("样例图读取失败");
        let rgba = img.to_rgba8();
        // 走默认入口 = 系统引擎 + 自动语言 + 放大 2 倍
        let (w, h) = rgba.dimensions();
        let text = recognize_rgba(rgba.into_raw(), w, h).expect("OCR 失败");
        println!("识别结果:\n{text}");
        let normalized: String = text.chars().filter(|c| !c.is_whitespace()).collect();
        assert!(
            normalized.contains("build_entry"),
            "默认预处理未能保留函数名下划线，识别: {text:?}"
        );
    }

    /// 真机 OCR 冒烟：`OCR_SAMPLE` 指向含已知文字的 PNG 时断言能识别出关键片段。
    ///
    /// 默认跳过（未设置环境变量），因为它依赖本机 OCR 语言包：
    /// `set OCR_SAMPLE=<png> && cargo test -- --ignored ocr_reads_known_text`
    #[test]
    #[ignore = "依赖本机 OCR 语言包，按需通过 OCR_SAMPLE 环境变量显式运行"]
    fn ocr_reads_known_text() {
        let path = std::env::var("OCR_SAMPLE")
            .expect("需设置 OCR_SAMPLE 指向含文字的 PNG（参见 docs/phase6.md 的验证步骤）");
        let img = image::open(&path).expect("样例图读取失败");
        let rgba = img.to_rgba8();
        let (w, h) = rgba.dimensions();
        let text = recognize_rgba(rgba.into_raw(), w, h).expect("OCR 失败");
        let normalized: String = text.chars().filter(|c| !c.is_whitespace()).collect();
        assert!(
            normalized.to_uppercase().contains("OCR12345"),
            "未识别出预期文字，实际识别: {text:?}"
        );
    }

    /// 质量对比：对同一批样例图比较「预处理 × 语言」的识别结果（`OCR_BENCH_DIR` 指向目录）。
    ///
    /// `set OCR_BENCH_DIR=<dir> && cargo test -- --ignored --nocapture ocr_quality_bench`
    #[test]
    #[ignore = "质量对比用，按需通过 OCR_BENCH_DIR 环境变量显式运行"]
    fn ocr_quality_bench() {
        let dir = std::env::var("OCR_BENCH_DIR").expect("需设置 OCR_BENCH_DIR");
        let variants = [
            ("原图", Preprocess::NONE, LanguageChoice::Auto),
            (
                "放大2x",
                Preprocess {
                    scale: 2,
                    binarize: false,
                },
                LanguageChoice::Auto,
            ),
            (
                "放大3x",
                Preprocess {
                    scale: 3,
                    binarize: false,
                },
                LanguageChoice::Auto,
            ),
            (
                "放大2x+二值",
                Preprocess {
                    scale: 2,
                    binarize: true,
                },
                LanguageChoice::Auto,
            ),
        ];
        let mut samples: Vec<_> = std::fs::read_dir(&dir)
            .expect("目录读取失败")
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.extension().is_some_and(|ext| ext == "png"))
            .collect();
        samples.sort();
        for path in samples {
            let img = image::open(&path).expect("样例图读取失败");
            let rgba = img.to_rgba8();
            println!("=== {} ===", path.file_name().unwrap().to_string_lossy());
            for (name, options, language) in &variants {
                let started = std::time::Instant::now();
                let text = recognize_with(
                    rgba.clone().into_raw(),
                    rgba.width(),
                    rgba.height(),
                    language,
                    *options,
                )
                .unwrap_or_else(|e| format!("<失败: {e}>"));
                println!("[{name}] ({:?}) {text}", started.elapsed());
            }
        }
    }
}
