#include "panel_window.hpp"

#include "host/window_effects.hpp"
#include "host/memory_probe.hpp"
#include "panel/qml_bootstrap.hpp"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <windows.h>

namespace nb {
namespace {
constexpr int BLUR_HIDE_DELAY_MS = 200; // 第一代结论:防拖动标题栏的虚假失焦
// 两种形态的尺寸与下限(见 PanelWindow::Mode):
//  - 列表:呼出即用的历史面板,宽一点好扫读;
//  - 翻译:520×660 是原独立翻译窗口的实测尺寸,输入输出双栏需要更高;
//  - 设置:860×640 是"侧栏 180 + 内容表单"的舒适尺寸;下限 700×520 保证
//    内容区仍有 ~500px 宽,字段行不折行(设置页含 URL/JSON 等技术字段)。
constexpr int PANEL_W = 800;
constexpr int PANEL_H = 600;
constexpr int PANEL_MIN_W = 620;   // 低于此尺寸头部(标题+搜索框+动作按钮)会挤变形
constexpr int PANEL_MIN_H = 380;
constexpr int TRANSLATE_W = 520;
constexpr int TRANSLATE_H = 660;
constexpr int TRANSLATE_MIN_W = 420;   // 低于此宽度翻译双栏信息会挤成一团
constexpr int TRANSLATE_MIN_H = 500;
constexpr int SETTINGS_W = 860;
constexpr int SETTINGS_H = 640;
constexpr int SETTINGS_MIN_W = 700;
constexpr int SETTINGS_MIN_H = 520;
/// 用户改完尺寸后多久写回设置(毫秒)。拖动缩放期间每帧都有 resizeEvent,
/// 立即写就是每秒几十次 sqlite 事务。
constexpr int PANEL_SIZE_SAVE_DELAY_MS = 500;
} // namespace

PanelWindow::PanelWindow(CoreBridge& core, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool), m_core(core) {
    setWindowTitle("Nextboard");
    applyModeSize(Mode::List);   // 起始形态=列表(内含初始 resize 与最小尺寸)
    setupUi();

    // 失焦自动隐藏(延迟复查,第一代 lib.rs 同参数)。
    // 仅在窗口**曾获得过焦点**之后启用:启动时 activateWindow 可能失败
    // (系统前台锁),此时若无脑 hide 会让应用"启动即退出"。
    // 尺寸落库防抖:用户拖边框时 resizeEvent 每帧都来,500ms 内无新事件才算"松手"。
    m_sizeSaveTimer = new QTimer(this);
    m_sizeSaveTimer->setSingleShot(true);
    m_sizeSaveTimer->setInterval(PANEL_SIZE_SAVE_DELAY_MS);
    connect(m_sizeSaveTimer, &QTimer::timeout, this, [this] { savePanelSize(); });

    m_blurTimer = new QTimer(this);
    m_blurTimer->setSingleShot(true);
    m_blurTimer->setInterval(BLUR_HIDE_DELAY_MS);
    connect(m_blurTimer, &QTimer::timeout, this, [this] {
        if (m_pinned) return;    // 固定:点其他地方也不隐藏
        if (m_holdOpen) return;  // 翻译页:输入长文时切走焦点不该消失
        if (m_everFocused && !isActiveWindow()) hide();
    });
}

void PanelWindow::setMode(Mode mode) {
    if (m_mode == mode) return;
    m_mode = mode;
    // 翻译/设置页要能停留,但**不动**用户的固定状态 —— 只临时抑制失焦隐藏。
    // 设置页额外配合 applyTopmost() 置顶:失焦不隐藏只保证"不消失",
    // 置顶才保证"看得见"(被别的窗口盖住的 Qt::Tool 窗口没有任何找回入口)。
    m_holdOpen = (mode == Mode::Translate || mode == Mode::Settings);
    applyModeSize(mode);
    applyTopmost();
    emit modeChanged();
    // 形态切换的内存读数:列表页释放/翻译页构建的净效果都体现在这两条上
    logMemory(mode == Mode::Translate ? "mode-translate"
              : mode == Mode::Settings ? "mode-settings"
                                       : "mode-list");
}

void PanelWindow::applyTopmost() {
    // 置顶与否:固定中或停在设置页 → HWND_TOPMOST,否则 HWND_NOTOPMOST。
    // 与 setPinned 同一机制;翻译页**刻意不置顶**(见其注释)。
    const bool topmost = m_pinned || m_mode == Mode::Settings;
    const HWND hwnd = m_cachedHwnd ? m_cachedHwnd : reinterpret_cast<HWND>(winId());
    if (hwnd) {
        ::SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

/// 读回用户上次调过的列表尺寸。
///
/// 三道回退(任一不满足就回到内置默认 800×600):
///   ① 设置里没有记录 / 不是二元组 → 默认;
///   ② 超出屏幕可用区(用户把外接屏拔了)→ 默认 —— 否则面板会大过当前屏幕,
///      底部按钮够不到;
///   ③ 小于最小尺寸 → 由 setMinimumSize 兜住(这里不判断,交给 Qt)。
QSize PanelWindow::preferredPanelSize() const {
    const QJsonObject resp = m_core.call("get_settings");
    const QJsonArray arr = resp.value("ok").toBool()
                               ? resp.value("data").toObject().value("panelSize").toArray()
                               : QJsonArray{};
    if (arr.size() != 2) return QSize(PANEL_W, PANEL_H);
    const QSize want(arr.at(0).toInt(), arr.at(1).toInt());
    if (!want.isValid()) return QSize(PANEL_W, PANEL_H);
    // 放不下就回默认:可用区取"光标所在屏",与呼出摆位用的是同一块屏
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        if (want.width() > avail.width() || want.height() > avail.height())
            return QSize(PANEL_W, PANEL_H).boundedTo(avail.size());
    }
    return want;
}

void PanelWindow::applyModeSize(Mode mode) {
    const bool translate = (mode == Mode::Translate);
    const bool settings = (mode == Mode::Settings);
    // 翻译/设置形态的尺寸是内容驱动的最优值,固定;列表形态优先用记住的尺寸
    const QSize minSize = translate ? QSize(TRANSLATE_MIN_W, TRANSLATE_MIN_H)
                         : settings  ? QSize(SETTINGS_MIN_W, SETTINGS_MIN_H)
                                     : QSize(PANEL_MIN_W, PANEL_MIN_H);
    const QSize target = translate ? QSize(TRANSLATE_W, TRANSLATE_H)
                        : settings  ? QSize(SETTINGS_W, SETTINGS_H)
                                    : preferredPanelSize();
    // ⚠️ 顺序:必须先设新下限再 resize —— 翻译尺寸宽 520 小于列表下限 620,
    // 若先 resize,窗口会被旧下限顶成 620 宽(高度同理)。
    // 闸门:本次 resize 是程序设定的,不算用户操作(否则会写回设置形成回环)
    m_applyingSize = true;
    struct Guard {
        bool& flag;
        ~Guard() { flag = false; }
    } guard{m_applyingSize};
    setMinimumSize(minSize);
    if (!isVisible()) {
        resize(target);
        return;
    }
    // 已显示:保持窗口中心不动(尺寸变了但位置不跳),再夹回屏幕可用区
    const QPoint center = frameGeometry().center();
    resize(target);
    QPoint topLeft = center - rect().center();
    QScreen* screen = QGuiApplication::screenAt(center);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        topLeft.setX(qBound(avail.left(), topLeft.x(),
                            qMax(avail.left(), avail.right() - width() + 1)));
        topLeft.setY(qBound(avail.top(), topLeft.y(),
                            qMax(avail.top(), avail.bottom() - height() + 1)));
    }
    move(topLeft);
}

void PanelWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    // 只记列表形态:翻译形态的尺寸是内容驱动的最优值,记下来会让"下次打开翻译页"
    // 变成用户上次意外拖出的比例;程序自己 resize(切形态/恢复尺寸)不算。
    if (m_applyingSize || m_mode != Mode::List) return;
    // 与最小尺寸一致时很可能只是"最小化/还原"链路,不覆盖用户留下的更大尺寸
    if (!isVisible()) return;
    scheduleSavePanelSize();
}

