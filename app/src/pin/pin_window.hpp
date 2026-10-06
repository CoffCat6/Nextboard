// 贴图窗口:置顶无边框,拖动/滚轮缩放/透明度/双击关闭(第一代 pin 交互语义)。
#pragma once

#include <QWidget>

namespace nb {

class PinWindow : public QWidget {
    Q_OBJECT
public:
    /// image:要贴出的图;logicalSize:逻辑尺寸(物理像素图在 2x 屏自动减半)。
    PinWindow(QImage image, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void applyScale();

    QImage m_image;
    qreal m_scale = 1.0;      // 0.1 ~ 4.0
    qreal m_opacity = 1.0;    // 0.3 ~ 1.0(滚轮 + Alt 调节,或 S3 工具条)
    bool m_dragging = false;
    QPoint m_dragOffset;
};

} // namespace nb
