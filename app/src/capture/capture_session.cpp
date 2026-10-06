#include "capture_session.hpp"
#include "host/core_bridge.hpp"

#include <QGuiApplication>
#include <QBuffer>
#include <QCursor>
#include <QIODevice>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <windows.h>

namespace nb {

namespace {
// 忙碌标记:防热键重入(第一代 capture/mod.rs CAPTURE_BUSY 语义)
bool g_busy = false;

QRect toGlobal(const QScreen* screen, const QRect& local) {
    return local.translated(screen->geometry().topLeft());
}

QString quickPngBase64(const QImage& image) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QString::fromLatin1(buffer.data().toBase64());
}
} // namespace

void startCaptureSession(CaptureIntent intent) {
    if (g_busy) {
        qInfo() << "截图进行中,忽略本次触发";
        return;
    }
    g_busy = true;
    struct BusyGuard {
        ~BusyGuard() { g_busy = false; }
    } guard;

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) return;

    // 找到光标所在屏(决策 D5)
    int cursorScreen = 0;
    const QPoint cursor = QCursor::pos();
    for (int i = 0; i < screens.size(); ++i) {
        if (screens[i]->geometry().contains(cursor)) { cursorScreen = i; break; }
    }

    CoreBridge& core = CoreBridge::instance();
    if (intent == CaptureIntent::Fullscreen) {
        // 直通:捕获 → 入库(source=capture)→ 写剪贴板 → 完成
        const QImage shot = core.captureMonitor(static_cast<unsigned>(cursorScreen));
        if (shot.isNull()) {
            qWarning() << "全屏捕获失败";
            return;
        }
        // v1 简化:PNG base64 走 ingest_capture_png(FFI 方法表);像素级通道 S3 优化
        QJsonObject resp = core.call("ingest_capture_png", {
            {"png_base64", quickPngBase64(shot)},
            {"source", "capture"},
        });
        qInfo() << "全屏截图入库:" << (resp.value("ok").toBool() ? "成功" : "失败");
        return;
    }

    // 框选:每屏一个遮罩,拿各自冻结底图
    for (int i = 0; i < screens.size(); ++i) {
        const QImage frozen = core.captureMonitor(static_cast<unsigned>(i));
        auto* overlay = new OverlayWidget(frozen, i);
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(overlay, &OverlayWidget::regionConfirmed, overlay,
                [i](const QRect& globalRect) {
            // S3 接裁剪入库(nb_capture_crop + ingest);PoC 先确认坐标
            qInfo() << "选区确认(屏" << i << "):" << globalRect;
        });
        QObject::connect(overlay, &OverlayWidget::cancelled, overlay, [] { qInfo() << "截图取消"; });
        overlay->showFullScreen();
    }
}

// ---- OverlayWidget ----

OverlayWidget::OverlayWidget(QImage frozen, int screenIndex, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      m_frozen(std::move(frozen)), m_screenIndex(screenIndex) {
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
}

void OverlayWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // 冻结底图铺满(缩放到窗口大小 = 物理像素映射)
    if (!m_frozen.isNull()) {
        painter.drawImage(rect(), m_frozen);
    } else {
        painter.fillRect(rect(), QColor(30, 30, 34));
    }
    // 半透明黑罩
    painter.fillRect(rect(), QColor(0, 0, 0, 90));
    // 选区内恢复原图(亮框)
    if (!m_selection.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(m_selection, m_frozen,
                          QRectF(m_selection).normalized());
        QPen pen(QColor(80, 160, 255), 2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(m_selection.normalized());
        // 尺寸提示
        const QSize size = m_selection.normalized().size();
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(QPoint(m_selection.normalized().x(),
                                m_selection.normalized().y() - 6),
                         QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
    } else {
        painter.setPen(QColor(255, 255, 255, 200));
        painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignTop,
                         QStringLiteral("拖拽框选,Esc 取消(屏 %1)").arg(m_screenIndex));
    }
}

void OverlayWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_origin = e->pos();
        m_selection = QRect(m_origin, QSize(0, 0));
        m_selecting = true;
        update();
    }
}

void OverlayWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_selecting) {
        m_selection = QRect(m_origin, e->pos()).normalized();
        update();
    }
}

void OverlayWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selection = QRect(m_origin, e->pos()).normalized();
        if (m_selection.width() < 4 || m_selection.height() < 4) {
            emit cancelled();
            close();
            return;
        }
        const QScreen* screen = screenForWidget();
        emit regionConfirmed(toGlobal(screen, m_selection));
        close();
    }
}

void OverlayWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        emit cancelled();
        close();
    }
}

const QScreen* OverlayWidget::screenForWidget() const {
    const QScreen* screen = QGuiApplication::screenAt(geometry().center());
    return screen ? screen : QGuiApplication::primaryScreen();
}

} // namespace nb
