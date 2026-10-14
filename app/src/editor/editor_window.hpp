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
class QGraphicsItem;
class QGraphicsScene;
class QGraphicsTextItem;
class QGraphicsView;
class QPainterPath;
class QResizeEvent;
class QToolBar;
class QUndoStack;

namespace nb {

/// 标注工具。前五项对齐第一代 editor-core.ts 的 Tool;
/// 直线 / 椭圆 / 模糊 / 序号 / 取色为后续补充。
enum class EditorTool { Pen, Arrow, Line, Rect, Ellipse, Mosaic, Blur, Number, Text, Picker };

/// 编辑器窗口。完成时发 committed(合成图),取消时发 cancelled。
class EditorWindow : public QWidget {
    Q_OBJECT
public:
    /// base:待编辑的底图(裁剪后的**物理像素**图)。
    explicit EditorWindow(QImage base, QWidget* parent = nullptr);
    ~EditorWindow() override;

    /// 合成底图 + 全部标注,按物理像素输出(导出与入库共用)。
    QImage compose() const;

signals:
    /// 「完成」:合成图交调用方入库 + 写剪贴板。
    void committed(const QImage& composed);
    /// 「取消」或 Esc。
    void cancelled();

protected:
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private slots:
    void onCanvasPressed(const QPointF& scenePos);
    void onCanvasMoved(const QPointF& scenePos);
    void onCanvasReleased(const QPointF& scenePos);

private:
    void buildToolBar();
    void fitCanvas();
    void setTool(EditorTool tool);
    void pushItem(QGraphicsItem* item, const QString& label);
    void bakeTextItem();
    void saveToFile();
    void finish();
    void cancel();
    /// 把色块选中态同步到 m_color(取色器采到调色板外的颜色时全部取消选中)。
    void syncColorSwatch();
    /// 把工具选中态同步到 m_tool(取色器采样后程序化切回上一个工具时用)。
    void syncToolAction();
    /// 序号标注圆点半径(随粗细档位变化)。
    qreal badgeRadius() const;

    /// 把笔画点串平滑成折线路径(相邻点取中点做二次贝塞尔,避免锯齿)。
    QPainterPath strokePath() const;
    QGraphicsItem* buildDragItem() const;      // 矩形/箭头:按下点 → 当前点
    QGraphicsItem* buildPreviewItem() const;   // 拖拽中的预览(不进撤销栈)
    QGraphicsItem* buildFinalItem() const;     // 松手后真正入撤销栈的图元
    void clearPreview();

    QImage m_base;
    QGraphicsScene* m_scene = nullptr;
    QGraphicsView* m_view = nullptr;
    QUndoStack* m_undo = nullptr;
    QList<QToolBar*> m_toolBars;   // 两行工具栏(工具+颜色 / 粗细字号+操作)
    QActionGroup* m_toolGroup = nullptr;

    EditorTool m_tool = EditorTool::Pen;
    QColor m_color = QColor(0xE0, 0x31, 0x31);   // 第一代 PALETTE 首色
    int m_size = 4;                              // 第一代 SIZES 中档
    int m_fontSize = 36;                         // 第一代 FONT_SIZES 中档

    // 拖拽状态(场景坐标 = 底图物理像素)
    QPointF m_start;
    QPointF m_current;
    bool m_dragging = false;
    QList<QPointF> m_stroke;                 // 画笔/马赛克的轨迹点
    QGraphicsItem* m_preview = nullptr;      // 拖拽中的临时图元(不入撤销栈)
    QGraphicsTextItem* m_textItem = nullptr; // 正在编辑的文字图元

    QPixmap m_mosaicSource;                  // 像素化底图(马赛克纹理,首次用马赛克工具时惰性生成)
    QPixmap m_blurSource;                    // 模糊纹理(首次用模糊工具时惰性生成)
    QActionGroup* m_colorGroup = nullptr;    // 色块动作组(取色器需同步选中态)

    EditorTool m_prevTool = EditorTool::Pen; // 取色器采样后回退到的工具
    int m_nextNumber = 1;                    // 序号标注的下一个编号
};

} // namespace nb
