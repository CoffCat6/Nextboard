//! RapidOCR(PP-OCRv4 mobile)引擎:纯 Rust 推理,经 ONNX Runtime。
//!
//! 本文件**同时是引擎模块的根**(原为 `rapid.rs` 封装层 + `rapid/` 引擎子树):
//!   - 上半部分:本项目门面(安装目录定位、可用性探测、session 生命周期);
//!   - 下半部分(`mod` 声明起):引擎实现,派生自 `rapid-ocr-rs` v0.7.0(Apache-2.0)。
//!     裁剪与归属见 docs/11-rapidocr-rewrite-design.md 与 同目录 ATTRIBUTION.md。
//!
//! 设计要点(见 docs/10-plan-b-implementation.md §12/§13):
//!   - **D1**:引擎嵌入 `nbcore`,不经独立 DLL / 子进程;
//!   - **D7**:session **空闲时销毁**(不在每次识别后销毁,也不常驻);
//!   - **B1.3**:ONNX Runtime 走**显式项目路径**动态加载,**不依赖 PATH / 环境变量**;
//!   - **模型**:PP-OCRv4 mobile,四件(det/cls/rec/keys)由宿主目录提供。
//!
//! ⚠️ 与 `system.rs`(Windows 内置 OCR)的关键差异:
//!   - RapidOCR **不产生中文字间空格**,故**不能**接 `normalize_cjk_spaces`(见 08a §2.3);
//!   - RapidOCR **自带 `max_side_len`**,故**不应用** `ocr_scale` 放大(对它再放大是纯浪费);
//!   - RapidOCR 中英混排一体,**不区分语言**,`ocr_language` 对它无效。

