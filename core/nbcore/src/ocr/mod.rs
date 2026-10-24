//! 截图文字识别(OCR)。
//!
//! 目录化结构(方案 B 准备步骤,纯搬迁、零行为变更):
//!   - `mod.rs`       公开契约:[Backend] / [OcrOptions] / [recognize_configured] / [probe] / [languages]
//!   - `preprocess.rs` 图像预处理:放大 + 可选 Otsu 二值化
//!   - `system.rs`     Windows 内置 OCR 引擎(`Windows.Media.Ocr`)
//!   - `rapid.rs`      RapidOCR(PP-OCRv4 mobile)纯 Rust 引擎,经 ONNX Runtime
//! 对外契约([recognize_configured] 等)保持不变 —— 宿主 `nb_call` 侧零改动。

use log::warn;

use crate::error::AppResult;

pub(crate) mod preprocess;
// 引擎模块裁减自 rapid-ocr-rs;上游暴露的 API 面远大于本项目所需,
// 未经过的代码与核心算法交织(删起来易伤活跃路径),故允许 dead_code。
// 裁剪清单见 src/ocr/rapid.rs 的模块文档与 docs/11-rapidocr-rewrite-design.md。
#[allow(dead_code, unused_imports)]
pub(crate) mod rapid;
pub mod system;

/// OCR 引擎语言选择策略(Phase 6 增强,可自定义)。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LanguageChoice {
    /// 自动:按 `zh-Hans-CN` → `en-US` → 用户配置语言 依次尝试。
    Auto,
    /// 指定语言标签(如 `en-US`),失败时回退自动链。
    Fixed(String),
}

impl LanguageChoice {
    /// 解析设置项字符串:空/`auto` 走自动链,其余视为语言标签。
    pub fn parse(value: &str) -> Self {
        let trimmed = value.trim();
        if trimmed.is_empty() || trimmed.eq_ignore_ascii_case("auto") {
            LanguageChoice::Auto
        } else {
            LanguageChoice::Fixed(trimmed.to_owned())
        }
    }
}

/// 识别前的图像预处理(Phase 6 增强)。
///
/// 实测依据(docs/phase6.md §3.6):同一张 12px 代码截图直接识别时 `l/I/1` 与 `0/9`
/// 大量混淆、`MAX_LIMIT` 被读成 `卜1厶X`;**放大 2~3 倍后明显改善** —— OCR 对小字号
/// 文本的识别率对有效像素高度非常敏感,放大比换引擎的收益更高,且零外部依赖。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Preprocess {
    /// 放大倍数(1 = 不放大,2/3 = 先插值放大再识别)。
    pub scale: u32,
    /// 是否二值化(按 Otsu 阈值转黑白),对低对比截图/带底纹截图有效。
    pub binarize: bool,
}

impl Default for Preprocess {
    fn default() -> Self {
        // 默认放大 2 倍:对常规截图提升明显,耗时与内存可接受
        Self {
            scale: 2,
            binarize: false,
        }
    }
}

impl Preprocess {
    /// 关闭预处理(对照基准用)。
    #[cfg(test)]
    pub const NONE: Self = Self {
        scale: 1,
        binarize: false,
    };
}

/// OCR 后端选择(Phase 6 增强,可自定义)。
///
/// 目前仅有系统内置引擎:开箱即用、零依赖,但小字号/等宽字体存在固有混淆
/// (见 `docs/phase6.md` §3.6 实测)。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    /// Windows 内置 OCR(离线、零依赖)。
    System,
    /// RapidOCR(PP-OCRv4 mobile):自带模型的纯 Rust 引擎,不依赖系统语言包。
    RapidOcr,
}

impl Backend {
    /// 解析设置项;未知值回退系统引擎并告警。
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "" | "system" | "windows" => Backend::System,
            "rapidocr" | "rapid" => Backend::RapidOcr,
            other => {
                warn!("未知 OCR 后端「{other}」,回退系统内置引擎");
                Backend::System
            }
        }
    }
}

