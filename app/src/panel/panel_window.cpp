#include "panel_window.hpp"

#include "host/window_effects.hpp"
#include "host/memory_probe.hpp"
#include "panel/qml_bootstrap.hpp"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
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
//  - 翻译:520×660 是原独立翻译窗口的实测尺寸,输入输出双栏需要更高。
constexpr int PANEL_W = 800;
constexpr int PANEL_H = 600;
constexpr int PANEL_MIN_W = 620;   // 低于此尺寸头部(标题+搜索框+动作按钮)会挤变形
constexpr int PANEL_MIN_H = 380;
constexpr int TRANSLATE_W = 520;
constexpr int TRANSLATE_H = 660;
constexpr int TRANSLATE_MIN_W = 420;   // 低于此宽度翻译双栏信息会挤成一团
constexpr int TRANSLATE_MIN_H = 500;
} // namespace

PanelWindow::PanelWindow(CoreBridge& core, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool), m_core(core) {
    setWindowTitle("Nextboard");
    applyModeSize(Mode::List);   // 起始形态=列表(内含初始 resize 与最小尺寸)
    setupUi();

    // 失焦自动隐藏(延迟复查,第一代 lib.rs 同参数)。
    // 仅在窗口**曾获得过焦点**之后启用:启动时 activateWindow 可能失败
    // (系统前台锁),此时若无脑 hide 会让应用"启动即退出"。
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
    // 翻译页要能停留,但**不动**用户的固定状态、也不置顶 —— 只临时抑制失焦隐藏。
    m_holdOpen = (mode == Mode::Translate);
    applyModeSize(mode);
    emit modeChanged();
    // 形态切换的内存读数:列表页释放/翻译页构建的净效果都体现在这两条上
    logMemory(mode == Mode::Translate ? "mode-translate" : "mode-list");
}

void PanelWindow::applyModeSize(Mode mode) {
    const bool translate = (mode == Mode::Translate);
    const QSize minSize = translate ? QSize(TRANSLATE_MIN_W, TRANSLATE_MIN_H)
                                    : QSize(PANEL_MIN_W, PANEL_MIN_H);
    const QSize target = translate ? QSize(TRANSLATE_W, TRANSLATE_H)
                                   : QSize(PANEL_W, PANEL_H);
    // ⚠️ 顺序:必须先设新下限再 resize —— 翻译尺寸宽 520 小于列表下限 620,
    // 若先 resize,窗口会被旧下限顶成 620 宽(高度同理)。
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

PanelWindow::~PanelWindow() = default;

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
    const HWND hwnd = m_cachedHwnd ? m_cachedHwnd : reinterpret_cast<HWND>(winId());
    if (hwnd) {
        ::SetWindowPos(hwnd, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    emit pinnedChanged();
}

void PanelWindow::applySearchText(const QString& text) {
    // 自动化验收入口:QML 侧 setSearch(text) 由 C++ 直调(Main.qml root 实现)。
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "setSearch",
                                  Q_ARG(QVariant, text));
}

void PanelWindow::openSettingsPanel() {
    // 自动化验收入口:QML 侧 openSettings() 由 C++ 直调(Main.qml root 实现):
    // 第一次调用会经 Loader 构建 SettingsDrawer,并验证其尺寸取自面板根。
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "openSettings");
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
