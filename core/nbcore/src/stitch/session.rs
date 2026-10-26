//! 拼接会话:按 uuid 持有一组「结果图 + 最近若干帧指纹」,增量吸收新帧。
//!
//! ## 为什么是 begin/append/finish/abort 四段式(docs/19 v2)
//!
//! 旧契约只有 append(..., reset):会话在**首次 append** 时隐式创建,尺寸/上限
//! 全部来自那一刻的实参。后果是「会话属于谁」只能靠宿主的一个全局 uuid 变量维持,
//! 两块屏同时进长截图态就会共用一个会话 —— 帧宽不符只报一句 warn 然后静默丢帧。
//!
//! 新契约把「这个会话截的是哪块屏、哪个矩形、什么 DPI」在 begin 时**固化**:
//! - 采集期不必每帧重算坐标(中途缩放变化会变成显式的帧尺寸错误,而不是错位);
//! - 跨屏混用在契约上就不成立(begin 记录 monitor + region,append 逐帧校验);
//! - reset 参数消失(语义移到 begin)。
//!
//! ## 内存:增量拼接 + 字节预算
//!
//! 只保留**结果图**(width × total_height × 4)+ 最近 BASELINE_DEPTH 帧的**指纹**
//! (每行 8 字节,一帧 1000 行仅 8KB)。不留任何历史帧 —— 30 帧 4K 选区全留就是
//! 数百 MB,与本项目内存收口目标直接冲突。
//!
//! 高度上限取 min(stitch_max_height, stitch_max_bytes / 每行字节):只按像素限额
//! 是不够的 —— 1200×30000 的 RGBA 就是 144MB,像素数没超、内存先炸。

use std::collections::{HashMap, VecDeque};
use std::sync::Mutex;

use crate::error::{AppError, AppResult};

use super::fingerprint::{find_shift, fingerprint_frame, overlap_threshold, FrameFingerprint};
use super::{clamp_max_bytes, clamp_max_height, validate_frame, StitchDebug, StitchResult, StitchStatus};

/// 保留几帧指纹作为匹配基线(含最新一帧)。
///
/// 为什么要多基线:动态内容(时间戳、光标闪烁、视频)会让某一帧与上一帧失配。
/// 若失配即丢帧并把基准换成这一帧,那么「滚动手势中夹着一次动态刷新」就会让
/// 后续每一帧都对不上,长图原地停止增长。留 3 帧后,下一次匹配可以跳过那一帧
/// 直接对上更早的基线 —— 内容仍会少一段(如实记缺口 + 画警示条),但采集不会停摆。
pub const BASELINE_DEPTH: usize = 3;

/// 缺口警示条的高度(px)。docs/19 §4.3 承诺过「漏行要能被看见」,旧实现从未落地。
pub const GAP_BAND_HEIGHT: u32 = 4;

/// 警示条颜色(RGBA,不透明橙红)。刻意选一个几乎不会出现在文档/网页里的色,
/// 便于用户与验收脚本都能一眼认出来。
pub const GAP_BAND_RGBA: [u8; 4] = [255, 96, 32, 255];

/// begin_session 的入参:把「截哪块屏、哪个矩形、什么上限」一次性固化。
#[derive(Debug, Clone)]
pub struct SessionSpec {
    /// 显示器设备名(形如 DISPLAY1);空串表示宿主未提供,仅测试可用。
    pub monitor: String,
    /// 选区在该显示器内的**设备像素**矩形(相对显示器左上角)。
    pub region_x: i32,
    pub region_y: i32,
    pub region_w: u32,
    pub region_h: u32,
    /// 该屏的缩放比(1.0 = 100%)。仅记录与回传,不参与拼接换算
    /// (拼接全在设备像素域,这正是要消除 DPI 参与的方式)。
    pub dpr: f32,
    /// 像素高度上限(会按 MIN_MAX_HEIGHT..MAX_MAX_HEIGHT 钳制)。
    pub max_height: i64,
    /// 字节上限(0 或负数 = 不限制;否则参与推导有效行数上限)。
    pub max_bytes: i64,
    /// 重叠判定门槛(百分比,钳到 1..=20)。
    pub overlap_ratio: i64,
}