void PanelWindow::scheduleSavePanelSize() {
    if (m_sizeSaveTimer) m_sizeSaveTimer->start();
}

void PanelWindow::savePanelSize() {
    // 存的是**用户看到并调过的列表尺寸**;高度/宽度都夹到设置校验接受的范围,
    // 免得一次异常尺寸让后续所有 save_settings 都被核心拒绝(连带别的字段存不进去)。
    if (m_mode != Mode::List) return;
    const QSize sz = size();
    if (!sz.isValid()) return;
    const QJsonArray arr{sz.width(), sz.height()};
    const QJsonObject resp = m_core.call("save_settings", {{"panelSize", arr}});
    if (!resp.value("ok").toBool()) {
        qWarning() << "panel: 保存面板尺寸失败" << resp.value("error").toObject();
    }
}

void PanelWindow::prepareHiddenStart() {
    // 触发原生窗口创建(HWND),但**不 show**:`winId()` 建的是**隐藏**窗口,不上屏。
    // 句柄本身是 main.cpp 稍后 `core.setNotifyHwnd(panel.nativeHandle())` 必需的
    // (那边也会调 winId() —— 提到这里只是让"静默启动"这一意图有明确落点与日志)。
    // 形态无需归位:构造时 m_mode 已是 List,用户下次按热键走的 showAtCursor()
    // 本身就会 setMode(List)。
    (void)nativeHandle();
    qInfo() << "panel: 静默启动(--hidden),不上屏";
}

