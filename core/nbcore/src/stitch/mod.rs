//! 滚动长截图的**帧拼接**核心(docs/19-scrolling-capture-plan.md §4)。
//!
//! 职责:接收一帧帧「选区矩形」的 RGBA 像素,靠**重叠区匹配**算出真实位移,
//! 增量拼接成一张长图。**不含任何 GUI 依赖**,故可完全离线做表驱动单测。
//!
//! ## 为什么必须匹配、不能硬拼
//!
//! 用户滚动时页面通常**不会**恰好滚动整数个「选区高度」:滚动常带惯性/弹性/亚像素
//! 偏移,滚动量与选区高度也不相等。所以相邻两帧之间**既有重叠又有位移**,必须
//! 由重叠区反算真实位移。硬拼(假设每帧滚了 h 像素)会累积错位,几十帧后整张图
//! 面目全非。
//!
//! ## 算法:行指纹 + 串匹配
//!
//! 1. **只取中间列**(默认横向 [20%, 80%]):滚动条、固定侧栏、浮动广告都在两侧边缘,
//!    参与匹配会让「页面没动」和「页面动了但侧栏没动」分不开。
//! 2. **每行一个指纹**:行均值的低 32 位 与 行内相邻差分绝对和的低 32 位拼成 64 位。
//!    两者互补 —— 纯均值会把「同一亮度但内容不同」的行判等,差分项正是为此。
//! 3. **候选位移搜索**:对每个候选位移 k,统计上一帧尾部与下一帧头部在该位移下
//!    **连续相等**的行数,取「连续相等行数 ≥ 阈值」的最大 k。
//! 4. **阈值**:max(12 行, 帧高 × overlap_check_ratio%)。阈值太低会把重复
//!    图案(表格线、纯色背景)误判为重叠 —— 那样拼出来的图会**缺行**。
//!
//! ## 失败模式(docs/19 §4.3)全部显式返回,不静默出图
//!
//! | reason | 含义 | 宿主应对 |
//! | --- | --- | --- |
//! | ok | 正常吸收 | 继续采集 |
//! | duplicate | 未滚动(内容与上一帧一致) | 丢弃该帧,不占预算 |
//! | no-overlap | 滚动过快,两帧无重叠 | 提示漏行并在长图上标记缺口 |
//! | backward | 检测到向上滚 | 不支持反向上滚续拼,提示到顶 |
//! | limit | 达到高度上限 | 停止采集并提示分段 |
//!
//! ## 内存
//!
//! **增量拼接**:会话里只保留「结果图 + 最近几帧指纹」,不留全部原始帧
//! (30 帧 4K 选区若全留就是数百 MB,与本项目内存收口目标直接冲突)。

pub mod fingerprint;
mod session;

pub use session::{
    abort_session, active_session_count, append_frame, begin_session, finish_session, Finished,
    SessionSpec, BASELINE_DEPTH, GAP_BAND_HEIGHT, GAP_BAND_RGBA,
};

use crate::error::{AppError, AppResult};

/// 拼接高度上限的**硬边界**(设置项 stitchMaxHeight 的校验范围,px)。
pub const MIN_MAX_HEIGHT: i64 = 2_000;
pub const MAX_MAX_HEIGHT: i64 = 100_000;

/// 拼接字节预算的**硬边界**(设置项 stitchMaxBytes,字节)。
///
/// 下限 16MB:1200 宽的选区也能装下约 3400 行,够拼一屏多;再小就不如直接截屏。
/// 上限 1GB:再高必然先撞 max_image_mb(入库那张 PNG 的限额),没有实际意义。
pub const MIN_MAX_BYTES: i64 = 16 * 1024 * 1024;
pub const MAX_MAX_BYTES: i64 = 1024 * 1024 * 1024;

/// 重叠判定阈值:最少连续相等行数不得低于此值(行)。
///
/// 12 行是「表格细线/纯色段」能被区分开的下限:再低就会把
/// 「同一张表格的不同位置」认成重叠,拼出来的图会**缺行**。
pub const MIN_OVERLAP_ROWS: u32 = 12;

/// 重叠判定的横向采样区间(相对宽度):避开两侧的滚动条与固定侧栏。
pub const SAMPLE_LEFT_RATIO: f32 = 0.20;
pub const SAMPLE_RIGHT_RATIO: f32 = 0.80;

