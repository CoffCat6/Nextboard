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
    /// 当前页:`list`(剪贴板历史)/ `translate`(翻译视图)。
    /// QML 据此互斥显示两页;尺寸、最小尺寸与失焦策略也随它切换(见 Mode)。
    Q_PROPERTY(QString mode READ modeString NOTIFY modeChanged)
public:
    /// 窗口的两种形态。合并翻译页之后,同一个窗口要同时服务"呼出即用的历史列表"
    /// 与"需要停留的翻译工作区",两者的尺寸/下限/失焦策略都不同。
    enum class Mode { List, Translate };
    Q_ENUM(Mode)

    explicit PanelWindow(CoreBridge& core, QWidget* parent = nullptr);
    ~PanelWindow() override;

    /// 显示在**光标处**(以光标为窗口中心,夹回屏幕可用区)并前置
    /// (热键/托盘/单实例唤起共用)。
    /// 热键的语义是"呼出剪贴板历史",故会先切回列表页。
    void showAtCursor();

    /// 切到翻译页并显示窗口 —— 三条打开链路(面板按钮/条目右键/热键)的统一入口。
    /// text 非空时预填原文并自动翻译。
    void showTranslate(const QString& text);

    Mode mode() const { return m_mode; }
    QString modeString() const {
        return m_mode == Mode::Translate ? QStringLiteral("translate") : QStringLiteral("list");
    }

    /// 自动化验收:注入搜索词(等价用户输入,走同一防抖链路)。
    void applySearchText(const QString& text);

    /// QML 侧 ESC 调用(关闭抽屉后的兜底隐藏)。
    Q_INVOKABLE void hidePanel() { hide(); }

    /// QML 侧请求显示(截图翻译完成后把面板带回前台)。
    Q_INVOKABLE void showPanel() { showAtCursor(); }

    /// QML 侧拖动标题栏移动窗口(无边框窗口的移动入口,交给系统处理)。
    /// 只是临时挪动:不记录位置,下次呼出仍按光标摆位(见 placeAtCursor)。
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
    /// 面板被收起:QML 侧据此收掉瞬时 UI(右键菜单等),避免下次呼出时残留。
    void panelHidden();
    /// 形态切换(list ⇄ translate):列表模型据此决定释放/重拉。
    void modeChanged();

private:
    void setupUi();
    /// 切换形态(含尺寸/下限调整、翻译页的"停留"抑制)。
    void setMode(Mode mode);
    /// 按形态设置最小尺寸与窗口尺寸;已显示时保持窗口中心不动并夹回屏幕可用区。
    void applyModeSize(Mode mode);
    /// 把窗口摆到光标处(以光标为中心并夹回可用区)。每次呼出都调用。
    void placeAtCursor();

    CoreBridge& m_core;
    QQuickWidget* m_quick = nullptr;
    QTimer* m_blurTimer = nullptr;
    /// 窗口是否曾获得过焦点(获得之前不启用失焦隐藏,防"启动即退出")。
    bool m_everFocused = false;
    /// 是否固定(失焦不隐藏 + 置顶)。
    bool m_pinned = false;
    /// 翻译页期间为真:只**抑制失焦隐藏**(输入长文时切走焦点不该消失),
    /// 但不动用户的固定状态、也不置顶 —— 与 `m_pinned` 是两件事。
    bool m_holdOpen = false;
    /// 当前形态。
    Mode m_mode = Mode::List;
    /// nativeEvent 用的 HWND 缓存(创建期间禁止 winId() 调用,防递归)。
    HWND m_cachedHwnd = nullptr;
};

} // namespace nb
