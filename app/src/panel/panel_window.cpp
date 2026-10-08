#include "panel_window.hpp"

#include "host/window_effects.hpp"
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
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <windows.h>

namespace nb {
namespace {
constexpr int BLUR_HIDE_DELAY_MS = 200; // 第一代结论:防拖动标题栏的虚假失焦
constexpr int PANEL_W = 800;
constexpr int PANEL_H = 600;
} // namespace

PanelWindow::PanelWindow(CoreBridge& core, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool), m_core(core) {
    setWindowTitle("Nextboard");
    resize(PANEL_W, PANEL_H);
    // 无边框窗口的缩放下限:低于此尺寸头部(标题 + 搜索框 + 四个带图标的动作按钮)
    // 会挤变形;620 是"搜索框仍可用"的实测下限估算
    setMinimumSize(620, 380);
    setupUi();

    // 失焦自动隐藏(延迟复查,第一代 lib.rs 同参数)。
    // 仅在窗口**曾获得过焦点**之后启用:启动时 activateWindow 可能失败
    // (系统前台锁),此时若无脑 hide 会让应用"启动即退出"。
    m_blurTimer = new QTimer(this);
    m_blurTimer->setSingleShot(true);
    m_blurTimer->setInterval(BLUR_HIDE_DELAY_MS);
    connect(m_blurTimer, &QTimer::timeout, this, [this] {
        if (m_pinned) return;   // 固定:点其他地方也不隐藏
        if (m_everFocused && !isActiveWindow()) hide();
    });
}

PanelWindow::~PanelWindow() = default;

void PanelWindow::setupUi() {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_quick = new QQuickWidget(this);
    m_quick->setResizeMode(QQuickWidget::ResizeMode::SizeRootObjectToView);
    m_quick->setClearColor(palette().window().color());
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

void PanelWindow::showAtCursor() {
    // 用户手动拖动过之后就不再自动居中 —— 否则刚拖到顺手的位置,一收一开又跳回屏幕中央
    if (!m_userMoved) {
        const QPoint cursor = QCursor::pos();
        QScreen* screen = QGuiApplication::screenAt(cursor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect avail = screen->availableGeometry();
            move(avail.center() - rect().center());
        }
    }
    show();
    raise();
    activateWindow();
}

HWND PanelWindow::nativeHandle() const {
    return reinterpret_cast<HWND>(winId());
}

void PanelWindow::startSystemMove() {
    m_userMoved = true;   // 之后不再自动居中(见 showAtCursor)
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

void PanelWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
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
    }
    return QWidget::event(e);
}

} // namespace nb
