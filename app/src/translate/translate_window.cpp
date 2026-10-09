#include "translate_window.hpp"

#include "host/window_effects.hpp"
#include "host/memory_probe.hpp"
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
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWindow>

namespace nb {
namespace {
constexpr int TRANSLATE_W = 520;
constexpr int TRANSLATE_H = 660;

/// 上次关闭时的窗口位置。窗口**关闭即销毁**(省下一个 QML 引擎),但位置必须延续 ——
/// 否则每次打开都跳回屏幕中央,用户拖到的顺手位置白拖:销毁只该省内存,不该改体验。
QPoint g_lastPos;
bool g_hasLastPos = false;
} // namespace

TranslateWindow::TranslateWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setWindowTitle(QStringLiteral("Nextboard 翻译"));
    // 关闭即销毁:见 hideWindow() 的说明(常驻一个 QML 引擎不划算)
    setAttribute(Qt::WA_DeleteOnClose);
    // 可缩放下限:低于此宽度双栏信息会挤成一团
    setMinimumSize(420, 500);
    resize(TRANSLATE_W, TRANSLATE_H);
    setupUi();
}

TranslateWindow::~TranslateWindow() {
    // 记下位置,供下次重建的实例沿用(见 showWindow)
    g_lastPos = pos();
    g_hasLastPos = true;
    // 析构体执行时 QQuickWidget 成员尚未释放,此刻读数是"还没回收"的水位;
    // 延后一拍(成员都已析构)再读,才是关闭翻译窗口后的真实回落值。
    QTimer::singleShot(0, qApp, [] { logMemory("translate-closed"); });
}

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
    if (g_hasLastPos) {
        // 沿用上次关闭时的位置;显示器布局可能已变,夹回屏幕可用区
        QScreen* screen = QGuiApplication::screenAt(g_lastPos);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect avail = screen->availableGeometry();
            const QSize size = rect().size();
            move(qBound(avail.left(), g_lastPos.x(),
                        qMax(avail.left(), avail.right() - size.width() + 1)),
                 qBound(avail.top(), g_lastPos.y(),
                        qMax(avail.top(), avail.bottom() - size.height() + 1)));
        }
    } else {
        // 首次打开:定位到光标所在屏中央
        const QPoint cursor = QCursor::pos();
        QScreen* screen = QGuiApplication::screenAt(cursor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) move(screen->availableGeometry().center() - rect().center());
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
