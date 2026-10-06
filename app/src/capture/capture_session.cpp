#include "capture_session.hpp"
#include "editor/editor_window.hpp"
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

// 本次会话的入库后回调(截图翻译/取字接续 OCR 用);会话结束即清空。
CaptureDone g_onCaptured;

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
    g_onCaptured = {};
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

} // namespace

/// 入库 + 写回系统剪贴板,返回条目 id(未入库或失败返回空串)。
///
/// 顺序遵循第一代 docs/phase5.md §3.6:**先入库、后写剪贴板**——剪贴板监听线程
/// 读到同一张图时按 hash 命中去重,只 touch 不新建,不会产生重复条目。
QString ingestImageAndCopy(const QImage& image) {
    CoreBridge& core = CoreBridge::instance();
    const QJsonObject resp = core.call("ingest_capture_png", {
        {"png_base64", quickPngBase64(image)},
        {"source", "capture"},
    });
    if (!resp.value("ok").toBool()) {
        qWarning() << "截图入库失败:"
                   << resp.value("error").toObject().value("message").toString();
        return {};
    }
    // entry 为 null = 超过图片大小上限被忽略(不落盘,自然也不写剪贴板)
    const QString id =
        resp.value("data").toObject().value("entry").toObject().value("id").toString();
    if (id.isEmpty()) {
        qInfo() << "截图超过大小上限,已忽略";
        return {};
    }
    const QJsonObject copied = core.call("copy_entry", {{"id", id}});
    if (!copied.value("ok").toBool()) {
        // 已入库但写剪贴板失败:仍把 id 交给后续链路(翻译/取字仍可进行)
        qWarning() << "截图写剪贴板失败:"
                   << copied.value("error").toObject().value("message").toString();
        return id;
    }
    qInfo() << "截图已入库并写入剪贴板:" << image.width() << "x" << image.height();
    return id;
}

namespace {

/// 入库 + 写剪贴板,成功后接续回调(截图翻译/取字)。
/// 编辑器是异步窗口,回调必须由调用方显式带过来,不能读会话级的 g_onCaptured。
void finishCapture(const QImage& image, const CaptureDone& done) {
    const QString id = ingestImageAndCopy(image);
    if (!id.isEmpty() && done) done(id);
}

/// regionSkipEditor(默认 true):框选确认后跳过编辑器,直接入库 + 写剪贴板。
bool skipEditor() {
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return true;   // 读不到设置时按默认(直通)
    return resp.value("data").toObject().value("regionSkipEditor").toBool(true);
}

/// regionAutoConfirm(默认 true):框选松开鼠标即确认;false 则选区保留、可用 8 向
/// 手柄微调,需 Enter 或双击确认(第一代 F10 的两种确认方式)。
bool autoConfirmRegion() {
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return true;
    return resp.value("data").toObject().value("regionAutoConfirm").toBool(true);
}
} // namespace