void PanelWindow::showTranslate(const QString& text, Orientation orientation) {
    const bool wasHidden = !isVisible();
    setMode(Mode::Translate);   // 尺寸/停留策略随形态切换
    // 原本隐藏(托盘/热键链路)→ 按调用方给的 orientation 摆位;
    // 已经显示着(从列表点「翻译」)→ 保持原位,只换内容与尺寸。
    if (wasHidden) (orientation == Orientation::ScreenCenter) ? placeScreenCenter()
                                                              : placeAtCursor();
    if (wasHidden || !isActiveWindow()) {
        show();
        raise();
        activateWindow();
    }
    // 预填 + 自动翻译交给 QML 根(Main.qml 的 openTranslate):它会按需构建翻译页。
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "openTranslate",
                                  Q_ARG(QVariant, text));
}

void PanelWindow::showSettings() {
    // 与 showTranslate 同构:隐藏时按光标摆位,已显示时原位只换形态;
    // 构建与回填交给 QML 根(Main.qml 的 openSettingsPage)。
    const bool wasHidden = !isVisible();
    setMode(Mode::Settings);   // 尺寸/停留策略随形态切换
    if (wasHidden) placeAtCursor();
    if (wasHidden || !isActiveWindow()) {
        show();
        raise();
        activateWindow();
    }
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "openSettingsPage");
}

void PanelWindow::showList() {
    setMode(Mode::List);
    if (!isVisible() || !isActiveWindow()) {
        show();
        raise();
        activateWindow();
    }
}

PanelWindow::~PanelWindow() {
    // 退出时那串 QML 噪声的根治(见 docs/progress.md「退出时的 TypeError」):
    // QObject 的析构顺序是「先 emit destroyed(this) → 再 deleteChildren()」,而 QML 根上下文里的
    // context property `panelWindow` 正指向本对象 —— destroyed 一发出,它在 QML 侧立刻变 null,
    // 于是 Main.qml 里一批**绑定**(panelWindow.mode / panelWindow.pinned)会在这一瞬重新求值,
    // 每次退出刷 5 条 "TypeError: Cannot read property 'xxx' of null"。
    //
    // 在析构体里**先拆掉 QML 树**:此时本对象仍是有效的 QObject(派生部分也还活着),
    // 绑定即便重算也读得到真实值;树没了之后,`destroyed` 发出时已没有绑定可重算。
    delete m_quick;
    m_quick = nullptr;
}

