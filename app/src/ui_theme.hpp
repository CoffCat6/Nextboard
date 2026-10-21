// 主题令牌的 **C++ 侧唯一来源**(自绘界面取色:截图浮条 / 长截图面板 / 标注工具条)。
//
// ## 为什么需要这个文件
//
// 截图浮条等界面**不是 QML**,拿不到 Theme.qml,于是历史上把颜色写死成深色:
// 底板 `#26272E`、正文 `#DFE1E8`、分隔线白色 28α —— 结果**无论什么主题,工具栏
// 都是深色**(用户报的现象)。而 QPalette 那条链(CoreFacade::applyTheme →
// QGuiApplication::setPalette)只覆盖 Qt 自带控件,自绘部分完全不受它影响。
//
// ## 三处的关系(改色时三处一起改,否则必然漂移)
//
// | 位置 | 角色 |
// | --- | --- |
// | `app/panel/qml/Theme.qml` | QML 侧唯一来源 |
// | **本文件** | C++ 侧唯一来源(取值与 Theme.qml 的 colors 逐项对齐) |
// | `CoreFacade::buildPalette` | QPalette 由此文件构建 → Basic 控件跟随 |
//
// 取值照抄 docs/theme.md §7/§8,但以 Theme.qml 的实现值为准(它对暗色
// textSecondary/textTertiary 各提亮过一档,见那里的注释)。
#pragma once

#include <QColor>
#include <QGuiApplication>
#include <QPalette>

namespace nb::ui {

/// 一套主题令牌。字段名与 Theme.qml `colors` 的属性名保持一致,
/// 便于两边对照(改一处时另一处能按名字找到)。
struct ThemeTokens {
    QColor background;
    QColor surface;
    QColor surfaceSecondary;
    QColor surfaceHover;
    QColor surfaceSelected;

    QColor border;
    QColor borderStrong;

    QColor textPrimary;
    QColor textSecondary;
    QColor textTertiary;
    QColor textDisabled;

    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor accentSoft;
    QColor accentText;

