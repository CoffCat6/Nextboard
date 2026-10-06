#include "pin_window.hpp"

#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

namespace nb {

namespace {
// 文本贴图的最大宽度与内边距(逻辑像素)
constexpr int kTextMaxWidth = 520;
constexpr int kTextPadding = 14;
constexpr int kTextMinWidth = 160;
} // namespace

QImage PinWindow::renderText(const QString& text) {
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(15);
    const QFontMetrics metrics(font);
    // 先按最大宽度做换行排版测量,再定画布尺寸
    const QRect bounds = metrics.boundingRect(
        QRect(0, 0, kTextMaxWidth - 2 * kTextPadding, 10000),
        Qt::TextWordWrap | Qt::AlignLeft, text);
    const int width = qBound(kTextMinWidth, bounds.width() + 2 * kTextPadding, kTextMaxWidth);
    const int height = bounds.height() + 2 * kTextPadding;

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    // 与主面板 Theme 同色的圆角底板,便于在任意背景上阅读
    painter.setBrush(QColor(38, 39, 46, 235));
    painter.setPen(QPen(QColor(79, 140, 255), 1));
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1.0, height - 1.0), 8, 8);
    painter.setPen(QColor(223, 225, 232));
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
    // 2x DPI 屏:物理像素图按逻辑尺寸显示
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    m_scale = 1.0 / dpr;
    // 落点 = 光标所在屏中央(第一代决策:四入口统一 at=None)
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QSize logical(static_cast<int>(m_image.width() * m_scale),
                        static_cast<int>(m_image.height() * m_scale));
    resize(logical);
    if (screen) {
        const QRect avail = screen->availableGeometry();
        // 图比屏大:退化为左上角起放(第一代 center_in 语义)
        move(avail.center() - rect().center());
    }
}

void PinWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setOpacity(m_opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(rect(), m_image);
}

void PinWindow::applyScale() {
    const QSize logical(static_cast<int>(m_image.width() * m_scale),
                        static_cast<int>(m_image.height() * m_scale));
    resize(logical);
    update();
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
        m_scale = qBound(0.1, m_scale * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1), 4.0);
    }
    applyScale();
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* e) {
    Q_UNUSED(e);
    // 双击关闭:第一代 Phase 9 默认(Snipaste 习惯);设置里可关掉
    if (m_doubleClickClose) close();
}

} // namespace nb
