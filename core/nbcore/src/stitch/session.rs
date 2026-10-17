//! 拼接会话:按 uuid 持有一组"结果图 + 上一帧指纹",增量吸收新帧。
//!
//! 状态放进程级 `Mutex<HashMap>`(与 `translate` 的 `HISTORY` 同一套路),
//! 宿主只持有 uuid 字符串 —— 无跨语言生命周期问题。
//!
//! ## 为什么要全局表而不是把会话交给宿主
//!
//! 宿主(C++ 侧)没有测试设施(docs/04 §8 D12),若会话状态放宿主,拼接的
//! "增量累加"逻辑就落到没有单测的一侧。放这里则可完全离线验证。
//!
//! ## 内存:增量拼接
//!
//! 只在会话里保留**结果图**(width × total_height × 4)+ **上一帧指纹**
//! (height × 8 字节)。不留任何历史帧 —— 30 帧 4K 选区全留就是数百 MB,
//! 与本项目内存收口目标直接冲突。

use std::collections::HashMap;
use std::sync::Mutex;

use crate::error::{AppError, AppResult};

use super::fingerprint::{fingerprint_frame, find_shift, overlap_threshold, FrameFingerprint};
use super::{clamp_max_height, validate_frame, StitchResult, StitchStatus};

/// 单个会话。
struct Session {
    width: u32,
    /// 结果图(RGBA8,宽 × 当前总高)。
    rgba: Vec<u8>,
    /// 当前总高。
    height: u32,
    /// 上一帧(最新一帧)的指纹,用于与下一帧匹配。
    last: FrameFingerprint,
    /// 累计缺口数(每次 `no-overlap` 记一次)。
    gaps: u32,
    /// 拼接高度上限(px),会话创建时快照(会话中途改设置不该影响进行中的会话)。
    max_height: u32,
    /// 重叠判定门槛比例(%)。
    overlap_ratio: i64,
}

/// 会话表。`Mutex` 内只做内存操作,持锁时间 = 一次帧拷贝,不涉及 IO。
static SESSIONS: Mutex<Option<HashMap<String, Session>>> = Mutex::new(None);

fn with_sessions<T>(f: impl FnOnce(&mut HashMap<String, Session>) -> T) -> AppResult<T> {
    let mut guard = SESSIONS
        .lock()
        .map_err(|_| AppError::Internal("长截图会话表锁失败".into()))?;
    let map = guard.get_or_insert_with(HashMap::new);
    Ok(f(map))
}

/// 当前活跃会话数(验收与泄漏排查用)。
pub fn active_session_count() -> usize {
    with_sessions(|m| m.len()).unwrap_or(0)
}