/// 识别请求参数(设置项 → 引擎)。
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
    /// 从设置构造(命令层、截图翻译、条目翻译共用,避免三处各写一遍导致漂移)。
    pub fn from_settings(settings: &crate::settings::Settings) -> Self {
        Self {
            backend: Backend::parse(&settings.ocr_backend),
            language: LanguageChoice::parse(&settings.ocr_language),
            preprocess: Preprocess {
                scale: settings.ocr_scale.clamp(1, 4) as u32,
                // 二值化实测有害(丢行),保持关闭(docs/phase6.md §3.6)
                binarize: false,
            },
        }
    }
}

/// OCR 识别出的**一行**文字及其在原图中的包围盒(「图中取字」的数据基础)。
///
/// 坐标系:**原图物理像素**(预处理放大之前的输入图),左上角原点 ——
/// 宿主拿到的截图 QImage 就是这个尺寸,拿到框即可直接换算显示,
/// 不需要知道 ocr_scale 做过几倍放大(两个引擎都负责还原,见各自实现)。
#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct OcrLineBox {
    /// 该行文本(与整体文本按换行拆分后的顺序一一对应)。
    pub text: String,
    /// 包围盒左上角与宽高(原图物理像素)。
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

/// [`recognize_configured`] 的**带行框**版本:整体文本 + 每行包围盒。
///
/// 「图中取字」(识别面板悬停某行 → 高亮原图对应区域)需要每行的 bbox;
/// 纯文本入口 [`recognize_configured`] 保持签名不动,既有调用方零感知。
/// 返回的 text 与 OcrLineBox 列表按同一顺序一一对应。
pub fn recognize_configured_lines(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    options: &OcrOptions,
) -> AppResult<(String, Vec<OcrLineBox>)> {
    match options.backend {
        Backend::System => {
            system::recognize_with_lines(rgba, width, height, &options.language, options.preprocess)
        }
        // RapidOCR 自带 max_side_len,不应用 ocr_scale 放大;中英混排一体,不分语言。
        Backend::RapidOcr => {
            rapid::recognize_with_lines(rgba, width, height, &options.language, options.preprocess)
        }
    }
}

/// 按指定后端/语言/预处理做 OCR(命令层与对比基准共用入口)。
///
/// 收 **owned** RGBA:内部预处理/转换会原地复用这份缓冲(见 `apply_preprocess` 与
/// `to_bitmap`),避免整图的多余拷贝;调用方传参后即不再需要该像素。
pub fn recognize_configured(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    options: &OcrOptions,
) -> AppResult<String> {
    match options.backend {
        Backend::System => {
            system::recognize_with(rgba, width, height, &options.language, options.preprocess)
        }
        // RapidOCR 自带 max_side_len,不应用 ocr_scale 放大;中英混排一体,不分语言。
        Backend::RapidOcr => {
            rapid::recognize_with(rgba, width, height, &options.language, options.preprocess)
        }
    }
}

/// 后端探测结果(序列化为 camelCase 供设置页展示)。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OcrProbe {
    /// 探测的后端标识(`system`)。
    pub backend: String,
    /// 该后端是否可用(未装系统 OCR 语言包时为 false)。
    pub available: bool,
    /// 该后端可用的语言标签列表。
    pub languages: Vec<String>,
    /// 不可用时的**可读原因**(缺 DLL / 缺模型 / 缺语言包);可用时为 None。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

/// 探测指定后端的可用性与语言。
pub fn probe(backend: Backend) -> OcrProbe {
    let (name, available, reason) = match backend {
        Backend::System => {
            let langs = languages(Backend::System);
            let reason = if langs.is_empty() {
                Some("未安装系统 OCR 语言包(设置 → 时间和语言 → 语言和区域)".to_owned())
            } else {
                None
            };
            ("system", !langs.is_empty(), reason)
        }
        Backend::RapidOcr => {
            let (ok, reason) = rapid::probe_available();
            ("rapidocr", ok, reason)
        }
    };
    OcrProbe {
        backend: name.to_owned(),
        available,
        languages: languages(backend),
        reason,
    }
}

