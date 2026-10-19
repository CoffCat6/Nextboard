// 主面板:Qt 无边框窗口内嵌 QQuickWidget(Qt Quick),承载历史/搜索/设置/翻译抽屉。
#pragma once

#include <QWidget>

#include "host/core_bridge.hpp"

class QHBoxLayout;
class QTimer;
class QQuickWidget;
class QQuickItem;

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

    /// 呼出时的摆放方式:
    /// - `AtCursor`:以光标为窗口中心(热键/单实例唤起用,鼠标在哪就在哪);
    /// - `ScreenCenter`:居中于光标所在屏(托盘菜单等"人在别处"的入口用)。
    enum class Orientation { AtCursor, ScreenCenter };

    explicit PanelWindow(CoreBridge& core, QWidget* parent = nullptr);
    ~PanelWindow() override;

    /// 显示在**光标处**(以光标为窗口中心,夹回屏幕可用区)并前置
    /// (热键/托盘/单实例唤起共用)。
    /// 热键的语义是"呼出剪贴板历史",故会先切回列表页。
    void showAtCursor();

    /// 显示在**光标所在屏的中央**并前置(托盘菜单「打开主面板」等入口)。
    /// 与热键的区别仅在摆放方式:托盘操作时鼠标常常停在任务栏角落,跟随光标
    /// 会把面板顶到屏幕边缘;居中更符合"从托盘打开一个窗口"的预期。
    void showCentered();

    /// 开机自启(`--hidden`)的**静默启动**:只创建原生窗口,不显示。
    ///
    /// 不能写成 `showAtCursor(); hide();` —— 两者之间已有一帧上屏,开机瞬间会闪一下
    /// 面板,用户开机后第一眼就被干扰。这里只调 `winId()` 建**隐藏**窗口:
    /// `core.setNotifyHwnd()` 需要有效句柄投递剪贴板通知,而该调用不上屏。
    ///
    /// 副作用是「从未 Show 过」→ 列表模型保持未装载;首次热键呼出时由
    /// `ClipEntryModel::acquire()` 的 `!m_loadedOnce` 分支补一次装载(docs/06 §11.3)。
    void prepareHiddenStart();

    /// 切到翻译页并显示窗口 —— 三条打开链路(面板按钮/条目右键/热键)的统一入口。
    /// text 非空时预填原文并自动翻译。
    /// orientation 决定首次显示时的摆放:热键链路传 AtCursor,托盘等传 ScreenCenter。
    void showTranslate(const QString& text, Orientation orientation = Orientation::AtCursor);

    Mode mode() const { return m_mode; }
    QString modeString() const {
        return m_mode == Mode::Translate ? QStringLiteral("translate") : QStringLiteral("list");
    }

    /// 自动化验收:注入搜索词(等价用户输入,走同一防抖链路)。
    void applySearchText(const QString& text);

    /// 自动化验收:打开设置抽屉(等价点「设置」按钮,验证懒构建链路)。
    void openSettingsPanel();

    /// 自动化验收:打开「新建待办」卡片(等价点头部按钮)。
    /// 用于断言编辑卡片的**字段顺序**(标题在上、备注在下)。
    void openTodoComposerForAcceptance();

    /// 弹面板并切到"待办"视图(docs/05-todo.md §4.3 气泡点击链路):
    /// 居中显示 + 通知 QML 清搜索词并切视图。落点在 PanelWindow 是因为
    /// "显示窗口"只有它有;视图切换由 QML 的 showTodos() 完成。
    void showCenteredOnTodos();

    /// 同上,但要求 QML 在切到待办视图后**定位并高亮**指定条目
    /// (托盘「今日待办」二级菜单点击某条时用,docs/05-todo.md §6.6)。
    /// id 无效(已删)时退化为普通 showCenteredOnTodos 行为。
    void showCenteredOnTodo(const QString& id);

    // ---- 自动化验收钩子(仅 --todo-flow 等验收入口使用,产品路径不调用) ----
    //
    // 存在理由:本会话中它抓出了多个只有"真实点击"才能暴露的缺陷
    // (委托重复声明导致列表整页空白、「点确定」事件被 Overlay 拦下),
    // 直调 onClicked 的验收方式会把这些全部掩盖。
    // 保留成本:不进产品交互路径,只增少量代码;收益是交互缺陷可回归。
    // 若要移除,连带删除 main.cpp 的 --todo-flow 块与 Main.qml 的 acceptance* 成员。

    /// 在 QML 根上调用一个**零参**验收函数。
    /// ⚠️ 刻意不带参数:invokeMethod 按签名查找,带参与不带参是不同签名。
    /// 参数走 setRootProperty 写属性传递。
    bool invokeAcceptance(const char* method);

    /// 读 / 写 QML 根上的属性(传参 + 观测验收状态)。
    QVariant rootProperty(const char* name) const;
    bool setRootProperty(const char* name, const QVariant& value);

    /// 在 QML 树中按 objectName 找项,**投递一次真实的左键按下+抬起**。
    ///
    /// ⚠️ 注入目标是 QQuickWidget(QWidget 层)而非 QQuickWindow:后者会绕过
    ///    窗口系统的鼠标状态,事件被 Overlay 的 popup 容器拦下,所有按钮
    ///    都点不着(实测踩坑,详见实现处注释)。
    /// xFrac:命中点在目标宽度里的相对位置(0~1,默认 0.5 = 正中)。
    /// ⚠️ 宽到面板右半区的目标(如列表卡片)用默认正中点会落在打开的抽屉底下 ——
    ///    "点抽屉外"的场景必须用 xFrac 把点挪进抽屉覆盖不到的左半区。
    /// 返回:是否找到目标且事件被派发(找不到返回 false)。
    bool clickByObjectName(const QString& objectName, qreal xFrac = 0.5);

    /// 验收专用:按 objectName 取项,返回它在**场景坐标**里的左上角 y;找不到返回 -1。
    /// 场景坐标是统一坐标系 —— 两个控件父级不同时,直接比各自的 y 没有意义。
    qreal acceptanceItemSceneY(const QString& objectName) const;

    /// 验收:向面板投递一次真实按键(press + release),走与真实键盘相同的
    /// 链路(QQuickWidget → QML 场景的 deliverKeyEvent → activeFocusItem)。
    /// text 供字符键插入文字用(如 "9");快捷键场景(如 Ctrl+A)传空串。
    /// 返回:是否派发(QML 侧是否有活动焦点项不在本函数的可见范围,
    /// 收不到时表现是"键没反应"而非崩溃)。
    bool sendKey(int key, int modifiers = 0, const QString& text = QString());

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
    /// 无边框边缘缩放会改尺寸:列表形态下记下来(走短延时,避免拖动中高频写库)。
    void resizeEvent(QResizeEvent* e) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qint64* result) override;
    bool event(QEvent* e) override;

