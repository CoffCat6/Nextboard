#include "pin_window.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

namespace nb {

PinWindow::PinWindow(QImage image, QWidget* parent)
    : QWidget(parent,
              Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                  Qt::NoDropShadowWindowHint),
      m_image(std::move(image)) {
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
    // 双击关闭:第一代 Phase 9 默认(Snipaste 习惯)
    close();
}

} // namespace nb
