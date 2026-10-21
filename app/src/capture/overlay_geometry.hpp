// 遮罩浮条 / 标注场景的**纯几何**(不依赖窗口,可被验收直接断言)。
//
// 为什么单独成文件:遮罩上同时存在三套坐标系,混用一次就出一次"标注画不准"的缺陷
// (历史上连续两次栽在这里),而浮条自身的摆位也有"跑出屏幕就点不到"的坑。
//   1) 鼠标事件 / 窗口 rect = **逻辑像素**(Qt 高 DPI 自动缩放);
//   2) 冻结底图 = **桌面物理像素**(核心按物理采);
//   3) 标注引擎场景 = **选区局部物理像素** —— 引擎底图是 `m_frozen.copy(选区)`,
//      原点已被搬到选区左上角,1 场景单位 = 1 物理像素。
// 本文件把 2↔3 的换算与浮条/二级条的摆位集中成纯函数:draw 与命中测试**共用**,
// 杜绝"画在一处、点在另一处"。
#pragma once

#include <QList>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

namespace nb::overlay {

// ---- 浮条公共尺寸(逻辑像素)----
// 放在头文件是为了让**验收脚本能按同一份数字**算出按钮位置去真点,不会出现
// "测试点到了别处"这种假通过。
// 按钮数含「透明」档位(docs/17 §17.5)。
// 8 个按钮:标注/OCR取字/长截图/贴图/保存/透明/完成/取消
// 长截图插在 OCR 之后、贴图之前(docs/19 §3.1):截图类动作相邻,存取类动作相邻。
constexpr int kToolbarButtons = 8;
// 按钮均分宽度:8 个按钮仍要落在 1366 宽屏可容纳的范围内(8×71 = 568,
// 与原 7×79 = 553 基本同宽)。再窄(68 以下)中文两字 + 内边距就挤了。
constexpr int kToolbarButtonWidth = 71;
constexpr int kToolbarWidth = kToolbarButtons * kToolbarButtonWidth;
constexpr int kButtonHeight = 44;                             // 单行按钮高
constexpr int kBarMargin = 8;                                 // 浮条离选区边的间距
constexpr int kSubToolsRow = 34;                              // 二级条·工具行高
constexpr int kSubParamsRow = 44;                             // 二级条·颜色/粗细/撤销行高
constexpr int kSubbarWidth = kToolbarWidth;
constexpr int kSubbarHeight = kSubToolsRow + kSubParamsRow;
constexpr int kBarGap = 6;                                    // 浮条与二级条之间的缝

/// 引擎场景矩形(选区**局部**物理像素):原点 (0,0),尺寸 = 选区逻辑尺寸 × dpr。
/// ⚠️ 绝不能拿 selectionPhysical()(桌面**全局**物理坐标)去索引场景:
///    场景原点已在选区左上,传全局 rect 等于"把场景的右下角一块当选区渲染",
///    表现为标注整体偏移 + 画面右侧/下方大片空白(实测 compose 图上标注落在左上 1/4)。
QRect annotationSceneRect(const QRect& selectionLogical, qreal dpr);

/// 遮罩逻辑坐标 → 引擎场景坐标(相对选区左上,物理像素)。
QPointF toScene(const QPoint& widgetPos, const QRect& selectionLogical, qreal dpr);

/// 主浮条矩形(局部逻辑坐标):默认压在选区下方 margin 处;底部放不下翻到选区上方;
/// 上方也放不下时**必须夹住** —— 否则整条落到屏幕外,表现为"框选后看不到工具栏/点不到"。
QRect toolbarRectFor(const QRect& selectionLogical, const QSize& widgetSize,
                     const QSize& barSize, int margin);

/// 二级条矩形:默认叠在浮条上方;顶部放不下改到浮条下方;最后夹在窗口内。
QRect subbarRectFor(const QRect& toolbarRect, const QSize& barSize, int gap,
                    const QSize& widgetSize);

// ---- 滚动长截图(docs/19)----

/// 长截图面板尺寸(逻辑像素):两个按钮均分 = 「开始/结束」切换 +「取消」。
///
/// 两种状态的**宽度相同**(切换按钮按最长文案算宽),这样面板不会在开始采集的
/// 瞬间跳一下 —— 跳变会让用户以为点错了。
constexpr int kStitchButtons = 2;
constexpr int kStitchButtonWidth = 96;
constexpr int kStitchPanelWidth = kStitchButtons * kStitchButtonWidth;
constexpr int kStitchPanelHeight = 44;
/// 采集节拍的**默认**值(毫秒)。实际值取设置项 stitchSampleIntervalMs。
///
/// v2 从 500ms 收到 150ms:采集从"整屏捕获 + 裁剪"改成"直抓选区"后单帧成本
/// 降到约 1/8,收紧节拍既更跟手,也让快速滚动时相邻帧重叠更充分(缺行更少)。
/// 重复帧由核心按 duplicate 丢弃(不占内存),故"定频采样"不会因为手停而膨胀。
constexpr int kStitchSampleIntervalMs = 150;
/// 节拍的合法范围(与核心 settings 的 MIN/MAX_STITCH_SAMPLE_MS 同口径)。
constexpr int kStitchSampleMinMs = 80;
constexpr int kStitchSampleMaxMs = 1000;

/// 长截图控制面板矩形(局部逻辑坐标)。
///
/// ## 硬约束:绝不能与选区相交(docs/19 v2 §3.2)
///
/// 旧实现在"上下都放不下"时会**压进选区底部内侧** —— 而选区正是被挖掉的采集窗口:
/// 那块区域不属于本窗口(画不出来)、鼠标事件直达下层(点不到)。于是整屏选区下
/// 用户看到的是"面板消失",点「开始」的点击被下层页面吃掉,采集根本不会启动。
///
/// 新约束:**面板与选区零相交**。退让顺序:下方 → 上方 → 左右两侧 → 最后夹进窗口
/// 并**继续避开选区**(宁可压住选区外的边缘内容,也不进采集区)。
/// 若窗口内确实无处可放(选区占满整窗且面板比剩余空白大),返回的矩形可能与选区
/// 相交 —— 调用方必须用 stitchPanelFitsOutside() 判一次并走降级路径。
QRect stitchPanelRectFor(const QRect& selectionLogical, const QSize& widgetSize,
                         const QSize& panelSize, int margin);

/// 面板是否完全落在选区之外(即"画得出来、点得到")。
/// 与 stitchPanelRectFor 配套:false 时调用方必须换形态(见 stitchPanelRectFor 注释)。
bool stitchPanelFitsOutside(const QRect& panelRect, const QRect& selectionLogical);

/// **候选**面板位置(按优先级排序,已剔除与选区相交者)。
///
/// 为什么给一串而不是一个:选区可能贴满某几条边,第一个位置放不下时调用方还要
/// 依次试后面的。返回空表 = 窗口内没有任何位置能放下(只能让面板浮在选区内,
/// 此时依赖"浮窗不进画面"的机制,见 docs/19 v2 §3.2 第 2/3 道保险)。
QList<QRect> stitchPanelCandidates(const QRect& selectionLogical, const QSize& widgetSize,
                                   const QSize& panelSize, int margin);

/// 采集期"描边"四条边(局部逻辑坐标):只在选区**外侧**画 2px,故天然不进入抓取区域。
/// 返回的顺序固定为 上/下/左/右,空矩形表示该边在屏外(或选区贴边)无需描边。
QList<QRect> stitchFrameRects(const QRect& selectionLogical, const QSize& widgetSize, int thickness);

} // namespace nb::overlay
