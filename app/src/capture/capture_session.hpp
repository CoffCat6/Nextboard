// 截图会话:遮罩窗口(每屏一个)+ 冻结底图 + 框选;确认后裁剪 → 入库 → 写剪贴板。
//
// 坐标约定(关键):
//  - 冻结底图 = 屏幕**物理像素**(xcap/GDI 原样);
//  - 鼠标事件 / 窗口 rect = **逻辑像素**(Qt 6 高 DPI 下自动缩放);
//  - 交给核心 nb_capture_crop 的选区必须是**物理像素**且相对该屏左上角。
// 三者靠 devicePixelRatio 换算,漏掉换算会在缩放屏上取错区域/显示错内容。
#pragma once

#include <QObject>
#include <QWidget>

class QScreen;

namespace nb {

enum class CaptureIntent { Region, Fullscreen };

/// 启动会话(Fullscreen = 光标屏整屏直通;Region = 各屏遮罩框选)。
/// 忙碌重入检查沿用第一代 CAPTURE_BUSY 语义。
void startCaptureSession(CaptureIntent intent);

/// 遮罩窗口:铺满**指定**屏幕,绘制冻结底图 + 半透明遮罩 + 选区。
class OverlayWidget : public QWidget {
    Q_OBJECT
public:
    /// screen:本遮罩所属屏幕(frozen 即该屏的物理像素底图)。
    OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, QWidget* parent = nullptr);

signals:
    /// 选区确认:**本屏物理像素**坐标,相对该屏左上角(直接供 nb_capture_crop)。
    void regionConfirmed(const QRect& physicalRect);
    void cancelled();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    qreal dpr() const;
    /// 选区(局部逻辑坐标)按 DPR 换算为物理像素;越界由核心钳制。
    QRect selectionPhysical() const;

    QImage m_frozen;          // 该屏冻结底图(物理像素)
    int m_screenIndex;
    QScreen* m_screen = nullptr;
    QPoint m_origin;          // 选区起点(本窗口局部逻辑坐标)
    QRect m_selection;        // 当前选区(局部逻辑坐标)
    bool m_selecting = false;
};

} // namespace nb
