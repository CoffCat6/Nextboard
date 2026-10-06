//! 屏幕捕获(第一代 Phase 5 F11/F10,决策 D3/D4/D5 的核心层平移)。
//!
//! 捕获层抽象为 `ScreenCapturer`,默认实现基于 xcap(Windows 走 GDI BitBlt +
//! GetDpiForMonitor,返回物理像素)。**会话层不迁移**:遮罩窗口/编辑器/贴图由
//! C++ 宿主实现(02-migration-core.md §2.2),本模块只提供枚举、整屏捕获与
//! 裁剪等数据能力,供 FFI RGBA 通道(nb_capture_monitor / nb_capture_crop)使用。

use crate::error::{AppError, AppResult};

/// 显示器几何信息(物理像素,全局虚拟屏幕坐标系)。
#[derive(Debug, Clone, PartialEq)]
pub struct MonitorInfo {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub scale_factor: f32,
    pub is_primary: bool,
}

/// 一次捕获的图像(RGBA 原始字节,物理像素)。
pub struct CapturedImage {
    pub rgba: Vec<u8>,
    pub width: u32,
    pub height: u32,
}

/// 屏幕捕获后端抽象:隔离 xcap 具体实现(决策 D3,PoC 不达标时替换)。
pub trait ScreenCapturer: Send + Sync {
    /// 枚举全部显示器;顺序不保证,按几何字段使用。
    fn monitors(&self) -> AppResult<Vec<MonitorInfo>>;
    /// 捕获指定显示器整屏。
    fn capture(&self, target: &MonitorInfo) -> AppResult<CapturedImage>;
}

/// xcap 后端(Windows GDI 路径)。
pub(crate) struct XCapCapturer;

impl ScreenCapturer for XCapCapturer {
    fn monitors(&self) -> AppResult<Vec<MonitorInfo>> {
        Ok(xcap::Monitor::all()?
            .iter()
            .map(|m| {
                Ok(MonitorInfo {
                    x: m.x()?,
                    y: m.y()?,
                    width: m.width()?,
                    height: m.height()?,
                    scale_factor: m.scale_factor()?,
                    is_primary: m.is_primary()?,
                })
            })
            .collect::<AppResult<Vec<_>>>()?)
    }

    fn capture(&self, target: &MonitorInfo) -> AppResult<CapturedImage> {
        // xcap 的显示器枚举顺序不稳定,按几何位置重新匹配目标
        let monitor = xcap::Monitor::all()?
            .into_iter()
            .find(|m| m.x().ok() == Some(target.x) && m.y().ok() == Some(target.y))
            .ok_or_else(|| {
                AppError::Internal(format!("未找到目标显示器 ({}, {})", target.x, target.y))
            })?;
        let image = monitor.capture_image()?;
        let (width, height) = (image.width(), image.height());
        Ok(CapturedImage {
            rgba: image.into_raw(),
            width,
            height,
        })
    }
}

/// 枚举全部显示器;失败时返回空表(调用方自行兜底)。
pub fn monitors() -> Vec<MonitorInfo> {
    let capturer = XCapCapturer;
    match capturer.monitors() {
        Ok(list) => list,
        Err(err) => {
            log::warn!("枚举显示器失败: {err}");
            Vec::new()
        }
    }
}

/// 捕获指定序号的显示器(序号对应 [`monitors`] 的返回顺序)。
pub fn capture_monitor(index: u32) -> AppResult<CapturedImage> {
    let list = monitors();
    let target = list.get(index as usize).ok_or_else(|| {
        AppError::InvalidInput(format!("显示器序号越界: {index}(共 {} 台)", list.len()))
    })?;
    XCapCapturer.capture(target)
}

/// 从整屏 RGBA 中裁剪子区域(物理像素;越界钳制到屏幕范围内)。
///
/// 返回按行紧凑排布的 RGBA(行跨度 = 裁剪宽 ×4),供框选确认后的入库/写剪贴板。
pub fn crop_rgba(
    rgba: &[u8],
    screen_w: u32,
    screen_h: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
) -> AppResult<CapturedImage> {
    if rgba.len() != screen_w as usize * screen_h as usize * 4 {
        return Err(AppError::InvalidInput(format!(
            "底图尺寸不符: 期望 {} 字节,实得 {} 字节",
            screen_w as usize * screen_h as usize * 4,
            rgba.len()
        )));
    }
    if width == 0 || height == 0 {
        return Err(AppError::InvalidInput("裁剪区域为空".into()));
    }
    // 选区可能从负坐标拖出/拖入屏幕:先求与屏幕的相交矩形
    let x0 = x.max(0) as u32;
    let y0 = y.max(0) as u32;
    let x1 = ((x + width as i32).max(0) as u32).min(screen_w);
    let y1 = ((y + height as i32).max(0) as u32).min(screen_h);
    if x1 <= x0 || y1 <= y0 {
        return Err(AppError::InvalidInput("裁剪区域完全越界或为空".into()));
    }
    let (out_w, out_h) = (x1 - x0, y1 - y0);
    let mut out = Vec::with_capacity(out_w as usize * out_h as usize * 4);
    let stride = screen_w as usize * 4;
    for row in y0..y1 {
        let start = row as usize * stride + x0 as usize * 4;
        out.extend_from_slice(&rgba[start..start + out_w as usize * 4]);
    }
    Ok(CapturedImage {
        rgba: out,
        width: out_w,
        height: out_h,
    })
}

