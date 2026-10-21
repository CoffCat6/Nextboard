//! 屏幕捕获(第一代 Phase 5 F11/F10,决策 D3/D4/D5 的核心层平移)。
//!
//! 捕获层抽象为 `ScreenCapturer`,默认实现基于 xcap(Windows 走 GDI BitBlt +
//! GetDpiForMonitor,返回物理像素)。**会话层不迁移**:遮罩窗口/编辑器/贴图由
//! C++ 宿主实现(02-migration-core.md §2.2),本模块只提供枚举、整屏捕获与
//! 裁剪等数据能力,供 FFI RGBA 通道(nb_capture_monitor / nb_capture_crop)使用。

use crate::error::{AppError, AppResult};

/// 显示器几何信息(物理像素,全局虚拟屏幕坐标系)。
///
/// ⚠️ `name` 是**跨模块配对的唯一可靠键**:
///    Qt 的 `QScreen::name()` 与 xcap 的 `Monitor::name()` 在 Windows 上都取自
///    `MONITORINFOEXW.szDevice`(形如 `\\.\DISPLAY1`),两边必然一一对应。
///    而**索引**不可用 —— Qt 把主屏排在最前、xcap 把它排在最后(实测三屏机器
///    两边顺序完全不同),用索引配对会把 A 屏的遮罩画上 B 屏的底图;
///    **坐标**也不可用 —— 混合 DPI 下 Qt 是逻辑像素、xcap 是物理像素,
///    1.5 倍缩放的屏上两者永不相等。
#[derive(Debug, Clone, PartialEq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MonitorInfo {
    /// 设备名(`\\.\DISPLAY1`);取不到时为空串。
    pub name: String,
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
                    // 取不到设备名不致命(配对失败的屏由宿主跳过);
                    // 用 unwrap_or_default 而不是 ? —— 缺 name 不该让整次枚举失败。
                    name: m.name().unwrap_or_default(),
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
        // ⚠️ 优先按**设备名**匹配:name 是稳定键,不随枚举顺序变化。
        //    旧实现只按 (x, y) 匹配,在混合 DPI 下必然失配(Qt 逻辑像素 vs
        //    xcap 物理像素),且 xcap 自己的枚举顺序也不保证稳定。
        //    仅在目标没有 name(老调用方按坐标构造,如单测)时才回退坐标匹配。
        let all = xcap::Monitor::all()?;
        let infos: Vec<MonitorInfo> = all
            .iter()
            .map(|m| MonitorInfo {
                name: m.name().unwrap_or_default(),
                x: m.x().unwrap_or_default(),
                y: m.y().unwrap_or_default(),
                width: m.width().unwrap_or_default(),
                height: m.height().unwrap_or_default(),
                scale_factor: m.scale_factor().unwrap_or(1.0),
                is_primary: m.is_primary().unwrap_or(false),
            })
            .collect();
        let picked_name = pick_target(&infos, target)
            .map(|m| m.name.clone())
            .ok_or_else(|| {
                AppError::Internal(format!(
                    "未找到目标显示器 (name={:?}, {}, {})",
                    target.name, target.x, target.y
                ))
            })?;
        let monitor = all
            .into_iter()
            .find(|m| m.name().ok() == Some(picked_name.clone()))
            .ok_or_else(|| AppError::Internal("目标显示器已消失".into()))?;
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

/// 从显示器列表中选出目标(按名优先,空名回退坐标)。
///
/// 抽成纯函数是为了可测:配对规则是"多屏位置/分辨率混乱"的根因所在,
/// 必须能脱离真实显示器直接断言。
fn pick_target<'a>(list: &'a [MonitorInfo], target: &MonitorInfo) -> Option<&'a MonitorInfo> {
    if !target.name.is_empty() {
        return list.iter().find(|m| m.name == target.name);
    }
    list.iter()
        .find(|m| m.x == target.x && m.y == target.y)
}

/// 宿主侧一个屏幕的描述(Qt `QScreen` 的口径),用于与核心的显示器配对。
///
/// 口径差异(实测,Windows 混合 DPI):
/// - `x`/`y`:**物理桌面原点**,与核心一致(Qt 在 Windows 上保留下发原点);
/// - `width`/`height`:**逻辑像素**(= 物理 ÷ DPR),与核心不同,
///   故比较尺寸时要乘 `dpr` 还原成物理;
/// - `name`:**显示器友好名**(EDID 型号,如 "S24 PRO"),与核心的
///   `\\.\DISPLAYn` **不同源** —— 故 name 只作为"能对上就优先"的加分项,
///   真正的主力键是原点。
#[derive(Debug, Clone, Default)]
pub struct MonitorHint {
    pub name: String,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub dpr: f32,
}

