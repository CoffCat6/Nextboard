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

} // namespace nb