/// 一次 append_frame 的结果。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StitchResult {
    /// 是否吸收了本帧(即拼接高度是否增加)。
    pub matched: bool,
    /// 相对匹配基线的**有效位移**(新吸收的像素行数)。matched=false 时为 0。
    pub shift_px: u32,
    /// 拼接后的当前总高(px)。
    pub total_height: u32,
    /// 判定结论,取值见模块文档的表格。
    pub reason: StitchStatus,
    /// 到目前为止累计的缺口数(每次无重叠记一次)。
    pub gaps: u32,
    /// 已经插入结果图的警示条数量(用户可见的「这里漏了」标记)。
    pub bands: u32,
    /// 匹配过程的数值明细(排查「为什么判 duplicate」用,不进产物)。
    pub debug: Option<StitchDebug>,
}

/// StitchResult::debug 的载荷:一次匹配判定的关键数值。
///
/// 之所以回传而不是只写日志:真机上「长图只有一屏高」这类问题的唯一可靠判据
/// 就是这几个数(k=0 还是 k>0、run 与 evidence 各多少)。宿主把它们直接打进
/// 日志,比事后猜「核心到底判了什么」快得多。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StitchDebug {
    /// 命中位移:next[0] 对上基线第 k 行。
    pub k: u32,
    /// 从对齐点起连续相等的行数。
    pub run: u32,
    /// 其中**非空白**的行数(真正的定位证据)。
    pub evidence: u32,
    /// 重叠行数上界。
    pub overlap: u32,
    /// 本次使用的门槛。
    pub threshold: u32,
    /// 命中的是第几老的基线(0 = 最新;>0 说明中间帧丢了)。
    pub baseline_age: u32,
}

/// `StitchResult::reason` 的取值。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum StitchStatus {
    Ok,
    Duplicate,
    NoOverlap,
    Backward,
    Limit,
}

impl StitchStatus {
    /// 稳定字符串(FFI JSON 用,与 serde 的 kebab-case 一致)。
    pub fn as_str(self) -> &'static str {
        match self {
            StitchStatus::Ok => "ok",
            StitchStatus::Duplicate => "duplicate",
            StitchStatus::NoOverlap => "no-overlap",
            StitchStatus::Backward => "backward",
            StitchStatus::Limit => "limit",
        }
    }
}

/// 校验一帧的尺寸是否自洽(RGBA 长度 = 宽 × 高 × 4,且均非零)。
///
/// 单独抽出来是因为**两个调用者**(会话 append 与 FFI 解包)都要这一条,
/// 而它们报错文案应当一致 —— 否则用户从两条链路会看到两种说法。
pub(crate) fn validate_frame(rgba_len: usize, width: u32, height: u32) -> AppResult<()> {
    if width == 0 || height == 0 {
        return Err(AppError::InvalidInput("长截图帧尺寸为 0".into()));
    }
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|px| px.checked_mul(4))
        .ok_or_else(|| AppError::InvalidInput("长截图帧尺寸溢出".into()))?;
    if rgba_len != expected {
        return Err(AppError::InvalidInput(format!(
            "长截图帧数据长度不符:期望 {expected}({width}×{height}×4),实际 {rgba_len}"
        )));
    }
    Ok(())
}

/// 把上限高度钳到合法范围(与 settings.rs 的 validate 同一口径兜底)。
pub(crate) fn clamp_max_height(v: i64) -> u32 {
    v.clamp(MIN_MAX_HEIGHT, MAX_MAX_HEIGHT) as u32
}

