//! 行指纹:把一帧的一行压成 64 位,用于快速比对"两帧的这两行是不是同一行"。
//!
//! 只取**中间列**(默认横向 [20%, 80%]):滚动条、固定侧栏、浮动广告都在两侧边缘,
//! 参与匹配会让"页面没动"与"页面动了但侧栏没动"分不开(docs/19 §4.2 步骤 1)。
//!
//! 指纹由两部分拼成,互为补充:
//!
//! - **低 32 位 = 行均值**:抓整体亮度。跨行比较稳定,能扛住有损压缩的轻微色偏;
//! - **高 32 位 = 行内相邻差分绝对和**:抓"这条行有没有纹理"。
//!   只有均值的话,"一整行纯白"与"另一处的一整行纯白"会判等 —— 表格、代码块里
//!   大片留白正会如此,于是位移被算错、拼出来缺行。差分项把这种情况区分开。
//!
//! 两者都是**顺序敏感**的:同一行像素的"均值 + 差分和"与内容强相关,而计算量是
//! 每像素两次加法 —— 对 4K 宽的一帧(单行 ~8000 像素)只有几十微秒。

/// 一行像素的指纹。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RowFingerprint(pub u64);

impl RowFingerprint {
    /// 空指纹(用于"头部/尾部不足"的填充,永不与真实行相等地参与匹配判定)。
    pub const EMPTY: RowFingerprint = RowFingerprint(0);

    /// 该行是否**没有可用于定位的信息**(纯白/近纯白且无纹理)。
    ///
    /// ## 为什么必须识别它 —— 这是"文档类内容拼接错位"的根因
    ///
    /// 指纹由「行均值 + 行内差分」构成。一个纯白的空白行,其指纹是
    /// `(0, 765)` —— 而**页面上所有空白行的指纹完全相同**。
    ///
    /// 于是当选区里大片是空白(白底文档、两段之间的间距)时:
    /// - `prev` 的第 0 行是空白,`next` 的第 0 行也是空白 → 指纹相等;
    /// - 连续几十行空白全部相等 → `run` 轻松超过门槛被判定"匹配成功";
    /// - 结论往往是 `k = 0`(没滚动)→ 该帧被当作 `duplicate` **整帧丢弃**。
    ///
    /// 实测(白底文档 233 行,80 个不同指纹,最长 33 行连续相同):真实滚动
    /// 195 行时被误判成"没滚动",那一帧的内容全部丢失 —— 表现就是长截图拼接处
    /// **凭空少了一段**。
    ///
    /// 故匹配时必须要求"证据行"里含足够多的**非空白行**;纯空白段不能作为
    /// "两帧在同一位置"的证据。
    pub fn is_blank(self) -> bool {
        let diff = (self.0 >> 32) as u32;
        let mean = self.0 as u32;
        // mean 是 R+G+B 的均值(0..765);纯白 = 765。
        // 阈值放到 740 是给有损压缩/抗锯齿留余量 —— 近白也算空白。
        diff <= 4 && mean >= 740
    }
}

/// 一帧的指纹表:每行一个指纹,加上采样区间与行数。
#[derive(Debug, Clone)]
pub struct FrameFingerprint {
    /// 逐行指纹,长度 = 帧高。
    pub rows: Vec<RowFingerprint>,
    /// 帧宽(像素,完整宽度 —— 采样是按它算出来的)。
    pub width: u32,
    /// 帧高(像素)。
    pub height: u32,
}

impl FrameFingerprint {
    /// 帧高(便捷访问)。
    pub fn height(&self) -> u32 {
        self.height
    }
}

/// 计算一帧的逐行指纹。
///
/// `rgba` 必须是 `width * height * 4` 字节的 RGBA8(调用方已用
/// [`super::validate_frame`] 校验过)。
pub fn fingerprint_frame(rgba: &[u8], width: u32, height: u32) -> FrameFingerprint {
    let stride = width as usize * 4;
    let (x0, x1) = sample_range(width);
    let mut rows = Vec::with_capacity(height as usize);
    for y in 0..height as usize {
        let row = &rgba[y * stride..(y + 1) * stride];
        rows.push(fingerprint_row(row, x0, x1));
    }
    FrameFingerprint { rows, width, height }
}

