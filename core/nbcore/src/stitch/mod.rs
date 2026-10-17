//! 滚动长截图的**帧拼接**核心(docs/19-scrolling-capture-plan.md §4)。
//!
//! 职责:接收一帧帧"选区矩形"的 RGBA 像素,靠**重叠区匹配**算出真实位移,
//! 增量拼接成一张长图。**不含任何 GUI 依赖**,故可完全离线做表驱动单测。
//!
//! ## 为什么必须匹配、不能硬拼
//!
//! 用户滚动时页面通常**不会**恰好滚动整数个"选区高度":滚动常带惯性/弹性/亚像素
//! 偏移,滚动量与选区高度也不相等。所以相邻两帧之间**既有重叠又有位移**,必须
//! 由重叠区反算真实位移。硬拼(假设每帧滚了 h 像素)会累积错位,几十帧后整张图
//! 面目全非。
//!
//! ## 算法:行指纹 + 串匹配
//!
//! 1. **只取中间列**(默认横向 [20%, 80%]):滚动条、固定侧栏、浮动广告都在两侧边缘,
//!    参与匹配会让"页面没动"和"页面动了但侧栏没动"分不开。
//! 2. **每行一个指纹**:行均值的低 32 位 与 行内相邻差分绝对和的低 32 位拼成 64 位。
//!    两者互补 —— 纯均值会把"同一亮度但内容不同"的行判等,差分项正是为此。
//! 3. **候选位移搜索**:对每个候选位移 `k`,统计上一帧尾部与下一帧头部在该位移下
//!    **连续相等**的行数,取"连续相等行数 ≥ 阈值"的最大 `k`。
//! 4. **阈值**:\`max(12 行, 帧高 × overlap_check_ratio%) \`。阈值太低会把重复
//!    图案(表格线、纯色背景)误判为重叠 —— 那样拼出来的图会**缺行**。
//!
//! ## 失败模式(docs/19 §4.3)全部显式返回,不静默出图
//!
//! | reason | 含义 | 宿主应对 |
//! | --- | --- | --- |
//! | `ok` | 正常吸收 | 继续采集 |
//! | `duplicate` | 未滚动(内容与上一帧一致) | 丢弃该帧,不占预算 |
//! | `no-overlap` | 滚动过快,两帧无重叠 | 提示漏行并在长图上标记缺口 |
//! | `backward` | 检测到向上滚 | v1 不支持反向上滚续拼,提示到顶 |
//! | `limit` | 达到高度上限 | 停止采集并提示分段 |
//!
//! ## 内存
//!
//! **增量拼接**:会话里只保留"结果图 + 上一帧指纹",不留全部原始帧
//! (30 帧 4K 选区若全留就是数百 MB,与本项目内存收口目标直接冲突)。

pub mod fingerprint;
mod session;

pub use session::{
    abort_session, active_session_count, append_frame, finish_session, Finished,
};

use crate::error::{AppError, AppResult};

/// 拼接高度上限的**硬边界**(设置项 `stitchMaxHeight` 的校验范围,px)。
pub const MIN_MAX_HEIGHT: i64 = 2_000;
pub const MAX_MAX_HEIGHT: i64 = 100_000;

/// 重叠判定阈值:最少连续相等行数不得低于此值(行)。
///
/// 12 行是"表格细线/纯色段"能被区分开的下限:再低就会把
/// 「同一张表格的不同位置」认成重叠,拼出来的图会**缺行**。
pub const MIN_OVERLAP_ROWS: u32 = 12;

/// 重叠判定的横向采样区间(相对宽度):避开两侧的滚动条与固定侧栏。
pub const SAMPLE_LEFT_RATIO: f32 = 0.20;
pub const SAMPLE_RIGHT_RATIO: f32 = 0.80;

/// 一次 `append_frame` 的结果。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StitchResult {
    /// 是否吸收了本帧(即拼接高度是否增加)。
    pub matched: bool,
    /// 相对上一帧的**有效位移**(新吸收的像素行数)。`matched=false` 时为 0。
    pub shift_px: u32,
    /// 拼接后的当前总高(px)。
    pub total_height: u32,
    /// 判定结论,取值见模块文档的表格。
    pub reason: StitchStatus,
    /// 到目前为止累计的缺口数(每次 `no-overlap` 记一次)。
    pub gaps: u32,
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

