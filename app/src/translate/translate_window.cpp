#include "translate_window.hpp"

#include "host/window_effects.hpp"
#include "panel/qml_bootstrap.hpp"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScreen>
#include <QUrl>
#include <QVariant>
#include <QWindow>

namespace nb {
namespace {
constexpr int TRANSLATE_W = 520;
constexpr int TRANSLATE_H = 660;
} // namespace

TranslateWindow::TranslateWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setWindowTitle(QStringLiteral("Nextboard 翻译"));
    // 可缩放下限:低于此宽度双栏信息会挤成一团
    setMinimumSize(420, 500);
    resize(TRANSLATE_W, TRANSLATE_H);
    setupUi();
}

TranslateWindow::~TranslateWindow() = default;

void TranslateWindow::setupUi() {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_quick = new QQuickWidget(this);
    m_quick->setResizeMode(QQuickWidget::ResizeMode::SizeRootObjectToView);
    m_quick->setClearColor(palette().window().color());
    // 注入 QML 根上下文:TranslateWindow.qml 需要调用 hideWindow / startSystemMove。
    // 必须在 setSource 之前注册 —— 组件在 setSource 时即编译/实例化。
    if (QQmlEngine* engine = m_quick->engine())
        engine->rootContext()->setContextProperty(QStringLiteral("translateWindow"), this);

    m_quick->setSource(QUrl("qrc:/qt/qml/Nextboard/panel/panel/qml/TranslateWindow.qml"));
    layout->addWidget(m_quick);
    // 与主面板同一兜底:加载失败时显示错误而非一片空白
    handleQmlLoadFailure(m_quick, layout);
}

void TranslateWindow::showWindow() {
    if (!m_placed) {
        const QPoint cursor = QCursor::pos();
        QScreen* screen = QGuiApplication::screenAt(cursor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) move(screen->availableGeometry().center() - rect().center());
        m_placed = true;
    }
    // 主题可能在两次打开之间被切换,QQuickWidget 的清屏色(窗口圆角外的底色)
    // 必须跟着走,否则圆角处会露出旧主题的底色。
    if (m_quick) m_quick->setClearColor(palette().window().color());
    show();
    raise();
    activateWindow();
}

void TranslateWindow::openWith(const QString& text) {
    showWindow();
    if (m_quick && m_quick->rootObject())
        QMetaObject::invokeMethod(qobject_cast<QObject*>(m_quick->rootObject()), "openWith",
                                  Q_ARG(QVariant, text));
}

void TranslateWindow::startSystemMove() {
    if (QWindow* handle = windowHandle()) {
        if (handle->startSystemMove()) return;
    }
    // 与主面板同一兜底:合成长按标题栏。无边框窗口是 WS_POPUP,Qt 的
    // startSystemMove 在部分环境下会被平台拒绝(false),Win32 老办法照样能进入
    // 系统模态移动循环(必须先 ReleaseCapture)。
    const HWND hwnd = m_cachedHwnd ? m_cachedHwnd : reinterpret_cast<HWND>(winId());
    if (hwnd) {
        ::ReleaseCapture();
        ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

void TranslateWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    m_cachedHwnd = reinterpret_cast<HWND>(winId());
    applyRoundedCorners(this);
}

bool TranslateWindow::nativeEvent(const QByteArray& eventType, void* message, qint64* result) {
    if (eventType == "windows_generic_MSG" && m_cachedHwnd) {
        auto* msg = static_cast<MSG*>(message);
        // 无边框窗口的边缘缩放(与主面板同一套逻辑)
        if (msg->hwnd == m_cachedHwnd && msg->message == WM_NCHITTEST) {
            const int hit = framelessResizeHitTest(this, m_cachedHwnd, msg->lParam);
            if (hit != -1) {
                if (result) *result = hit;
                return true;
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace nb