impl MonitorHint {
    /// 还原成物理尺寸(逻辑 × DPR),四舍五入到整数像素。
    fn physical_size(&self) -> (u32, u32) {
        let dpr = if self.dpr > 0.0 { self.dpr } else { 1.0 };
        (
            (self.width as f32 * dpr).round() as u32,
            (self.height as f32 * dpr).round() as u32,
        )
    }
}

/// 按宿主给的屏幕描述选出核心侧的显示器。
///
/// 优先级(逐级放宽,绝不"随便挑一个" —— 静默配错正是多屏混乱的成因):
/// 1. **设备名相同**(两边口径偶尔一致时最可靠);
/// 2. **原点相同**(主力键:Qt 与核心的物理桌面原点一致,实测三屏全中);
/// 3. **物理尺寸相同**(原点因 DPI 缩放口径不同而错位时的兜底);
/// 4. 都失败返回 `None`,由调用方跳过该屏并告警。
pub fn match_monitor<'a>(monitors: &'a [MonitorInfo], hint: &MonitorHint) -> Option<&'a MonitorInfo> {
    if !hint.name.is_empty() {
        if let Some(m) = monitors.iter().find(|m| m.name == hint.name) {
            return Some(m);
        }
    }
    if let Some(m) = monitors.iter().find(|m| m.x == hint.x && m.y == hint.y) {
        return Some(m);
    }
    // 第三级:物理尺寸。⚠️ 只在**唯一命中**时才采纳。
    //
    // 同型号同分辨率的双屏(办公最常见的配置)在这一级会命中两台,而原点又各自
    // 不同(说明前面两级都没对上:主机换了、缩放改了、或枚举口径变了)。
    // 此时「取第一个」等于随机挑一块屏 —— 长截图会把 A 屏的画面拼进 B 屏的长图,
    // 用户看到的是"内容对了但位置不对"的长图。宁可硬失败让调用方提示用户。
    let (pw, ph) = hint.physical_size();
    let mut candidates = monitors.iter().filter(|m| m.width == pw && m.height == ph);
    let first = candidates.next()?;
    if candidates.next().is_some() {
        return None;
    }
    Some(first)
}

/// 按**宿主屏幕描述**捕获整屏(多屏遮罩配对的正确入口)。
///
/// 宿主传 `QScreen` 的 name/x/y/width/height/DPR,这里用 [`match_monitor`]
/// 选出对应显示器 —— 不再用索引(两端枚举顺序不同)或纯坐标(混合 DPI 下
/// Qt 是逻辑像素)配对。配对失败时报错,不静默取别的屏。
pub fn capture_monitor_matched(hint: &MonitorHint) -> AppResult<CapturedImage> {
    let list = monitors();
    if list.is_empty() {
        return Err(AppError::Internal("枚举显示器为空".into()));
    }
    let target = match_monitor(&list, hint).ok_or_else(|| {
        AppError::InvalidInput(format!(
            "无法为屏幕(name={:?}, {},{} {}x{} dpr={})匹配显示器;核心侧有: {}",
            hint.name,
            hint.x,
            hint.y,
            hint.width,
            hint.height,
            hint.dpr,
            list.iter()
                .map(|m| format!(
                    "{}@{},{} {}x{}",
                    m.name, m.x, m.y, m.width, m.height
                ))
                .collect::<Vec<_>>()
                .join(" | ")
        ))
    })?;
    XCapCapturer.capture(target)
}

/// 捕获指定序号的显示器(序号对应 [`monitors`] 的返回顺序)。
///
/// ⚠️ **索引配对不可跨模块使用**:本函数的序号是 xcap 的枚举顺序,与 Qt 的
///    `QGuiApplication::screens()` 顺序**不一致**(Qt 主屏在前、xcap 主屏在后)。
///    宿主按屏布置遮罩时应改用 [`capture_monitor_by_name`]。
///    保留本入口是为兼容既有调用方(如 `--screenshot` 的单屏场景)。
pub fn capture_monitor(index: u32) -> AppResult<CapturedImage> {
    let list = monitors();
    let target = list.get(index as usize).ok_or_else(|| {
        AppError::InvalidInput(format!("显示器序号越界: {index}(共 {} 台)", list.len()))
    })?;
    XCapCapturer.capture(target)
}