/// 吸收一帧。
///
/// - `session`:会话 id(宿主生成;首次调用即创建)。空串非法。
/// - `reset`:true = 先丢弃该会话已有内容(换选区/重新开始)再吸收本帧。
/// - `max_height` / `overlap_ratio`:仅**首次创建**会话时生效(中途改设置不影响进行中的会话)。
///
/// 第一帧总是被完全吸收(`shift_px = height`,`reason = ok`)。
pub fn append_frame(
    session: &str,
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    reset: bool,
    max_height: i64,
    overlap_ratio: i64,
) -> AppResult<StitchResult> {
    if session.is_empty() {
        return Err(AppError::InvalidInput("长截图会话 id 为空".into()));
    }
    validate_frame(rgba.len(), width, height)?;
    let limit = clamp_max_height(max_height);
    let ratio = overlap_ratio.clamp(1, 20);

    with_sessions(|map| {
        if reset {
            map.remove(session);
        }

        // ---- 会话不存在:创建并吸收首帧 ----
        let Some(state) = map.get_mut(session) else {
            let cap_rows = height.min(limit);
            let keep = cap_rows as usize * width as usize * 4;
            let mut stored = rgba;
            stored.truncate(keep);
            let last = fingerprint_frame(&stored, width, cap_rows);
            map.insert(
                session.to_owned(),
                Session {
                    width,
                    height: cap_rows,
                    rgba: stored,
                    last,
                    gaps: 0,
                    max_height: limit,
                    overlap_ratio: ratio,
                },
            );
            return Ok(StitchResult {
                matched: true,
                shift_px: cap_rows,
                total_height: cap_rows,
                reason: StitchStatus::Ok,
                gaps: 0,
            });
        };

        // ---- 已有会话 ----
        if state.width != width {
            return Err(AppError::InvalidInput(format!(
                "长截图帧宽不一致:会话为 {},本次为 {}(选区在采集中被改变?)",
                state.width, width
            )));
        }

        let prev = state.last.clone();
        let threshold = overlap_threshold(height, state.overlap_ratio);
        let next_fp = fingerprint_frame(&rgba, width, height);

        let Some(m) = find_shift(&prev, &next_fp, threshold) else {
            // 匹配失败:可能是滚太快(无重叠),也可能是向上滚。
            // 区分方式:若 next 的**末行**出现在 prev 里且位置更靠前,说明往回滚了。
            state.gaps = state.gaps.saturating_add(1);
            let backward = looks_backward(&prev, &next_fp);
            state.last = next_fp; // 以本帧为新基准,避免后续全部失配
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: if backward {
                    StitchStatus::Backward
                } else {
                    StitchStatus::NoOverlap
                },
                gaps: state.gaps,
            });
        };

        // k == 0 且重叠覆盖整帧 → 完全没动
        //
        // ⚠️ 判 duplicate 前必须**确认整帧逐行一致**,不能只看 `run >= 门槛`:
        //    `run` 只保证"从头开始连续相等了 N 行",N 可能远小于帧高。
        //    白底文档上大片空白会让 k=0 轻松拿到几十行"相等",而实际画面
        //    已经滚过一大截 —— 那样这一帧会被当成"没滚动"整帧丢掉,
        //    长图拼接处凭空少一段(实测就是这样丢掉了 195 行)。
        let new_rows = m.new_rows(height);
        if m.k == 0 && new_rows == 0 {
            if rows_all_equal(&prev, &next_fp) {
                return Ok(StitchResult {
                    matched: false,
                    shift_px: 0,
                    total_height: state.height,
                    reason: StitchStatus::Duplicate,
                    gaps: state.gaps,
                });
            }
            // 首行对齐但后续不同 → 构图变了(动画/重绘),不是"没滚动"。
            // 按无重叠处理:明确记缺口,不静默丢内容。
            state.gaps = state.gaps.saturating_add(1);
            state.last = next_fp;
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: StitchStatus::NoOverlap,
                gaps: state.gaps,
            });
        }

        // 位移为 0 但 next 比 prev 高(选区变过?)—— 不该发生,按无重叠处理
        if m.k == 0 && new_rows > 0 {
            state.gaps = state.gaps.saturating_add(1);
            state.last = next_fp;
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: StitchStatus::NoOverlap,
                gaps: state.gaps,
            });
        }

        // 高度上限:只吸收"还能放下"的部分
        let room = state.max_height.saturating_sub(state.height);
        if room == 0 {
            state.last = next_fp;
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: StitchStatus::Limit,
                gaps: state.gaps,
            });
        }
        let take = new_rows.min(room);

        // 新内容在 next 里的起始行 = overlap 行之后
        let src_start_row = m.overlap as usize;
        let src_start = src_start_row * width as usize * 4;
        let take_bytes = take as usize * width as usize * 4;
        let src_end = (src_start + take_bytes).min(rgba.len());

        state.rgba.extend_from_slice(&rgba[src_start..src_end]);
        state.height = state.height.saturating_add(take);
        // ⚠️ 触发上限时只吸收了前 `take` 行,基准必须同步截断:
        //    若仍以整帧为基准,下一帧会按"重叠 = min(prev_h - k, next_h)"算,
        //    而 prev_h 里那部分内容其实没进结果图 —— 位移会算小,产物缺行。
        //    (上限命中后宿主会停止采集,这里是"宿主不听话也不出错"的兜底。)
        state.last = if take < new_rows {
            fingerprint_frame(&rgba[src_start..src_end], width, take)
        } else {
            next_fp
        };

        Ok(StitchResult {
            matched: take > 0,
            shift_px: take,
            total_height: state.height,
            reason: if take < new_rows {
                StitchStatus::Limit // 只装下一部分:已达上限
            } else {
                StitchStatus::Ok
            },
            gaps: state.gaps,
        })
    })?
}