use std::path::{Path, PathBuf};
use std::sync::{Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use log::info;

use crate::error::{AppError, AppResult};

use super::Preprocess;

/// 模型目录相对安装目录的位置(与 `nbocr.dll` 时代的 `models/ocr/` 布局保持一致)。
pub const MODELS_SUBDIR: &str = "models/ocr";

/// ONNX Runtime 动态库文件名。
#[cfg(windows)]
pub const ORT_DYLIB: &str = "onnxruntime.dll";
#[cfg(not(windows))]
pub const ORT_DYLIB: &str = "libonnxruntime.so";

/// 四件模型的文件名(与发布包一致;换档位时集中改这里)。
pub const DET_FILE: &str = "ch_PP-OCRv4_det_infer.onnx";
pub const CLS_FILE: &str = "ch_ppocr_mobile_v2.0_cls_infer.onnx";
pub const REC_FILE: &str = "ch_PP-OCRv4_rec_infer.onnx";
pub const KEYS_FILE: &str = "ppocr_keys_v1.txt";

/// 空闲多久后销毁 session(D7 决策 B)。
const IDLE_TTL: Duration = Duration::from_secs(60);

/// det 输入允许的最大**放大倍数**。
///
/// 上游 `limit_type = "min"` 会把短边撑到 736 且**放大倍数无上界**:
/// 720x70 的代码截图被放大 10.5 倍(det 输入 736x7584 = 557 万像素),
/// 实测峰值内存 339MB(docs/12-ocr-memory-analysis.md)。
///
/// 取 3.0 的依据(实测真实截图):
///   有收益的放大都 ≤ 2.45(screen 1.36 / 区域 1.75 / 全屏裁切 1.84 / 横幅 2.45);
///   而爆炸的都 ≥ 9.2(mixed14 10.5 / code12 9.2)。
/// 且 mixed14 在 0.4M~5.6M 像素区间识别结果**完全不变** —— 那段放大对识别零贡献。
/// 压到 3.0 后:4 个全屏/区域样例指纹逐字不变,mixed14 逐字不变且峰值 339→186MB,
/// code12 反而更准(全角括号 `(` 恢复为半角,空格保留)。
const DET_MAX_UPSCALE: f32 = 3.0;

/// det 输入允许的最大**最长边**(像素)。
///
/// 与 `DET_MAX_UPSCALE` 共同生效,兜住极端宽高比(如 8000x120 的长条),
/// 保证 det 输入像素数有硬上界。取 2560:高于所有实测"有收益"场景的长边
/// (最大 2368),又足以压住长条图。
const DET_MAX_INPUT_SIDE: usize = 2560;

/// 默认识别线程上限。
///
/// ⚠️ **必须显式设置**:`rapid-ocr-rs` 的自动调参取 `available.clamp(1, physical_cores)`,
/// 在 14 核机器上会给到 14 线程(实测 code12 耗时从 ~320ms 劣化到 ~1400ms)。
/// 且本引擎在 `nb_call` 内串行执行,占满 CPU 会与 Qt 宿主线程争抢。
const DEFAULT_THREADS: usize = 4;

/// ONNX Runtime 动态库是否在位(只做存在性检查)。
pub fn runtime_available(install_dir: &Path) -> bool {
    install_dir.join(ORT_DYLIB).is_file()
}

/// 探测可读的失败原因(供设置页展示"为什么不可用")。返回 None 表示可用。
pub fn unavailability_reason(models_dir: &Path, install_dir: &Path) -> Option<String> {
    if !runtime_available(install_dir) {
        return Some(format!("缺少 ONNX Runtime 运行库({})", ORT_DYLIB));
    }
    let missing: Vec<&str> = [DET_FILE, CLS_FILE, REC_FILE, KEYS_FILE]
        .into_iter()
        .filter(|f| !models_dir.join(f).is_file())
        .collect();
    if !missing.is_empty() {
        return Some(format!("缺少模型文件:{}", missing.join(", ")));
    }
    None
}

/// ONNX Runtime 全局初始化(进程内仅一次)。
///
/// ⚠️ **必须显式指定路径**:`ort` 在 Windows 上若不设 `ORT_DYLIB_PATH` 会回退到裸名
/// `onnxruntime.dll`,从而命中系统目录里的**任意版本**(本机实测 system32 有 1.17.1,
/// 而 `ort 2.0.0-rc.13` 要求 1.28+,必然 `BadVersion` panic)。
fn ensure_runtime(install_dir: &Path) -> AppResult<()> {
    static INIT: Once = Once::new();
    // ⚠️ 显式限定 std 的 Result:引擎的 `Result` 是单泛型别名(见 rapid/error.rs),
    //    不加限定会被解析成别名,导致"期望 1 个泛型却给了 2 个"。
    static RESULT: OnceLock<std::result::Result<(), String>> = OnceLock::new();

    INIT.call_once(|| {
        let dll = install_dir.join(ORT_DYLIB);
        let outcome = if !dll.is_file() {
            Err(format!("缺少 ONNX Runtime 运行库:{}", dll.display()))
        } else {
            // 失败(如版本不符)会返回 Err —— 不 panic,由调用方转成可读错误。
            match ort::init_from(&dll).map(|b| b.with_telemetry(false).commit()) {
                Ok(_) => Ok(()),
                Err(err) => Err(format!("加载 ONNX Runtime 失败({}):{err}", dll.display())),
            }
        };
        let _ = RESULT.set(outcome);
    });

    match RESULT.get() {
        Some(Ok(())) => Ok(()),
        Some(Err(msg)) => Err(AppError::Internal(msg.clone())),
        None => Err(AppError::Internal("ONNX Runtime 初始化状态异常".into())),
    }
}

/// 已构建的 OCR 引擎 + 最后使用时刻(D7:空闲销毁用)。
struct Session {
    engine: Option<crate::ocr::rapid::RapidOcr>,
    last_used: Option<Instant>,
}

static SESSION: Mutex<Session> = Mutex::new(Session {
    engine: None,
    last_used: None,
});

/// 解析安装目录(exe 同级)。
///
/// `nbcore.dll` 由宿主 `LoadLibrary` 加载,故 `current_exe()` 即宿主 exe 路径,
/// 其父目录就是安装目录 —— 与 app-local 的 `data/`、`models/` 布局一致。
fn install_dir() -> AppResult<PathBuf> {
    let exe =
        std::env::current_exe().map_err(|e| AppError::Internal(format!("无法定位程序目录:{e}")))?;
    exe.parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| AppError::Internal("程序目录不可解析".into()))
}

/// 模型目录(安装目录下的 `models/ocr/`)。
pub fn models_dir() -> AppResult<PathBuf> {
    Ok(install_dir()?.join(MODELS_SUBDIR))
}

