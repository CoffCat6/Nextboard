// 主面板:Qt 无边框窗口内嵌 Sciter 子窗口,承载历史/搜索/设置/翻译抽屉。
#pragma once

#include <QWidget>

#include "host/core_bridge.hpp"

class QHBoxLayout;
class QTimer;

namespace nb {

class PanelWindow : public QWidget {
    Q_OBJECT
public:
    explicit PanelWindow(CoreBridge& core, QWidget* parent = nullptr);
    ~PanelWindow() override;

    /// 显示在光标所在显示器中央并前置(热键/托盘/单实例唤起共用)。
    void showAtCursor();

    /// 自动化验收:注入搜索词(等价用户输入,走同一防抖链路)。
    void applySearchText(const QString& text);

    /// 给单实例过滤器用的 HWND 唤起对接(内部实现细节)。
    HWND nativeHandle() const;

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qint64* result) override;
    bool event(QEvent* e) override;

signals:
    /// 剪贴板变化(nbcore PostMessage 通知)→ 面板刷新。
    void clipChanged();

private:
    void setupUi();

    CoreBridge& m_core;
    class SciterView* m_sciter = nullptr;
    QTimer* m_blurTimer = nullptr;
    /// 窗口是否曾获得过焦点(获得之前不启用失焦隐藏,防"启动即退出")。
    bool m_everFocused = false;
    /// nativeEvent 用的 HWND 缓存(创建期间禁止 winId() 调用,防递归)。
    HWND m_cachedHwnd = nullptr;
};

} // namespace nb