/// 单行指纹:`[x0, x1)` 列范围内,均值(低 32 位)+ 相邻差分绝对和(高 32 位)。
fn fingerprint_row(row: &[u8], x0: usize, x1: usize) -> RowFingerprint {
    if x1 <= x0 {
        return RowFingerprint::EMPTY;
    }
    let mut sum: u64 = 0;
    // 差分按"灰度"算(而不是逐通道):人眼对亮度结构敏感,通道间的轻微色偏
    // (有损压缩常见)不该影响"这两行是不是同一行"的判定。
    let mut diff_sum: u64 = 0;
    let mut prev_gray: i32 = -1;
    for x in x0..x1 {
        let p = x * 4;
        let r = row[p] as u64;
        let g = row[p + 1] as u64;
        let b = row[p + 2] as u64;
        sum += r + g + b;
        // 整数灰度近似(0.299/0.587/0.114 → 移位近似),只为结构比较,不追求精确
        let gray = ((r * 77 + g * 150 + b * 29) >> 8) as i32;
        if prev_gray >= 0 {
            diff_sum += (gray - prev_gray).unsigned_abs() as u64;
        }
        prev_gray = gray;
    }
    let count = (x1 - x0) as u64;
    let mean = sum / count.max(1);
    RowFingerprint(((diff_sum & 0xFFFF_FFFF) << 32) | (mean & 0xFFFF_FFFF))
}

/// 采样区间 `[x0, x1)`(像素列)。
///
/// 中间 60% 默认值来自 [`super::SAMPLE_LEFT_RATIO`] / [`super::SAMPLE_RIGHT_RATIO`]。
/// 极窄帧(宽度不足 16px)退化为整行 —— 此时"避开侧栏"已经没有意义。
pub fn sample_range(width: u32) -> (usize, usize) {
    if width < 16 {
        return (0, width as usize);
    }
    let x0 = (width as f32 * super::SAMPLE_LEFT_RATIO).floor() as usize;
    let x1 = (width as f32 * super::SAMPLE_RIGHT_RATIO).ceil() as usize;
    let x1 = x1.min(width as usize);
    if x1 <= x0 {
        (0, width as usize)
    } else {
        (x0, x1)
    }
}

/// 一次成功的位移匹配。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ShiftMatch {
    /// `next` 的首行对应 `prev` 的第 `k` 行(即"上一帧被滚过去多少行")。
    pub k: u32,
    /// 从对齐点起**连续相等**的行数。
    pub run: u32,
    /// 重叠行数上界 = `min(prev.height - k, next.height)`。
    pub overlap: u32,
    /// `run` 里**非空白**的行数(真正的定位证据)。
    ///
    /// 空白行在全页随处可见,不能证明"两帧在同一位置"。选择最优位移时
    /// 必须以本字段为主序,否则白底文档上"空白对空白"的假位移会胜出。
    pub evidence: u32,
}

impl ShiftMatch {
    /// 本帧带来的**新内容行数** = `next.height - overlap`。
    ///
    /// 为 0 表示整帧都在上一帧里出现过 —— 即"没滚动"(调用方判 `duplicate`)。
    pub fn new_rows(&self, next_height: u32) -> u32 {
        next_height.saturating_sub(self.overlap)
    }
}