struct Session {
    monitor: String,
    region_x: i32,
    region_y: i32,
    /// 结果图宽度 = 选区宽(设备像素)。
    width: u32,
    /// 选区高(设备像素)。**每一帧都必须是这个高度**,否则报错(不再静默)。
    region_h: u32,
    dpr: f32,
    /// 结果图(RGBA8,宽 × 当前总高)。
    rgba: Vec<u8>,
    /// 当前总高。
    height: u32,
    /// 最近若干帧的指纹,新的在尾部。
    baselines: VecDeque<FrameFingerprint>,
    /// 上一帧的**原始像素**(仅保留一帧,供互相关兜底用)。
    ///
    /// 为什么要多留一份像素:固定背景画面下逐行比对必然失败,而互相关需要真的
    /// 像素才能算行投影。只留一帧,内存增量 = 选区一帧(927×724×4 ≈ 2.7MB),
    /// 与"结果图"相比可忽略。
    last_frame: Vec<u8>,
    /// 累计缺口数(内容确实丢了)。
    gaps: u32,
    /// 已插入的警示条数量。
    bands: u32,
    /// 上一次匹配失败、还没画警示条(等下一次真正吸收时补一条)。
    pending_gap: bool,
    /// 有效高度上限(= min(像素上限, 字节上限推导值),且至少容纳一帧)。
    max_height: u32,
    overlap_ratio: i64,
    /// 是否已吸收过首帧。
    has_frame: bool,
}

/// 会话表。Mutex 内只做内存操作,持锁时间 = 一次帧拷贝,不涉及 IO。
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

/// 创建一个新会话(尚未吸收任何帧)。
///
/// 同一 id 重复 begin 视为**错误**:要么宿主漏了 abort,要么两个遮罩共用了 id ——
/// 两种情况都不该被「悄悄重置」掩盖过去。
pub fn begin_session(session: &str, spec: SessionSpec) -> AppResult<()> {
    if session.is_empty() {
        return Err(AppError::InvalidInput("长截图会话 id 为空".into()));
    }
    if spec.region_w == 0 || spec.region_h == 0 {
        return Err(AppError::InvalidInput(format!(
            "长截图选区尺寸为 0: {}x{}",
            spec.region_w, spec.region_h
        )));
    }
    let pixel_limit = clamp_max_height(spec.max_height);
    let byte_limit = clamp_max_bytes(spec.max_bytes);
    // 每行字节 = 宽 × 4;字节上限 / 每行 = 允许的行数
    let row_bytes = spec.region_w as u64 * 4;
    let by_bytes = if byte_limit == 0 {
        pixel_limit
    } else {
        (byte_limit / row_bytes).min(u32::MAX as u64) as u32
    };
    // 至少容纳一帧:否则 begin 成功但 append 必然失败,错误会推迟到更难懂的地方。
    let effective = pixel_limit.min(by_bytes.max(spec.region_h));
    with_sessions(|map| {
        if map.contains_key(session) {
            return Err(AppError::InvalidInput(format!(
                "长截图会话 {session} 已存在(重复 begin;上次未 abort?)"
            )));
        }
        log::info!(
            "长截图会话开始: {session} monitor={} region=({},{},{}x{}) dpr={} 像素上限={} 字节上限行数={}",
            spec.monitor,
            spec.region_x,
            spec.region_y,
            spec.region_w,
            spec.region_h,
            spec.dpr,
            pixel_limit,
            by_bytes
        );
        map.insert(
            session.to_owned(),
            Session {
                monitor: spec.monitor,
                region_x: spec.region_x,
                region_y: spec.region_y,
                width: spec.region_w,
                region_h: spec.region_h,
                dpr: spec.dpr,
                rgba: Vec::new(),
                height: 0,
                baselines: VecDeque::with_capacity(BASELINE_DEPTH),
                last_frame: Vec::new(),
                gaps: 0,
                bands: 0,
                pending_gap: false,
                max_height: effective,
                overlap_ratio: spec.overlap_ratio.clamp(1, 20),
                has_frame: false,
            },
        );
        Ok(())
    })?
}

