#include "pin_window.hpp"

#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

namespace nb {

namespace {
constexpr int kTextPadding = 14;
constexpr int kTextMinWidth = 160;
} // namespace

QImage PinWindow::renderText(const QString& text, const PinTextStyle& style) {
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(style.fontSize);
    const int maxWidth = qMax(kTextMinWidth, style.maxWidth);
    const QFontMetrics metrics(font);
    // 先按最大宽度做换行排版测量,再定画布尺寸
    const QRect bounds = metrics.boundingRect(
        QRect(0, 0, maxWidth - 2 * kTextPadding, 10000),
        Qt::TextWordWrap | Qt::AlignLeft, text);
    const int width = qBound(kTextMinWidth, bounds.width() + 2 * kTextPadding, maxWidth);
    const int height = bounds.height() + 2 * kTextPadding;

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setBrush(style.background);
    painter.setPen(QPen(style.background.lighter(140), 1));
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1.0, height - 1.0), 8, 8);
    painter.setPen(style.textColor);
    painter.setFont(font);
    painter.drawText(QRect(kTextPadding, kTextPadding,
                           width - 2 * kTextPadding, height - 2 * kTextPadding),
                     Qt::TextWordWrap | Qt::AlignLeft, text);
    painter.end();
    return image;
}

PinWindow::PinWindow(QImage image, const PinOptions& options, QWidget* parent)
    : QWidget(parent,
              Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                  Qt::NoDropShadowWindowHint),
      m_image(std::move(image)),
      // 初始透明度取自设置(第一代 pinDefaultOpacity,30–100)
      m_opacity(qBound(0.3, options.opacityPercent / 100.0, 1.0)),
      m_doubleClickClose(options.doubleClickClose) {
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    // 落点 = 光标所在屏中央(第一代决策:四入口统一 at=None)
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    // 关键:按**目标屏**的 DPR 换算逻辑尺寸,而不是主屏 —— 窗口 show 之前的
    // devicePixelRatioF 仍是主屏值,拿它换算会在不同缩放比的屏上整体放大/缩小。
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    resize(logicalSizeFor(dpr));
    if (screen) {
        const QRect avail = screen->availableGeometry();
        // 图比屏大:退化为左上角起放(第一代 center_in 语义)
        move(avail.center() - rect().center());
    }
}

qreal PinWindow::currentDpr() const {
    // 用所属屏的 DPR:窗口跨屏后 Qt 会更新 screen(),据此保持物理尺寸不变。
    if (QScreen* s = screen()) return s->devicePixelRatio();
    return devicePixelRatioF();
}

QSize PinWindow::logicalSizeFor(qreal dpr) const {
    if (dpr <= 0.0) dpr = 1.0;
    // 原图是**物理像素**,窗口坐标是**逻辑像素**:除以 DPR 后,窗口的物理尺寸
    // 恰为 原图 × 缩放,与所在屏的缩放比无关(跨屏拖来拖去不会变大变小)。
    return QSize(qMax(1, qRound(m_image.width() * m_zoom / dpr)),
                 qMax(1, qRound(m_image.height() * m_zoom / dpr)));
}

void PinWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setOpacity(m_opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(rect(), m_image);
}

void PinWindow::applyScale() {
    resize(logicalSizeFor(currentDpr()));
    update();
}

bool PinWindow::event(QEvent* e) {
    // 窗口被拖到不同缩放比的屏幕,或系统缩放变化:Qt 只重绘、不调整窗口几何,
    // 窗口逻辑尺寸不变 → 物理尺寸随 DPR 变化(忽大忽小)。这里按新 DPR 重算逻辑
    // 尺寸,使物理尺寸恒为 原图 × 缩放。
    //
    // 延到事件循环末尾再算:Windows 平台随后会在 WM_DPICHANGED 里 SetWindowPos
    // 原生调整窗口几何(且本事件在其之前同步送达),立即 resize 会被它覆盖;
    // 延后则保证最终尺寸由我们决定。
    const QEvent::Type type = e->type();
    if (type == QEvent::DevicePixelRatioChange || type == QEvent::ScreenChangeInternal) {
        QTimer::singleShot(0, this, [this] { applyScale(); });
    }
    return QWidget::event(e);
}

void PinWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = e->globalPosition().toPoint() - pos();
    }
}

void PinWindow::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging) move(e->globalPosition().toPoint() - m_dragOffset);
}

void PinWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) m_dragging = false;
}

void PinWindow::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::AltModifier) {
        // Alt + 滚轮:透明度(第一代语义)
        m_opacity = qBound(0.3, m_opacity + (e->angleDelta().y() > 0 ? 0.05 : -0.05), 1.0);
    } else {
        // 滚轮:缩放(1.0 = 1:1 物理像素);下限 0.1 防止缩成一个点
        m_zoom = qBound(0.1, m_zoom * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1), 4.0);
    }
    applyScale();
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* e) {
    Q_UNUSED(e);
    // 双击关闭:第一代 Phase 9 默认(Snipaste 习惯);设置里可关掉
    if (m_doubleClickClose) close();
}

} // namespace nb
