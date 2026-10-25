//! 二维码识别(docs/18 §3.6):对任意 RGBA 解码 QR 码,返回首个成功解码的文本。
//!
//! 定位:与 OCR 平行的「内容提取」能力,但**无引擎、无模型、无驻留**——
//! 解码器是纯 Rust 的 rqrr(quirc 检测算法移植),毫秒级、零 native 依赖,
//! 因此没有 backend/档位/会话那套设置,也没有内存驻留问题。
//!
//! 能力口径(v1 有意收窄):
//! - 只做 QR(不做一维码);
//! - 一张图里多个码时**不承诺多码枚举**,取第一个可解码的;
//! - 不解析内容结构(WiFi/名片/vCard 只交原始文本,链接的识别与打开由宿主做)。

use crate::error::{AppError, AppResult};
use image::RgbaImage;
use serde::{Deserialize, Serialize};

/// 解码结果:`found=false` 表示图中没有可解码的二维码(不是错误)。
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct QrDecodeResult {
    pub found: bool,
    pub text: String,
}

impl QrDecodeResult {
    fn none() -> Self {
        Self { found: false, text: String::new() }
    }
}

/// 对 RGBA(预乘与否都行,转灰度时按直通处理)解码二维码。
///
/// 与 `ocr::recognize_rgba` 同一入参口径(宿主侧同一套 QImage→RGBA 转换可复用)。
pub fn decode_rgba(rgba: Vec<u8>, width: u32, height: u32) -> AppResult<QrDecodeResult> {
    if rgba.len() != (width as usize) * (height as usize) * 4 {
        return Err(AppError::InvalidInput(format!(
            "二维码输入长度不匹配:len={} 期望={}×{}×4",
            rgba.len(),
            width,
            height
        )));
    }
    // rqrr 吃灰度图:RGBA→Luma 由 image crate 一次完成(浮点加权,抗彩色码面)
    let Some(src) = RgbaImage::from_raw(width, height, rgba) else {
        return Err(AppError::InvalidInput("二维码输入无法构成 RGBA 图".into()));
    };
    let luma = image::DynamicImage::ImageRgba8(src).to_luma8();

    // **尝试梯**(一次点击内的多次廉价重试,按命中即返回排序):
    //   ① 原样 —— 覆盖绝大多数情况;
    //   ② 反色 —— rqrr(quirc 系)不自动反色,深色 UI 的反色码(白码点深底)
    //      按原极性永远找不到定位格(真机「深色模式里全识别失败」的根因);
    //   ③④ ×2 放大后原样/反色 —— 小码(2~3px 模块)在干扰内容里是边际案例,
    //      放大让定位采样与模块判定稳定(屏幕截图是锐利的,放大不引入糊化)。
    //      放大档有输入门槛(见 UPSCALE_MAX_PIXELS):只在**失败路径**上才付
    //      代价 —— 命中在前两档的绝大多数点击不受影响。实测 release:4MP 全梯
    //      最坏 ~0.5s、8MP ~1.1s,与「未识别」的错误结果相比值得。
    if let Some(hit) = decode_from_gray(luma.clone()) {
        return Ok(hit);
    }
    let mut inverted = luma.clone();
    image::imageops::invert(&mut inverted);
    if let Some(hit) = decode_from_gray(inverted) {
        return Ok(hit);
    }
    /// ×2 档的输入上限:2000 万像素(覆盖 4K 整屏 8.3MP)。超过它的输入
    /// (多屏拼接级)连 1× 的 prepare 都要数百毫秒,不再放大。
    const UPSCALE_MAX_PIXELS: u64 = 20_000_000;
    if width as u64 * height as u64 <= UPSCALE_MAX_PIXELS {
        let up = image::imageops::resize(
            &luma,
            width.saturating_mul(2),
            height.saturating_mul(2),
            image::imageops::FilterType::Triangle,
        );
        if let Some(hit) = decode_from_gray(up.clone()) {
            return Ok(hit);
        }
        let mut up_inverted = up;
        image::imageops::invert(&mut up_inverted);
        if let Some(hit) = decode_from_gray(up_inverted) {
            return Ok(hit);
        }
    }
    Ok(QrDecodeResult::none())
}

