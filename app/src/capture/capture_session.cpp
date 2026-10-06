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

QString quickPngBase64(const QImage& image) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QString::fromLatin1(buffer.data().toBase64());
}

/// 入库 + 写回系统剪贴板。
///
/// 顺序遵循第一代 docs/phase5.md §3.6:**先入库、后写剪贴板**——剪贴板监听线程
/// 读到同一张图时按 hash 命中去重,只 touch 不新建,不会产生重复条目。
void ingestAndCopyToClipboard(const QImage& image) {
    CoreBridge& core = CoreBridge::instance();
    const QJsonObject resp = core.call("ingest_capture_png", {
        {"png_base64", quickPngBase64(image)},
        {"source", "capture"},
    });
    if (!resp.value("ok").toBool()) {
        qWarning() << "截图入库失败:"
                   << resp.value("error").toObject().value("message").toString();
        return;
    }
    // entry 为 null = 超过图片大小上限被忽略(不落盘,自然也不写剪贴板)
    const QString id =
        resp.value("data").toObject().value("entry").toObject().value("id").toString();
    if (id.isEmpty()) {
        qInfo() << "截图超过大小上限,已忽略";
        return;
    }
    const QJsonObject copied = core.call("copy_entry", {{"id", id}});
    if (!copied.value("ok").toBool()) {
        qWarning() << "截图写剪贴板失败:"
                   << copied.value("error").toObject().value("message").toString();
        return;
    }
    qInfo() << "截图已入库并写入剪贴板:" << image.width() << "x" << image.height();
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
        // 直通:捕获 → 入库(source=capture)→ 写剪贴板
        const QImage shot = core.captureMonitor(static_cast<unsigned>(cursorScreen));
        if (shot.isNull()) {
            qWarning() << "全屏捕获失败";
            return;
        }
        ingestAndCopyToClipboard(shot);
        return;
    }

    // 框选:每屏一个遮罩,拿各自冻结底图
    for (int i = 0; i < screens.size(); ++i) {
        const QImage frozen = core.captureMonitor(static_cast<unsigned>(i));
        auto* overlay = new OverlayWidget(frozen, i);
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        // 冻结底图已在宿主侧(QImage 隐式共享,捕获代价为 0):
        // 裁剪仍走核心 RGBA 通道 nb_capture_crop,保持"有逻辑的归 Rust"
        QObject::connect(overlay, &OverlayWidget::regionConfirmed, overlay,
                [frozen](const QRect& physicalRect) {
            const QImage cropped = CoreBridge::instance().captureCrop(frozen, physicalRect);
            if (cropped.isNull()) {
                qWarning() << "选区裁剪失败(空选区或越界)";
                return;
            }
            ingestAndCopyToClipboard(cropped);
        });
        QObject::connect(overlay, &OverlayWidget::cancelled, overlay,
                         [] { qInfo() << "截图取消"; });
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
        // 选区是窗口局部**逻辑**坐标,而冻结底图是**物理**像素:
        // 按本窗口 DPR 换算后再交给核心裁剪(否则 2x 屏上会裁错区域)。
        const qreal dpr = devicePixelRatioF();
        const QRect physical =
            QRectF(QPointF(m_selection.topLeft()) * dpr, QSizeF(m_selection.size()) * dpr)
                .toAlignedRect();
        emit regionConfirmed(physical);
        close();
    }
}

void OverlayWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        emit cancelled();
        close();
    }
}

} // namespace nb
