// 截图会话:遮罩窗口(每屏一个)+ 冻结底图 + 框选;确认后入库+写剪贴板。
#pragma once

#include <QObject>
#include <QWidget>

namespace nb {

enum class CaptureIntent { Region, Fullscreen };

/// 启动会话(Fullscreen = 光标屏整屏直通;Region = 全屏遮罩框选)。
/// 忙碌重入检查沿用第一代 CAPTURE_BUSY 语义。
void startCaptureSession(CaptureIntent intent);

/// 遮罩窗口:全屏无边框置顶,绘制冻结底图 + 半透明遮罩 + 选区。
class OverlayWidget : public QWidget {
    Q_OBJECT
public:
    OverlayWidget(QImage frozen, int screenIndex, QWidget* parent = nullptr);

signals:
    /// 选区确认(全局物理坐标 + 尺寸)。
    void regionConfirmed(const QRect& globalRect);
    void cancelled();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QImage m_frozen;     // 该屏冻结底图(物理像素)
    int m_screenIndex;
    QPoint m_origin;     // 选区起点(本窗口局部坐标)
    QRect m_selection;   // 当前选区(局部坐标)
    bool m_selecting = false;
    const QScreen* screenForWidget() const;
};

} // namespace nb