/// 按**宿主屏幕描述**捕获显示器内的一个矩形(设备像素,相对显示器左上角)。
///
/// ## 为什么需要单独的入口
///
/// 长截图按固定节拍反复采同一块区域。走「capture_monitor_matched 整屏 + crop_rgba」
/// 每帧要搬整屏(4K 约 33MB)再拷一份裁剪图,而实际需要的只有选区那一小块。
/// xcap 本身就提供 monitor.capture_region(相对坐标、带越界校验),直接用它。
///
/// ## 与整屏捕获的两点不同
///
/// 1. **不做索引回退**:配对失败一律报错。索引在 Qt 与 xcap 两端口径不同
///    (Qt 主屏在前、xcap 主屏在后),回退等于把 A 屏内容拼进 B 屏长图。
/// 2. **尺寸必须原样返回**:调用方(拼接会话)会逐帧把返回尺寸与选区比对,
///    任何"算了别处"的偏差都会在下一次 append 变成显式错误,而不是错位长图。
pub fn capture_monitor_region(
    hint: &MonitorHint,
    region_x: u32,
    region_y: u32,
    region_w: u32,
    region_h: u32,
) -> AppResult<CapturedImage> {
    if region_w == 0 || region_h == 0 {
        return Err(AppError::InvalidInput(format!(
            "选区尺寸为 0: {region_w}x{region_h}"
        )));
    }
    let all = xcap::Monitor::all()?;
    if all.is_empty() {
        return Err(AppError::Internal("枚举显示器为空".into()));
    }
    let infos: Vec<MonitorInfo> = all
        .iter()
        .map(|m| MonitorInfo {
            name: m.name().unwrap_or_default(),
            x: m.x().unwrap_or_default(),
            y: m.y().unwrap_or_default(),
            width: m.width().unwrap_or_default(),
            height: m.height().unwrap_or_default(),
            scale_factor: m.scale_factor().unwrap_or(1.0),
            is_primary: m.is_primary().unwrap_or(false),
        })
        .collect();
    // 配对失败 → 硬失败(附两端实际布局,便于用户把显示器布局贴进 issue)
    let target = match_monitor(&infos, hint).ok_or_else(|| {
        AppError::InvalidInput(format!(
            "无法为屏幕(name={:?}, {},{} {}x{} dpr={})匹配显示器;核心侧有: {}",
            hint.name,
            hint.x,
            hint.y,
            hint.width,
            hint.height,
            hint.dpr,
            infos
                .iter()
                .map(|m| format!("{}@{},{} {}x{}", m.name, m.x, m.y, m.width, m.height))
                .collect::<Vec<_>>()
                .join(" | ")
        ))
    })?;
    let monitor = all
        .iter()
        .find(|m| m.name().ok().as_deref() == Some(target.name.as_str()))
        .ok_or_else(|| AppError::Internal("目标显示器已消失".into()))?;

    // 越界在 xcap 里会报 InvalidCaptureRegion;这里先给出**带数值**的可读错误,
    // 因为该路径的典型成因是"混合 DPI 下区域按逻辑像素算了",需要数字才能定位。
    let mw = monitor.width()?;
    let mh = monitor.height()?;
    if region_w > mw || region_h > mh || region_x.saturating_add(region_w) > mw
        || region_y.saturating_add(region_h) > mh
    {
        return Err(AppError::InvalidInput(format!(
            "采集区域 ({region_x},{region_y} {region_w}x{region_h}) 超出显示器 {} 范围 {mw}x{mh}(坐标须为**设备像素**且相对该屏左上角)",
            target.name
        )));
    }

    let image = monitor.capture_region(region_x, region_y, region_w, region_h)?;
    let (width, height) = (image.width(), image.height());
    if width != region_w || height != region_h {
        // 后端"按请求尺寸返回"是拼接正确性的前提;不符就必须说出来。
        return Err(AppError::Internal(format!(
            "采集区域尺寸不符:请求 {region_w}x{region_h},实得 {width}x{height}"
        )));
    }
    Ok(CapturedImage {
        rgba: image.into_raw(),
        width,
        height,
    })
}