/// 吸收一帧(帧必须是 begin 时约定的选区尺寸)。
///
/// 第一帧总是被完全吸收(shift_px = 帧高)。
pub fn append_frame(session: &str, rgba: Vec<u8>, width: u32, height: u32) -> AppResult<StitchResult> {
    if session.is_empty() {
        return Err(AppError::InvalidInput("长截图会话 id 为空".into()));
    }
    validate_frame(rgba.len(), width, height)?;

    with_sessions(|map| {
        let Some(state) = map.get_mut(session) else {
            return Err(AppError::InvalidInput(format!(
                "长截图会话 {session} 不存在(未 begin 或已结束)"
            )));
        };

        // ---- 尺寸校验:不符就报错,绝不静默丢帧 ----
        // 旧实现只比宽度;高度变化(选区被改/取错区域)会让「重叠 = min(prev_h-k, next_h)」
        // 算出一个看似合理的值,拼出来的图缺行却毫无提示。
        if state.width != width {
            return Err(AppError::InvalidInput(format!(
                "长截图帧宽与选区不符:选区为 {},本次为 {}(选区在采集中被改变?)",
                state.width, width
            )));
        }
        if state.region_h != height {
            return Err(AppError::InvalidInput(format!(
                "长截图帧高与选区不符:选区为 {},本次为 {}(取错屏幕或选区被改变?)",
                state.region_h, height
            )));
        }

        // ---- 首帧:整帧吸收 ----
        if !state.has_frame {
            let cap_rows = height.min(state.max_height);
            let keep = cap_rows as usize * width as usize * 4;
            // 首帧像素留一份给互相关兜底:必须先克隆,再让 stored 被 truncate 消费。
            // (顺序写反过一次 —— 先 move 进 stored 再读 rgba 是编译期就能抓到的错。)
            state.last_frame = rgba.clone();
            if state.last_frame.len() != width as usize * height as usize * 4 {
                state.last_frame.clear();
            }
            let mut stored = rgba;
            stored.truncate(keep);
            let fp = fingerprint_frame(&stored, width, cap_rows);
            state.baselines.clear();
            state.baselines.push_back(fp);
            state.rgba = stored;
            state.height = cap_rows;
            state.has_frame = true;
            let truncated = cap_rows < height;
            if truncated {
                state.gaps = state.gaps.saturating_add(1);
                log::warn!("长截图:首帧被高度上限截断({cap_rows}/{height} 行)");
            }
            return Ok(StitchResult {
                matched: true,
                shift_px: cap_rows,
                total_height: cap_rows,
                reason: if truncated {
                    StitchStatus::Limit
                } else {
                    StitchStatus::Ok
                },
                gaps: state.gaps,
                bands: state.bands,
                debug: None,
            });
        }

        let next_fp = fingerprint_frame(&rgba, width, height);
        let threshold = overlap_threshold(height, state.overlap_ratio);

        // ---- 多基线匹配:从最新往前试 ----
        // 命中较老基线(age > 0)意味着中间那几帧的内容没进结果图 —— 如实记缺口。
        let mut hit: Option<(usize, super::fingerprint::ShiftMatch)> = None;
        for age in 0..state.baselines.len() {
            let idx = state.baselines.len() - 1 - age;
            let base = &state.baselines[idx];
            // ⚠️ 只校验宽度,**不校验高度**:命中上限时基线会被截断成
            //    「实际上进了结果图的那几行」,它比选区矮是**正常**的
            //    (旧实现正是靠 find_shift 允许两帧高度不同才没在这里出错)。
            if base.width != width {
                continue;
            }
            if let Some(m) = find_shift(base, &next_fp, threshold) {
                hit = Some((age, m));
                break;
            }
        }

        let Some((age, m)) = hit else {
            // ---- 逐行比对失配:先用**互相关**兜底 ----
            //
            // 固定背景画面(应用设了个人背景图)在数学上不可能让"连续相等"成立:
            // 真实位移处占多数的背景行全部不匹配(真机实测 838/927 行是背景)。
            // 互相关把整帧压成一条曲线比较整体形状,背景贡献被去均值抵消,
            // 内容贡献真正的相关峰 —— 这是这类画面唯一可靠的估计方式。
            if !state.last_frame.is_empty()
                && state.last_frame.len() == width as usize * height as usize * 4
            {
                let threshold = overlap_threshold(height, state.overlap_ratio);
                if let Some((m2, corr)) = crate::stitch::fingerprint::find_shift_correlation(
                    &state.last_frame,
                    &rgba,
                    width,
                    height,
                    threshold,
                ) {
                    let new_rows2 = m2.new_rows(height);
                    if new_rows2 > 0 {
                        log::info!(
                            "长截图:行比对失配,互相关兜底命中 k={} corr={:.3} new_rows={new_rows2}",
                            m2.k,
                            corr
                        );
                        let room = state.max_height.saturating_sub(state.height);
                        let take = new_rows2.min(room);
                        if take > 0 {
                            if state.pending_gap {
                                push_gap_band(state);
                                state.pending_gap = false;
                            }
                            let src_start = m2.overlap as usize * width as usize * 4;
                            let take_bytes = take as usize * width as usize * 4;
                            let src_end = (src_start + take_bytes).min(rgba.len());
                            state.rgba.extend_from_slice(&rgba[src_start..src_end]);
                            state.height = state.height.saturating_add(take);
                            let truncated = take < new_rows2;
                            if truncated {
                                state.baselines.clear();
                                push_baseline(
                                    state,
                                    fingerprint_frame(&rgba[src_start..src_end], width, take),
                                );
                                // 截断分支不走 commit_frame:整帧仍要留作互相关兜底的参照。
                                state.last_frame = rgba;
                            } else {
                                // commit_frame 内部已把 rgba 收进 last_frame,无需再赋值。
                                commit_frame(state, next_fp, rgba, width, height);
                            }
                            return Ok(StitchResult {
                                matched: true,
                                shift_px: take,
                                total_height: state.height,
                                reason: if truncated {
                                    StitchStatus::Limit
                                } else {
                                    StitchStatus::Ok
                                },
                                gaps: state.gaps,
                                bands: state.bands,
                                debug: Some(StitchDebug {
                                    k: m2.k,
                                    run: m2.run,
                                    evidence: m2.evidence,
                                    overlap: m2.overlap,
                                    threshold,
                                    baseline_age: 0,
                                }),
                            });
                        }
                    }
                }
            }

            // ---- 兜底也失配:可能是滚太快,也可能是向上滚 ----
            let backward = state
                .baselines
                .back()
                .map(|b| looks_backward(b, &next_fp))
                .unwrap_or(false);
            state.gaps = state.gaps.saturating_add(1);
            state.pending_gap = true;
            // 以本帧为新基线(否则后续全部失配),但**保留**更老的基线:
            // 下一帧若能对上上一帧,就能继续拼,不必等用户重新滚。
            commit_frame(state, next_fp, rgba, width, height);
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
                bands: state.bands,
                debug: Some(StitchDebug {
                    k: 0,
                    run: 0,
                    evidence: 0,
                    overlap: 0,
                    threshold,
                    baseline_age: 0,
                }),
            });
        };

        let new_rows = m.new_rows(height);

        // 整帧都在基线里 → 真的没滚动。
        // 必须**逐行全等**才判 duplicate:白底文档上大片空白能让 k=0 凑够门槛,
        // 而画面其实滚过一大截(见 fingerprint::is_blank 的实测记录)。
        if new_rows == 0 {
            let identical = state
                .baselines
                .back()
                .map(|b| rows_all_equal(b, &next_fp))
                .unwrap_or(false);
            if m.k == 0 && identical {
                commit_frame(state, next_fp, rgba, width, height);
                return Ok(StitchResult {
                    matched: false,
                    shift_px: 0,
                    total_height: state.height,
                    reason: StitchStatus::Duplicate,
                    gaps: state.gaps,
                    bands: state.bands,
                    debug: Some(debug_of(&m, threshold, age)),
                });
            }
            // 首行对齐但内容已变(动画/重绘):不是「没滚动」,按失配处理
            state.gaps = state.gaps.saturating_add(1);
            state.pending_gap = true;
            commit_frame(state, next_fp, rgba, width, height);
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: StitchStatus::NoOverlap,
                gaps: state.gaps,
                bands: state.bands,
                debug: Some(debug_of(&m, threshold, age)),
            });
        }

        // ---- 高度上限:只吸收「还能放下」的部分 ----
        // 命中较老基线,或之前有失配未记账,都说明这一帧之前丢过内容 → 补警示条。
        //
        // ⚠️ 失配本身也要补一条:失配 = 这段内容**永远进不了结果图**,
        //    若只在"下一次成功吸收前"补,用户若失配后就点「结束」,产物上
        //    完全没有"这里缺了一段"的痕迹 —— 那正是真实缺陷(静默缺行)。
        //    故 finish 时对 pending_gap 兜底补条(见 finish_session)。
        let lost_before = age > 0 || state.pending_gap;
        let band_rows = if lost_before { GAP_BAND_HEIGHT } else { 0 };
        if age > 0 {
            log::warn!("长截图:跳过 {age} 帧后仍匹配上(中间内容缺失)");
        }

        let room = state.max_height.saturating_sub(state.height);
        if room <= band_rows {
            commit_frame(state, next_fp, rgba, width, height);
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: StitchStatus::Limit,
                gaps: state.gaps,
                bands: state.bands,
                debug: Some(debug_of(&m, threshold, age)),
            });
        }

        // 先补警示条(插在「缺的那一段」与「新内容」之间)
        if band_rows > 0 {
            push_gap_band(state);
            state.pending_gap = false;
        }

        let room_left = state.max_height.saturating_sub(state.height);
        let take = new_rows.min(room_left);
        if take == 0 {
            commit_frame(state, next_fp, rgba, width, height);
            return Ok(StitchResult {
                matched: false,
                shift_px: 0,
                total_height: state.height,
                reason: StitchStatus::Limit,
                gaps: state.gaps,
                bands: state.bands,
                debug: Some(debug_of(&m, threshold, age)),
            });
        }

        // 新内容在 next 里的起始行 = overlap 行之后
        let src_start = m.overlap as usize * width as usize * 4;
        let take_bytes = take as usize * width as usize * 4;
        let src_end = (src_start + take_bytes).min(rgba.len());

        state.rgba.extend_from_slice(&rgba[src_start..src_end]);
        state.height = state.height.saturating_add(take);

        // 触发上限时只吸收了前 take 行,基线必须同步截断:
        // 若仍以整帧为基线,下一帧会按「重叠 = min(prev_h - k, next_h)」算,
        // 而 prev_h 里那部分内容其实没进结果图 —— 位移会算小,产物缺行。
        if take < new_rows {
            state.baselines.clear();
            push_baseline(state, fingerprint_frame(&rgba[src_start..src_end], width, take));
        } else {
            commit_frame(state, next_fp, rgba, width, height);
        }

        Ok(StitchResult {
            matched: true,
            shift_px: take,
            total_height: state.height,
            reason: if take < new_rows {
                StitchStatus::Limit // 只装下一部分:已达上限
            } else {
                StitchStatus::Ok
            },
            gaps: state.gaps,
            bands: state.bands,
            debug: Some(debug_of(&m, threshold, age)),
        })
    })?
}