void PanelWindow::setupUi() {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_quick = new QQuickWidget(this);
    m_quick->setResizeMode(QQuickWidget::ResizeMode::SizeRootObjectToView);
    m_quick->setClearColor(palette().window().color());
    // 面板绝大部分时间是隐藏的(托盘常驻 + 失焦即隐),但 Qt 默认「持久场景图」:
    // 隐藏后场景节点与纹理仍继续驻留。这是 QQuickWidget 文档
    // "Scene Graph and Context Persistency" 一节明确支持的开关 —— 关掉它,
    // 隐藏即释放场景相关资源、显示时重建,常驻水位因此明显下降。
    // 代价:每次呼出要重建场景,首次显示略有延迟(需真机确认可接受)。
    if (QQuickWindow* window = m_quick->quickWindow())
        window->setPersistentSceneGraph(false);
    // 把本窗口注入 QML 根上下文:Main.qml 的 ESC 处理要调 panelWindow.hidePanel()。
    // 必须在 setSource 之前注册——QML 组件在 setSource 时即编译/实例化。
    if (QQmlEngine* engine = m_quick->engine())
        engine->rootContext()->setContextProperty(QStringLiteral("panelWindow"), this);
    // QML 源经 qt_add_qml_module 打进 qrc(QML_FILES 以 panel/qml/ 相对路径引用,
    // 别名拼接后实际地址多一层 panel/qml,无运行期文件依赖)。
    m_quick->setSource(QUrl("qrc:/qt/qml/Nextboard/panel/panel/qml/Main.qml"));
    layout->addWidget(m_quick);
    // 加载失败时用原生标签顶替内容:空白窗口对用户零信息量(本项目已多次踩到)
    handleQmlLoadFailure(m_quick, layout);
}

/// 把窗口摆到**光标处**(以光标为窗口中心,再夹回屏幕可用区)。
///
/// 面板是"呼出即用"的:出现在鼠标旁边,就省掉了"每次把鼠标挪到屏幕中央"这一步。
/// **每次呼出都重新摆位** —— 拖动只是临时挪一下,不改变"下次从光标处弹出"的行为
/// (这里刻意不记"用户拖动过"的位置:面板的定位语义是"跟着人走",不是"记住一个坑")。
void PanelWindow::placeAtCursor() {
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    // 以光标为中心:光标落在面板正中,离列表、语言栏、按钮都近。
    // 靠近屏幕边缘时夹回可用区(此时光标自然偏离中心,这是预期行为)。
    const QRect avail = screen->availableGeometry();
    QPoint topLeft = cursor - rect().center();
    topLeft.setX(qBound(avail.left(), topLeft.x(),
                        qMax(avail.left(), avail.right() - width() + 1)));
    topLeft.setY(qBound(avail.top(), topLeft.y(),
                        qMax(avail.top(), avail.bottom() - height() + 1)));
    move(topLeft);
}

void PanelWindow::showAtCursor() {
    // 热键/托盘的语义是"呼出剪贴板历史",故一律切回列表页
    // (若停在翻译页时按热键,先回到列表;翻译内容仍在,再点「翻译」即可回去)。
    setMode(Mode::List);
    // 摆位放在 setMode 之后:形态切换会改窗口尺寸,得按新尺寸算中心
    placeAtCursor();
    show();
    raise();
    activateWindow();
}

/// 把窗口摆在**光标所在屏的中央**(再夹回该屏可用区)。
/// 用于托盘菜单等入口:此时鼠标常在任务栏角落,跟随光标会把面板顶到屏幕边缘,
/// 居中更符合"从托盘打开一个窗口"的预期。热键链路仍走 placeAtCursor(跟随光标)。
void PanelWindow::placeScreenCenter() {
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect avail = screen->availableGeometry();
    move(avail.center() - rect().center());
}

void PanelWindow::showCentered() {
    // 与 showAtCursor 相同的语义(切回列表页 + 前置),仅摆放方式不同。
    setMode(Mode::List);
    placeScreenCenter();
    show();
    raise();
    activateWindow();
}

HWND PanelWindow::nativeHandle() const {
    return reinterpret_cast<HWND>(winId());
}