/// 在一张灰度图上找码:任一候选格解出非空文本即返回(不承诺多码枚举)。
fn decode_from_gray(luma: image::GrayImage) -> Option<QrDecodeResult> {
    // rqrr 的 image feature 让任何 image crate 的 GrayImage 直接实现其
    // ImageBuffer trait —— 无需中间包装(实测 0.7.1 无 BitmapImage 类型,
    // 网上流传的那套 API 是别的版本)。
    let mut prepared = rqrr::PreparedImage::prepare(luma);
    // detect_grids 给出所有候选定位格;逐个尝试解码。候选格解码失败是常态
    // (反光/残缺的误检),不视为整体错误。
    for grid in prepared.detect_grids() {
        if let Ok((_meta, text)) = grid.decode() {
            if !text.trim().is_empty() {
                return Some(QrDecodeResult { found: true, text });
            }
        }
    }
    None
}

/// 测试专用:按模块放大倍率与静区宽度生成一张白底黑码 RGBA 图
/// (qrcode 是 dev-dependency,故本函数只在测试构建下存在;
/// ffi.rs 的 FFI 契约测试也从这里拿样本)。
#[cfg(test)]
pub fn __test_only_make_qr_rgba(payload: &str, module_px: usize, quiet: usize) -> (Vec<u8>, u32, u32) {
    let code = qrcode::QrCode::new(payload).expect("生成二维码样本失败");
    let modules = code.width();
    let total = modules + quiet * 2;
    let size = total * module_px;
    let mut rgba = vec![255u8; size * size * 4];
    for my in 0..modules {
        for mx in 0..modules {
            if code[(mx, my)] == qrcode::Color::Dark {
                for dy in 0..module_px {
                    for dx in 0..module_px {
                        let x = (mx + quiet) * module_px + dx;
                        let y = (my + quiet) * module_px + dy;
                        let i = (y * size + x) * 4;
                        rgba[i] = 0;
                        rgba[i + 1] = 0;
                        rgba[i + 2] = 0;
                        rgba[i + 3] = 255;
                    }
                }
            }
        }
    }
    (rgba, size as u32, size as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_url() {
        let payload = "https://example.com/nextboard?qr=1";
        let (rgba, w, h) = __test_only_make_qr_rgba(payload, 4, 4);
        let out = decode_rgba(rgba, w, h).expect("解码不应报错");
        assert!(out.found, "应识别出二维码");
        assert_eq!(out.text, payload);
    }

    #[test]
    fn round_trip_cjk_text() {
        let payload = "下一板·剪贴板与截图工具";
        let (rgba, w, h) = __test_only_make_qr_rgba(payload, 4, 4);
        let out = decode_rgba(rgba, w, h).expect("解码不应报错");
        assert!(out.found);
        assert_eq!(out.text, payload);
    }

    #[test]
    fn small_module_still_decodes() {
        // 模块 2px 是 rqrr 的实测极限(1px 干净合成图也解不出 —— 检测算法
        // 需要梯度;真机截图里这么小的码本来就糊,不追)。极限值留档于此。
        let payload = "https://example.com";
        let (rgba, w, h) = __test_only_make_qr_rgba(payload, 2, 4);
        let out = decode_rgba(rgba, w, h).expect("解码不应报错");
        assert!(out.found, "2px 模块 + 4 模块静区应可解码");
        assert_eq!(out.text, payload);
    }

    #[test]
    fn blank_image_finds_nothing() {
        let rgba = vec![255u8; 200 * 120 * 4];
        let out = decode_rgba(rgba, 200, 120).expect("空白图不是错误");
        assert!(!out.found);
        assert!(out.text.is_empty());
    }

    #[test]
    fn mismatched_length_is_invalid_input() {
        let err = decode_rgba(vec![0u8; 100], 10, 10).unwrap_err();
        assert!(matches!(err, AppError::InvalidInput(_)), "实际:{err:?}");
    }
}

#[cfg(test)]
mod realism_tests {
    use super::*;

    /// 生成「屏幕渲染感」样本:先生成 ×8 大图,再平滑缩到目标模块宽 ——
    /// 与屏幕上抗锯齿渲染的二维码等价(边缘半色调、模块有灰阶)。
    /// 墨色与底色按"ink 与 bg 线性插值"合成(抗锯齿的像素就是部分覆盖);
    /// invert = 深色 UI 的反色码(白墨、深底)。⚠️ 不要用 g*bg/255 之类的
    /// 乘法混合 —— 反色时会把对比度乘没(实测:码点灰 40、底纯黑,解不出
    /// 是样本的错,不是解码器的)。
    fn screen_like_rgba(payload: &str, module_px: u32, invert: bool) -> (Vec<u8>, u32, u32) {
        let (big, w, h) = __test_only_make_qr_rgba(payload, 8, 4);
        let big_img = image::RgbaImage::from_raw(w, h, big).unwrap();
        let scale = module_px as f64 / 8.0;
        let tw = ((w as f64) * scale).round() as u32;
        let th = ((h as f64) * scale).round() as u32;
        let small = image::imageops::resize(&big_img, tw.max(1), th.max(1),
                                            image::imageops::FilterType::CatmullRom);
        let (ink, bg): (i32, i32) = if invert { (225, 45) } else { (25, 240) };
        let mut out = Vec::with_capacity((tw * th * 4) as usize);
        for p in small.pixels() {
            // 覆盖率 c:原黑码点处 = 255(整格墨),白底处 = 0;
            // 抗锯齿像素介于两者之间。mixed = bg + (ink−bg)·c。
            // (invert 只是换成"亮墨深底",模块位置不变,不动 c。)
            let c = 255 - p.0[0] as i32;
            let mixed = (bg + (ink - bg) * c / 255).clamp(0, 255) as u8;
            out.extend_from_slice(&[mixed, mixed, mixed, 255]);
        }
        (out, tw, th)
    }

    #[test]
    fn realistic_antialiased_3px() {
        let (rgba, w, h) = screen_like_rgba("https://example.com/real", 3, false);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "3px 抗锯齿浅底应可解码");
        assert_eq!(out.text, "https://example.com/real");
    }

    #[test]
    fn realistic_antialiased_2px() {
        let (rgba, w, h) = screen_like_rgba("HELLO-2PX", 2, false);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "2px 抗锯齿应可解码");
    }

    #[test]
    fn realistic_inverted_on_dark() {
        // 深色 UI 里的反色码(白码点、深底):rqrr 不自动反色,靠 decode_rgba
        // 的第二轮反极性重解。真机上「深色模式应用里全识别失败」的正脸。
        let (rgba, w, h) = screen_like_rgba("https://example.com/inv", 4, true);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "反色码(深底白点)应可解码");
        assert_eq!(out.text, "https://example.com/inv");
    }
}

