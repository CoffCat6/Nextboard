// 贴图窗口:置顶无边框,拖动/滚轮缩放/透明度/双击关闭(第一代 pin 交互语义)。
// DPI:窗口逻辑尺寸 = 原图 ÷ 当前屏 DPR × 用户缩放,物理尺寸恒为「原图 × 缩放」,
//      跨不同缩放比的屏幕拖动、或改系统缩放,都不会忽大忽小。
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
    /// 窗口被拖到不同缩放比的屏幕(或系统缩放变化)时重算尺寸。
    bool event(QEvent* e) override;

private:
    void applyScale();
    /// 窗口**当前所在屏**的 DPR;窗口未 show 时回退窗口自身/主屏值。
    /// 必须用当前屏而非主屏 —— 否则贴到不同缩放比的屏上会整体放大/缩小。
    qreal currentDpr() const;
    /// 目标逻辑尺寸 = 原图 × 用户缩放 ÷ DPR。
    /// 由此保证**物理尺寸 = 原图 × 用户缩放**,与所在屏的 DPR 无关。
    QSize logicalSizeFor(qreal dpr) const;

    QImage m_image;
    /// 用户缩放(0.1 ~ 4.0);1.0 = 1 图像像素对 1 物理像素。与 DPI 换算解耦。
    qreal m_zoom = 1.0;
    qreal m_opacity = 1.0;    // 0.3 ~ 1.0(滚轮 + Alt 调节,或 S3 工具条)
    bool m_dragging = false;
    QPoint m_dragOffset;
    bool m_doubleClickClose = true;
};

} // namespace nb
