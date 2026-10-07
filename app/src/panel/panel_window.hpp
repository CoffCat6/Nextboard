// 主面板:Qt 无边框窗口内嵌 QQuickWidget(Qt Quick),承载历史/搜索/设置/翻译抽屉。
#pragma once

#include <QWidget>

#include "host/core_bridge.hpp"

class QHBoxLayout;
class QTimer;
class QQuickWidget;

namespace nb {

class PanelWindow : public QWidget {
    Q_OBJECT
    /// 是否"固定"面板:固定后失焦不自动隐藏(并同步置顶,便于随时可见)。
    /// QML 侧直接绑定 `panelWindow.pinned` 做按钮选中态。
    Q_PROPERTY(bool pinned READ isPinned NOTIFY pinnedChanged)
public:
    explicit PanelWindow(CoreBridge& core, QWidget* parent = nullptr);
    ~PanelWindow() override;

    /// 显示在光标所在显示器中央并前置(热键/托盘/单实例唤起共用)。
    void showAtCursor();

    /// 自动化验收:注入搜索词(等价用户输入,走同一防抖链路)。
    void applySearchText(const QString& text);

    /// QML 侧 ESC 调用(关闭抽屉后的兜底隐藏)。
    Q_INVOKABLE void hidePanel() { hide(); }

    /// QML 侧请求显示(截图翻译完成后把面板带回前台)。
    Q_INVOKABLE void showPanel() { showAtCursor(); }

    /// QML 侧拖动标题栏移动窗口(无边框窗口的移动入口,交给系统处理)。
    /// 内部会置"用户已移动"标记,此后 showAtCursor 不再自动居中。
    Q_INVOKABLE void startSystemMove();

    bool isPinned() const { return m_pinned; }
    /// 切换"固定"状态(面板标题栏的固定按钮)。
    /// 固定时:失焦不隐藏,并把窗口置顶 —— 本窗口是 Qt::Tool(不占任务栏),
    /// 若被其它窗口盖住将无处可点,因此必须配合置顶才真正可用。
    Q_INVOKABLE void setPinned(bool pinned);

    /// 给单实例过滤器用的 HWND 唤起对接(内部实现细节)。
    HWND nativeHandle() const;

protected:
    void showEvent(QShowEvent* e) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qint64* result) override;
    bool event(QEvent* e) override;

signals:
    /// 剪贴板变化(nbcore PostMessage 通知)→ 模型刷新。
    void clipChanged();
    /// "固定"状态变化(QML 按钮选中态)。
    void pinnedChanged();

private:
    void setupUi();

    CoreBridge& m_core;
    QQuickWidget* m_quick = nullptr;
    QTimer* m_blurTimer = nullptr;
    /// 窗口是否曾获得过焦点(获得之前不启用失焦隐藏,防"启动即退出")。
    bool m_everFocused = false;
    /// 是否固定(失焦不隐藏 + 置顶)。
    bool m_pinned = false;
    /// 用户是否手动拖动过窗口(拖过之后不再自动居中,尊重用户站位)。
    bool m_userMoved = false;
    /// nativeEvent 用的 HWND 缓存(创建期间禁止 winId() 调用,防递归)。
    HWND m_cachedHwnd = nullptr;
};

} // namespace nb