#[cfg(test)]
mod world_tests {
    //! 真实世界场景库(用户真机反馈驱动,2026-10-25):
    //! Logo 码、周围内容干扰、小码边际 —— 每个场景都有明确的预期与出处。

    use super::*;

    /// 带中心 Logo 遮挡的码:logo_modules 见方的灰块盖住中心
    /// (Logo 意象:非纯黑的中间色)。ECC 可调 —— 能否解出取决于
    /// 遮挡是否落在纠错容量**且未击穿单个 RS 块**的容量。
    fn logo_rgba(payload: &str, ecc: qrcode::EcLevel, logo_modules: usize)
                 -> (Vec<u8>, u32, u32) {
        let code = qrcode::QrCode::with_error_correction_level(payload, ecc).unwrap();
        let modules = code.width();
        let quiet = 4;
        let px = 6;
        let total = modules + quiet * 2;
        let size = total * px;
        let mut rgba = vec![255u8; size * size * 4];
        for my in 0..modules {
            for mx in 0..modules {
                if code[(mx, my)] == qrcode::Color::Dark {
                    for dy in 0..px {
                        for dx in 0..px {
                            let i = ((my + quiet) * px + dy) * size * 4
                                + ((mx + quiet) * px + dx) * 4;
                            rgba[i] = 0;
                            rgba[i + 1] = 0;
                            rgba[i + 2] = 0;
                        }
                    }
                }
            }
        }
        if logo_modules > 0 {
            let start = (total * px - logo_modules * px) / 2;
            for y in start..start + logo_modules * px {
                for x in start..start + logo_modules * px {
                    let i = (y * size + x) * 4;
                    rgba[i] = 128;
                    rgba[i + 1] = 128;
                    rgba[i + 2] = 128;
                }
            }
        }
        (rgba, size as u32, size as u32)
    }