void PanelWindow::startSystemMove() {
    // 只移动窗口,不记录位置:拖动是临时挪动,下次呼出仍按光标摆位(见 placeAtCursor)
    if (QWindow* handle = windowHandle()) {
        if (handle->startSystemMove()) return;
    }
    // 兜底:合成长按标题栏。无边框窗口走 WS_POPUP,部分环境下 Qt 的
    // startSystemMove 会被平台拒绝(false),此时用 Win32 的老办法照样能进入
    // 系统的模态移动循环(ReleaseCapture 必须先做,否则系统不会接管)。
    const HWND hwnd = m_cachedHwnd ? m_cachedHwnd : reinterpret_cast<HWND>(winId());
    if (hwnd) {
        ::ReleaseCapture();
        ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

void PanelWindow::setPinned(bool pinned) {
    if (m_pinned == pinned) return;
    m_pinned = pinned;
    if (pinned) m_blurTimer->stop();   // 正在计时的隐藏请求作废
    // 置顶与否:固定后面板不再自动隐藏,而本窗口是 Qt::Tool(不占任务栏),
    // 一旦被其它窗口盖住就没有入口找回,所以固定期间同步置顶。
    applyTopmost();
    emit pinnedChanged();
}

void PanelWindow::applySearchText(const QString& text) {
    // 自动化验收入口:QML 侧 setSearch(text) 由 C++ 直调(Main.qml root 实现)。
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "setSearch",
                                  Q_ARG(QVariant, text));
}

void PanelWindow::openSettingsPanel() {
    // 自动化验收入口:QML 侧 openSettings() 由 C++ 直调(Main.qml root 实现),
    // 与「设置」按钮同一条链路(→ panelWindow.showSettings() → 切设置页形态)。
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "openSettings");
}

void PanelWindow::openTodoComposerForAcceptance() {
    if (m_quick && m_quick->rootObject()) {
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()),
                                  "acceptanceOpenTodoComposer");
    }
}

void PanelWindow::showCenteredOnTodos() {
    // 先切回列表形态(面板可能停在翻译页),再居中弹出并让 QML 切到待办视图。
    // ⚠️ 顺序要紧:setMode(Translate 之外的形态)会重设窗口尺寸/下限,
    //    放在 show 之前可以避免"先按旧尺寸显示再跳一下"。
    setMode(Mode::List);
    showCentered();
    // QML 侧 showTodos():清搜索词(残留关键词会把待办过滤掉)+ 切"待办"视图 + 重拉
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "showTodos");
}

void PanelWindow::showCenteredOnTodo(const QString& id) {
    // 与 showCenteredOnTodos 同一摆位链路,只是把目标条目 id 一并传给 QML:
    // QML 侧 showTodos(id) 在切视图后按 id 定位居中并高亮(托盘「今日待办」
    // 子菜单点击某条时用)。id 无效时 QML 侧退化为普通切换。
    setMode(Mode::List);
    showCentered();
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "showTodos",
                                  Q_ARG(QVariant, id));
}

bool PanelWindow::invokeAcceptance(const char* method) {
    if (!m_quick || !m_quick->rootObject()) return false;
    return QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), method);
}

QVariant PanelWindow::rootProperty(const char* name) const {
    if (!m_quick || !m_quick->rootObject()) return {};
    return m_quick->rootObject()->property(name);
}

bool PanelWindow::setRootProperty(const char* name, const QVariant& value) {
    if (!m_quick || !m_quick->rootObject()) return false;
    return m_quick->rootObject()->setProperty(name, value);
}

namespace {
/// 深度优先在 QQuickItem 子树里按 objectName 找项(QML 侧没有 findChild)。
QQuickItem* findItemByName(QQuickItem* node, const QString& name) {
    if (!node) return nullptr;
    if (node->objectName() == name) return node;
    const QList<QQuickItem*> kids = node->childItems();
    for (QQuickItem* kid : kids) {
        if (QQuickItem* hit = findItemByName(kid, name)) return hit;
    }
    return nullptr;
}
} // namespace

