// 贴图窗口:置顶无边框,拖动/滚轮缩放/透明度/双击关闭(第一代 pin 交互语义)。
#pragma once

#include <QColor>
#include <QWidget>

namespace nb {

/// 贴图窗口的初始行为,来自设置 `pinDefaultOpacity` / `pinDoubleclickClose`。
/// (这两个设置此前在代码里零引用 —— 改设置对贴图窗口毫无影响。)
struct PinOptions {
    int opacityPercent = 100;        // 30–100
    bool doubleClickClose = true;
};

/// 文本贴图的渲染样式,来自设置 `pinTextBg` / `pinTextColor` /
/// `pinTextFontSize` / `pinTextMaxWidth` / `pinTextBgOpacity`。
struct PinTextStyle {
    QColor background = QColor(0x26, 0x27, 0x2E, 235);
    QColor textColor = QColor(0xDF, 0xE1, 0xE8);
    int fontSize = 15;
    int maxWidth = 520;
};

class PinWindow : public QWidget {
    Q_OBJECT
public:
    /// image:要贴出的图;options:初始透明度与双击关闭行为。
    PinWindow(QImage image, const PinOptions& options = {}, QWidget* parent = nullptr);

    /// 文本条目贴图:把文本按 style 渲染成位图,复用同一套窗口与交互。
    /// 第一代允许"无图时贴最近文本内容",这里是它的渲染实现。
    static QImage renderText(const QString& text, const PinTextStyle& style);

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
    bool m_doubleClickClose = true;
};

} // namespace nb