    QColor success;
    QColor warning;
    QColor error;
    QColor onWarning;
};

/// 暗色令牌(Theme.qml colors 的 dark 分支)。
inline const ThemeTokens& darkTokens() {
    static const ThemeTokens t{
        QColor(0x0E, 0x12, 0x17),   // background
        QColor(0x15, 0x1A, 0x21),   // surface
        QColor(0x1A, 0x20, 0x28),   // surfaceSecondary
        QColor(0x20, 0x27, 0x31),   // surfaceHover
        QColor(0x19, 0x2E, 0x4B),   // surfaceSelected
        QColor(0x29, 0x32, 0x3C),   // border
        QColor(0x37, 0x42, 0x4E),   // borderStrong
        QColor(0xF1, 0xF4, 0xF7),   // textPrimary
        QColor(0xC2, 0xCB, 0xD4),   // textSecondary
        QColor(0x8D, 0x99, 0xA6),   // textTertiary
        QColor(0x4D, 0x57, 0x62),   // textDisabled
        QColor(0x4C, 0x9A, 0xFF),   // accent
        QColor(0x6A, 0xA8, 0xFF),   // accentHover
        QColor(0x34, 0x78, 0xD4),   // accentPressed
        QColor(0x17, 0x2B, 0x46),   // accentSoft
        QColor(0xFF, 0xFF, 0xFF),   // accentText
        QColor(0x4A, 0xDE, 0x80),   // success
        QColor(0xFB, 0xBF, 0x24),   // warning
        QColor(0xF8, 0x71, 0x71),   // error
        QColor(0x1A, 0x14, 0x05),   // onWarning
    };
    return t;
}

/// 浅色令牌(Theme.qml colors 的 light 分支)。
inline const ThemeTokens& lightTokens() {
    static const ThemeTokens t{
        QColor(0xF4, 0xF6, 0xF8),   // background
        QColor(0xFF, 0xFF, 0xFF),   // surface
        QColor(0xF8, 0xFA, 0xFC),   // surfaceSecondary
        QColor(0xEE, 0xF2, 0xF6),   // surfaceHover
        QColor(0xE8, 0xF1, 0xFF),   // surfaceSelected
        QColor(0xD9, 0xE0, 0xE7),   // border
        QColor(0xC7, 0xD0, 0xDA),   // borderStrong
        QColor(0x17, 0x20, 0x2A),   // textPrimary
        QColor(0x5E, 0x6A, 0x78),   // textSecondary
        QColor(0x89, 0x94, 0xA1),   // textTertiary
        QColor(0xB6, 0xBE, 0xC8),   // textDisabled
        QColor(0x3B, 0x82, 0xF6),   // accent
        QColor(0x25, 0x63, 0xEB),   // accentHover
        QColor(0x1D, 0x4E, 0xD8),   // accentPressed
        QColor(0xE8, 0xF1, 0xFF),   // accentSoft
        QColor(0xFF, 0xFF, 0xFF),   // accentText
        QColor(0x16, 0xA3, 0x4A),   // success
        QColor(0xD9, 0x77, 0x06),   // warning
        QColor(0xDC, 0x26, 0x26),   // error
        QColor(0xFF, 0xFF, 0xFF),   // onWarning
    };
    return t;
}

/// 当前是否为暗色主题。
///
/// 判据是**应用调色板**,不是设置项原文:`CoreFacade::applyTheme` 已把
/// system/light/dark 解析成一套具体调色板,这里读结果就不会与设置页各算一遍
/// (note_window.cpp 的 isDarkTheme 也是同一口径)。取不到 QGuiApplication
/// (理论上只有极端测试环境)时按暗色兜底 —— 与 Theme.qml 的默认一致。
inline bool isDarkTheme() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return true;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

/// 按显式 dark 取令牌(测试/自检用:不依赖当前调色板)。
inline const ThemeTokens& themeTokens(bool dark) { return dark ? darkTokens() : lightTokens(); }

/// 当前主题的令牌(自绘界面在 paintEvent 里取;不缓存 —— 主题可即时切换)。
inline const ThemeTokens& themeTokens() { return themeTokens(isDarkTheme()); }

/// 按令牌构建一套 QPalette(Basic 风格控件的取色来源)。
///
/// 放在头文件而不是 core_facade.cpp 的匿名命名空间里,是为了让验收能**显式**
/// 切到另一套主题再断言自绘颜色 —— 否则验"浅色主题下浮条是不是浅色"就只能
/// 靠改系统设置,那是不可自动化的。
inline QPalette paletteFor(bool dark) {
    const ThemeTokens& t = themeTokens(dark);
    QPalette p;
    p.setColor(QPalette::Window, t.background);
    p.setColor(QPalette::WindowText, t.textPrimary);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.surfaceSecondary);
    p.setColor(QPalette::Button, t.surfaceSecondary);
    p.setColor(QPalette::ButtonText, t.textPrimary);
    p.setColor(QPalette::Text, t.textPrimary);
    // Theme.qml 的 textTertiary(暗色提亮一档保可读,见那边的注释)
    p.setColor(QPalette::PlaceholderText, t.textTertiary);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.accentText);
    p.setColor(QPalette::ToolTipBase, t.surfaceSecondary);
    p.setColor(QPalette::ToolTipText, t.textPrimary);
    // BrightText:暗色下用纯白(替代按钮等"反衬"文字),浅色下用纯黑。
    p.setColor(QPalette::BrightText, dark ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Link, t.accent);
    // 控件禁用态:Basic 风格会取这几个,不设就退回系统默认(与界面其余不一致)
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textDisabled);
    return p;
}

/// 复制一份颜色并改 alpha(主题令牌都是不透明的,自绘底板/分隔线需要透明度)。
inline QColor withAlpha(const QColor& color, int alpha) {
    QColor out = color;
    out.setAlpha(alpha);
    return out;
}

/// 自绘面板底板(浮条/二级条/长截图面板):主题 surface + 用户可调的不透明度。
///
/// 为什么用 surface 而不是 background:底板是**浮在内容之上的一块面**,
/// 语义就是 surface(theme.md §7/§8);background 是窗口底,压不住截图内容。
inline QColor panelPlate(int alpha) { return withAlpha(themeTokens().surface, alpha); }

/// 面板 1px 描边(theme.md §9:边框承担结构划分)。
///
/// 半透底板压在任意截图内容上,没有描边时边界会随底图"消失";2026-10-21
/// 之前完全没有描边,浅色主题下白底压在白页面上就是"看不见的浮条"。
inline QColor panelBorder() { return withAlpha(themeTokens().border, 210); }

/// 按钮悬停底:主题前景色的低 alpha —— 自动在深色下变"提亮"、浅色下变"压暗",
/// 不必为两套主题各写一个色值。
inline QColor hoverFill(int alpha = 28) { return withAlpha(themeTokens().textPrimary, alpha); }

/// 激活/选中按钮底(标注中、长截图态):accent 低 alpha 衬底 + accent 文字。
inline QColor activeFill(int alpha = 52) { return withAlpha(themeTokens().accent, alpha); }

} // namespace nb::ui