/// 两帧是否**逐行完全相同**(真正的"没滚动")。
///
/// 只比指纹,不比像素:指纹已覆盖均值与纹理,逐行全等足以判定"画面没变";
/// 而逐像素比对在 4K 帧上是几十毫秒的开销,不值得。
fn rows_all_equal(prev: &FrameFingerprint, next: &FrameFingerprint) -> bool {
    if prev.height != next.height {
        return false;
    }
    prev.rows == next.rows
}

/// 弱判据:本帧末行是否出现在上一帧的**更早位置**(说明内容往回走了)。
///
/// 只用末行一个点,故意保守 —— 误判 `backward` 的代价是"提示到顶"而用户其实
/// 在往下滚,而漏判的代价只是把"到顶"说成"滚太快",后者无害。
fn looks_backward(prev: &FrameFingerprint, next: &FrameFingerprint) -> bool {
    let Some(last) = next.rows.last() else {
        return false;
    };
    let Some(first) = next.rows.first() else {
        return false;
    };
    // 本帧首行在上一帧中的位置
    let pos_first = prev.rows.iter().position(|r| r == first);
    let pos_last = prev.rows.iter().position(|r| r == last);
    match (pos_first, pos_last) {
        // 本帧的末行在上一帧里比首行更靠上 → 内容回退
        (Some(pf), Some(pl)) => pl < pf,
        _ => false,
    }
}

/// 结束会话并取出拼接结果(PNG 字节)。
///
/// 成功后会话被销毁(无论成功失败,取完即弃):长截图是**一次成型**的产物,
/// 不支持"取完继续追加"(docs/19 §2.2 N5)。
pub fn finish_session(session: &str) -> AppResult<Finished> {
    if session.is_empty() {
        return Err(AppError::InvalidInput("长截图会话 id 为空".into()));
    }
    let state = with_sessions(|map| map.remove(session))?
        .ok_or_else(|| AppError::InvalidInput("长截图会话不存在(可能已结束或已取消)".into()))?;
    if state.height == 0 || state.width == 0 {
        return Err(AppError::Internal("长截图会话为空".into()));
    }
    let png = encode_png(&state.rgba, state.width, state.height)?;
    Ok(Finished {
        png,
        width: state.width,
        height: state.height,
        gaps: state.gaps,
    })
}

/// `finish_session` 的产物。
pub struct Finished {
    /// PNG 字节(未 base64;FFI 层负责编码)。
    pub png: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub gaps: u32,
}

/// 取消会话(释放缓冲)。会话不存在时静默成功(幂等)。
pub fn abort_session(session: &str) -> AppResult<()> {
    if session.is_empty() {
        return Ok(());
    }
    with_sessions(|map| {
        map.remove(session);
    })?;
    Ok(())
}

fn encode_png(rgba: &[u8], width: u32, height: u32) -> AppResult<Vec<u8>> {
    let mut out = std::io::Cursor::new(Vec::new());
    use image::ImageEncoder as _;
    // 长图纵向重复度高(deflate 对重复行极友好),用 Fast 压缩已经很快;
    // 用 Default 在 30000px 上会明显卡住 UI 线程(编码发生在 FFI 调用内)。
    image::codecs::png::PngEncoder::new_with_quality(
        &mut out,
        image::codecs::png::CompressionType::Fast,
        image::codecs::png::FilterType::Sub,
    )
    .write_image(rgba, width, height, image::ExtendedColorType::Rgba8)
    .map_err(AppError::Image)?;
    Ok(out.into_inner())
}