/// 构建引擎。
///
/// `profile`:识别偏好档位(决定 det 放大上界,见 settings::ocr_profile)。
fn build_engine(models: &Path, profile: &str) -> AppResult<crate::ocr::rapid::RapidOcr> {
    use crate::ocr::rapid::{
        DictionarySource, EngineConfig, ModelSource, PipelineSources, ProviderPreference,
    };

    let mut cfg = EngineConfig::default();
    cfg.global.use_det = true;
    // 截图不会倒置,方向分类纯属浪费(CLI 侧对应 -a 0)。
    cfg.global.use_cls = false;
    cfg.global.use_rec = true;
    // 与既有 spike/CLI 口径一致(CLI 用 -s 960)。
    cfg.global.max_side_len = 960;
    // det 放大上界(两道上闸,见常量说明与 docs/12-ocr-memory-analysis.md)。
    // 这是相对上游的关键加固:上游放大倍数无上界,宽扁截图会把 det 输入
    // 撑到几百万像素,峰值内存 265~340MB。
    // 档位决定 det 放大上界(docs/12 §14.2 实测:三档下小图指纹不变,内存 215/116/82M)。
    let (max_upscale, max_input_side) = crate::settings::ocr_profile::det_caps(profile);
    cfg.det.max_upscale = max_upscale;
    cfg.det.max_input_side = max_input_side;

    // CPU-only:本项目不引入 CUDA / DirectML。
    cfg.det.runtime.provider_preference = ProviderPreference::Cpu;
    cfg.cls.runtime.provider_preference = ProviderPreference::Cpu;
    cfg.rec.runtime.provider_preference = ProviderPreference::Cpu;

    // 显式钉线程:见 DEFAULT_THREADS 的说明。
    let threads = DEFAULT_THREADS
        .min(
            std::thread::available_parallelism()
                .map(|v| v.get())
                .unwrap_or(DEFAULT_THREADS),
        )
        .max(1);
    for rt in [
        &mut cfg.det.runtime,
        &mut cfg.cls.runtime,
        &mut cfg.rec.runtime,
    ] {
        rt.intra_threads = Some(threads);
        rt.inter_threads = Some(1);
        rt.auto_tune_threads = false;
        // ⚠️ **必须关闭** ONNX Runtime 的 CPU arena 分配器。
        //
        // 实测(1920x1080 全屏、150 个文本框,同一进程同一张图):
        //   arena = true  → 识别后私有内存 **874 MB**
        //   arena = false → 识别后私有内存 ** 79 MB**
        //
        // 差 11 倍。原因:arena 会为每轮推理**保留并累积**中间张量、
        // 全屏场景下文本框数量多(150 个),累积量被放大到近 1 GB。
        // `rapid-ocr-rs` 的 `RuntimeConfig::default()` 取 `true`,
        // 而我们只要"单次识别完就还"的语义,故显式关掉。
        rt.enable_cpu_mem_arena = false;
    }

    let (det, cls, rec, keys) = (
        models.join(DET_FILE),
        models.join(CLS_FILE),
        models.join(REC_FILE),
        models.join(KEYS_FILE),
    );
    let sources = PipelineSources {
        det: Some(ModelSource::File(&det)),
        cls: Some(ModelSource::File(&cls)),
        rec: Some(ModelSource::File(&rec)),
        rec_dictionary: Some(DictionarySource::File(&keys)),
    };

    crate::ocr::rapid::RapidOcr::new_with_sources(cfg, sources)
        .map_err(|err| AppError::Internal(format!("OCR 引擎初始化失败:{err}")))
}

/// 取(必要时新建)引擎;空闲超时则先销毁再重建(D7)。
fn with_engine<T>(f: impl FnOnce(&mut crate::ocr::rapid::RapidOcr) -> AppResult<T>) -> AppResult<T> {
    let install = install_dir()?;
    let models = install.join(MODELS_SUBDIR);

    if let Some(reason) = unavailability_reason(&models, &install) {
        return Err(AppError::Internal(reason));
    }
    ensure_runtime(&install)?;

    // 驻留策略:每档的空闲销毁时限(resident = None 即永不)。
    let ttl: Option<Duration> = {
        let settings = crate::core::Core::get()
            .ok()
            .and_then(|c| c.settings().ok());
        match crate::settings::ocr_session_mode::normalize(
            settings.as_ref().map(|s| s.ocr_session_mode.as_str()).unwrap_or(""),
        ) {
            crate::settings::ocr_session_mode::RESIDENT => None,
            crate::settings::ocr_session_mode::IDLE60 => Some(IDLE_TTL),
            _ => Some(Duration::ZERO),
        }
    };
    let profile = crate::core::Core::get()
        .ok()
        .and_then(|c| c.settings().ok())
        .map(|s| s.ocr_profile.clone())
        .unwrap_or_default();

    let mut session = SESSION
        .lock()
        .map_err(|_| AppError::Internal("OCR session 锁已污染".into()))?;

    // 空闲超时 → 销毁(session 生命周期唯一出口之一)。
    if let (Some(last), Some(ttl)) = (session.last_used, ttl) {
        if session.engine.is_some() && last.elapsed() >= ttl {
            info!("OCR session 空闲超过 {:?},销毁", last.elapsed());
            session.engine = None;
        }
    }

    if session.engine.is_none() {
        let started = Instant::now();
        session.engine = Some(build_engine(&models, &profile)?);
        info!("OCR 引擎已构建({:?})", started.elapsed());
    }

    let engine = session
        .engine
        .as_mut()
        .ok_or_else(|| AppError::Internal("OCR 引擎不可用".into()))?;
    let out = f(engine);
    session.last_used = Some(Instant::now());
    // 用完即销毁:识别完立刻释放,不留给空闲期(docs/12 §14.3 档位 ephemeral)。
    if ttl == Some(Duration::ZERO) {
        session.engine = None;
        session.last_used = None;
    }
    out
}

