// Sciter 嵌入(ProcX 位图模式)——官方 demos.lite/lite-bitmap 的 Qt 适配。
//
// 为什么不用 mixin(SciterProcND):Qt 拥有窗口的 WM_PAINT/backing store,引擎的
// GDI 绘制与 Qt 合成互相覆盖(实测窗口透明)。ProcX 模式下引擎渲染进**我们提供的
// DIB 位图**,Qt 在 paintEvent 里把它画出来——渲染路径完全可控。
//
// 消息映射:
//   attach           → SXM_CREATE(SL_TARGET_BITMAP) + SXM_RESOLUTION
//   resizeEvent      → SXM_SIZE(width, height, surface{pixels,stride})
//   paintEvent       → SXM_PAINT(rc) 后把 DIB 位图 drawImage 到 Qt canvas
//   鼠标/键盘        → Qt 事件转 SXM_MOUSE / SXM_KEY
//   心跳             → QTimer 发 SXM_HEARTBIT(动画/计时器驱动)
//   SC_INVALIDATE_RECT 回调 → update() 触发重绘
#pragma once

#include <QWidget>
#include <QImage>

#include "host/core_bridge.hpp"

#include <sciter-x.h>

namespace nb {

class SciterView : public QWidget {
    Q_OBJECT
public:
    explicit SciterView(QWidget* parent = nullptr);
    ~SciterView() override;

    /// winId 就绪后调用一次:创建引擎实例并加载主面板 HTML。
    void attach();

    /// 自动化验收:在面板文档里设置搜索框内容并触发 input 事件。
    void setSearchText(const QString& text);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private:
    bool ensureSurface(const QSize& size);
    void sendMouse(QMouseEvent* e, MOUSE_EVENTS evt);
    void sendKey(QKeyEvent* e, KEY_EVENTS evt);
    static UINT SC_CALLBACK sciterCallback(LPSCITER_CALLBACK_NOTIFICATION pns, LPVOID param);

    // DIB 表面(引擎渲染目标;dibBits 同时作为 QImage 的数据源)
    QImage m_surface;            // ARGB32Premultiplied? 用 Format_RGB32(引擎输出为不透明 BGRA)
    UINT m_dpi = 96;
    bool m_created = false;
    bool m_loaded = false;
    QTimer* m_heartbit = nullptr;
};

} // namespace nb
