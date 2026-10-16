// 标注引擎:QGraphicsScene 承载标注图元 + QUndoStack 撤销重做(docs/14 §16.2)。
//
// 从 EditorWindow 提取的**纯标注逻辑**,与宿主窗口解耦:
//   - EditorWindow(独立编辑窗)与 OverlayWidget(框选浮条,S3)共用同一份实现;
//   - 场景坐标 = 底图物理像素(两处同口径);
//   - 宿主只负责:视图缩放/工具栏/键盘/OCR/贴图等窗口级事务。
//
// 行为对齐第一代 docs/phase5.md §3.4:画笔/箭头/矩形/马赛克/文字等十类工具;
// 马赛克 = 底图像素化图层以笔画为 mask;文字确认即烘焙(可撤销重来)。
#pragma once

#include <QColor>
#include <QObject>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QPixmap>
#include <QRect>
#include <QString>

class QGraphicsItem;
class QGraphicsScene;
class QGraphicsTextItem;
class QPainterPath;
class QUndoStack;

namespace nb {

/// 标注工具(定义随本引擎走,编辑窗/遮罩共用)。
enum class EditorTool { Pen, Arrow, Line, Rect, Ellipse, Mosaic, Blur, Number, Text, Picker };

/// 工具的中文显示名(编辑窗与遮罩浮条共用)。
QString toolLabel(EditorTool tool);

class AnnotationEngine : public QObject {
    Q_OBJECT
public:
    /// base:底图(物理像素)。场景与底图同尺寸,compose 输出亦然。
    explicit AnnotationEngine(QImage base, QObject* parent = nullptr);
    ~AnnotationEngine() override;

    QGraphicsScene* scene() const { return m_scene; }
    QUndoStack* undoStack() const { return m_undo; }

    EditorTool tool() const { return m_tool; }
    QColor color() const { return m_color; }
    void setColor(const QColor& c) { m_color = c; }
    /// 取色器采到调色板外颜色时,宿主需要据此清空色块选中态。
    QColor colorChanged() { return m_color; }
    int strokeWidth() const { return m_size; }
    void setStrokeWidth(int w) { m_size = w; }
    int fontSize() const { return m_fontSize; }
    void setFontSize(int s) { m_fontSize = s; }
    /// 是否正处文字编辑态(宿主据此把 Enter 留给换行)。
    bool editingText() const { return m_textItem != nullptr; }

    void setTool(EditorTool tool);
    /// 取色器:从底图采样(不受已画标注影响);返回采到的颜色。
    QColor pickColor(const QPointF& scenePos);
    /// 上一个绘图工具(取色器采样后回退用)。
    EditorTool previousTool() const { return m_prevTool; }

    /// 笔画事件(场景坐标)。方法名沿用 EditorWindow 的 onCanvas*(与迁移代码一致)。
    void onCanvasPressed(const QPointF& scenePos);
    void onCanvasMoved(const QPointF& scenePos);
    void onCanvasReleased(const QPointF& scenePos);

    /// 烘焙进行中的文字图元(切换工具/完成前必须调)。
    void bakeTextItem();

    /// 合成底图 + 全部标注,按物理像素输出。
    QImage compose() const;
    /// 只合成某个子区域(物理像素,遮罩浮条用:选区外不重画)。
    QImage compose(const QRect& physicalRect) const;

signals:
    /// 文字图元建立:宿主负责 setFocus(item) 与视图 passthrough。
    void textEditingStarted(QGraphicsTextItem* item);
    /// 取色完成(宿主同步色块选中态)。
    void colorPicked(const QColor& color);

private:
    void pushItem(QGraphicsItem* item, const QString& label);
    qreal badgeRadius() const;
    QPainterPath strokePath() const;
    QGraphicsItem* buildDragItem() const;
    QGraphicsItem* buildPreviewItem() const;
    QGraphicsItem* buildFinalItem() const;
    void clearPreview();

    QImage m_base;
    QGraphicsScene* m_scene = nullptr;
    QUndoStack* m_undo = nullptr;

    EditorTool m_tool = EditorTool::Pen;
    QColor m_color = QColor(0xE0, 0x31, 0x31);
    int m_size = 4;
    int m_fontSize = 36;

    QPointF m_start;
    QPointF m_current;
    bool m_dragging = false;
    QList<QPointF> m_stroke;
    QGraphicsItem* m_preview = nullptr;
    QGraphicsTextItem* m_textItem = nullptr;

    QPixmap m_mosaicSource;
    QPixmap m_blurSource;
    EditorTool m_prevTool = EditorTool::Pen;
    int m_nextNumber = 1;
};

} // namespace nb
