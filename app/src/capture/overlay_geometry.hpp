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

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

namespace nb::overlay {

// ---- 浮条公共尺寸(逻辑像素)----
// 放在头文件是为了让**验收脚本能按同一份数字**算出按钮位置去真点,不会出现
// "测试点到了别处"这种假通过。
// 按钮数含「透明」档位(docs/17 §17.5)。
constexpr int kToolbarButtons = 7;                            // 标注/OCR/贴图/保存/透明/完成/取消
// 按钮均分宽度:7 个按钮仍要落在原先的浮条总宽附近(552 → 553),故从 92 收到 79。
constexpr int kToolbarButtonWidth = 79;
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

} // namespace nb::overlay
