#include "capture_session.hpp"
#include "host/core_bridge.hpp"

#include <QBuffer>
#include <QCursor>
#include <QGuiApplication>
#include <QIODevice>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>

#include <windows.h>

namespace nb {

namespace {
// 当前会话的全部遮罩(多屏各一块);用 QPointer 防关闭后悬空。
// "会话进行中"直接以它为准(见 pruneOverlays),不再单独维护布尔标记——
// 遮罩若被外部销毁(Alt+F4 / WM 关闭),下一次触发会自动解除忙碌,不会卡死。
QList<QPointer<OverlayWidget>> g_overlays;

/// 剔除已销毁的遮罩;返回是否仍有活跃遮罩(即会话进行中)。
bool pruneOverlays() {
    g_overlays.removeIf([](const QPointer<OverlayWidget>& p) { return p.isNull(); });
    return !g_overlays.isEmpty();
}

/// 收起本次会话的全部遮罩。
///
/// 先把列表清空再逐个 close():close() 触发 WA_DeleteOnClose → deleteLater(),
/// 因此关闭**当前发信的那块遮罩**也是安全的(延迟删除,不在信号发射栈上析构),
/// 且重入调用会看到空列表而直接返回。
void closeAllOverlays() {
    const QList<QPointer<OverlayWidget>> overlays = g_overlays;
    g_overlays.clear();
    for (const QPointer<OverlayWidget>& overlay : overlays) {
        if (overlay) overlay->close();
    }
}

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
    // 会话进行中 = 屏幕上还有遮罩(第一代 CAPTURE_BUSY 语义)
    if (pruneOverlays()) {
        qInfo() << "截图进行中,忽略本次触发";
        return;
    }

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) return;

    // 找到光标所在屏(决策 D5)
    int cursorScreen = 0;
    const QPoint cursor = QCursor::pos();
    for (int i = 0; i < screens.size(); ++i) {
        if (screens[i]->geometry().contains(cursor)) {
            cursorScreen = i;
            break;
        }
    }

    CoreBridge& core = CoreBridge::instance();
    if (intent == CaptureIntent::Fullscreen) {
        // 直通:捕获 → 入库(source=capture)→ 写剪贴板,同步完成
        const QImage shot = core.captureMonitor(static_cast<unsigned>(cursorScreen));
        if (shot.isNull()) {
            qWarning() << "全屏捕获失败";
        } else {
            ingestAndCopyToClipboard(shot);
        }
        return;
    }

    // 框选:每屏一块遮罩,各自拿本屏冻结底图
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        const QImage frozen = core.captureMonitor(static_cast<unsigned>(i));
        if (frozen.isNull()) {
            qWarning() << "屏" << i << "捕获失败,跳过该屏遮罩";
            continue;
        }
        auto* overlay = new OverlayWidget(frozen, i, screen);
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        g_overlays.append(overlay);

        // 冻结底图已在宿主侧(QImage 隐式共享,lambda 捕获代价为 0);
        // 裁剪仍走核心 RGBA 通道 nb_capture_crop,保持"有逻辑的归 Rust"。
        QObject::connect(overlay, &OverlayWidget::regionConfirmed, overlay,
                [frozen](const QRect& physicalRect) {
            closeAllOverlays();   // 任一屏确认即收起全部遮罩
            const QImage cropped = CoreBridge::instance().captureCrop(frozen, physicalRect);
            if (cropped.isNull()) {
                qWarning() << "选区裁剪失败(空选区或越界)";
                return;
            }
            ingestAndCopyToClipboard(cropped);
        });
        QObject::connect(overlay, &OverlayWidget::cancelled, overlay, [] {
            closeAllOverlays();
            qInfo() << "截图已取消";
        });

        // ⚠️ showFullScreen() 只把窗口放到"它当前所在的屏",而新建窗口默认落在主屏:
        // 多屏时若不先定位,所有遮罩会**叠在主屏上**,看到的其实是别屏的底图被拉伸
        // (既糊又错位),点一次只关掉最上面那块 → 表现为"逐屏依次确认"。
        // 先指定目标屏 + 设好几何,再全屏,才能做到每屏一块。
        overlay->setScreen(screen);
        overlay->setGeometry(screen->geometry());
        overlay->showFullScreen();
    }

    if (g_overlays.isEmpty()) return;
    // 让最后一块遮罩拿到键盘焦点,Esc 才有接收者;点击其它屏时 Windows 会自动
    // 激活被点的窗口,所以任意屏上的 Esc 都能取消整个会话。
    if (OverlayWidget* last = g_overlays.last().data()) {
        last->raise();
        last->activateWindow();
        last->setFocus();
    }
}

// ---- OverlayWidget ----

OverlayWidget::OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      m_frozen(std::move(frozen)),
      m_screenIndex(screenIndex),
      m_screen(screen) {
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
}

qreal OverlayWidget::dpr() const {
    // 用所属屏幕的 DPR 而非窗口的:窗口在 show 之前 DPR 可能还是主屏的值,
    // 而本遮罩的底图是按 m_screen 采的,两者必须同一口径。
    return m_screen ? m_screen->devicePixelRatio() : devicePixelRatioF();
}

QRect OverlayWidget::selectionPhysical() const {
    const qreal scale = dpr();
    const QRect sel = m_selection.normalized();
    return QRectF(QPointF(sel.topLeft()) * scale, QSizeF(sel.size()) * scale).toAlignedRect();
}

void OverlayWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const qreal scale = dpr();

    // 冻结底图是**物理像素**,窗口坐标是**逻辑像素**:
    // 按 DPR 反向缩放绘制,使 1 图像像素 = 1 设备像素。若直接
    // drawImage(rect(), frozen),Qt 会把图重采样到逻辑矩形,高分屏上预览发虚。
    if (!m_frozen.isNull()) {
        painter.save();
        painter.scale(1.0 / scale, 1.0 / scale);
        painter.drawImage(QPoint(0, 0), m_frozen);
        painter.restore();
    } else {
        painter.fillRect(rect(), QColor(30, 30, 34));
    }

    // 半透明黑罩(逻辑坐标,覆盖整窗)
    painter.fillRect(rect(), QColor(0, 0, 0, 90));

    const QRect sel = m_selection.normalized();
    if (!sel.isNull()) {
        // 选区内恢复原图:目标矩形是逻辑坐标,但**源矩形必须是物理像素**,
        // 否则会从底图上取到错位的区域。
        const QRect src = selectionPhysical();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(sel, m_frozen, src);

        painter.setPen(QPen(QColor(80, 160, 255), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(sel);

        // 尺寸提示按**物理像素**显示,即最终截出来的分辨率
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(QPoint(sel.x(), sel.y() - 6),
                         QStringLiteral("%1 × %2").arg(src.width()).arg(src.height()));
    } else if (!m_selecting) {
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
            return;
        }
        emit regionConfirmed(selectionPhysical());
    }
}

void OverlayWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        emit cancelled();
    }
}

} // namespace nb
