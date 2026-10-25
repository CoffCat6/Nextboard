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
/// Crop(裁剪)是构图类操作:不产生标注图元,而是改变底图本身 —— 引擎层与标注
/// 同源,但 v1 只在编辑窗暴露(浮条的"裁剪"= 重新框选,键位也与会话键冲突)。
enum class EditorTool
{
    Pen, Arrow, Line, Rect, Ellipse, Mosaic, Blur, Number, Text, Picker, Crop
};

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

    /// 构图类操作(裁剪/旋转)的撤销命令:需要动引擎私有态(换底图/换背景项/
    /// 重建纹理),声明为嵌套类以获得私有访问权(嵌套类是外围类的成员)。
    class TransformCommand;

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

    // ---- 构图类操作(裁剪/旋转)----
    //
    // 两者共用一套模型:「换底图 + 仿射变换全部标注图元」的一次原子操作,
    // 经 TransformCommand 进撤销栈,与逐笔标注的 AddItemCommand 严格 LIFO 兼容。
    //
    // 变换规则(QGraphicsItem 的场景映射 = translate(pos) ∘ transform):
    //   item->setPos(A·pos); item->setTransform(M * T);   // A = T 的线性部分
    // (平移分量不能进 pos —— 会与 transform 里的平移重复计入,推导见 .cpp)

    /// 整图旋转 90°(顺/逆时针)。像素置换、无损(不做插值,反复旋转不糊化;
    /// 手法与 PinWindow::rotate90cw 同源)。完成后切回上一个绘图工具。
    void rotate90(bool clockwise);

    /// 应用当前裁剪框(Crop 工具交互产物)。框无效(<2px 或未画)则不动。
    /// 标注图元保持为可撤销图元(不烘焙进底图):被裁掉一半的标注剩余部分
    /// 仍可见、仍可 Ctrl+Z 撤销。完成后切回上一个绘图工具。
    void applyCrop();

    /// 取消裁剪:丢弃裁剪框并回到上一个绘图工具(取消按钮 / Esc / 再按 C 共用)。
    void cancelCrop();

    /// 当前裁剪框(场景坐标 = 图像物理像素;无效时为空矩形)。
    QRectF cropRect() const { return m_cropRect; }
    /// 裁剪工具激活且已有有效矩形(宿主据此把 Enter 从「完成」改派给「应用裁剪」)。
    bool cropReady() const;
    /// 裁剪确认/取消按钮的场景矩形(index 0 = 确认 ✓,1 = 取消 ✕;
    /// 尺寸按视图缩放取反保持屏幕常量,随裁剪框右下角走,贴底图下缘时翻进框内)。
    /// 供验收 flow 计算点击坐标;无有效框时返回空矩形。
    QRectF cropButtonRect(int index) const;

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
    /// 底图被构图操作改变(旋转/裁剪应用,含撤销/重做恢复)。宿主据此重新适配视图。
    void baseChanged();

private:
    void pushItem(QGraphicsItem* item, const QString& label);
    qreal badgeRadius() const;
    QPainterPath strokePath() const;
    QGraphicsItem* buildDragItem() const;
    QGraphicsItem* buildPreviewItem() const;
    QGraphicsItem* buildFinalItem() const;
    void clearPreview();

    // ---- 构图类操作的内部支撑(实现见 .cpp)----
    class CropOverlayItem;                       // 裁剪框覆盖层(遮罩/手柄/三分线)
    /// 把两张底图之间共享的换底图动作一次做完(背景项、sceneRect、纹理失效)。
    void swapBase(const QImage& next);
    /// 惰性生成马赛克/模糊纹理;换底图后当前工具若正用它们须立即重建,
    /// 否则空纹理画笔会静默画出无效内容。
    void ensureFilterTextures();
    /// 裁剪覆盖层的建/拆/更新(Crop 工具专用,不入撤销栈)。
    void rebuildCropOverlay();
    void clearCropOverlay();
    /// 手柄命中(返回 0-7 之一;框内命中 = 8;框外/无框 = -1)。场景坐标。
    int cropHandleAt(const QPointF& scenePos) const;
    /// 确认/取消按钮命中(0 = 确认,1 = 取消,-1 = 不在按钮上)。按钮画在
    /// 最上层,命中判定也排在手柄之前(见 onCanvasPressed)。
    int cropButtonAt(const QPointF& scenePos) const;
    /// 视图当前缩放(无视图时 1.0):屏幕常量元素(手柄/按钮)的尺寸基准。
    qreal viewScale() const;
    /// 裁剪按钮悬停高亮(实现在覆盖层类定义之后 —— 那里才是完整类型)。
    void updateCropHover(const QPointF& scenePos);
    /// 裁剪框夹回底图范围内并保持最小尺寸。
    void clampCropRect();
    /// 原子地完成一次构图变换:采集前后状态 → 对全部标注图元施加仿射 T →
    /// 换底图 → TransformCommand 入栈(redo 幂等,undo/redo 都做绝对还原)。
    void applyTransform(const QTransform& T, const QImage& newBase, const QString& label);

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

    // ---- 裁剪交互态 ----
    QRectF m_cropRect;                    // 当前裁剪框(空 = 未画)
    QPointF m_cropAnchor;                 // 本次调整起算参照(按下点或原框角)
    QRectF m_cropAnchorRect;              // 按下时的原框(整体移动/缩放都要参照)
    int m_cropDragHandle = -1;            // -1 = 非手柄拖拽;8 = 框内移动;9 = 拖新框
    int m_cropPressButton = -1;           // 按下命中的确认/取消按钮(-1 = 无)
    int m_cropHoverButton = -1;           // 悬停命中的按钮(高亮反馈)
    CropOverlayItem* m_cropOverlay = nullptr;

    QPixmap m_mosaicSource;
    QPixmap m_blurSource;
    EditorTool m_prevTool = EditorTool::Pen;
    int m_nextNumber = 1;
};

} // namespace nb