/// 更新基线,并把本帧像素留作互相关兜底的参照。
///
/// ⚠️ 两条路径都必须更新 last_frame:否则下一次失配时会用**两帧之前**的像素
/// 做互相关,位移算出来是累计值,拼出来必然错位。
fn commit_frame(state: &mut Session, fp: FrameFingerprint, rgba: Vec<u8>, width: u32, height: u32) {
    push_baseline(state, fp);
    let expected = width as usize * height as usize * 4;
    if rgba.len() == expected {
        state.last_frame = rgba;
    } else {
        state.last_frame.clear();
    }
}

fn debug_of(m: &super::fingerprint::ShiftMatch, threshold: u32, age: usize) -> StitchDebug {
    StitchDebug {
        k: m.k,
        run: m.run,
        evidence: m.evidence,
        overlap: m.overlap,
        threshold,
        baseline_age: age as u32,
    }
}

fn push_baseline(state: &mut Session, fp: FrameFingerprint) {
    state.baselines.push_back(fp);
    while state.baselines.len() > BASELINE_DEPTH {
        state.baselines.pop_front();
    }
}

/// 在结果图末尾插入一条 4px 警示条 —— 让「这里漏了内容」在产物上可见。
fn push_gap_band(state: &mut Session) {
    for _ in 0..GAP_BAND_HEIGHT {
        for _ in 0..state.width {
            state.rgba.extend_from_slice(&GAP_BAND_RGBA);
        }
    }
    state.height = state.height.saturating_add(GAP_BAND_HEIGHT);
    state.bands = state.bands.saturating_add(1);
}