/// 列出可用的 OCR 语言标签(当前后端)。
pub fn languages(backend: Backend) -> Vec<String> {
    match backend {
        Backend::System => system::available_languages(),
        // RapidOCR PP-OCRv4 mobile 中英混排一体,不按语言标签区分。
        Backend::RapidOcr => Vec::new(),
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
    system::recognize_with(rgba, width, height, language, preprocess)
}

/// 对 RGBA 像素做 OCR(自动语言 + 默认预处理),返回按行拼接的文本。
///
/// 阻塞调用(内部等待 WinRT 异步操作完成),须在非主线程执行。
/// 生产代码走 `recognize_configured`(带设置项);本入口供测试与内置引擎直连使用。
#[cfg_attr(not(test), allow(dead_code))]
pub fn recognize_rgba(rgba: Vec<u8>, width: u32, height: u32) -> AppResult<String> {
    recognize_with(
        rgba,
        width,
        height,
        &LanguageChoice::Auto,
        Preprocess::default(),
    )
}

/// 折叠 OCR 输出里**中文字符之间**的空白。
///
/// 为什么要做(真机缺陷,Phase 9):Windows OCR 对中文常输出字间空格
/// ("会 议 纪 要"),原样入库后 FTS 与 LIKE 双通道都搜不到连续词
/// (用户实测"只能搜单字")。归一化后中文词恢复连续,可正常搜索。
///
/// **只折叠 CJK 邻接的空白** —— 英文词间空格必须保留,否则 "meeting notes"
/// 会连成 "meetingnotes",把英文搜索破坏掉。换行同样按此规则处理
/// (行尾中文接行首中文时合并,视觉上属同一段文字)。
pub fn normalize_cjk_spaces(text: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len());
    for (i, &c) in chars.iter().enumerate() {
        if c.is_whitespace() {
            let prev_cjk = i > 0 && is_cjk(chars[i - 1]);
            let next_cjk = chars.get(i + 1).copied().is_some_and(is_cjk);
            if prev_cjk && next_cjk {
                continue; // 中文…中文之间的空白:折叠
            }
        }
        out.push(c);
    }
    out
}