/// 找 `prev` 尾部与 `next` 头部的最优对齐位移。
///
/// 语义:`next[0]` 对上 `prev[k]`。若 `k = s` 则是"页面向下滚了 s 行"。
///
/// `min_overlap_rows` 是接受门槛:连续相等行数低于它就判为**匹配失败**
/// (`None`),由调用方按"滚动太快、有内容漏掉"处理。
///
/// ## 为什么用"最长连续相等"而不是"相等行数统计"
///
/// 统计法在重复内容上会挑出错误位移:表格里每隔几十行就有相同表头,把这些零散
/// 命中累加起来反而比真位移得分高。要求**从对齐点起连续相等**,假位移几乎立刻
/// 中断,真位移则能一路连下去。
///
/// ## 为什么允许"没跑到重叠终点"
///
/// 早期版本要求重叠区`逐行全等`才算命中,结果是**一行动态内容就整体失配**
/// (时间戳、光标闪烁、视频帧)。改为"取连续段最长的候选,只要够 `min_overlap_rows`
/// 就采纳":动态内容只会让 `run` 提前中断,不至于否定整次匹配。
/// 漏行风险由调用方按 `overlap` 与 `run` 的差值判断(见 `session.rs`)。
pub fn find_shift(
    prev: &FrameFingerprint,
    next: &FrameFingerprint,
    min_overlap_rows: u32,
) -> Option<ShiftMatch> {
    if prev.height == 0 || next.height == 0 {
        return None;
    }
    // 两帧宽度必须一致 —— 否则像素列没有可比性(调用方会先报错,这里是兜底)
    if prev.width != next.width {
        return None;
    }
    let prev_h = prev.height as usize;
    let next_h = next.height as usize;

    // 候选位移 k ∈ [0, prev_h):next[0] 对上 prev[k]。从**小到大**遍历,
    // 同一得分取最小 k(重叠最多的解)—— 页面小幅回弹时多个 k 都成立,
    // 取重叠多的那个不会漏内容。
    let mut best: Option<ShiftMatch> = None;
    for k in 0..prev_h {
        let overlap = (prev_h - k).min(next_h);
        if (overlap as u32) < min_overlap_rows {
            break; // 再往后重叠更少,不可能达标
        }
        // 快速预筛:首行不等则这个 k 连起点都不成立(绝大多数 k 死在这里)
        if prev.rows[k] != next.rows[0] {
            continue;
        }
        let mut run = 0usize;
        // 其中的"证据行":非空白的相等行。**纯空白行不能算证据** ——
        // 它们在全页随处可见,会把任意位移都凑成"连续相等"(见 is_blank 的注释)。
        let mut evidence = 0usize;
        while run < overlap && prev.rows[k + run] == next.rows[run] {
            if !prev.rows[k + run].is_blank() {
                evidence += 1;
            }
            run += 1;
        }
        if (run as u32) < min_overlap_rows {
            continue;
        }
        // 证据行不足 → 这个位移只是"两边都白",不构成匹配。
        // 门槛取 min_overlap_rows 的一半且至少 4 行:真正的重叠区里总会有
        // 文字/线条,而假匹配(空白对空白)几乎拿不出证据行。
        let need_evidence = ((min_overlap_rows / 2).max(4)) as usize;
        if evidence < need_evidence {
            continue;
        }
        let candidate = ShiftMatch {
            k: k as u32,
            run: run as u32,
            overlap: overlap as u32,
            evidence: evidence as u32,
        };
        // 择优顺序:**证据行数 > 连续行数 > 较小的 k**。
        //
        // 为什么证据行优先:白底文档里 k=0 常能靠几十行空白凑出很长的 run,
        // 而真位移的重叠区里文字少、run 反而更短。若按 run 择优,假位移必胜 ——
        // 这正是"拼接处凭空少一段"的直接原因(实测位移 195 被判成 0)。
        match best {
            Some(b) if (b.evidence, b.run) >= (candidate.evidence, candidate.run) => {}
            _ => best = Some(candidate),
        }
    }
    best
}

/// 重叠门槛:行数与比例取大者。
///
/// 比例的意义:高帧(4K 竖屏 2160)上 12 行太弱,一处重复图案就能凑够;
/// 低帧(200 行的小窗口)上比例门槛又太严。故两者取大。
pub fn overlap_threshold(frame_height: u32, ratio_percent: i64) -> u32 {
    let by_ratio = (frame_height as f32 * (ratio_percent.clamp(1, 20) as f32) / 100.0)
        .round() as u32;
    by_ratio.max(super::MIN_OVERLAP_ROWS)
}