/// 返回包含全局物理坐标点的显示器(左上闭、右下开区间)。
pub fn monitor_containing(monitors: &[MonitorInfo], x: i32, y: i32) -> Option<MonitorInfo> {
    monitors
        .iter()
        .find(|m| x >= m.x && x < m.x + m.width as i32 && y >= m.y && y < m.y + m.height as i32)
        .cloned()
}

/// 在给定显示器内居中放置 `width`×`height` 的窗口,返回其左上角(物理像素)。
///
/// - 内容比屏小(或恰好相等):四边留出相等空白;
/// - 内容比屏大:退化为该屏左上角起放——居中会让左上角(标题栏/工具栏)跑到屏外,
///   反而不便操作;超出部分留在屏外,用户可拖动查看。
pub fn center_in(screen: &MonitorInfo, width: u32, height: u32) -> (i32, i32) {
    if width >= screen.width || height >= screen.height {
        return (screen.x, screen.y);
    }
    (
        screen.x + (screen.width - width) as i32 / 2,
        screen.y + (screen.height - height) as i32 / 2,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn monitor(x: i32, y: i32, w: u32, h: u32, primary: bool) -> MonitorInfo {
        MonitorInfo {
            x,
            y,
            width: w,
            height: h,
            scale_factor: 1.0,
            is_primary: primary,
        }
    }

    #[test]
    fn containing_finds_monitor_by_point() {
        let monitors = vec![
            monitor(0, 0, 1920, 1080, true),
            monitor(1920, 0, 2560, 1440, false),
        ];
        assert_eq!(
            monitor_containing(&monitors, 100, 100),
            Some(monitors[0].clone())
        );
        assert_eq!(
            monitor_containing(&monitors, 1920, 5),
            Some(monitors[1].clone())
        );
        assert_eq!(
            monitor_containing(&monitors, 4479, 1439),
            Some(monitors[1].clone())
        );
    }

    #[test]
    fn containing_treats_right_bottom_edge_as_outside() {
        let monitors = vec![monitor(0, 0, 100, 100, true)];
        assert_eq!(
            monitor_containing(&monitors, 0, 0),
            Some(monitors[0].clone())
        );
        assert_eq!(monitor_containing(&monitors, 100, 99), None);
        assert_eq!(monitor_containing(&monitors, 99, 100), None);
    }

    #[test]
    fn containing_misses_out_of_range_points() {
        let monitors = vec![monitor(0, 0, 1920, 1080, true)];
        assert_eq!(monitor_containing(&monitors, -1, 0), None);
        assert_eq!(monitor_containing(&monitors, 0, -1), None);
        assert_eq!(monitor_containing(&monitors, 1920 + 2560, 0), None);
    }

    #[test]
    fn center_in_puts_window_in_middle_of_screen() {
        // 主屏 3072x1920 放 1745x704:左上角 = 原点 + 半差
        let screen = monitor(0, 0, 3072, 1920, true);
        assert_eq!(center_in(&screen, 1745, 704), (663, 608));
        // 左侧副屏原点为负:居中不能丢掉负偏移
        let left = monitor(-1920, 432, 1920, 1080, false);
        assert_eq!(center_in(&left, 800, 600), (-1920 + 560, 432 + 240));
    }

    #[test]
    fn center_in_falls_back_to_top_left_when_content_exceeds_screen() {
        let screen = monitor(0, 0, 1920, 1080, true);
        assert_eq!(center_in(&screen, 2000, 600), (0, 0)); // 宽超出
        assert_eq!(center_in(&screen, 800, 1200), (0, 0)); // 高超出
        assert_eq!(center_in(&screen, 1920, 1080), (0, 0)); // 与屏等大
    }

    #[test]
    fn crop_rgba_extracts_subregion() {
        // 2x2 屏,四像素分别为红绿蓝白
        let mut rgba = Vec::new();
        for px in [(255u8, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)] {
            rgba.extend_from_slice(&[px.0, px.1, px.2, px.3]);
        }
        // 裁右上 1x1(绿)
        let out = crop_rgba(&rgba, 2, 2, 1, 0, 1, 1).expect("裁剪应成功");
        assert_eq!((out.width, out.height), (1, 1));
        assert_eq!(out.rgba, vec![0, 255, 0, 255]);
    }

    #[test]
    fn crop_rgba_clamps_partial_overlap() {
        let rgba = vec![128; 4 * 4]; // 2x2 半透明灰
        // 选区 (-1,-1,3,3) 与屏幕相交部分即整屏
        let out = crop_rgba(&rgba, 2, 2, -1, -1, 3, 3).expect("部分越界应钳制");
        assert_eq!((out.width, out.height), (2, 2));
        assert_eq!(out.rgba.len(), 16);
    }

    #[test]
    fn crop_rgba_rejects_size_mismatch_and_empty() {
        assert!(crop_rgba(&[0; 3], 2, 2, 0, 0, 1, 1).is_err());
        assert!(crop_rgba(&[0; 16], 2, 2, 0, 0, 0, 0).is_err());
        // 完全越界:钳制后为空
        assert!(crop_rgba(&[0; 16], 2, 2, 5, 5, 1, 1).is_err());
    }
}