/// 把字节预算钳到合法范围;0 或负数表示「不限制」(回 0)。
pub(crate) fn clamp_max_bytes(v: i64) -> u64 {
    if v <= 0 {
        0
    } else {
        v.clamp(MIN_MAX_BYTES, MAX_MAX_BYTES) as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stitch::fingerprint::{
        find_shift, fingerprint_frame, overlap_threshold, sample_range,
    };

    const W: u32 = 40;

    /// 造一页**每行都不同**的内容,行数任意(上限测试要用 2000+ 行)。
    ///
    /// ## 为什么不是「两个字节直接拼行号」
    ///
    /// 指纹的行均值项是 sum(R+G+B)。若 B 恒为 0,两行只要 lo+hi 相同就撞上
    /// —— 例如 y=550(lo=38,hi=2,和 40)与 y=295(lo=39,hi=1,和 40)。
    /// 实测就是这一个碰撞让「无重叠」用例被误判成匹配成功(k=195)。
    ///
    /// 故第三个通道取 lo ^ hi:三元组 (lo, hi, lo^hi) 对每个 y 都唯一,
    /// 从而 sum = 3W·(lo+hi+(lo^hi)) 也唯一,65536 行内不会有两行撞上。
    fn page_of(rows: u32) -> Vec<u8> {
        let mut buf = Vec::with_capacity((W * rows * 4) as usize);
        for y in 0..rows {
            let lo = (y & 0xFF) as u8;
            let hi = ((y >> 8) & 0xFF) as u8;
            let tag = lo ^ hi;
            for _x in 0..W {
                buf.push(lo);
                buf.push(hi);
                buf.push(tag);
                buf.push(255);
            }
        }
        buf
    }

    /// 从长页 src(高 src_h)里裁出从 top 开始的 rows 行 —— 模拟「滚动到某位置后的截图」。
    fn slice_rows(src: &[u8], top: u32, rows: u32) -> Vec<u8> {
        let stride = (W * 4) as usize;
        let start = top as usize * stride;
        src[start..start + rows as usize * stride].to_vec()
    }

    /// 一页「内容」:高 200 行,每行唯一(见 page_of)。
    fn page() -> Vec<u8> {
        page_of(200)
    }

    /// 测试用规格:宽 W、高 h、无字节预算限制。
    fn spec(h: u32) -> SessionSpec {
        SessionSpec {
            monitor: String::from("test-monitor"),
            region_x: 0,
            region_y: 0,
            region_w: W,
            region_h: h,
            dpr: 1.0,
            max_height: 30_000,
            max_bytes: 0,
            overlap_ratio: 3,
        }
    }

    /// 开一个会话并吸收首帧,返回会话 id。
    fn start(tag: &str, rgba: Vec<u8>, h: u32) -> String {
        let id = sid(tag);
        begin_session(&id, spec(h)).unwrap();
        append_frame(&id, rgba, W, h).unwrap();
        id
    }

    // ---- 指纹 ----

    #[test]
    fn sample_range_skips_side_edges() {
        // 100px 宽 → 取 [20, 80)
        assert_eq!(sample_range(100), (20, 80));
        // 极窄帧退化为整行(「避开侧栏」已无意义)
        assert_eq!(sample_range(10), (0, 10));
    }

    #[test]
    fn identical_rows_share_fingerprint() {
        let src = page();
        let a = fingerprint_frame(&slice_rows(&src, 0, 50), W, 50);
        let b = fingerprint_frame(&slice_rows(&src, 0, 50), W, 50);
        assert_eq!(a.rows, b.rows, "同一内容必须算出同一指纹");
    }

    #[test]
    fn different_rows_differ_even_at_same_mean() {
        // 两行「均值相同但纹理疏密不同」:
        //   行 0 = 交替 120/160(多次小跳变)
        //   行 1 = 左半 100 右半 180(一次大跳变)
        // 均值都是 140 → 纯均值指纹会判等,差分项把两者分开。
        let stride = (W * 4) as usize;
        let mut buf = vec![0u8; stride * 2];
        for x in 0..W as usize {
            let v: u8 = if x % 2 == 0 { 120 } else { 160 };
            buf[x * 4] = v;
            buf[x * 4 + 1] = v;
            buf[x * 4 + 2] = v;
            buf[x * 4 + 3] = 255;
        }
        for x in 0..W as usize {
            let v: u8 = if x < W as usize / 2 { 100 } else { 180 };
            let p = stride + x * 4;
            buf[p] = v;
            buf[p + 1] = v;
            buf[p + 2] = v;
            buf[p + 3] = 255;
        }
        let fp = fingerprint_frame(&buf, W, 2);
        assert_ne!(fp.rows[0], fp.rows[1], "同均值不同纹理必须判为不同行");
    }

    // ---- 位移匹配 ----

    #[test]
    fn finds_exact_shift() {
        let src = page();
        let prev = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let next = fingerprint_frame(&slice_rows(&src, 30, 100), W, 100);
        let m = find_shift(&prev, &next, 12).expect("应能匹配");
        assert_eq!(m.k, 30, "位移应为 30 行");
        assert_eq!(m.overlap, 70, "重叠 = min(100-30, 100)");
        assert_eq!(m.new_rows(100), 30, "新内容 30 行");
    }

    #[test]
    fn zero_shift_means_duplicate() {
        let src = page();
        let prev = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let next = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let m = find_shift(&prev, &next, 12).expect("完全相同也要匹配上");
        assert_eq!(m.k, 0);
        assert_eq!(m.new_rows(100), 0, "没滚动 → 新内容 0 行");
    }

    #[test]
    fn no_overlap_returns_none() {
        let src = page();
        let prev = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let next = fingerprint_frame(&slice_rows(&src, 150, 50), W, 50);
        assert!(
            find_shift(&prev, &next, 12).is_none(),
            "滚过整屏后应判为无重叠,不能硬拼"
        );
    }

    #[test]
    fn threshold_rejects_short_overlap() {
        let src = page();
        let prev = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let next = fingerprint_frame(&slice_rows(&src, 95, 50), W, 50);
        assert!(find_shift(&prev, &next, 12).is_none(), "5 行重叠 < 门槛 12,应失配");
        let m = find_shift(&prev, &next, 5).expect("门槛 5 应能匹配");
        assert_eq!(m.k, 95);
    }

    #[test]
    fn overlap_threshold_uses_larger_of_ratio_and_floor() {
        assert_eq!(overlap_threshold(200, 3), 12);
        assert_eq!(overlap_threshold(3000, 3), 90);
        assert_eq!(overlap_threshold(1000, 0), 12);
        assert_eq!(overlap_threshold(1000, 100), 200);
    }

    #[test]
    fn survives_dynamic_row_between_frames() {
        let src = page();
        let prev = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let mut next_raw = slice_rows(&src, 20, 100);
        let stride = (W * 4) as usize;
        for x in 0..W as usize {
            let p = 40 * stride + x * 4;
            next_raw[p] = 7;
            next_raw[p + 1] = 7;
            next_raw[p + 2] = 7;
        }
        let next = fingerprint_frame(&next_raw, W, 100);
        let m = find_shift(&prev, &next, 12).expect("一行动态内容不应否定整次匹配");
        assert_eq!(m.k, 20, "位移仍应算对");
    }

    // ---- 会话:基本累加 ----

    /// 会话 id 唯一化:测试并行跑,固定 id 会互相踩。
    fn sid(tag: &str) -> String {
        format!("test-{tag}")
    }

    #[test]
    fn session_accumulates_and_finishes() {
        let src = page();
        let id = start("accumulate", slice_rows(&src, 0, 100), 100);

        let r2 = append_frame(&id, slice_rows(&src, 30, 100), W, 100).unwrap();
        assert!(r2.matched);
        assert_eq!(r2.shift_px, 30);
        assert_eq!(r2.total_height, 130);

        let r3 = append_frame(&id, slice_rows(&src, 70, 100), W, 100).unwrap();
        assert_eq!(r3.shift_px, 40);
        assert_eq!(r3.total_height, 170);

        let out = finish_session(&id).unwrap();
        assert_eq!(out.width, W);
        assert_eq!(out.height, 170);
        assert_eq!(out.gaps, 0);
        assert_eq!(out.bands, 0, "没有漏行就不该有警示条");
        assert_eq!(out.monitor, "test-monitor", "finish 应回传采集所属显示器");
        assert!(out.png.starts_with(&[0x89, b'P', b'N', b'G']), "产物必须是 PNG");
        assert!(
            finish_session(&id).is_err(),
            "取完即弃:同一会话不能再 finish 一次"
        );
    }

    #[test]
    fn session_detects_duplicate() {
        let src = page();
        let id = start("dup", slice_rows(&src, 0, 100), 100);
        let dup = append_frame(&id, slice_rows(&src, 0, 100), W, 100).unwrap();
        assert!(!dup.matched);
        assert_eq!(dup.reason, StitchStatus::Duplicate);
        assert_eq!(dup.total_height, 100);
        assert_eq!(dup.gaps, 0, "duplicate 不是缺口(没滚动而已)");
        abort_session(&id).unwrap();
    }

    // ---- 会话:缺口与警示条 ----

    #[test]
    fn session_flags_no_overlap_as_gap() {
        let src = page();
        let id = start("gap", slice_rows(&src, 0, 100), 100);
        // 跳到完全没有重叠的位置
        let r = append_frame(&id, slice_rows(&src, 150, 50), W, 100);
        // 帧高与选区不符 → 现在直接报错(旧实现会静默拼错)
        assert!(r.is_err());
        abort_session(&id).unwrap();
    }

    #[test]
    fn no_overlap_records_gap_when_frame_size_matches() {
        // 帧高不变但内容滚过整屏 → no-overlap + 记缺口
        let long = page_of(600);
        let id = start("noov", slice_rows(&long, 0, 100), 100);
        let r = append_frame(&id, slice_rows(&long, 300, 100), W, 100).unwrap();
        assert!(!r.matched);
        assert_eq!(r.reason, StitchStatus::NoOverlap);
        assert_eq!(r.gaps, 1, "无重叠要记账,供宿主提示漏行");
        abort_session(&id).unwrap();
    }

    #[test]
    fn gap_band_is_drawn_into_result() {
        // 真实缺陷的正面回归:漏了内容,产物上必须**看得见**(docs/19 §4.3)。
        let long = page_of(600);
        let id = start("band", slice_rows(&long, 0, 100), 100);
        // 滚过整屏 → 失配
        let miss = append_frame(&id, slice_rows(&long, 400, 100), W, 100).unwrap();
        assert_eq!(miss.reason, StitchStatus::NoOverlap);
        assert_eq!(miss.bands, 0, "失配当下还没拼,不该已经插条");
        // 再滚一小段 → 与上一帧匹配上,此时补一条警示条
        let hit = append_frame(&id, slice_rows(&long, 440, 100), W, 100).unwrap();
        assert!(hit.matched, "与上一帧能对上,应吸收");
        assert_eq!(hit.bands, 1, "漏过内容 → 必须补一条警示条");

        let out = finish_session(&id).unwrap();
        assert_eq!(out.bands, 1);
        // 高度 = 首帧 100 + 警示条 4 + 新增 40
        assert_eq!(out.height, 100 + GAP_BAND_HEIGHT + 40);
        // 警示条像素必须是 GAP_BAND_RGBA:解码后逐点核对
        let decoded = image::load_from_memory(&out.png).unwrap().to_rgba8();
        let band_y = 100;
        for y in band_y..band_y + GAP_BAND_HEIGHT {
            for x in 0..W {
                let px = decoded.get_pixel(x, y);
                assert_eq!(
                    [px[0], px[1], px[2], px[3]],
                    GAP_BAND_RGBA,
                    "警示条像素应在 y={y} x={x}"
                );
            }
        }
    }

    #[test]
    fn no_overlap_then_finish_draws_band() {
        // 失配后立刻结束(用户滚太快就直接点结束):产物上**必须**留下缺口的痕迹,
        // 否则用户拿到一张静默缺行的长图 —— docs/19 §4.3 明令禁止。
        let long = page_of(600);
        let id = start("band-at-finish", slice_rows(&long, 0, 100), 100);
        let miss = append_frame(&id, slice_rows(&long, 400, 100), W, 100).unwrap();
        assert_eq!(miss.reason, StitchStatus::NoOverlap);
        assert_eq!(miss.bands, 0, "失配当下还没有可拼的内容,先不画");
        let out = finish_session(&id).unwrap();
        assert_eq!(out.bands, 1, "收尾时必须补上警示条");
        assert_eq!(out.height, 100 + GAP_BAND_HEIGHT);
        let decoded = image::load_from_memory(&out.png).unwrap().to_rgba8();
        for y in 100..100 + GAP_BAND_HEIGHT {
            let px = decoded.get_pixel(0, y);
            assert_eq!([px[0], px[1], px[2], px[3]], GAP_BAND_RGBA, "警示条像素在 y={y}");
        }
    }

    // ---- 会话:多基线 ----

    #[test]
    fn multi_baseline_skips_dynamic_frame_and_keeps_going() {
        // 场景:滚 20 → 遇到一帧毫无重叠(动态内容/滚太快)→ 再滚 20。
        // 旧实现失配后把基准换成那一帧,下一帧能对上,但**中间那 40 行**全丢且不提示。
        // 新实现:下一帧也允许对上更老的基线,并且必须如实记缺口 + 画条。
        let long = page_of(600);
        let id = start("multi", slice_rows(&long, 0, 100), 100);

        // 正常滚 20
        let r1 = append_frame(&id, slice_rows(&long, 20, 100), W, 100).unwrap();
        assert_eq!(r1.shift_px, 20);

        // 一帧对不上任何基线(内容整块换了)
        let junk = vec![9u8; (W * 100 * 4) as usize];
        let miss = append_frame(&id, junk, W, 100).unwrap();
        assert_eq!(miss.reason, StitchStatus::NoOverlap);
        assert_eq!(miss.gaps, 1);

        // 再滚 20:应当能对上「滚 20」那一帧(age=1),仍然吸收
        let r2 = append_frame(&id, slice_rows(&long, 40, 100), W, 100).unwrap();
        assert!(r2.matched, "多基线应让它仍能吸收,而不是整段停摆");
        assert_eq!(r2.shift_px, 20, "相对 age=1 基线的位移仍是 20");
        assert_eq!(r2.bands, 1, "跳过一帧 = 漏了内容 → 补警示条");
        abort_session(&id).unwrap();
    }

    #[test]
    fn baselines_are_bounded() {
        // 基线队列不能无限长(每帧 8 字节/行,长图几百帧会累积成几十 MB)
        let long = page_of(4000);
        let id = start("bounded", slice_rows(&long, 0, 100), 100);
        for i in 1..10u32 {
            let _ = append_frame(&id, slice_rows(&long, i * 20, 100), W, 100).unwrap();
        }
        abort_session(&id).unwrap();
        assert!(BASELINE_DEPTH >= 2, "多基线至少 2 帧才有意义");
    }

    // ---- 会话:上限与预算 ----

    #[test]
    fn session_respects_height_limit() {
        // 上限会被 clamp_max_height 钳到 [MIN_MAX_HEIGHT(2000), 100000],
        // 所以必须造一个超过 2000 行的场景,不能用小数字取巧。
        let rows: u32 = 1_000;
        let long = page_of(rows * 3);
        let id = sid("limit");
        begin_session(&id, SessionSpec {
            max_height: 2000,
            ..spec(rows)
        })
        .unwrap();
        append_frame(&id, slice_rows(&long, 0, rows), W, rows).unwrap();
        let r2 = append_frame(&id, slice_rows(&long, 600, rows), W, rows).unwrap();
        assert_eq!(r2.total_height, 1600);
        let r3 = append_frame(&id, slice_rows(&long, 1200, rows), W, rows).unwrap();
        assert_eq!(r3.total_height, 2000, "不得越过上限");
        assert_eq!(r3.reason, StitchStatus::Limit);

        // 已经满了:再送帧一律 Limit
        let r4 = append_frame(&id, slice_rows(&long, 1800, rows), W, rows).unwrap();
        assert_eq!(r4.reason, StitchStatus::Limit);
        assert_eq!(r4.total_height, 2000);
        abort_session(&id).unwrap();
    }

    #[test]
    fn byte_budget_caps_effective_height() {
        // 字节预算优先于像素上限:1200×30000 的 RGBA 是 144MB,
        // 只按像素限额会让内存先炸(docs/19 v2 §4.2)。
        //
        // W=40 → 每行 160 字节。预算 16MB(下限)→ 可容纳 104857 行,
        // 仍高于像素上限 2000 → 以像素上限为准。故这里用一个**更窄的预算**
        // 无法越过下限,只能反证"下限保护生效"。
        let id = sid("bytes-floor");
        begin_session(&id, SessionSpec {
            max_height: 100_000,
            max_bytes: MIN_MAX_BYTES,
            ..spec(100)
        })
        .unwrap();
        let long = page_of(3000);
        append_frame(&id, slice_rows(&long, 0, 100), W, 100).unwrap();
        // 滚 50 行(不是整帧):整帧位移 = 零重叠,那是另一条用例
        let r = append_frame(&id, slice_rows(&long, 50, 100), W, 100).unwrap();
        assert!(r.matched, "预算充足时应正常吸收");
        abort_session(&id).unwrap();
    }

    #[test]
    fn byte_budget_derives_row_cap_for_wide_frames() {
        // 宽 4000(设备像素的 4K 选区):每行 16000 字节。
        // 预算 16MB → 1048 行上限,低于像素上限 10 万 → 应被字节预算钳住。
        let w = 4000u32;
        let h = 200u32;
        let id = sid("bytes-wide");
        begin_session(&id, SessionSpec {
            monitor: String::from("m"),
            region_x: 0,
            region_y: 0,
            region_w: w,
            region_h: h,
            dpr: 1.0,
            max_height: 100_000,
            max_bytes: MIN_MAX_BYTES,
            overlap_ratio: 3,
        })
        .unwrap();
        // 造 4000 宽、每行唯一
        let mk = |rows: u32| {
            let mut buf = Vec::with_capacity((w * rows * 4) as usize);
            for y in 0..rows {
                let lo = (y & 0xFF) as u8;
                let hi = ((y >> 8) & 0xFF) as u8;
                let tag = lo ^ hi;
                for _x in 0..w {
                    buf.extend_from_slice(&[lo, hi, tag, 255]);
                }
            }
            buf
        };
        let long = mk(3000);
        let slice = |top: u32| {
            let stride = (w * 4) as usize;
            long[top as usize * stride..(top + h) as usize * stride].to_vec()
        };
        append_frame(&id, slice(0), w, h).unwrap();
        // 每次滚 50 行,直到撞上限(1048 行 / 50 ≈ 20 帧内必撞)
        let mut last = StitchResult {
            matched: true,
            shift_px: 0,
            total_height: h,
            reason: StitchStatus::Ok,
            gaps: 0,
            bands: 0,
            debug: None,
        };
        for i in 1..40u32 {
            last = append_frame(&id, slice(i * 50), w, h).unwrap();
            if last.reason == StitchStatus::Limit {
                break;
            }
        }
        assert_eq!(last.reason, StitchStatus::Limit, "字节预算应最终触发 Limit");
        assert!(
            last.total_height <= MIN_MAX_BYTES as u32 / (w * 4),
            "总高必须落在字节预算内: {} 行",
            last.total_height
        );
        abort_session(&id).unwrap();
    }

    // ---- 会话:契约校验 ----

    #[test]
    fn session_rejects_width_change() {
        let src = page();
        let id = start("width", slice_rows(&src, 0, 100), 100);
        let err = append_frame(&id, vec![0u8; 60 * 20 * 4], 60, 20);
        assert!(err.is_err(), "帧宽变化必须报错");
        let msg = format!("{}", err.unwrap_err());
        assert!(msg.contains("帧宽"), "报错应说明是宽度不符: {msg}");
        abort_session(&id).unwrap();
    }

    #[test]
    fn session_rejects_height_change() {
        // 取错屏幕/选区被改 → 帧高变化。旧实现只查宽度,这种帧会被静默拼错。
        let src = page();
        let id = start("height", slice_rows(&src, 0, 100), 100);
        let err = append_frame(&id, vec![0u8; (W * 80 * 4) as usize], W, 80);
        assert!(err.is_err(), "帧高变化必须报错");
        let msg = format!("{}", err.unwrap_err());
        assert!(msg.contains("帧高"), "报错应说明是高度不符: {msg}");
        abort_session(&id).unwrap();
    }

    #[test]
    fn session_rejects_bad_length() {
        let id = sid("badlen");
        begin_session(&id, spec(10)).unwrap();
        // 声称 10×10×4 但只给 100 字节
        let err = append_frame(&id, vec![0u8; 100], 10, 10);
        assert!(err.is_err(), "长度不符必须报错");
        abort_session(&id).unwrap();
    }

    #[test]
    fn append_without_begin_errors() {
        let err = append_frame("never-begun", vec![0u8; (W * 10 * 4) as usize], W, 10);
        assert!(err.is_err(), "没 begin 就 append 必须报错,不能隐式建会话");
    }

    #[test]
    fn duplicate_begin_errors() {
        let id = sid("dup-begin");
        begin_session(&id, spec(100)).unwrap();
        let err = begin_session(&id, spec(100));
        assert!(err.is_err(), "重复 begin 必须报错(上次没 abort = 会话泄漏)");
        abort_session(&id).unwrap();
    }

    #[test]
    fn begin_rejects_zero_region() {
        let err = begin_session("zero-region", SessionSpec {
            region_w: 0,
            ..spec(100)
        });
        assert!(err.is_err(), "选区宽 0 必须拒绝");
        let err2 = begin_session("zero-region2", SessionSpec {
            region_h: 0,
            ..spec(100)
        });
        assert!(err2.is_err(), "选区高 0 必须拒绝");
    }

    #[test]
    fn sessions_are_isolated() {
        let a = sid("iso-a");
        let b = sid("iso-b");
        let src = page();
        begin_session(&a, spec(50)).unwrap();
        begin_session(&b, spec(80)).unwrap();
        append_frame(&a, slice_rows(&src, 0, 50), W, 50).unwrap();
        append_frame(&b, slice_rows(&src, 0, 80), W, 80).unwrap();
        let ra = finish_session(&a).unwrap();
        let rb = finish_session(&b).unwrap();
        assert_eq!(ra.height, 50);
        assert_eq!(rb.height, 80);
    }

    #[test]
    fn finish_unknown_session_errors() {
        assert!(finish_session("no-such-session").is_err());
    }

    #[test]
    fn abort_is_idempotent() {
        assert!(abort_session("never-existed").is_ok());
        assert!(abort_session("").is_ok());
    }

    #[test]
    fn finish_without_frames_errors() {
        let id = sid("empty");
        begin_session(&id, spec(100)).unwrap();
        assert!(finish_session(&id).is_err(), "一帧都没采到不该产出空图");
    }

    #[test]
    fn single_frame_finish_keeps_full_height() {
        let src = page();
        let id = start("single", slice_rows(&src, 0, 60), 60);
        let out = finish_session(&id).unwrap();
        assert_eq!(out.height, 60, "只采一帧时长图高度 = 帧高");
    }

    // ---- 空白行导致的假匹配(docs/progress 长截图拼接错位)----

    /// 造一页「白底文档」:(start_row, len, darkness) 三段描述各段文字。
    ///
    /// ## 为什么每段墨色必须不同
    ///
    /// 若所有段落长得一样(周期性内容),则「位移 30」与「位移 0」在**数据上真的
    /// 等价** —— 匹配到哪个都算「对」,用例会把算法缺陷掩盖成「测试写法问题」。
    /// 实测踩过:用等距同款文字时 k=0 与真位移的证据行数几乎相同(18 对 12),
    /// 无从区分。真实文档段落长短/字重各异,故这里也必须各异。
    fn document_page(rows: u32, segs: &[(u32, u32, u8)]) -> Vec<u8> {
        let mut buf = vec![255u8; (W * rows * 4) as usize];
        for &(start, len, dark) in segs {
            for i in 0..len {
                let y = start + i;
                if y >= rows {
                    continue;
                }
                for x in 0..W {
                    let p = ((y * W + x) * 4) as usize;
                    let v: u8 = if x < W / 2 { dark } else { dark.saturating_add(60) };
                    buf[p] = v;
                    buf[p + 1] = v.wrapping_add((x as u8).wrapping_mul(5));
                    buf[p + 2] = v;
                    buf[p + 3] = 255;
                }
            }
        }
        buf
    }

    /// 段落布局:长短与墨色各异(非周期),避免「多个位移数据上等价」。
    const DOC_SEGS: [(u32, u32, u8); 5] = [
        (8, 5, 20),
        (38, 7, 60),
        (66, 4, 110),
        (92, 6, 35),
        (128, 5, 150),
    ];

    #[test]
    fn blank_rows_are_detected() {
        let page = document_page(10, &[]);
        let fp = fingerprint_frame(&page, W, 10);
        assert!(fp.rows.iter().all(|r| r.is_blank()), "纯白页应全部判为空白");

        let with_text = document_page(10, &[(3, 3, 30)]);
        let fp2 = fingerprint_frame(&with_text, W, 10);
        assert!(!fp2.rows[3].is_blank(), "文字行不应判为空白");
        assert!(fp2.rows[0].is_blank(), "未着墨的行仍是空白");
    }

    #[test]
    fn blank_region_does_not_fake_k_zero() {
        let page = document_page(200, &DOC_SEGS);
        let h = 100u32;
        let prev = fingerprint_frame(&slice_rows(&page, 0, h), W, h);
        let next = fingerprint_frame(&slice_rows(&page, 40, h), W, h);
        let m = find_shift(&prev, &next, 12);
        assert!(m.is_some(), "应有匹配");
        assert_ne!(m.unwrap().k, 0, "大片空白下绝不能误判为 k=0(没滚动)");
    }

    #[test]
    fn true_duplicate_still_detected() {
        let page = document_page(200, &DOC_SEGS);
        let h = 100u32;
        let prev = fingerprint_frame(&slice_rows(&page, 0, h), W, h);
        let next = fingerprint_frame(&slice_rows(&page, 0, h), W, h);
        let m = find_shift(&prev, &next, 12).expect("同一帧必须匹配上");
        assert_eq!(m.k, 0);
        assert_eq!(prev.rows, next.rows, "逐行全等 → 可判 duplicate");
    }

    #[test]
    fn document_shift_accuracy() {
        let page = document_page(200, &DOC_SEGS);
        let h = 100u32;
        for truth in [12u32, 30, 45] {
            let prev = fingerprint_frame(&slice_rows(&page, 0, h), W, h);
            let next = fingerprint_frame(&slice_rows(&page, truth, h), W, h);
            let m = find_shift(&prev, &next, 12);
            assert_eq!(m.map(|x| x.k), Some(truth), "白底文档上位移 {truth} 应算准");
        }
    }

    #[test]
    fn document_session_never_fakes_duplicate() {
        // 端到端:白底文档滚 40 行,会话必须吸收 40,而不是判 duplicate
        let page = document_page(300, &DOC_SEGS);
        let h = 100u32;
        let id = start("doc", slice_rows(&page, 0, h), h);
        let r = append_frame(&id, slice_rows(&page, 40, h), W, h).unwrap();
        assert_eq!(r.reason, StitchStatus::Ok, "白底文档不该被判 duplicate");
        assert_eq!(r.shift_px, 40);
        abort_session(&id).unwrap();
    }

    #[test]
    fn debug_payload_explains_duplicate() {
        // 「为什么判 duplicate」必须能在日志里读到具体数值
        let src = page();
        let id = start("debug", slice_rows(&src, 0, 100), 100);
        let dup = append_frame(&id, slice_rows(&src, 0, 100), W, 100).unwrap();
        let dbg = dup.debug.expect("duplicate 也应带回判定数值");
        assert_eq!(dbg.k, 0);
        assert_eq!(dbg.threshold, 12);
        assert!(dbg.run >= dbg.threshold, "run 必须够门槛才会被采纳");
        abort_session(&id).unwrap();
    }
}
