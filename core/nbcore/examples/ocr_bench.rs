//! OCR 耗时基准:对指定 PNG 跑 RapidOCR 识别,输出每次耗时与中位数。
//!
//! 用途:rayon 线程数等引擎参数的 A/B 实测(参数由环境变量控制,见
//! `build_engine` 的 NB_OCR_RAYON_THREADS)。运行前须把 `models/ocr/` 与
//! `onnxruntime.dll` 放到本 example 的 exe 同级(install_dir 按 exe 定位)。
//!
//! ```text
//! cargo run --release -p nbcore --example ocr_bench -- <image.png> [次数=5]
//! ```

use std::time::Instant;

use nbcore::ocr::{recognize_configured_lines, OcrOptions};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let image_path = args.next().expect("用法: ocr_bench <image.png> [次数=5]");
    let iterations: usize = args.next().and_then(|v| v.parse().ok()).unwrap_or(5);

    let rgba = image::open(&image_path)
        .map_err(|e| format!("打开图片失败:{e}"))?
        .into_rgba8();
    let (width, height) = (rgba.width(), rgba.height());
    println!("image {width}x{height}, iterations {iterations}");

    // 明确走 RapidOCR(默认是 system,本基准测的是 rapid 引擎参数)。
    let options = OcrOptions {
        backend: nbcore::ocr::Backend::RapidOcr,
        ..OcrOptions::default()
    };

    // 预热(含 ORT 初始化与引擎构建,不计入统计)
    let started = Instant::now();
    let (text, lines) =
        recognize_configured_lines(&rgba, width, height, &options)?;
    println!(
        "warmup(含引擎构建): {:?}, {} 行 {} 字符",
        started.elapsed(),
        lines.len(),
        text.len()
    );

    let mut samples = Vec::with_capacity(iterations);
    for i in 1..=iterations {
        let started = Instant::now();
        let (text, lines) =
            recognize_configured_lines(&rgba, width, height, &options)?;
        let elapsed = started.elapsed();
        samples.push(elapsed);
        println!("run{i}: {:?}  {} 行 {} 字符", elapsed, lines.len(), text.len());
    }
    samples.sort();
    println!("median: {:?}", samples[samples.len() / 2]);
    Ok(())
}