void startCaptureSession(CaptureIntent intent, CaptureDone onCaptured, bool allowEditor) {
    // 会话进行中 = 屏幕上还有遮罩(第一代 CAPTURE_BUSY 语义)
    if (pruneOverlays()) {
        qInfo() << "截图进行中,忽略本次触发";
        return;
    }
    g_onCaptured = onCaptured;

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
            const CaptureDone done = g_onCaptured;
            g_onCaptured = {};
            finishCapture(shot, done);
        }
        return;
    }

    // 框选:每屏一块遮罩,各自拿本屏冻结底图。
    // 确认方式由设置 regionAutoConfirm 决定,会话内只读一次。
    const bool autoConfirm = autoConfirmRegion();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        const QImage frozen = core.captureMonitor(static_cast<unsigned>(i));
        if (frozen.isNull()) {
            qWarning() << "屏" << i << "捕获失败,跳过该屏遮罩";
            continue;
        }
        auto* overlay = new OverlayWidget(frozen, i, screen, autoConfirm);
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        g_overlays.append(overlay);

        // 冻结底图已在宿主侧(QImage 隐式共享,lambda 捕获代价为 0);
        // 裁剪仍走核心 RGBA 通道 nb_capture_crop,保持"有逻辑的归 Rust"。
        QObject::connect(overlay, &OverlayWidget::regionConfirmed, overlay,
                [frozen, allowEditor](const QRect& physicalRect) {
            const CaptureDone done = g_onCaptured;   // closeAllOverlays 会清空,先取出
            closeAllOverlays();                      // 任一屏确认即收起全部遮罩
            const QImage cropped = CoreBridge::instance().captureCrop(frozen, physicalRect);
            if (cropped.isNull()) {
                qWarning() << "选区裁剪失败(空选区或越界)";
                return;
            }
            if (!allowEditor || skipEditor()) {
                finishCapture(cropped, done);
                return;
            }
            // regionSkipEditor=false:先进编辑器标注,「完成」时才入库 + 写剪贴板
            auto* editor = new EditorWindow(cropped);
            QObject::connect(editor, &EditorWindow::committed, editor,
                             [done](const QImage& composed) { finishCapture(composed, done); });
            QObject::connect(editor, &EditorWindow::cancelled, editor,
                             [] { qInfo() << "截图编辑已取消"; });
            editor->show();
            editor->raise();
            editor->activateWindow();
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

namespace {
/// 手柄命中半径与绘制边长(逻辑像素)
constexpr int kHandleGrab = 6;
constexpr int kHandleSize = 6;
/// 判定"选区可用"的最小边长(逻辑像素):太小多半是误点
constexpr int kMinSelection = 4;
} // namespace

OverlayWidget::OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, bool autoConfirm,
                             QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      m_frozen(std::move(frozen)),
      m_screenIndex(screenIndex),
      m_screen(screen),
      m_autoConfirm(autoConfirm) {
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

bool OverlayWidget::selectionUsable() const {
    const QRect sel = m_selection.normalized();
    return !sel.isNull() && sel.width() >= kMinSelection && sel.height() >= kMinSelection;
}

QList<QPoint> OverlayWidget::handlePoints(const QRect& rect) const {
    const int cx = rect.center().x();
    const int cy = rect.center().y();
    return {
        rect.topLeft(),     QPoint(cx, rect.top()),    rect.topRight(),
        QPoint(rect.right(), cy),                      rect.bottomRight(),
        QPoint(cx, rect.bottom()),                     rect.bottomLeft(),
        QPoint(rect.left(), cy),
    };
}

OverlayWidget::Handle OverlayWidget::handleAt(const QPoint& pos) const {
    if (m_selection.isNull()) return Handle::None;
    const QRect r = m_selection.normalized();
    // 先看是否贴近某条边/某个角(留 kHandleGrab 的容差)
    const bool nearLeft = qAbs(pos.x() - r.left()) <= kHandleGrab;
    const bool nearRight = qAbs(pos.x() - r.right()) <= kHandleGrab;
    const bool nearTop = qAbs(pos.y() - r.top()) <= kHandleGrab;
    const bool nearBottom = qAbs(pos.y() - r.bottom()) <= kHandleGrab;
    const bool withinX = pos.x() >= r.left() - kHandleGrab && pos.x() <= r.right() + kHandleGrab;
    const bool withinY = pos.y() >= r.top() - kHandleGrab && pos.y() <= r.bottom() + kHandleGrab;
    if (!withinX || !withinY) return Handle::None;

    if (nearLeft && nearTop) return Handle::TopLeft;
    if (nearRight && nearTop) return Handle::TopRight;
    if (nearLeft && nearBottom) return Handle::BottomLeft;
    if (nearRight && nearBottom) return Handle::BottomRight;
    if (nearTop) return Handle::Top;
    if (nearBottom) return Handle::Bottom;
    if (nearLeft) return Handle::Left;
    if (nearRight) return Handle::Right;
    if (r.contains(pos)) return Handle::Inside;   // 内部 = 整体拖动
    return Handle::None;
}

Qt::CursorShape OverlayWidget::cursorForHandle(Handle handle) {
    switch (handle) {
        case Handle::TopLeft:
        case Handle::BottomRight: return Qt::SizeFDiagCursor;
        case Handle::TopRight:
        case Handle::BottomLeft: return Qt::SizeBDiagCursor;
        case Handle::Top:
        case Handle::Bottom: return Qt::SizeVerCursor;
        case Handle::Left:
        case Handle::Right: return Qt::SizeHorCursor;
        case Handle::Inside: return Qt::SizeAllCursor;
        case Handle::None: break;
    }
    return Qt::CrossCursor;
}

void OverlayWidget::applyAdjustment(const QPoint& pos) {
    const QPoint delta = pos - m_adjustStartPos;
    QRect r = m_adjustStartRect;
    if (m_activeHandle == Handle::Inside) {
        r.translate(delta);
    } else {
        if (m_activeHandle == Handle::TopLeft || m_activeHandle == Handle::Left ||
            m_activeHandle == Handle::BottomLeft)
            r.setLeft(r.left() + delta.x());
        if (m_activeHandle == Handle::TopRight || m_activeHandle == Handle::Right ||
            m_activeHandle == Handle::BottomRight)
            r.setRight(r.right() + delta.x());
        if (m_activeHandle == Handle::TopLeft || m_activeHandle == Handle::Top ||
            m_activeHandle == Handle::TopRight)
            r.setTop(r.top() + delta.y());
        if (m_activeHandle == Handle::BottomLeft || m_activeHandle == Handle::Bottom ||
            m_activeHandle == Handle::BottomRight)
            r.setBottom(r.bottom() + delta.y());
        r = r.normalized();
    }
    // 夹在窗口内:遮罩就是整屏,拖到屏外没有意义
    m_selection = r.intersected(rect());
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

        // 微调模式:画 8 个手柄(提示可拖动调整)
        if (!m_autoConfirm && selectionUsable()) {
            painter.setPen(QPen(QColor(80, 160, 255), 1));
            painter.setBrush(QColor(255, 255, 255));
            for (const QPoint& point : handlePoints(sel)) {
                painter.drawRect(QRect(point.x() - kHandleSize / 2,
                                       point.y() - kHandleSize / 2, kHandleSize, kHandleSize));
            }
        }

        // 尺寸提示按**物理像素**显示,即最终截出来的分辨率
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(QPoint(sel.x(), sel.y() - 6),
                         QStringLiteral("%1 × %2").arg(src.width()).arg(src.height()));
    } else if (!m_selecting) {
        painter.setPen(QColor(255, 255, 255, 200));
        painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignTop,
                         m_autoConfirm
                             ? QStringLiteral("拖拽框选,Esc 取消(屏 %1)").arg(m_screenIndex)
                             : QStringLiteral("拖拽框选;可拖手柄微调,Enter 或双击确认,Esc 取消(屏 %1)")
                                   .arg(m_screenIndex));
    }
}

void OverlayWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;

    // 微调模式:已有选区时,按在手柄/内部上就进入调整,而不是重新框选
    if (!m_autoConfirm && !m_selection.isNull()) {
        const Handle handle = handleAt(e->pos());
        if (handle != Handle::None) {
            m_adjusting = true;
            m_activeHandle = handle;
            m_adjustStartRect = m_selection.normalized();
            m_adjustStartPos = e->pos();
            return;
        }
    }

    // 起新选区
    m_origin = e->pos();
    m_selection = QRect(m_origin, QSize(0, 0));
    m_selecting = true;
    update();
}

void OverlayWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_selecting) {
        m_selection = QRect(m_origin, e->pos()).normalized();
        update();
        return;
    }
    if (m_adjusting) {
        applyAdjustment(e->pos());
        update();
        return;
    }
    // 悬停时给出光标反馈(仅微调模式有意义)
    if (!m_autoConfirm && !m_selection.isNull()) {
        setCursor(cursorForHandle(handleAt(e->pos())));
    }
}

void OverlayWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;

    if (m_adjusting) {
        m_adjusting = false;
        m_activeHandle = Handle::None;
        return;   // 微调结束不触发确认
    }
    if (!m_selecting) return;

    m_selecting = false;
    m_selection = QRect(m_origin, e->pos()).normalized();
    if (!selectionUsable()) {
        // 误点(没拖出有效范围):清掉选区,等用户重新框
        m_selection = QRect();
        update();
        if (m_autoConfirm) emit cancelled();   // 直通模式保持"点空即取消"的原有手感
        return;
    }
    // 直通模式:松开即确认;微调模式:保留选区,等 Enter/双击
    if (m_autoConfirm) emit regionConfirmed(selectionPhysical());
    update();
}

void OverlayWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (m_autoConfirm || e->button() != Qt::LeftButton) return;
    if (!selectionUsable()) return;
    emit regionConfirmed(selectionPhysical());
}

void OverlayWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        emit cancelled();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (!m_autoConfirm && selectionUsable()) emit regionConfirmed(selectionPhysical());
    }
}

} // namespace nb
