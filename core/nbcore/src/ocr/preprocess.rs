//! OCR 图像预处理:放大 + 可选 Otsu 二值化(原样迁自 `ocr.rs`,零行为变更)。

use crate::error::{AppError, AppResult};

use super::Preprocess;

/// 预处理后允许的最大边长(物理像素):超过则**不再放大**。
///
/// 放大(`ocr_scale`,默认 2)是为**小字号截图**设的:12px 代码截图放大后 OCR 明显
/// 更准(docs/phase6.md §3.6)。但它对大图是净损失:
///   1. 内存:3072x1920 全屏 ×2 → 6144x3840,一份 RGBA 就是 90MB(原图仅 22.5MB),
///      且其后每一步(BGRA 转换 / WinRT buffer / SoftwareBitmap / 引擎灰度)都按 90MB 计;
///   2. 正确性:Windows OCR 的 `MaxImageDimension` 为 10000,5120x2880(5K)×2 = 10240
///      会**直接失败**(见 `platform::recognize` 的尺寸闸),而不是多占内存。
///
/// 故按"预处理后最长边不超过本值"钳制实际倍数:大图退到 1(不放大),
/// 1080p 及以下行为**完全不变**(仍是 ×2)。
pub const MAX_OCR_EDGE: u32 = 4096;

/// 把请求的放大倍数钳到"预处理后最长边 ≤ [`MAX_OCR_EDGE`]"。返回值至少为 1。
pub(crate) fn effective_scale(width: u32, height: u32, requested: u32) -> u32 {
    let longest = width.max(height).max(1);
    let max_scale = (MAX_OCR_EDGE / longest).max(1);
    requested.min(max_scale)
}

/// 统一预处理:放大 + 可选 Otsu 二值化。返回 (RGBA, w, h)。
///
/// 收 **owned** 缓冲:不处理时直接沿用(零拷贝),需要处理时也只在必要时新建 ——
/// 调用方(`ocr_entry`)本就不再需要原始像素。
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

/// Otsu 阈值二值化(原地修改 RGBA)。
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

/// BT.601 灰度(整数近似,避免浮点开销)。
fn luma(pixel: &[u8]) -> u32 {
    (299 * pixel[0] as u32 + 587 * pixel[1] as u32 + 114 * pixel[2] as u32) / 1000
}