    /// 码居中 + 四周"内容"(高密度文本行 + 嵌套方块「回」,定位图案的天然干扰)。
    /// squeeze_quiet = true 时内容一直画到码边(静区被挤到 8px)。
    fn jungle_rgba(payload: &str, module_px: usize, squeeze_quiet: bool) -> (Vec<u8>, u32, u32) {
        let (code_rgba, w, h) = __test_only_make_qr_rgba(payload, module_px, 4);
        let margin = if squeeze_quiet { 8 } else { w as usize };
        let size = w as usize + margin * 2;
        let mut rgba = vec![255u8; size * size * 4];
        let mut seed = 7u32;
        let mut rnd = || { seed = seed.wrapping_mul(1103515245); ((seed >> 16) & 0x7fff) as usize };
        // 文本行:行高 4px(2px 字 + 2px 行距),字宽 1-4px、字距 2px
        let mut y = 2usize;
        while y < size - 4 {
            let mut x = 2 + rnd() % 8;
            while x < size - 6 {
                let cw = 1 + rnd() % 4;
                for dy in 0..2usize {
                    for dx in 0..cw {
                        let i = ((y + dy) * size + x + dx) * 4;
                        if i + 2 < rgba.len() {
                            rgba[i] = 30;
                            rgba[i + 1] = 30;
                            rgba[i + 2] = 30;
                        }
                    }
                }
                x += cw + 2;
            }
            y += 4;
        }
        // 嵌套方块「回」×10:三层边框,与定位图案同构的强干扰
        for _ in 0..10 {
            let bx = rnd() % (size - 24);
            let by = rnd() % (size - 24);
            for ring in [0usize, 2, 4] {
                let len = 10 - ring * 2;
                for d in 0..len {
                    for (xx, yy) in [(bx + ring + d, by + ring),
                                     (bx + ring + len - 1, by + ring + d),
                                     (bx + ring + d, by + ring + len - 1),
                                     (bx + ring, by + ring + d)] {
                        if xx < size && yy < size {
                            let i = (yy * size + xx) * 4;
                            if i + 2 < rgba.len() {
                                rgba[i] = 20;
                                rgba[i + 1] = 20;
                                rgba[i + 2] = 20;
                            }
                        }
                    }
                }
            }
        }
        for yy in 0..h as usize {
            for xx in 0..w as usize {
                let src = (yy * w as usize + xx) * 4;
                let dst = ((margin + yy) * size + margin + xx) * 4;
                rgba[dst..dst + 4].copy_from_slice(&code_rgba[src..src + 4]);
            }
        }
        (rgba, size as u32, size as u32)
    }