signals:
    /// 剪贴板变化(nbcore PostMessage 通知)→ 模型刷新。
    void clipChanged();
    /// 待办到点(WM_APP_TODO_DUE):托盘据此拉取待播报列表弹气泡、
    /// 列表模型据此重拉(docs/05-todo.md §4.3)。
    void todoDue();
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
    /// 列表形态会优先用设置里记住的 panelSize(见 restorePanelSize)。
    void applyModeSize(Mode mode);
    /// 从 settings.panelSize 读回用户上次调过的列表尺寸;无记录/非法/放不下时
    /// 各自回退(见实现处注释),不阻塞启动。
    QSize preferredPanelSize() const;
    /// 把当前列表尺寸写回 settings.panelSize。只在**用户改过尺寸**时调用
    /// (无边框窗口的边缘缩放会触发 resizeEvent,鼠标拖动时每秒几十次 —— 必须在
    /// 松手后再存,否则是往 sqlite 里灌高频写)。
    void savePanelSize();
    /// 本次用户缩放产生的新尺寸(拖动中不写库,等一个短延时确认已松手)。
    void scheduleSavePanelSize();
    /// 把窗口摆到光标处(以光标为中心并夹回可用区)。每次呼出都调用。
    void placeAtCursor();
    /// 把窗口摆在光标所在屏的中央(并夹回可用区)。托盘等"人在别处"的入口用。
    void placeScreenCenter();
    /// 验收专用:按 objectName 在**整棵场景树**里找项。
    /// ⚠️ 根取窗口 contentItem 而非 QML rootObject:Popup 内容挂在 Overlay 层,
    ///    是 rootObject 之外的兄弟子树,从 rootObject 递归找不到。
    QQuickItem* findAcceptanceItem(const QString& objectName) const;
    /// 验收专用:返回 scenePos 处**最顶层**的可交互项。
    /// 只在 clickByObjectName 的探针日志里用(定位"点到了别的东西"这类问题);
    /// 注意它与 Qt 官方 childAt 的语义差异——本函数按"可见+contains"递归,
    /// 命中祖先也算,不能单独作为"按钮是否被遮挡"的判据(实测误导过一次)。
    QQuickItem* topItemAt(QQuickItem* node, const QPointF& scenePos) const;

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
    /// 尺寸落库的防抖计时器(鼠标拖动缩放期间不写库)。
    QTimer* m_sizeSaveTimer = nullptr;
    /// 是否正处于"程序自己设尺寸"的过程 —— 期间的 resizeEvent 不算用户操作。
    /// 没有这个闸门,applyModeSize 的 resize 会被当成用户缩放而写回,形成
    /// "程序写入 → 触发 resize → 再写回"的回环。
    bool m_applyingSize = false;
};

} // namespace nb
