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
class QGraphicsView;
class QLabel;
class QResizeEvent;
class QToolBar;
class AnnotationEngine;

#include "annotation_engine.hpp"

namespace nb {

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
    /// 「贴图」:把合成图贴成置顶窗口(宿主负责 showPinWindow)。
    /// 贴图/OCR 都会结束本次编辑(决策:操作后工具栏关闭)。
    void pinRequested(const QImage& composed);
    /// 「OCR 取字」完成:text 为识别文本;失败时为空串(reason 给原因)。
    void ocrFinished(const QString& text, const QString& reason);

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
    void bakeTextItem();
    void saveToFile();
    void finish();
    void cancel();
    void pinToScreen();
    void ocrTakeText();
    void showOcrResult(const QString& text, const QString& reason);
    void syncColorSwatch();
    void syncToolAction();
    /// 画布的光标形状与"穿透态"和当前状态对齐(当前工具 / 是否文字编辑中)。
    /// **单一入口**:任何会改变这两者的地方都调它 —— 否则两处状态各走各的
    /// (此前 passthrough 只置 true 不复位,文字确认后画布再也画不上东西)。
    void syncCanvasState();

    AnnotationEngine* m_engine = nullptr;

    QImage m_base;
    QGraphicsView* m_view = nullptr;
        QList<QToolBar*> m_toolBars;   // 两行工具栏(工具+颜色 / 粗细字号+操作)
    QActionGroup* m_toolGroup = nullptr;
    QActionGroup* m_colorGroup = nullptr;    // 色块动作组(取色器需同步选中态)
    QLabel* m_ocrPopup = nullptr;            // OCR 结果弹层(自适应大小,随编辑器销毁)
};

} // namespace nb