/// 是否为 CJK 统一表意文字(含扩展 A)与兼容表意文字。
fn is_cjk(ch: char) -> bool {
    matches!(ch as u32,
        0x4E00..=0x9FFF        // CJK 统一表意文字
        | 0x3400..=0x4DBF      // 扩展 A
        | 0xF900..=0xFAFF      // 兼容表意文字
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::AppError;
    use preprocess::{apply_preprocess, effective_scale};

    #[test]
    fn reject_empty_dimensions() {
        assert!(recognize_rgba(vec![], 0, 0).is_err());
    }

    // ---------- CJK 空白归一化(Phase 9:中文 OCR 文本可搜索) ----------

    #[test]
    fn normalize_collapses_spaces_between_cjk() {
        assert_eq!(
            normalize_cjk_spaces("会 议 纪 要"),
            "会议纪要",
            "Windows OCR 的中文字间空格应被折叠"
        );
        assert_eq!(
            normalize_cjk_spaces("项目进度:三项 已完成"),
            "项目进度:三项已完成",
            "标点邻接的中文空白也折叠"
        );
    }

    #[test]
    fn normalize_keeps_spaces_between_latin_words() {
        assert_eq!(
            normalize_cjk_spaces("meeting notes"),
            "meeting notes",
            "英文词间空格必须保留,否则英文搜索被破坏"
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
            "CJK 与西文交界处的空格保留(两侧不同类)"
        );
        assert_eq!(
            normalize_cjk_spaces("会议纪要\n项目进度"),
            "会议纪要项目进度",
            "换行两侧都是中文时折叠(OCR 按行输出,视觉上同段)"
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
        // 长度与尺寸不符:应在进入引擎前就被拒绝
        let err = recognize_rgba(vec![0u8; 16], 8, 8).unwrap_err();
        assert!(matches!(err, AppError::Internal(_)));
    }

    #[test]
    fn backend_and_language_parsing() {
        // 后端:大小写/别名容错,未知值回退系统引擎
        assert_eq!(Backend::parse("system"), Backend::System);
        assert_eq!(Backend::parse("WINDOWS"), Backend::System);
        assert_eq!(Backend::parse("  windows "), Backend::System);
        assert_eq!(Backend::parse("paddle"), Backend::System);
        // 语言:空/auto 走自动链,其余为固定标签
        assert_eq!(LanguageChoice::parse(""), LanguageChoice::Auto);
        assert_eq!(LanguageChoice::parse("AUTO"), LanguageChoice::Auto);
        assert_eq!(
            LanguageChoice::parse("en-US"),
            LanguageChoice::Fixed("en-US".into())
        );
    }

    #[test]
    fn preprocess_default_scales_up_for_small_text() {
        // 默认放大 2 倍(实测 12px 代码截图 2x 可修复 l/1 与下划线混淆)
        let default = Preprocess::default();
        assert_eq!(default.scale, 2);
        assert!(!default.binarize, "二值化实测会丢行,默认关闭");
    }

    #[test]
    fn apply_preprocess_scales_pixels_and_reports_size() {
        // 2x2 图放大 2 倍 → 4x4,且不改变像素数以外的语义
        let rgba = vec![
            10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255,
        ];
        let (out, w, h) = apply_preprocess(rgba.clone(), 2, 2, Preprocess::default()).unwrap();
        assert_eq!((w, h), (4, 4));
        assert_eq!(out.len(), 4 * 4 * 4);

        // scale=1 且不二值化:原样返回
        let (same, w, h) = apply_preprocess(rgba.clone(), 2, 2, Preprocess::NONE).unwrap();
        assert_eq!((w, h), (2, 2));
        assert_eq!(same, rgba);

        // 长度不符 → 明确报错,不进入引擎
        assert!(apply_preprocess(vec![0u8; 8], 2, 2, Preprocess::default()).is_err());
    }

    // ---------- 自适应放大(内存收口:大图不放大) ----------

    #[test]
    fn effective_scale_leaves_small_images_at_requested() {
        // 1080p 及以下:2x 后最长边仍 ≤ MAX_OCR_EDGE,行为**完全不变**
        assert_eq!(effective_scale(1920, 1080, 2), 2, "1080p 仍放大 2 倍");
        assert_eq!(effective_scale(1280, 720, 2), 2, "720p 仍放大 2 倍");
        // 边界:最长边 2048 × 2 = 4096 = MAX_OCR_EDGE,正好放得下
        assert_eq!(effective_scale(2048, 1152, 2), 2, "2048×2 == 4096,仍在限内");
        // 2049 × 2 = 4098 > 4096 → 退到 1
        assert_eq!(effective_scale(2049, 1152, 2), 1, "超过 4096 则不再放大");
    }

    #[test]
    fn effective_scale_disables_upscale_for_large_screens() {
        // 3072x1920 全屏(本机屏):×2 会到 6144 > 4096 → 退到 1
        assert_eq!(effective_scale(3072, 1920, 2), 1, "4K 级全屏不放大");
        // 5K:×2 = 10240 会超过 Windows OCR 的 MaxImageDimension(10000) 而直接失败,
        // 退到 1 后 5120 < 10000,可正常识别(修掉现存溢出)
        assert_eq!(
            effective_scale(5120, 2880, 2),
            1,
            "5K 不放大,避免超出 OCR 上限"
        );
        // 用户请求 3x 时同样受钳制
        assert_eq!(
            effective_scale(3072, 1920, 3),
            1,
            "请求 3x 的大图同样退到 1"
        );
    }

    #[test]
    fn effective_scale_never_below_one() {
        // 超大图:MAX_OCR_EDGE / longest 下取整为 0,必须夹到 1(不可 0,否则尺寸为 0)
        assert_eq!(effective_scale(10000, 5000, 2), 1);
        assert_eq!(
            effective_scale(20_000, 20_000, 2),
            1,
            "超过上限也不得退到 0"
        );
        assert_eq!(effective_scale(0, 0, 2), 2, "非法尺寸不触发除零路径");
    }

    #[test]
    fn apply_preprocess_does_not_upscale_large_image() {
        // 3072x1920(> MAX_OCR_EDGE)× 2:应保持原尺寸,而不是 6144x3840
        let (w, h) = (3072u32, 1920u32);
        let rgba = vec![0u8; (w * h * 4) as usize];
        let (out, ow, oh) = apply_preprocess(rgba, w, h, Preprocess::default()).unwrap();
        assert_eq!(
            (ow, oh),
            (3072, 1920),
            "大图不放大(内存从 90MB 降到 22.5MB)"
        );
        assert_eq!(out.len(), (w * h * 4) as usize);
    }

    /// OCR 后端探测(诊断用):打印内置引擎在本机的可用性与语言列表。
    ///
    /// `cargo test -- --ignored --nocapture ocr_probe_backends`
    #[test]
    #[ignore = "依赖本机环境,按需显式运行以诊断 OCR 后端可用性"]
    fn ocr_probe_backends() {
        let result = probe(Backend::System);
        println!(
            "后端 {} 可用={} 语言=[{}]",
            result.backend,
            result.available,
            result.languages.join(", ")
        );
    }

    /// 真机质量回归:`OCR_CODE_SAMPLE` 指向等宽代码截图时,断言默认预处理(放大 2 倍)
    /// 能识别出 `build_entry` 的下划线(原图直识别会丢成 `build entry`)。
    ///
    /// `set OCR_CODE_SAMPLE=<png> && cargo test -- --ignored --nocapture ocr_reads_code_underscore`
    #[test]
    #[ignore = "依赖本机 OCR 语言包与样例图,按需显式运行"]
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
            "默认预处理未能保留函数名下划线,识别: {text:?}"
        );
    }

    /// 真机 OCR 冒烟:`OCR_SAMPLE` 指向含已知文字的 PNG 时断言能识别出关键片段。
    ///
    /// 默认跳过(未设置环境变量),因为它依赖本机 OCR 语言包:
    /// `set OCR_SAMPLE=<png> && cargo test -- --ignored ocr_reads_known_text`
    #[test]
    #[ignore = "依赖本机 OCR 语言包,按需通过 OCR_SAMPLE 环境变量显式运行"]
    fn ocr_reads_known_text() {
        let path = std::env::var("OCR_SAMPLE")
            .expect("需设置 OCR_SAMPLE 指向含文字的 PNG(参见 docs/phase6.md 的验证步骤)");
        let img = image::open(&path).expect("样例图读取失败");
        let rgba = img.to_rgba8();
        let (w, h) = rgba.dimensions();
        let text = recognize_rgba(rgba.into_raw(), w, h).expect("OCR 失败");
        let normalized: String = text.chars().filter(|c| !c.is_whitespace()).collect();
        assert!(
            normalized.to_uppercase().contains("OCR12345"),
            "未识别出预期文字,实际识别: {text:?}"
        );
    }

    /// 质量对比:对同一批样例图比较「预处理 × 语言」的识别结果(`OCR_BENCH_DIR` 指向目录)。
    ///
    /// `set OCR_BENCH_DIR=<dir> && cargo test -- --ignored --nocapture ocr_quality_bench`
    #[test]
    #[ignore = "质量对比用,按需通过 OCR_BENCH_DIR 环境变量显式运行"]
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
