// 标注工具栏的**几何图标**(按工具/动作绘制 QIcon,零图片资源)。
//
// 与 QML 侧的 HeaderIcon.qml 是同一套造型语言:
//   - Outline 线性风、16px 视口、1.5px 描边(docs/theme.md §38);
//   - 只用几何图形,**不用字体字形/emoji** —— 跨字体渲染不可控(实测 emoji 在
//     不同 Windows 版本上宽度/基线漂移,工具条会抖);
//   - 几何数字一律按 16px 基准写,再由 size 换算,换尺寸不会走样。
//
// ⚠️ 为什么 C++ 侧要再画一份(而不是复用 HeaderIcon.qml):
//    编辑器是 QWidget/QToolBar,拿不到 QML 的类型;跨语言桥(离屏渲染取图)
//    复杂度远高于收益。代价是"造型存在两份",故把几何集中在本文件,
//    并在 HeaderIcon.qml 顶部注释互指,改造型时两边一起看。
#pragma once

#include <QColor>
#include <QCursor>
#include <QIcon>
#include <QPalette>

#include "annotation_engine.hpp"

namespace nb {

/// 描边色。**必须由调用方注入**:应用有暗/亮两套主题(docs/theme.md §7),
/// 写死浅色会让图标在浅色主题下整片看不见(与 HeaderIcon.qml 由调用方注入 color 同一手法)。
QColor defaultIconInk(bool dark);
/// 从控件当前调色板取描边色(工具条用;自动跟随 applyTheme 注入的 QPalette)。
QColor iconInkFor(const QPalette& palette);

/// 工具图标(与 toolbarTools() 的每个枚举一一对应)。
QIcon toolIcon(EditorTool tool, int size, const QColor& ink);

/// 动作图标:撤销 / 重做 / 另存为 / 贴图 / OCR / 完成 / 取消 / 透明度 / 缩放 / 旋转。
enum class BarIcon { Undo, Redo, SaveAs, Pin, Ocr, Done, Cancel, Opacity, ZoomFit,
                     ZoomOriginal, Pan, RotateLeft, RotateRight };
/// 手型(Pan)与绘图工具同属"画布模式",故不放进 EditorTool 枚举。
QIcon barIcon(BarIcon icon, int size, const QColor& ink);

/// **鼠标光标**(不是图标):给绘图工具用的精致小十字。
///
/// 为什么自定义而不是直接用 Qt::CrossCursor:内置十字是**1px 硬边、无中心点**的
/// 老样式,在高分屏上又细又糊(用户反馈"不太美观")。这里用几何绘制做一个
/// "细描边 + 中心点 + 浅色描边保证任何底色上都能看清"的准星,
/// 与 theme.md §38 的"几何绘制、零图片资源"一致。
///
/// @param dark 当前是否深色主题(决定描边取浅色还是深色)
/// @param dpr  设备像素比:光标是位图,**必须按 dpr 渲染并设 devicePixelRatio**,
///             否则高分屏上会被放大成模糊的色块。
QCursor crosshairCursor(bool dark, qreal dpr);

/// 粗细档位的示意图标(实心圆点,半径随档位)。
QIcon strokeIcon(int width, int size, const QColor& ink);
/// 字号档位的示意图标(方框高度示意字号)。
QIcon fontSizeIcon(int pixelSize, int size, const QColor& ink);

} // namespace nb