QQuickItem* PanelWindow::findAcceptanceItem(const QString& objectName) const {
    if (!m_quick || !m_quick->rootObject()) return nullptr;
    QQuickWindow* win = m_quick->quickWindow();
    if (!win) return nullptr;
    // ⚠️ 根必须是窗口的 contentItem,不是 QML rootObject:
    //    Popup 的 contentItem 挂在 **Overlay 层**(contentItem 之外的兄弟子树),
    //    从 rootObject 递归找不到浮层里的按钮(实测 "找不到 objectName")。
    //    从 contentItem 出发能覆盖普通内容 + Overlay + 所有 popup。
    return findItemByName(win->contentItem(), objectName);
}

qreal PanelWindow::acceptanceItemSceneY(const QString& objectName) const {
    QQuickItem* item = findAcceptanceItem(objectName);
    if (!item) return -1.0;
    return item->mapToScene(QPointF(0, 0)).y();
}

bool PanelWindow::acceptanceItemVisible(const QString& objectName) const {
    QQuickItem* item = findAcceptanceItem(objectName);
    // 找不到也报 false:调用方要的是"能不能看到",不是"找没找到"
    return item && item->isVisible();
}

bool PanelWindow::clickByObjectName(const QString& objectName, qreal xFrac) {
    QQuickItem* target = findAcceptanceItem(objectName);
    if (!target) {
        qWarning() << "验收:找不到 objectName" << objectName;
        return false;
    }
    QQuickWindow* win = m_quick->quickWindow();
    if (!win) return false;
    // QQuickWindow 的场景坐标系原点 = 窗口内容区左上角,与投递到窗口的
    // QMouseEvent 使用的坐标一致 → mapToScene 的结果可直接用,**不要**再减
    // win->x()/y()(那是窗口在屏幕上的位置,减了会得到负数,实测 -2517)。
    const qreal frac = qBound(qreal(0.0), xFrac, qreal(1.0));
    const QPointF local = target->mapToScene(QPointF(0, 0))
                          + QPointF(target->width() * frac, target->height() / 2);
    qInfo() << "验收点击" << objectName << "local=" << local
            << "size=" << target->width() << target->height()
            << "visible=" << target->isVisible() << "enabled=" << target->isEnabled()
            << "opacity=" << target->opacity();

    // ---- 探针:该点上真正"最顶层"的项是谁 ----
    // 两套判据:
    //  ① childAt:Qt 官方命中测试(递归找可见项,与事件派发同源,最可信)
    //  ② 自研 topItemAt:按"可见 + contains"递归
    QQuickItem* top = win->contentItem()->childAt(local.x(), local.y());
    if (top) {
        const QPointF topPos = top->mapToScene(QPointF(0, 0));
        qInfo() << "验收探针①childAt =" << top->metaObject()->className()
                << "objectName:" << top->objectName()
                << "scene:" << topPos << "size:" << top->width() << top->height()
                << "是目标/目标祖先? " << (top == target
                    || top->isAncestorOf(target)
                    || target->isAncestorOf(top));
        QQuickItem* top2 = topItemAt(win->contentItem(), local);
        if (top2) {
            qInfo() << "验收探针②自研 =" << top2->metaObject()->className()
                    << "objectName:" << top2->objectName()
                    << "是目标/目标祖先? " << (top2 == target
                        || top2->isAncestorOf(target)
                        || target->isAncestorOf(top2));
        }
    } else {
        qInfo() << "验收探针 = <null>(该点无可见项)";
    }

    // ⚠️ 注入方式:把事件发给 **QQuickWidget**(QWidget 层)而不是直接塞给
    //    QQuickWindow —— QQuickWidget 会像收到真实系统鼠标一样把事件转发进
    //    QML 场景(含 DeliveryAgent 的抓取/命中链)。直接 sendEvent 给
    //    QQuickWindow 会绕过窗口系统的鼠标状态,事件被 Overlay 的 popup 容器
    //    拦下而不落到按钮(实测:按钮永远点不着,连卡片上的「保存」也一样)。
    //    坐标:QQuickWidget 铺满面板,场景坐标即 widget 局部坐标。
    QMouseEvent press(QEvent::MouseButtonPress, local, m_quick->mapToGlobal(local),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, m_quick->mapToGlobal(local),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_quick, &press);
    QCoreApplication::sendEvent(m_quick, &release);
    return true;
}

