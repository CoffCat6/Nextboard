// 截图编辑器:QGraphicsScene 承载底图 + 标注,QUndoStack 撤销重做。
//
// 对齐第一代 docs/phase5.md §3.4 与 F12:
//  - 五类工具:画笔 / 箭头 / 矩形 / 马赛克 / 文字;
//  - 颜色、粗细、字号可选;撤销重做;Ctrl+S 另存;
//  - **画布按底图自然分辨率(物理像素)渲染**,工具栏按逻辑像素布局,
//    保证缩放屏上文字清晰、导出与预览一致;
//  - 马赛克 = 底图预生成像素化图层,以笔画路径为 mask 揭示;
//  - 文字确认后即烘焙(不可再改字,可撤销重来)——v1 取舍。
#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QPixmap>
#include <QWidget>

class QActionGroup;
class QCloseEvent;
class QGraphicsView;
class QLabel;
class QResizeEvent;
class QToolBar;

#include "annotation_engine.hpp"

namespace nb {

struct OcrLineBox;   // host/core_bridge.hpp(图中取字的行数据;此处只需前向声明)

// 前置声明必须**在命名空间内**:放在全局作用域会声明出一个 ::AnnotationToolbar,
// 与本项目真正的 nb::AnnotationToolbar 是两个类型 —— 表现为
// "无法从 nb::AnnotationToolbar* 转换为 AnnotationToolbar*"。
class AnnotationToolbar;

/// 编辑器窗口。完成时发 committed(合成图),取消时发 cancelled。
class EditorWindow : public QWidget {
    Q_OBJECT
public:
    /// base:待编辑的底图(裁剪后的**物理像素**图)。
    explicit EditorWindow(QImage base, QWidget* parent = nullptr);
    ~EditorWindow() override;

    /// 合成底图 + 全部标注,按物理像素输出(导出与入库共用)。
    QImage compose() const;

    /// 标注引擎(场景/撤销栈/构图操作)。宿主自身状态一律经信号同步、不绕过;
    /// 暴露出来主要供验收 flow 直查引擎态(裁剪框/按钮几何/撤销栈)。
    AnnotationEngine* engine() const { return m_engine; }

signals:
    /// 「完成」:合成图交调用方入库 + 写剪贴板。
    void committed(const QImage& composed);
    /// 「取消」或 Esc。
    void cancelled();
    /// 「贴图」:把合成图贴成置顶窗口(宿主负责 showPinWindow)。
    /// ⚠️ 贴图会结束本次编辑(决策:操作后工具栏关闭);OCR **不会** ——
    ///    识别结果面板驻留,由用户自己「关闭」或直接关编辑器。
    void pinRequested(const QImage& composed);
    /// 「OCR 取字」完成:text 为识别文本;失败时为空串(reason 给原因)。
    void ocrFinished(const QString& text, const QString& reason);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private slots:
    void onCanvasPressed(const QPointF& scenePos);
    void onCanvasMoved(const QPointF& scenePos);
    void onCanvasReleased(const QPointF& scenePos);

private:
    void buildToolBar();
    void fitCanvas();
    /// 缩放:**明确入口**(原先只能滚轮缩、缩了回不去)
    ///  fit = 适应窗口(并清掉"用户已手动缩放"标记);original = 1:1 实际大小。
    void zoomToFit();
    void zoomToOriginal();
    /// 窗口几何记忆(§17.3 D):关窗时写回,开窗时恢复(不可见则回退默认尺寸)。
    void saveGeometryToSettings() const;
    void restoreGeometryFromSettings();
    void bakeTextItem();
    /// 旋转底图 90°(顺/逆)。构图动作,与当前绘图工具无关 —— 用户画着画着按 R
    /// 顺手转图,不应被打断工具状态。
    void rotateBase(bool clockwise);
    void saveToFile();
    void finish();
    void cancel();
    void pinToScreen();
    void ocrTakeText();
    /// 识别画布合成图中的二维码(与浮条「二维码」按钮同一能力;毫秒级但仍异步)。
    void qrTakeText();
    void showOcrResult(const QString& text, const QString& reason);
    /// 画布的光标形状与"穿透态"和当前状态对齐(当前工具 / 是否文字编辑中)。
    /// **单一入口**:任何会改变这两者的地方都调它 —— 否则两处状态各走各的
    /// (此前 passthrough 只置 true 不复位,文字确认后画布再也画不上东西)。
    void syncCanvasState();

    AnnotationEngine* m_engine = nullptr;

    QImage m_base;
    QGraphicsView* m_view = nullptr;
    /// 标注工具条(工具/颜色/粗细/字号/撤销重做)—— 与遮罩浮条**同一个类**。
    /// 工具/颜色等状态都在引擎里,这里不再各存一份选中态(原先的
    /// m_toolGroup/m_colorGroup + syncToolAction/syncColorSwatch 已随之删除)。
    AnnotationToolbar* m_annotationBar = nullptr;
    /// 是否已选中绘图工具:true = 画布可绘制;false = 默认抓手(§18.1)。
    /// 初始 false —— 打开编辑器先能看/能拖,点工具后才进入绘制。
    bool m_drawingTool = false;

    // OCR 弹层:识别中用轻量 QLabel 提示,出结果后换成带按钮的 QFrame 面板,
    // 故类型取公共基类 QWidget*(用 QLabel* 装 QFrame 会编译不过)。
    QWidget* m_ocrPopup = nullptr;
    /// 最近一次识别的每行结果(图中取字);识别完成后由结果回调填充,
    /// showOcrResult 弹面板时转发给 OcrResultPanel。
    QVector<OcrLineBox> m_ocrLines;
};

} // namespace nb