/// 按**设备名**捕获整屏(形如 `\\.\DISPLAY1`)。
///
/// 跨模块配对的正确入口:宿主用 `QScreen::name()` 拿到同一个标识再来这里取图,
/// 不受两端枚举顺序差异与混合 DPI(逻辑/物理像素)影响。
/// 名字为空或不存在时报错 —— 静默回退到"别的屏"正是要修掉的 bug。
pub fn capture_monitor_by_name(name: &str) -> AppResult<CapturedImage> {
    let name = name.trim();
    if name.is_empty() {
        return Err(AppError::InvalidInput("显示器设备名为空".into()));
    }
    let list = monitors();
    let target = list
        .iter()
        .find(|m| m.name == name)
        .ok_or_else(|| {
            AppError::InvalidInput(format!(
                "未找到设备名为 {name} 的显示器(共 {} 台: {})",
                list.len(),
                list.iter().map(|m| m.name.as_str()).collect::<Vec<_>>().join(", ")
            ))
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
            // 单测走坐标回退路径(老调用方语义),不给 name —— 顺带覆盖
            // "无 name 时仍按 (x,y) 匹配"这条兼容分支。
            name: String::new(),
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

    // ---------- 多屏配对(位置/分辨率混乱的根因) ----------

    fn named(name: &str, x: i32, y: i32, w: u32, h: u32) -> MonitorInfo {
        MonitorInfo {
            name: name.to_string(),
            x,
            y,
            width: w,
            height: h,
            scale_factor: 1.0,
            is_primary: false,
        }
    }

    /// 核心回归:两端**枚举顺序不同**时,按 name 仍能选对目标。
    /// 构造的就是实测那台机器的布局(Qt 主屏在前、xcap 主屏在后)。
    #[test]
    fn pick_target_matches_by_name_regardless_of_order() {
        // xcap 的顺序:副屏、竖屏、主屏(主屏在最后)
        let xcap_order = vec![
            named(r"\\.\DISPLAY1", 5, 1080, 3072, 1920),
            named(r"\\.\DISPLAY2", -1080, 0, 1080, 1920),
            named(r"\\.\DISPLAY3", 0, 0, 1920, 1080),
        ];
        // 宿主要的是主屏(DISPLAY3):按索引 0 会错拿到 DISPLAY1
        let want = named(r"\\.\DISPLAY3", 0, 0, 1920, 1080);
        let picked = pick_target(&xcap_order, &want).expect("按名应能选中");
        assert_eq!(picked.name, r"\\.\DISPLAY3");
        assert_eq!(picked.width, 1920);
    }

    /// 混合 DPI:目标只有 name 时,即使坐标口径不同(逻辑 vs 物理)也能选中。
    #[test]
    fn pick_target_ignores_coords_when_name_present() {
        let list = vec![named(r"\\.\DISPLAY1", 5, 1080, 3072, 1920)];
        // 同名但坐标是按逻辑像素算出来的(150% 缩放 → 2048x1280 @ (3,720))
        let want = named(r"\\.\DISPLAY1", 3, 720, 2048, 1280);
        assert_eq!(pick_target(&list, &want).unwrap().width, 3072);
    }

    /// 老调用方不带 name:仍按坐标匹配(兼容分支)。
    #[test]
    fn pick_target_falls_back_to_coords_without_name() {
        let list = vec![
            named(r"\\.\DISPLAY1", 0, 0, 1920, 1080),
            named(r"\\.\DISPLAY2", 1920, 0, 2560, 1440),
        ];
        let want = monitor(1920, 0, 2560, 1440, false);   // name 为空
        assert_eq!(pick_target(&list, &want).unwrap().name, r"\\.\DISPLAY2");
    }

    /// 名字对不上时**返回 None**,而不是悄悄退回"列表第一个"——
    /// 静默回退正是"遮罩画了别的屏"的成因。
    #[test]
    fn pick_target_returns_none_on_unknown_name() {
        let list = vec![named(r"\\.\DISPLAY1", 0, 0, 1920, 1080)];
        assert!(pick_target(&list, &named(r"\\.\DISPLAY9", 0, 0, 800, 600)).is_none());
    }

    // ---------- 宿主配对(实测机器布局的真实回归) ----------

    /// **实测机器**的三屏布局:核心侧顺序与 Qt 侧顺序不同,且含 150% 缩放屏。
    ///
    /// 这是本缺陷的核心回归 —— 按索引配对时,Qt 的第 0 屏(主屏)会拿到
    /// 核心的第 0 项(3072x1920 副屏),表现就是"位置错、分辨率乱"。
    fn real_machine_core_order() -> Vec<MonitorInfo> {
        vec![
            // xcap 顺序:副屏(150%)、竖屏、主屏(主屏在最后)
            MonitorInfo {
                name: r"\\.\DISPLAY1".into(),
                x: 5,
                y: 1080,
                width: 3072,
                height: 1920,
                scale_factor: 1.5,
                is_primary: false,
            },
            MonitorInfo {
                name: r"\\.\DISPLAY2".into(),
                x: -1080,
                y: 0,
                width: 1080,
                height: 1920,
                scale_factor: 1.0,
                is_primary: false,
            },
            MonitorInfo {
                name: r"\\.\DISPLAY3".into(),
                x: 0,
                y: 0,
                width: 1920,
                height: 1080,
                scale_factor: 1.0,
                is_primary: true,
            },
        ]
    }

    fn hint(name: &str, x: i32, y: i32, w: u32, h: u32, dpr: f32) -> MonitorHint {
        MonitorHint {
            name: name.into(),
            x,
            y,
            width: w,
            height: h,
            dpr,
        }
    }

    /// 三个屏各自都要配到**正确**的那一台(原点键)。
    #[test]
    fn match_monitor_by_origin_on_real_machine() {
        let list = real_machine_core_order();
        // Qt 侧:友好名与核心设备名**不同源**,故这里名字全用友好名(配不上),
        // 全靠原点键 —— 正是实机情形。
        let cases = [
            // Qt 友好名, 逻辑原点, 逻辑尺寸, dpr, 期望核心屏
            ("Mi Monitor",     0,     0, 1920, 1080, 1.0, r"\\.\DISPLAY3"),
            ("TL160MDMP03-0",  5,  1080, 2048, 1280, 1.5, r"\\.\DISPLAY1"),
            ("S24 PRO",      -1080,   0, 1080, 1920, 1.0, r"\\.\DISPLAY2"),
        ];
        for (name, x, y, w, h, dpr, want) in cases {
            let h0 = hint(name, x, y, w, h, dpr);
            let got = match_monitor(&list, &h0)
                .unwrap_or_else(|| panic!("{name} 应能配到显示器"));
            assert_eq!(got.name, want, "{name} 配错了屏(位置/分辨率会乱)");
            // 物理尺寸自洽:核心尺寸 == 逻辑尺寸 × DPR
            let (pw, ph) = h0.physical_size();
            assert_eq!((got.width, got.height), (pw, ph), "{name} 物理尺寸不自洽");
        }
    }

    /// 名字能对上时**优先用名字**(哪怕原点因口径差异而错位)。
    #[test]
    fn match_monitor_prefers_name_when_same_source() {
        let list = vec![
            named(r"\\.\DISPLAY1", 0, 0, 1920, 1080),
            // 同名但原点不同:名字命中即应选它
            named(r"\\.\DISPLAY7", 999, 999, 1920, 1080),
        ];
        let h0 = hint(r"\\.\DISPLAY7", 0, 0, 1920, 1080, 1.0);
        assert_eq!(match_monitor(&list, &h0).unwrap().name, r"\\.\DISPLAY7");
    }

    /// 原点对不上时用**物理尺寸**兜底(尺寸需乘 DPR 还原)。
    #[test]
    fn match_monitor_falls_back_to_physical_size() {
        let list = vec![named(r"\\.\DISPLAY1", 100, 200, 3072, 1920)];
        // 原点与列表都不同,但逻辑 2048x1280 × 1.5 = 3072x1920 命中
        let h0 = hint("Whatever", -5, -5, 2048, 1280, 1.5);
        assert_eq!(match_monitor(&list, &h0).unwrap().name, r"\\.\DISPLAY1");
    }

    /// 三项都配不上时必须返回 None(由调用方跳过并告警),绝不乱配。
    #[test]
    fn match_monitor_returns_none_when_nothing_matches() {
        let list = vec![named(r"\\.\DISPLAY1", 0, 0, 1920, 1080)];
        let h0 = hint("X", 5000, 5000, 640, 480, 1.0);
        assert!(match_monitor(&list, &h0).is_none());
    }
}