bool PanelWindow::sendKey(int key, int modifiers, const QString& text) {
    if (!m_quick) return false;
    // 与 clickByObjectName 同一注入哲学(见上):发给 QQuickWidget,QQuickWidget::
    // keyPressEvent 会把事件转投给 QML 场景的 offscreenWindow,再经
    // QQuickDeliveryAgent::deliverKeyEvent 交给 activeFocusItem 并沿父链冒泡 ——
    // 与真实键盘完全同一条链路(直调 QML 函数会绕开焦点/冒泡语义)。
    const auto mods = static_cast<Qt::KeyboardModifiers>(modifiers);
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(m_quick, &press);
    QCoreApplication::sendEvent(m_quick, &release);
    return true;
}

QQuickItem* PanelWindow::topItemAt(QQuickItem* node, const QPointF& scenePos) const {
    if (!node || !node->isVisible() || node->opacity() <= 0) return nullptr;
    // 只看"包含该点"的子树(与 QQuickWindow 的命中测试同序:后声明的在上)
    const QPointF local = node->mapFromScene(scenePos);
    if (!node->contains(local)) return nullptr;
    const QList<QQuickItem*> kids = node->childItems();
    for (int i = kids.size() - 1; i >= 0; --i) {
        if (QQuickItem* hit = topItemAt(kids[i], scenePos)) return hit;
    }
    // 叶子或"子项都不含"的节点:只要它自己能收鼠标就算命中
    return node;
}

void PanelWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    logMemory("panel-show");
    // 无边框窗口在 Windows 11 上取系统圆角(Win10 上无副作用,退化为直角)
    applyRoundedCorners(this);
    m_blurTimer->stop();
    // showAtCursor 的 activateWindow 是异步的:give the WM a beat, then re-check
    QTimer::singleShot(120, this, [this] {
        if (isActiveWindow()) m_everFocused = true;
    });
}

bool PanelWindow::nativeEvent(const QByteArray& eventType, void* message, qint64* result) {
    if (eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        // ⚠️ 此处禁止调用 winId():窗口创建期间(WM_NCCREATE 等)调用会递归触发
        // CreateWindowEx → 创建失败(Qt 报 "CreateWindowEx failed")。用缓存 HWND。
        if (msg->hwnd != m_cachedHwnd)
            return QWidget::nativeEvent(eventType, message, result);
        // 无边框窗口的边缘缩放:命中边框返回 HT*,其余交给系统(拖动/Aero Snap/双击最大化)
        if (msg->message == WM_NCHITTEST) {
            const int hit = framelessResizeHitTest(this, m_cachedHwnd, msg->lParam);
            if (hit != -1) {
                if (result) *result = hit;
                return true;
            }
        }
        // nbcore 通知:剪贴板/设置变化 → 模型刷新
        if (msg->message == nb::WM_APP_CLIP_CHANGED) {
            emit clipChanged();
            if (result) *result = 0;
            return true;
        }
        if (msg->message == nb::WM_APP_SETTINGS_CHANGED) {
            emit clipChanged(); // 设置变化也刷新一次(监听开关等影响列表)
            if (result) *result = 0;
            return true;
        }
        // 待办到点:只当信号,payload(哪条待办/什么正文)由接收方拉取
        // (与 CLIP_CHANGED 同哲学,不往 PostMessage 塞指针 —— §4.2)
        if (msg->message == nb::WM_APP_TODO_DUE) {
            emit todoDue();
            if (result) *result = 0;
            return true;
        }
        // 失焦自动隐藏
        if (msg->message == WM_ACTIVATE) {
            if (LOWORD(msg->wParam) == WA_INACTIVE) {
                m_blurTimer->start();
            } else {
                m_blurTimer->stop();
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

bool PanelWindow::event(QEvent* e) {
    if (e->type() == QEvent::WinIdChange) {
        m_cachedHwnd = reinterpret_cast<HWND>(winId());
    } else if (e->type() == QEvent::Hide) {
        // 面板收起 → 通知 QML 收掉瞬时 UI(右键菜单/屏障),避免下次呼出时残留
        emit panelHidden();
        logMemory("panel-hide");
    }
    return QWidget::event(e);
}

} // namespace nb
