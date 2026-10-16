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
#include <QCursor>
#include <QObject>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
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

/// 工具对应的鼠标光标形状(编辑窗画布与遮罩标注态共用,**单一来源**)。
///
/// 为什么要集中一处:光标是"当前在画什么"最直接的反馈,而两个宿主各写一份必然漂移
/// (一处改形状、另一处忘了)。落点语义:
///   - 文字 = 竖线(IBeam):提示"这里会落一个字";
///   - 取色 = 手型:Qt 内置没有吸管形状,手型已是"取一下"的最接近表达;
///   - 其余(画笔/箭头/直线/矩形/椭圆/马赛克/模糊/序号)= 十字准星:精确落点。
Qt::CursorShape toolCursor(EditorTool tool);

// ---- 工具栏元数据:工具顺序 / 调色板 / 粗细 / 字号(**单一来源**)----
//
// 为什么要集中:这些是**纯粹的枚举数据**,与宿主(遮罩浮条 / 编辑窗口)无关。
// 此前两个宿主各写一份平行数组(浮条的 toolOrder()/paletteColors()/penSizes()…
// 与编辑窗的 kTools[]/kPalette[]/kSizes[]…),改一处忘一处就"两边工具不一样"。
// 声明在引擎头文件里,是因为它们本就随引擎走 —— 引擎改了工具/档位,这里跟着改。
//
// 返回 const 引用:调用方只读遍历,不拷贝(列表都是静态的)。
const QList<EditorTool>& toolbarTools();      // 工具栏展示顺序
const QList<QColor>& annotationPalette();     // 调色板(第一代七色)
const QList<int>& strokeSizes();              // 粗细档位
const QList<int>& textFontSizes();            // 字号档位

class AnnotationEngine : public QObject {
    Q_OBJECT
public:
    /// base:底图(物理像素)。场景与底图同尺寸,compose 输出亦然。
    explicit AnnotationEngine(QImage base, QObject* parent = nullptr);
    ~AnnotationEngine() override;

    QGraphicsScene* scene() const { return m_scene; }
    QUndoStack* undoStack() const { return m_undo; }
    /// 场景首项(底图)。遮罩画布已经画过同一张冻结底图,渲染标注层时可临时隐藏它
    /// —— 省掉每次鼠标移动都重合成一遍整图。
    QGraphicsItem* backgroundItem() const { return m_background; }

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
    /// 场景坐标是否落在**正在编辑的文字图元**上(含少量容差)。
    ///
    /// 用途(§18.2):文字工具要支持"一次点选、连续落多个"。宿主据此区分
    /// "点在这段文字里(定位光标/选中)"与"点在文字外(确认这段、就地再落一段)"。
    /// 容差是必要的:文字图元可能尚无内容(刚落下、还没输入),包围盒为空。
    bool editingTextContains(const QPointF& scenePos) const;

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

    /// 落一条**已烘焙**的文字标注。遮罩浮条没有 QGraphicsView,而 QGraphicsTextItem
    /// 的内联编辑要靠视图转发键盘事件(遮罩是把场景 render 进画布的),所以在浮条上
    /// 文字由宿主控件输入、确认后交到这里入场景,不进编辑态。
    void addText(const QString& text, const QPointF& topLeft, const QColor& color,
                 int pixelSize);

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
    QGraphicsItem* m_background = nullptr;   // 场景首项(底图),见 backgroundItem()
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
