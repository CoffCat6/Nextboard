// 标注工具条(共享控件):工具 / 颜色 / 粗细 / 字号 / 撤销重做。
//
// 为什么抽出来(§17.3 A):这些控件的**数据源完全相同**(都来自 AnnotationEngine),
// 却曾被两个宿主各写一份 —— 遮罩浮条自绘一套(paintEvent + 手写命中测试),
// 编辑窗用 QToolBar 一套。两份必然漂移:同一产品里"浮条上的工具"与
// "编辑器里的工具"看着不一样,改一处忘一处。
//
// 本控件是**真 QWidget**,所以:
//   - 绘制、命中、高亮、键盘导航全部交给 Qt,不再有"绘制与命中各写一份算式"
//     这一类缺陷(浮条曾为此在 subbarCells() 里手工弥合过);
//   - 两个宿主嵌同一个控件 → UI 自然统一,不需要额外的"对齐"工作。
//
// 状态**不复制**:当前工具/颜色/粗细/字号都存在 AnnotationEngine 里,本控件只读它。
// 因此宿主与引擎谁来改状态都不会不一致(宿主改了调 refresh() 即可)。
#pragma once

#include <QWidget>

#include "annotation_engine.hpp"
#include "tool_icons.hpp"

class QAction;
class QActionGroup;
class QToolBar;

namespace nb {

/// 标注工具条。两行布局:
///   第一行 = **手型 + 工具(10 个)** + 调色板(七色)
///   第二行 = 粗细(三档) + 字号(三档) + 撤销 / 重做 [+ 宿主挂载的动作按钮(靠右)]
///
/// 「手型」是**画布模式**而不是绘图工具:选中它 = 不画、只拖画面(默认)。
/// 它与绘图工具互斥 —— 这是"点了工具就回不到抓手"的解法:
/// 工具行里始终有一个"回到手型"的入口,不必靠切换别的工具来退出绘制。
///
/// 宿主负责把本控件摆到自己的布局里;需要"模式/工具变化"时接对应信号。
class AnnotationToolbar : public QWidget {
    Q_OBJECT
public:
    explicit AnnotationToolbar(QWidget* parent = nullptr);

    /// 绑定引擎。可传 nullptr(先建控件后接引擎);nullptr 时控件禁用。
    void setEngine(AnnotationEngine* engine);
    /// 按引擎当前状态同步全部选中态(工具/颜色/粗细/字号)。
    /// **单一入口**:宿主与引擎任何一方改了状态都调它,不必各自维护 UI。
    void refresh();

    /// 前景色(取自当前调色板,跟随主题)。宿主画自己的按钮时用它保持一致。
    QColor ink() const { return iconInkFor(palette()); }
    /// 主题切换后重建图标(描边色需重取)。
    void reloadIcons();

    /// 把宿主的动作按钮挂到**参数行最右侧**(如编辑器的 贴图/OCR/另存/完成/取消)。
    /// 布局动机:工具在左、动作在右,符合"工具选完了去右下角执行"的手感;
    /// 一律左对齐会把动作挤在左下角,和工具混在一起分不清。
    void setTrailingWidget(QWidget* trailing);
    /// 当前是否处于"手型"(非绘制)模式。
    bool panActive() const { return m_panActive; }
    /// 以程序方式进入手型模式(宿主若需要,例如按 Esc 退出绘制)。
    void activatePan();
    /// 遮罩浮条模式:浮条上没有"可拖动的画布",手型无意义 → 隐藏手型按钮,
    /// 且默认选中第一个绘图工具(遮罩上一进来就该能画)。
    /// ⚠️ 必须在 setEngine 之前调用(它要改初始 panActive)。
    void setOverlayMode(bool on);

    /// 工具条图标边长(逻辑像素)。
    static constexpr int kIconPx = 20;

signals:
    /// 工具已切换 —— 宿主据此同步鼠标光标等窗口级状态。
    void toolChanged(EditorTool tool);
    /// 画布模式变化:true = 有绘图工具选中(可绘制);false = 手型(只拖画面)。
    /// 宿主据此切换画布的拖拽态 —— **必须用它而不是"选过工具就永远可绘制"**,
    /// 否则点了工具就回不到抓手。
    void drawingModeChanged(bool drawing);
    /// 前景色 / 粗细 / 字号变化。遮罩浮条没有 QGraphicsView,文字由宿主 QLineEdit
    /// 内联输入,需要这三个值去同步自己的成员(m_annoColor/m_penSize/m_fontSize)。
    void colorChanged(const QColor& color);
    void strokeWidthChanged(int width);
    void fontSizeChanged(int px);

private:
    void rebuildActions();

    AnnotationEngine* m_engine = nullptr;
    QToolBar* m_toolsBar = nullptr;
    QToolBar* m_paramsBar = nullptr;
    /// 参数行容器(参数栏 + 尾部动作)。存为成员而不是靠 parentWidget() 反查 ——
    /// 反查依赖"控件已被 addWidget reparent"这一隐式时序,重构时容易静默失效。
    QWidget* m_paramsRow = nullptr;
    QActionGroup* m_toolGroup = nullptr;
    QActionGroup* m_colorGroup = nullptr;
    QActionGroup* m_sizeGroup = nullptr;
    QActionGroup* m_fontGroup = nullptr;
    QAction* m_panAction = nullptr;      // 手型(与绘图工具互斥)
    QWidget* m_trailing = nullptr;       // 宿主挂载的动作按钮(参数行最右)
    /// 手型是否激活。默认 true —— 打开编辑器先能看/能拖(§18.1)。
    bool m_panActive = true;
    /// 遮罩浮条模式(见 setOverlayMode)。
    bool m_overlayMode = false;
};

} // namespace nb