// ---------- 行投影互相关(固定背景场景)----------
//
// ## 为什么必须再有一条估计路径
//
// 真机日志(DSH 聊天区设了个人背景图)显示:k=0 时 838/927 行逐行相等,
// 但没有**任何** k>0 能让「从第 0 行起连续相等」成立 —— 于是每一帧都被判
// no-overlap,长图只有一屏高。
//
// 原因:背景图**固定在屏幕上**,同一行号两帧像素一致;而文字在内容坐标里移动。
// 真实位移 k 处,占多数的背景行全部不匹配,只有约 10% 的文字行能对上 ——
// 「连续相等」这个前提在这种画面上数学上不成立。
//
// ## 解法:把每帧压成一条一维曲线,做归一化互相关
//
// 每行取「采样列区间内的水平梯度能量」作为该行的值(文字是强边缘,平滑背景
// 边缘很弱 → 曲线主要由内容决定)。背景给两条曲线贡献的是**常数基线**,
// 去均值后相互抵消;文字则贡献一个真正的相关峰。
//
// 这条路径与逐行比对的关键差别:**允许近似匹配**。内容行只占 10% 时,
// 逐行精确比较必然失败,而相关性仍能认出峰值。
//
// 实测(离线,按真机参数 H=927 / 内容 89 行构造):位移 23/40/87/150/400 全部
// 命中,corr 0.88~0.998;静止时峰在 k=0。

/// 互相关的最小可接受相关系数。
///
/// 0.75 的依据:离线实测固定背景 + 内容行仅 10% 时,真实位移处 corr 为
/// 0.88~0.998;而两张不相关画面(动态内容/换页)corr 通常 < 0.3。
/// 取 0.75 留出足够间隔,避免把「不同源」错当成「匹配上」。
pub const CORR_MIN: f32 = 0.75;

/// 一帧的**行投影曲线**:每行的水平梯度能量。
pub fn row_profile(rgba: &[u8], width: u32, height: u32) -> Vec<f32> {
    let stride = width as usize * 4;
    let (x0, x1) = sample_range(width);
    let mut out = Vec::with_capacity(height as usize);
    for y in 0..height as usize {
        let row = &rgba[y * stride..(y + 1) * stride];
        let mut energy = 0.0f32;
        let mut prev: Option<i32> = None;
        for x in x0..x1 {
            let i = x * 4;
            let g = (row[i] as i32 * 77 + row[i + 1] as i32 * 150 + row[i + 2] as i32 * 29) >> 8;
            if let Some(p) = prev {
                energy += (g - p).abs() as f32;
            }
            prev = Some(g);
        }
        out.push(energy);
    }
    out
}

/// 去掉均值(消除固定背景的常数基线贡献)。
fn demean(v: &mut [f32]) {
    if v.is_empty() {
        return;
    }
    let mean = v.iter().sum::<f32>() / v.len() as f32;
    for x in v.iter_mut() {
        *x -= mean;
    }
}

fn l2(v: &[f32]) -> f32 {
    v.iter().map(|x| x * x).sum::<f32>().sqrt()
}

/// 用归一化互相关找 `prev` 相对 `next` 的位移(即页面向下滚了多少行)。
///
/// 返回 `(k, correlation)`;相关系数低于 [`CORR_MIN`] 时视为失败 ——
/// 相关性弱意味着两张画面不同源(动态内容/换了页面),不能硬拼。
pub fn find_shift_correlation(
    prev: &[u8],
    next: &[u8],
    width: u32,
    height: u32,
    min_overlap_rows: u32,
) -> Option<(ShiftMatch, f32)> {
    let mut pa = row_profile(prev, width, height);
    let mut pb = row_profile(next, width, height);
    demean(&mut pa);
    demean(&mut pb);

    let ph = pa.len();
    let nh = pb.len();
    let mut best: Option<(usize, f32)> = None;
    for k in 0..ph {
        let overlap = (ph - k).min(nh);
        if (overlap as u32) < min_overlap_rows {
            break;
        }
        let a = &pa[k..k + overlap];
        let b = &pb[..overlap];
        let (na, nb) = (l2(a), l2(b));
        if na < 1e-6 || nb < 1e-6 {
            continue;
        }
        let dot: f32 = a.iter().zip(b.iter()).map(|(x, y)| x * y).sum();
        let corr = dot / (na * nb);
        if best.map_or(true, |(_, bc)| corr > bc) {
            best = Some((k, corr));
        }
    }
    let (k, corr) = best?;
    if corr < CORR_MIN {
        return None;
    }
    let overlap = (ph - k).min(nh) as u32;
    Some((
        ShiftMatch {
            k: k as u32,
            run: overlap,
            overlap,
            evidence: overlap,
        },
        corr,
    ))
}
