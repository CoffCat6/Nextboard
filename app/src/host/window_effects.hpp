// 窗口外观工具:无边框窗口的系统圆角。
//
// 为什么用 DWM 而不是"QML 圆角 + 透明窗口":
// 本项目对渲染异常极其敏感(历史上多次出现"整片空白"回归),而透明分层窗口在
// QQuickWidget 上有额外的合成风险(需要 WA_TranslucentBackground + 引擎 clearColor
// 同时透明,任一处不对就是全黑/全透明)。DWM 由系统在窗口层完成圆角,零渲染改动;
// Windows 10 及更早不支持该属性,自动退化为直角,不会出错。
#pragma once

#include <QWidget>

#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

namespace nb {

/// 让无边框窗口在 Windows 11 上获得系统圆角(其它系统上无副作用)。
/// 必须在窗口已创建(show 之后)调用;内部 winId() 会按需创建原生窗口。
inline void applyRoundedCorners(QWidget* widget) {
    if (!widget) return;
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    if (!hwnd) return;
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33,DWMWCP_ROUND = 2。
    // 老版本 SDK 头文件没有该枚举,直接传字面量避免依赖 SDK 版本。
    const DWORD preference = 2;
    ::DwmSetWindowAttribute(hwnd, 33, &preference, sizeof(preference));
}

/// 无边框窗口的边缘缩放命中测试。
///
/// 无边框(FramelessWindowHint)窗口没有非客户区,系统不会自行提供缩放边框。
/// 在 `nativeEvent` 里拦 WM_NCHITTEST 并返回 HTLEFT / HTBOTTOMRIGHT 等命中码,
/// 剩下的拖动、Aero Snap、双击最大化全部交回系统完成 —— 比在 QML 里自己算
/// 鼠标位移要平滑得多,也不会有跟手抖动。
///
/// @return HT* 命中码;返回 -1 表示不是缩放热区,交由 Qt 默认处理。
/// @note 只用 GetWindowRect / ScreenToClient 取物理像素,与 WM_* 消息坐标系一致,
///       因此不受高 DPI 缩放影响。调用方必须传入已缓存的 HWND —— 在 nativeEvent
///       里调 winId() 会递归触发窗口创建。
inline int framelessResizeHitTest(QWidget* widget, HWND hwnd, LPARAM lParam,
                                  int borderLogicalPx = 6) {
    if (!widget || !hwnd) return -1;
    // 最大化 / 全屏时不提供边缘缩放,避免把窗口从最大化状态拖出来
    if (widget->isMaximized() || widget->isFullScreen()) return -1;

    const qreal dpr = widget->devicePixelRatioF();
    const int border = qMax(4, qRound(borderLogicalPx * dpr));

    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc)) return -1;
    const long x = GET_X_LPARAM(lParam);
    const long y = GET_Y_LPARAM(lParam);
    if (x < rc.left || x >= rc.right || y < rc.top || y >= rc.bottom) return -1;

    const bool left = x < rc.left + border;
    const bool right = x >= rc.right - border;
    const bool top = y < rc.top + border;
    const bool bottom = y >= rc.bottom - border;

    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return -1;
}

} // namespace nb