/// 两帧是否**逐行完全相同**(真正的「没滚动」)。
///
/// 只比指纹,不比像素:指纹已覆盖均值与纹理,逐行全等足以判定「画面没变」;
/// 而逐像素比对在 4K 帧上是几十毫秒的开销,不值得。
fn rows_all_equal(prev: &FrameFingerprint, next: &FrameFingerprint) -> bool {
    if prev.height != next.height || prev.width != next.width {
        return false;
    }
    prev.rows == next.rows
}

/// 弱判据:本帧末行是否出现在上一帧的**更早位置**(说明内容往回走了)。
///
/// 只用末行一个点,故意保守 —— 误判 backward 的代价是「提示到顶」而用户其实
/// 在往下滚,而漏判的代价只是把「到顶」说成「滚太快」,后者无害。
fn looks_backward(prev: &FrameFingerprint, next: &FrameFingerprint) -> bool {
    let Some(last) = next.rows.last() else {
        return false;
    };
    let Some(first) = next.rows.first() else {
        return false;
    };
    let pos_first = prev.rows.iter().position(|r| r == first);
    let pos_last = prev.rows.iter().position(|r| r == last);
    match (pos_first, pos_last) {
        (Some(pf), Some(pl)) => pl < pf,
        _ => false,
    }
}