/// 显式释放 session(设置变更/退出时调用;空闲销毁是兜底,不必依赖它)。
pub fn release_session() {
    if let Ok(mut session) = SESSION.lock() {
        if session.engine.is_some() {
            info!("OCR session 已显式释放");
        }
        session.engine = None;
        session.last_used = None;
    }
}

/// RapidOCR 识别入口(签名与 `system::recognize_with` 对齐,便于 `mod.rs` 分派)。
///
/// 收 **owned** RGBA(`nbcore` 既有链路口径),内部转为引擎所需格式。
///
/// ⚠️ `_preprocess` / `_language` **有意忽略**:
///   - RapidOCR 自带 `max_side_len`,不应用 `ocr_scale` 放大;
///   - 中英混排一体,不区分语言标签;
///   - 也**不接** `normalize_cjk_spaces`(它不产生中文字间空格)。
pub fn recognize_with(
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    _language: &super::LanguageChoice,
    _preprocess: Preprocess,
) -> AppResult<String> {
    if width == 0 || height == 0 {
        return Err(AppError::InvalidInput("图片尺寸为空,无法识别".into()));
    }
    let expected = width as usize * height as usize * 4;
    if rgba.len() != expected {
        return Err(AppError::Internal("RGBA 长度与尺寸不匹配".into()));
    }

    let started = Instant::now();
    let text = with_engine(|engine| {
        // 复用 nbcore 既有的 owned RGBA 缓冲,避免整图拷贝。
        let input = crate::ocr::rapid::OcrInput::RgbaU8 {
            width: width as usize,
            height: height as usize,
            data: rgba,
        };
        let out = engine
            .run(input, crate::ocr::rapid::OcrCallOptions::default())
            .map_err(|err| AppError::Internal(format!("OCR 识别失败:{err}")))?;

        let lines: Vec<String> = out
            .txts
            .unwrap_or_default()
            .into_iter()
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect();
        Ok(lines.join("\n"))
    })?;

    info!(
        "RapidOCR 完成:{width}x{height},{} 行,耗时 {:?}",
        if text.is_empty() {
            0
        } else {
            text.lines().count()
        },
        started.elapsed()
    );
    Ok(text)
}

/// 探测可用性(不建 session,只查文件)。
pub fn probe_available() -> (bool, Option<String>) {
    match (install_dir(), models_dir()) {
        (Ok(install), Ok(models)) => match unavailability_reason(&models, &install) {
            None => (true, None),
            Some(reason) => (false, Some(reason)),
        },
        (Err(err), _) | (_, Err(err)) => (false, Some(err.to_string())),
    }
}


// ---- 引擎模块(裁减自 rapid-ocr-rs) ----
//
// ⚠️ 为什么允许 dead_code:
// 上游是**通用 OCR 库**,暴露了远超本项目所需的 API 面(多后端 provider 解析、
// 单字坐标、JSON/Markdown 输出、YAML 配置、模型下载、批量运行选项…)。
// 本项目只用其中一条路径(CPU + PP-OCRv4 mobile + 整行文本),
// 其余代码虽不经过,但**与核心算法交织**(如 det/postprocess 里的两套偏移实现、
// config 的各枚举 as_str),删起来容易伤到活跃路径。
// 故此处显式允许 dead_code,并把"裁剪了什么"记录在模块文档与
// docs/11-rapidocr-rewrite-design.md —— 这比删代码更安全、也更可追溯。
mod cls;
mod config;
mod det;
mod diagnostics;
mod error;
mod input;
mod model_source;
mod model_store;
mod pipeline;
mod rec;
mod runtime;
mod types;
mod vision;

pub use config::{ColorOrder, ProviderPreference};
pub use error::{RapidOcrError, Result};
pub use input::image_loader::OcrInput;
pub use model_source::{DictionarySource, ModelSource, PipelineSources};
pub use pipeline::{
    config::EngineConfig,
    rapid_ocr::RapidOcr,
    types::OcrCallOptions,
};

pub type Quad = [[f32; 2]; 4];