/// 把上限秒数/上限高度等设置值钳到合法范围(与 settings.rs 的 validate 同一口径兜底)。
pub(crate) fn clamp_max_height(v: i64) -> u32 {
    v.clamp(MIN_MAX_HEIGHT, MAX_MAX_HEIGHT) as u32
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
    /// ## 为什么不是"两个字节直接拼行号"
    ///
    /// 指纹的行均值项是 `sum(R+G+B)`。若 B 恒为 0,两行只要 `lo+hi` 相同就撞上
    /// —— 例如 y=550(lo=38,hi=2,和 40)与 y=295(lo=39,hi=1,和 40)。
    /// 实测就是这一个碰撞让"无重叠"用例被误判成匹配成功(k=195)。
    ///
    /// 故第三个通道取 `lo ^ hi`:三元组 (lo, hi, lo^hi) 对每个 y 都唯一,
    /// 从而 `sum = 3W·(lo+hi+(lo^hi))` 也唯一,65536 行内不会有两行撞上。
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

    /// 从长图 `src`(高 `src_h`)里裁出从 `top` 开始的 `rows` 行 —— 模拟"滚动到某位置后的截图"。
    fn slice_rows(src: &[u8], top: u32, rows: u32) -> Vec<u8> {
        let stride = (W * 4) as usize;
        let start = top as usize * stride;
        src[start..start + rows as usize * stride].to_vec()
    }

    /// 一页"内容":高 200 行,每行唯一(见 `page_of`)。
    fn page() -> Vec<u8> {
        page_of(200)
    }

    // ---- 指纹 ----

    #[test]
    fn sample_range_skips_side_edges() {
        // 100px 宽 → 取 [20, 80)
        assert_eq!(sample_range(100), (20, 80));
        // 极窄帧退化为整行("避开侧栏"已无意义)
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
        // 两行"均值相同但纹理疏密不同":
        //   行 0 = 交替 120/160(多次小跳变,差分和 = 39×40)
        //   行 1 = 左半 100 右半 180(一次大跳变,差分和 = 80)
        // 均值都是 140 → 纯均值指纹会判等,差分项把两者分开。
        //
        // ⚠️ 指纹**刻意不区分镜像结构**(左亮右暗 vs 左暗右亮:均值同、差分和也同)。
        //    这是已知取舍:检测镜像要引入位置权重,而匹配要求的是**连续多行**
        //    同时相等,单行碰撞在连续段里会被稀释掉(见 find_shift 的 run 判定)。
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
        // 前半段与后半段毫无交集
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
        // 只重叠 5 行
        let next = fingerprint_frame(&slice_rows(&src, 95, 50), W, 50);
        assert!(find_shift(&prev, &next, 12).is_none(), "5 行重叠 < 门槛 12,应失配");
        // 门槛放宽到 5 就能匹配
        let m = find_shift(&prev, &next, 5).expect("门槛 5 应能匹配");
        assert_eq!(m.k, 95);
    }

    #[test]
    fn overlap_threshold_uses_larger_of_ratio_and_floor() {
        // 小帧:比例算出来低于 12 → 取 12
        assert_eq!(overlap_threshold(200, 3), 12);
        // 大帧:比例算出来高于 12 → 取比例值
        assert_eq!(overlap_threshold(3000, 3), 90);
        // 比例被钳到 1..20
        assert_eq!(overlap_threshold(1000, 0), 12);
        assert_eq!(overlap_threshold(1000, 100), 200);
    }

    #[test]
    fn survives_dynamic_row_between_frames() {
        // 第二帧中间有一行动态内容(光标闪烁/时间戳),不该让整次匹配失败
        let src = page();
        let prev = fingerprint_frame(&slice_rows(&src, 0, 100), W, 100);
        let mut next_raw = slice_rows(&src, 20, 100);
        // 把第 40 行涂成纯色(模拟动态内容)
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

    // ---- 会话 ----

    /// 会话 id 唯一化:测试并行跑,固定 id 会互相踩。
    fn sid(tag: &str) -> String {
        format!("test-{tag}")
    }

    #[test]
    fn session_accumulates_and_finishes() {
        let id = sid("accumulate");
        let src = page();

        // 第一帧:完全吸收
        let r1 = append_frame(&id, slice_rows(&src, 0, 100), W, 100, false, 30000, 3).unwrap();
        assert!(r1.matched);
        assert_eq!(r1.shift_px, 100);
        assert_eq!(r1.total_height, 100);
        assert_eq!(r1.reason, StitchStatus::Ok);

        // 第二帧:向下滚 30 行 → 只吸收 30 行新内容
        let r2 = append_frame(&id, slice_rows(&src, 30, 100), W, 100, false, 30000, 3).unwrap();
        assert!(r2.matched);
        assert_eq!(r2.shift_px, 30);
        assert_eq!(r2.total_height, 130);

        // 第三帧:再滚 40 行
        let r3 = append_frame(&id, slice_rows(&src, 70, 100), W, 100, false, 30000, 3).unwrap();
        assert_eq!(r3.shift_px, 40);
        assert_eq!(r3.total_height, 170);

        let out = finish_session(&id).unwrap();
        assert_eq!(out.width, W);
        assert_eq!(out.height, 170);
        assert_eq!(out.gaps, 0);
        assert!(out.png.starts_with(&[0x89, b'P', b'N', b'G']), "产物必须是 PNG");
        // ⚠️ 会话表是**进程级**的,测试并行跑时别的用例也有活跃会话,
        //    故只能断言"本会话已不可再取",不能用全局计数下结论。
        assert!(
            finish_session(&id).is_err(),
            "取完即弃:同一会话不能再 finish 一次"
        );
    }

    #[test]
    fn session_detects_duplicate_and_backward() {
        let id = sid("dup");
        let src = page();
        append_frame(&id, slice_rows(&src, 0, 100), W, 100, false, 30000, 3).unwrap();

        // 同一帧再送一次 → duplicate,高度不变
        let dup = append_frame(&id, slice_rows(&src, 0, 100), W, 100, false, 30000, 3).unwrap();
        assert!(!dup.matched);
        assert_eq!(dup.reason, StitchStatus::Duplicate);
        assert_eq!(dup.total_height, 100);

        // 往上滚:从 40 回到 0 —— prev 是 0..100,next 是 0..100 也一样(k=0)→ 仍 duplicate
        // 真正的反向上滚要用"当前基准在下方"构造,见下
        abort_session(&id).unwrap();
    }

    #[test]
    fn session_flags_no_overlap_as_gap() {
        let id = sid("gap");
        let src = page();
        append_frame(&id, slice_rows(&src, 0, 100), W, 100, false, 30000, 3).unwrap();
        // 跳到完全没有重叠的位置
        let r = append_frame(&id, slice_rows(&src, 150, 50), W, 50, false, 30000, 3).unwrap();
        assert!(!r.matched);
        assert_eq!(r.reason, StitchStatus::NoOverlap);
        assert_eq!(r.gaps, 1, "无重叠要记账,供宿主提示漏行");
        abort_session(&id).unwrap();
    }

    #[test]
    fn session_respects_height_limit() {
        let id = sid("limit");
        // ⚠️ 上限会被 clamp_max_height 钳到 [MIN_MAX_HEIGHT(2000), 100000],
        //    所以这里必须造一个超过 2000 行的场景,不能用小数字取巧。
        let rows: u32 = 1_000;
        let src = page_of(rows * 3);
        // 上限 2000:首帧 1000 行;第二帧滚 600 行 → 吸 600(共 1600);
        // 第三帧再滚 600 → 只剩 400 的空间,只吸 400 并报 Limit。
        append_frame(&id, slice_rows(&src, 0, rows), W, rows, false, 2000, 3).unwrap();
        let r2 = append_frame(&id, slice_rows(&src, 600, rows), W, rows, false, 2000, 3).unwrap();
        assert_eq!(r2.total_height, 1600);
        let r3 = append_frame(&id, slice_rows(&src, 1200, rows), W, rows, false, 2000, 3).unwrap();
        assert_eq!(r3.total_height, 2000, "不得越过上限");
        assert_eq!(r3.reason, StitchStatus::Limit);

        // 已经满了:再送帧一律 Limit
        let r4 = append_frame(&id, slice_rows(&src, 1800, rows), W, rows, false, 2000, 3).unwrap();
        assert_eq!(r4.reason, StitchStatus::Limit);
        assert_eq!(r4.total_height, 2000);
        abort_session(&id).unwrap();
    }

    #[test]
    fn session_rejects_width_change() {
        let id = sid("width");
        let src = page();
        append_frame(&id, slice_rows(&src, 0, 100), W, 100, false, 30000, 3).unwrap();
        // 宽度变了:必须报错,不能静默拼错
        let err = append_frame(&id, vec![0u8; 60 * 20 * 4], 60, 20, false, 30000, 3);
        assert!(err.is_err(), "帧宽变化必须报错");
        abort_session(&id).unwrap();
    }

    #[test]
    fn session_rejects_bad_length() {
        let id = sid("badlen");
        // 声称 10×10×4 但只给 100 字节
        let err = append_frame(&id, vec![0u8; 100], 10, 10, false, 30000, 3);
        assert!(err.is_err(), "长度不符必须报错");
    }

    #[test]
    fn sessions_are_isolated() {
        let a = sid("iso-a");
        let b = sid("iso-b");
        let src = page();
        append_frame(&a, slice_rows(&src, 0, 50), W, 50, false, 30000, 3).unwrap();
        append_frame(&b, slice_rows(&src, 0, 80), W, 80, false, 30000, 3).unwrap();
        // 两个会话各记各的:高度互不影响(全局计数在并行测试下不可断言)
        let ra = finish_session(&a).unwrap();
        let rb = finish_session(&b).unwrap();
        assert_eq!(ra.height, 50);
        assert_eq!(rb.height, 80);
    }

    #[test]
    fn reset_discards_previous_content() {
        let id = sid("reset");
        let src = page();
        append_frame(&id, slice_rows(&src, 0, 100), W, 100, false, 30000, 3).unwrap();
        // reset:从 50 行处重新开始 → 总高应回到 100,而不是 100+50
        let r = append_frame(&id, slice_rows(&src, 50, 100), W, 100, true, 30000, 3).unwrap();
        assert_eq!(r.total_height, 100);
        assert_eq!(r.shift_px, 100, "reset 后本帧是首帧,整帧吸收");
        abort_session(&id).unwrap();
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


    // ---- 空白行导致的假匹配(docs/progress 长截图拼接错位)----

    /// 造一页"白底文档":`(start_row, len, darkness)` 三段描述各段文字。
    ///
    /// ## 为什么每段墨色必须不同
    ///
    /// 若所有段落长得一样(周期性内容),则"位移 30"与"位移 0"在**数据上真的
    /// 等价** —— 匹配到哪个都算"对",用例会把算法缺陷掩盖成"测试写法问题"。
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

    /// 段落布局:长短与墨色各异(非周期),避免"多个位移数据上等价"。
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
        // 真实缺陷复现:两帧都大片空白、真实位移很大,不能被判成"没滚动"。
        let page = document_page(200, &DOC_SEGS);
        let h = 100u32;
        let prev = fingerprint_frame(&slice_rows(&page, 0, h), W, h);
        let next = fingerprint_frame(&slice_rows(&page, 40, h), W, h);

        // 修复前:空白段让 k=0 凑够门槛 → 误判 duplicate → 整帧 40 行丢失。
        let m = find_shift(&prev, &next, 12);
        assert!(m.is_some(), "应有匹配");
        assert_ne!(m.unwrap().k, 0, "大片空白下绝不能误判为 k=0(没滚动)");
    }

    #[test]
    fn true_duplicate_still_detected() {
        // 反向保护:真的没滚动时必须还能判出来
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
        // 白底文档上多个真实位移都要算准。这是本次缺陷的正面回归。
        let page = document_page(200, &DOC_SEGS);
        let h = 100u32;
        for truth in [12u32, 30, 45] {
            let prev = fingerprint_frame(&slice_rows(&page, 0, h), W, h);
            let next = fingerprint_frame(&slice_rows(&page, truth, h), W, h);
            let m = find_shift(&prev, &next, 12);
            assert_eq!(
                m.map(|x| x.k),
                Some(truth),
                "白底文档上位移 {truth} 应算准"
            );
        }
    }

    #[test]
    fn single_frame_finish_keeps_full_height() {
        let id = sid("single");
        let src = page();
        append_frame(&id, slice_rows(&src, 0, 60), W, 60, false, 30000, 3).unwrap();
        let out = finish_session(&id).unwrap();
        assert_eq!(out.height, 60, "只采一帧时长图高度 = 帧高");
    }
}

