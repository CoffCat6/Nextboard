#include "panel_window.hpp"

#include "host/window_effects.hpp"

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
    setupUi();

    // 失焦自动隐藏(延迟复查,第一代 lib.rs 同参数)。
    // 仅在窗口**曾获得过焦点**之后启用:启动时 activateWindow 可能失败
    // (系统前台锁),此时若无脑 hide 会让应用"启动即退出"。
    m_blurTimer = new QTimer(this);
    m_blurTimer->setSingleShot(true);
    m_blurTimer->setInterval(BLUR_HIDE_DELAY_MS);
    connect(m_blurTimer, &QTimer::timeout, this, [this] {
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
    if (m_quick->status() == QQuickWidget::Error)
        qWarning() << "QML load errors:" << m_quick->errors();
    layout->addWidget(m_quick);
}

void PanelWindow::showAtCursor() {
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        move(avail.center() - rect().center());
    }
    show();
    raise();
    activateWindow();
}

HWND PanelWindow::nativeHandle() const {
    return reinterpret_cast<HWND>(winId());
}

void PanelWindow::startSystemMove() {
    if (QWindow* handle = windowHandle()) handle->startSystemMove();
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
    }
    return QWidget::event(e);
}

} // namespace nb