/// 结束会话并取出拼接结果(PNG 字节)。
///
/// 收尾公共段:取走会话并补上未画的警示条(编码/移交前的最后一步)。
fn finish_take_state(session: &str) -> AppResult<Session> {
    if session.is_empty() {
        return Err(AppError::InvalidInput("长截图会话 id 为空".into()));
    }
    let state = with_sessions(|map| map.remove(session))?
        .ok_or_else(|| AppError::InvalidInput("长截图会话不存在(可能已结束或已取消)".into()))?;
    if state.height == 0 || state.width == 0 {
        return Err(AppError::Internal("长截图会话为空(一帧都没采到)".into()));
    }
    // 收尾兜底:最后一次失配之后若没再成功吸收,警示条还没画 —— 但那段内容确实
    // 丢了。必须在编码前补上,否则产物是一张"静默缺行"的长图(docs/19 §4.3 禁止)。
    let mut state = state;
    if state.pending_gap && state.height > 0 {
        push_gap_band(&mut state);
    }
    Ok(state)
}

pub fn finish_session(session: &str) -> AppResult<Finished> {
    let state = finish_take_state(session)?;
    let png = encode_png(&state.rgba, state.width, state.height)?;
    Ok(Finished {
        png,
        width: state.width,
        height: state.height,
        gaps: state.gaps,
        bands: state.bands,
        monitor: state.monitor,
        region_x: state.region_x,
        region_y: state.region_y,
        dpr: state.dpr,
    })
}

/// finish_session 的产物。
pub struct Finished {
    /// PNG 字节(未 base64;FFI 层负责编码)。
    pub png: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub gaps: u32,
    /// 已插入的警示条数量。
    pub bands: u32,
    /// 采集所属显示器(回传给宿主做日志/诊断)。
    pub monitor: String,
    pub region_x: i32,
    pub region_y: i32,
    pub dpr: f32,
}

/// [`finish_session`] 的**未编码**形态:直接交出拼接 RGBA 缓冲(零拷贝 move)。
///
/// 长图成品入库走 RGBA 直通道(`ingest_capture_rgba`)时,整条链路只剩落盘
/// blob 那一次 PNG 编码 —— 此前"拼完编码 → 宿主解码 → 入库再编码"的两次
/// 整图编码 + 一次解码与 base64/JSON 往返(128MB 级长图上秒级 CPU、~200MB
/// 峰值)全部省去。
pub struct FinishedRaw {
    /// RGBA8888 紧凑排布,width × height × 4 字节。
    pub rgba: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub gaps: u32,
    pub bands: u32,
    pub monitor: String,
    pub region_x: i32,
    pub region_y: i32,
    pub dpr: f32,
}

/// 同 [`finish_session`],产物为原始 RGBA(见 [`FinishedRaw`])。
pub fn finish_session_rgba(session: &str) -> AppResult<FinishedRaw> {
    let state = finish_take_state(session)?;
    Ok(FinishedRaw {
        rgba: state.rgba,
        width: state.width,
        height: state.height,
        gaps: state.gaps,
        bands: state.bands,
        monitor: state.monitor,
        region_x: state.region_x,
        region_y: state.region_y,
        dpr: state.dpr,
    })
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