    /// 主流 Logo 码(ECC-H):中心 9 模块灰块可解。微信系带 Logo 的码
    /// 生成时普遍用 H,这是「带 Logo 的码」的常见形态。
    #[test]
    fn logo_code_ecc_h_decodes() {
        let (rgba, w, h) = logo_rgba("https://example.com/logo-h", qrcode::EcLevel::H, 9);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "ECC-H + 9 模块中心 Logo 应可解(纠错容量内)");
        assert_eq!(out.text, "https://example.com/logo-h");
    }

    /// 已知限制的哨兵:ECC-M + 中心 7 模块遮挡**当前解不出** —— 中心遮挡
    /// 击穿了单个 RS 块的可纠错数(errors-only 解码容量 = 纠错符的一半)。
    /// rqrr 未暴露 erasure(已知位置擦除)解码,无法进一步救。
    /// 若未来 rqrr 支持或换引擎后此测试**变红**,意味着限制解除了 ——
    /// 更新本注释与 docs 里的能力口径。
    #[test]
    fn logo_beyond_block_capacity_is_known_limit() {
        let (rgba, w, h) = logo_rgba("https://example.com/logo-m", qrcode::EcLevel::M, 7);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(!out.found, "ECC-M + 中心 7 模块遮挡按当前实现解不出(已知限制)");
    }

    /// 周围内容干扰(用户反馈「周围多出内容就识别不到」):高密度文本 +
    /// 「回」形干扰下,码(≥3px 模块)带正常静区应稳定解出。
    #[test]
    fn dense_surroundings_decodes() {
        let (rgba, w, h) = jungle_rgba("https://example.com/jungle", 3, false);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "文本丛林 + 静区正常应可解");
        assert_eq!(out.text, "https://example.com/jungle");
    }

    /// 静区被周围内容挤到 8px:仍应可解(检测靠定位图案,不依赖宽静区)。
    #[test]
    fn squeezed_quiet_zone_decodes() {
        let (rgba, w, h) = jungle_rgba("https://example.com/squeeze", 3, true);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "静区被挤到 8px 仍应可解");
    }

    /// 尝试梯的 ×2 放大档:2px 模块小码在干扰里是边际案例 —— 若 1× 恰好
    /// 没过,放大档兜底(此测试钉住整条梯,不区分命中哪一档)。
    #[test]
    fn marginal_2px_code_saved_by_ladder() {
        let (rgba, w, h) = jungle_rgba("https://example.com/m2px", 2, false);
        let out = decode_rgba(rgba, w, h).unwrap();
        assert!(out.found, "2px 模块小码 + 干扰应被尝试梯(含 ×2 放大)解出");
        assert_eq!(out.text, "https://example.com/m2px");
    }
}

#[cfg(test)]
mod dpr_tests {
    //! 分数缩放(Windows 150%/175% 屏)下的真实渲染形态:
    //! 模块像素不是整数,3/4px 交替 + 相位漂移;再叠加高版本密集码
    //! (微信/支付码动辄 v10+,57+ 模块)—— 这是「宽选区包小码」的真凶候选。

    use super::*;

    /// 长 payload(逼出高版本)+ 指定逻辑模块宽,按分数倍率渲染
    /// (先 ×16 生成,再缩到 16×ratio 倍,模拟 175% 缩放屏的物理像素)。
    fn fractional_rgba(payload: &str, logical_module: f64, dpr: f64) -> (Vec<u8>, u32, u32) {
        let (big, w, h) = __test_only_make_qr_rgba(payload, 16, 4);
        let img = image::RgbaImage::from_raw(w, h, big).unwrap();
        let phys_module = logical_module * dpr;
        let scale = phys_module / 16.0;
        let tw = ((w as f64) * scale).round() as u32;
        let th = ((h as f64) * scale).round() as u32;
        let small = image::imageops::resize(&img, tw.max(1), th.max(1),
                                            image::imageops::FilterType::Triangle);
        (small.into_raw(), tw, th)
    }

    const LONG_URL: &str = "https://example.com/nextboard/clipboard-history?q=\
                            %E5%89%AA%E8%B4%B4%E6%9D%BF&session=abc1234567890&from=share";

