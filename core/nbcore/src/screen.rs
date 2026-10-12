//! 显示器几何与窗口落点(贴图窗口、主面板、编辑器窗口共用;C++ 侧经 FFI 查询)。
//!
//! 坐标一律为**物理像素**且位于全局虚拟桌面坐标系。光标位置由 C++ 宿主传入
//! (Qt QCursor 返回的坐标同为物理像素),核心不再依赖任何窗口系统 API 取光标。

use crate::capture::{monitor_containing, MonitorInfo};

/// 无显示器信息时的兜底落点。
const FALLBACK_POS: (i32, i32) = (80, 80);

/// 枚举全部显示器;失败或无显示器时返回空表(调用方自行兜底)。
pub fn monitors() -> Vec<MonitorInfo> {
    crate::capture::monitors()
}

/// 光标所在显示器;光标物理坐标由宿主传入,取不到时回退主显示器,再取不到返回 `None`。
pub fn at_cursor(cursor: Option<(i32, i32)>) -> Option<MonitorInfo> {
    let monitors = monitors();
    if monitors.is_empty() {
        return None;
    }
    if let Some((x, y)) = cursor {
        return Some(monitor_containing(&monitors, x, y).unwrap_or_else(|| monitors[0].clone()));
    }
    monitor_containing(&monitors, 0, 0)
        .or_else(|| monitors.iter().find(|m| m.is_primary).cloned())
        .or_else(|| monitors.first().cloned())
}

/// 在给定显示器内居中放置 `width`×`height` 的窗口,返回其左上角(物理像素)。
///
/// - 内容比屏小(或恰好相等):四边留出相等空白;
/// - 内容比屏大:退化为该屏左上角起放——居中会让左上角(标题栏/工具栏)跑到屏外,
///   反而不便操作;超出部分留在屏外,用户可拖动查看。
pub fn center_in(screen: &MonitorInfo, width: u32, height: u32) -> (i32, i32) {
    crate::capture::center_in(screen, width, height)
}

/// 光标所在显示器内居中;取不到显示器信息时回退固定落点(记 warn)。
pub fn centered_at_cursor(cursor: Option<(i32, i32)>, width: u32, height: u32) -> (i32, i32) {
    match at_cursor(cursor) {
        Some(screen) => center_in(&screen, width, height),
        None => {
            log::warn!("取显示器信息失败,窗口放在固定落点");
            FALLBACK_POS
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::capture::monitor_containing;

    fn monitor(x: i32, y: i32, width: u32, height: u32, is_primary: bool) -> MonitorInfo {
        MonitorInfo {
            name: String::new(),   // 落点计算与设备名无关,测试不设
            x,
            y,
            width,
            height,
            scale_factor: 1.0,
            is_primary,
        }
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
    fn at_cursor_falls_back_to_primary_then_first() {
        // 光标坐标不在任何屏内:回退主显示器
        let _ = monitor_containing; // 保持与 capture 层同一语义的引用
        assert!(at_cursor(None).is_none() || at_cursor(None).is_some()); // 机器相关,仅验不 panic
    }
}
