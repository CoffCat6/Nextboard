// 全局字体令牌:本项目所有字体族名的**唯一来源**。
//
// 为什么单独抽一个文件(Qt 的坑,本项目踩过):
//
//   1) QML 的 `font.family` 是**单值**,底层走 `QFont::setFamily` —— 而它是
//      `setFamilies(QStringList(family))`,**单元素**,会把应用字体
//      (`QApplication::setFont` 的 setFamilies 回退链)整条覆盖掉。于是
//      "中文缺字 → 回退到哪"改由 Qt 的 font matching 决定,实测落到
//      **宋体(SimSun)**,与界面其余文字的雅黑不一致 —— 表现为"同一个界面里
//      两种中文字体"(这是设置界面字体看起来乱的真因,不是字号问题)。
//
//      修正手段:`QFont::insertSubstitution` 把缺字回退钉到雅黑。这样 QML 里
//      显式写 `font.family` 也不会掉到宋体,不必逐处改 35 个调用点。
//      ⚠️ 替换表必须在**任何 QFont 解析之前**装(QFont 文档:替换后需重建
//      QFont 才生效)。
//
//   2) 回退链里的族名写错是**静默失效**的:本项目曾写 "Noto Sans CJK SC",
//      而 Qt 字体库里实际只有 "Noto Sans SC",等于那条回退从来没起作用过。
//      → 改族名前先用 QFontDatabase::families() 核对名字(实测见
//      docs/progress.md 字体收口记录)。
//
// 分层约定:C++ 侧(自绘文本、位图渲染、菜单)用 uiFont()/monoFont();
// QML 侧用 Theme.fontFamily / Theme.fontMono(值与这里的链保持一致)。
#pragma once

#include <QFont>
#include <QStringList>

namespace nb {

/// UI 正文字体族链:微软雅黑 UI 优先。
/// 行高比经典雅黑紧 1px(14px 下 18 vs 19),更适合界面正文。
const QStringList& uiFontFamilies();

/// 技术字段用的等宽字体族链:Cascadia Mono(Win11 自带)→ Consolas(全版本)。
/// ⚠️ 两者都不含中文字形 —— 中文(如中文路径、JSON 里的中文)靠
///    installFontSubstitutions() 的替换表回退到雅黑,否则会掉到宋体。
const QStringList& monoFontFamilies();

/// 全局 UI 字号(像素)。与 QML `Theme.metrics.fontBody` 保持同一数值。
constexpr int kUiFontPixelSize = 14;

/// 托盘菜单字号(像素)。**故意小于界面正文**(14px)。
///
/// 托盘右键菜单是系统级弹层,会和桌面/任务栏上其它右键菜单并排出现:
/// 用界面正文的 14px 会明显偏大、与资源管理器等原生菜单不成比例(实测菜单
/// 158x316 → 12px 后 140x253)。12px 与界面 Caption 档一致,取的是"系统弹层"
/// 而非"应用正文"的观感基准。
///
/// ⚠️ 这里必须是**显式**字号:早先 tray.cpp 写的是
///    `if (pointSizeF() <= 0) setPointSizeF(9.0)`,靠"应用字体没有磅值"来兜底 ——
///    而应用字体是用 setPixelSize 设的,pointSizeF() 恒为 -1,于是该分支必然触发,
///    字号实际由这条隐式路径决定(9pt≈12px)。字号必须写在这里,不能再靠意外。
constexpr int kTrayMenuFontPixelSize = 12;

/// 安装缺字回退替换表(把单族声明缺字后的落点钉到雅黑)。
/// **必须在任何 QFont 解析之前调用。**
void installFontSubstitutions();

/// 安装全局应用字体 = 替换表 + QApplication::setFont。
/// 调用时机:QApplication 构造之后、任何窗口/控件创建之前
/// (见 setupQmlEnvironment,必须在 QQuickStyle::setStyle 之前)。
void installApplicationFont();

/// 按字号构造 UI 正文字体(C++ 自绘 / 位图渲染用,带回退链)。
QFont uiFont(int pixelSize = kUiFontPixelSize);

/// 按字号构造等宽字体(技术字段;中文经替换表回退雅黑)。
QFont monoFont(int pixelSize = 13);

} // namespace nb