    #[test]
    fn dense_v10_logical3_dpr175() {
        // 逻辑 3px 模块 @175% = 5.25px 物理:3/4... 不,5/6px 交替 + 相位漂移
        let (rgba, w, h) = fractional_rgba(LONG_URL, 3.0, 1.75);
        let out = decode_rgba(rgba, w, h).unwrap();
        eprintln!("DENSE-3px-175% size={}x{} found={}", w, h, out.found);
    }

    #[test]
    fn dense_v10_logical2_dpr175() {
        let (rgba, w, h) = fractional_rgba(LONG_URL, 2.0, 1.75);
        let out = decode_rgba(rgba, w, h).unwrap();
        eprintln!("DENSE-2px-175% size={}x{} found={}", w, h, out.found);
    }

    #[test]
    fn dense_v10_logical4_dpr100() {
        // 对照:整数 4px 模块(100% 缩放屏)
        let (rgba, w, h) = fractional_rgba(LONG_URL, 4.0, 1.0);
        let out = decode_rgba(rgba, w, h).unwrap();
        eprintln!("DENSE-4px-100% size={}x{} found={}", w, h, out.found);
    }
}

#[cfg(test)]
mod bigimage_tests {
    use super::*;

    /// 密集码(v10,3px 模块)居中在 2800×1600(4.5MP)高密度文本背景里:
    /// 「宽选区包小码」的极限形态。⚠️ #[ignore]:debug 构建下整梯要 10s+,
    /// 常规 cargo test 跳过,--release 显式跑(release 全梯 <1s)。
    #[test]
    #[ignore]
    fn dense_small_code_in_big_noisy_image() {
        let (code_rgba, w, h) = __test_only_make_qr_rgba(
            "https://example.com/nextboard/clipboard-history?q=%E5%89%AA%E8%B4%B4%E6%9D%BF&session=abc1234567890&from=share",
            3, 4);
        let (sw, sh) = (2800u32, 1600u32);
        let mut rgba = vec![242u8; (sw * sh * 4) as usize];
        let mut seed = 42u32;
        let mut rnd = || { seed = seed.wrapping_mul(1103515245); ((seed >> 16) & 0x7fff) as usize };
        let mut y = 2usize;
        while y < sh as usize - 4 {
            let mut x = 2 + rnd() % 8;
            while x < sw as usize - 6 {
                let cw = 1 + rnd() % 4;
                for dy in 0..2usize { for dx in 0..cw {
                    let i = ((y + dy) * sw as usize + x + dx) * 4;
                    if i + 2 < rgba.len() { rgba[i] = 40; rgba[i + 1] = 40; rgba[i + 2] = 40; }
                }}
                x += cw + 2;
            }
            y += 4;
        }
        let ox = ((sw - w) / 2) as usize;
        let oy = ((sh - h) / 2) as usize;
        for yy in 0..h as usize { for xx in 0..w as usize {
            let src = (yy * w as usize + xx) * 4;
            let dst = ((oy + yy) * sw as usize + ox + xx) * 4;
            rgba[dst..dst + 4].copy_from_slice(&code_rgba[src..src + 4]);
        }}
        let out1 = decode_rgba(rgba.clone(), sw, sh).unwrap();
        eprintln!("BIG-1x({}MP) found={}", sw as u64 * sh as u64 / 1_000_000, out1.found);
        // ×2 放大后解(当前被 4MP 门槛挡住的档)
        let img = image::RgbaImage::from_raw(sw, sh, rgba).unwrap();
        let up = image::imageops::resize(&img, sw * 2, sh * 2,
                                         image::imageops::FilterType::Triangle);
        let out2 = decode_rgba(up.into_raw(), sw * 2, sh * 2).unwrap();
        eprintln!("BIG-2x found={} text_len={}", out2.found, out2.text.len());
    }
}
