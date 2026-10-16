// 截图会话:遮罩窗口(每屏一个)+ 冻结底图 + 框选;确认后裁剪 → 入库 → 写剪贴板。
//
// 坐标约定(关键):
//  - 冻结底图 = 屏幕**物理像素**(xcap/GDI 原样);
//  - 鼠标事件 / 窗口 rect = **逻辑像素**(Qt 6 高 DPI 下自动缩放);
//  - 交给核心 nb_capture_crop 的选区必须是**物理像素**且相对该屏左上角。
// 三者靠 devicePixelRatio 换算,漏掉换算会在缩放屏上取错区域/显示错内容。
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

#include "editor/annotation_engine.hpp"

class QLineEdit;
class QScreen;

namespace nb {

class AnnotationToolbar;   // 二级条共享控件(编辑器/浮条同一类)

enum class CaptureIntent { Region, Fullscreen };

/// 裁剪入库完成后的回调,参数是刚入库条目的 id(未入库/失败时为空串)。
/// 截图翻译/取字用它接续 OCR → 翻译链路;不传则只做"入库 + 写剪贴板"。
using CaptureDone = std::function<void(const QString& entryId)>;

/// 启动会话(Fullscreen = 光标屏整屏直通;Region = 各屏遮罩框选)。
/// 忙碌重入检查沿用第一代 CAPTURE_BUSY 语义。
///
/// ⚠️ 框选确认后**一律直通**(入库 + 写剪贴板),不再有"进编辑器"这条出口(§18.5):
/// 编辑器只保留唯一入口 = 右键编辑(主面板条目右键 / 贴图右键),框选链路的编辑
/// 能力由**浮条工具栏**承担。原先的 allowEditor 参数随之移除。
void startCaptureSession(CaptureIntent intent, CaptureDone onCaptured = {});

/// 延时截图(F16):先弹一个置顶的倒计时数字(鼠标穿透),倒计时结束再启动会话。
/// 用途:按下入口后有时间去展开菜单 / 等悬浮提示出现。seconds<=0 时等同立即截图。
void startDelayedCapture(int seconds, CaptureIntent intent, CaptureDone onCaptured = {});

/// 读取设置里的延时截图等待秒数(钳制到 1~10;读取失败按 3 秒)。
/// 托盘入口与快捷键共用,避免两处各写一份。
int captureDelaySeconds();

/// 入库一张图片并写入系统剪贴板(截图与编辑器「完成」共用同一通道)。
/// 返回条目 id;未入库(超大小上限)或失败时返回空串。
///
/// 顺序遵循第一代 docs/phase5.md §3.6:**先入库、后写剪贴板** —— 剪贴板监听线程
/// 读到同一张图时按 hash 命中去重,只 touch 不新建,不会产生重复条目。
QString ingestImageAndCopy(const QImage& image);

/// 遮罩窗口:铺满**指定**屏幕,绘制冻结底图 + 半透明遮罩 + 选区。
class OverlayWidget : public QWidget {
    Q_OBJECT
public:
    /// screen:本遮罩所属屏幕(frozen 即该屏的物理像素底图)。
    /// autoConfirm:对应设置 regionAutoConfirm —— true = 松开鼠标即确认(默认);
    ///              false = 选区保留,可用 8 向手柄微调,Enter/双击确认。
    /// rightClickCancel:对应设置 regionRightClickCancel —— 右键是否取消本次截图。
    OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, bool autoConfirm,
                  bool rightClickCancel, QWidget* parent = nullptr);

    // ---- 浮条底板不透明度三档(docs/17 §17.5)----
    // 三档常量放 public:会话建立处(startCaptureSession,自由函数)要用它做兜底值。
    /// 三档 alpha:不透明 / 半透(默认) / 极简。
    static constexpr int kBarAlphaSolid = 242;
    static constexpr int kBarAlphaHalf = 217;
    static constexpr int kBarAlphaMinimal = 179;
    /// 应用档位(只接受三档,其余按半透处理)。
    void setBarAlpha(int alpha);
    /// 当前档位(循环切换用)。
    int barAlpha() const { return m_barAlpha; }
    /// 切到下一档并写回设置(一键循环,不弹滑块 —— 浮条空间紧张且截图时不该精细调节)。
    void cycleBarAlpha();
    /// 当前档位的说明文案(按钮 ToolTip 用)。
    QString barAlphaTip() const;
    /// 极简档下底板仅 70% 不透明 → 按钮文字需更高对比度,否则"透到看不清"。
    bool barNeedsContrastBoost() const { return m_barAlpha <= kBarAlphaMinimal; }

signals:
    /// 选区确认:**本屏物理像素**坐标,相对该屏左上角(直接供 nb_capture_crop)。
    void regionConfirmed(const QRect& physicalRect);
    /// 带标注的确认:合成图已含标注(浮条标注态确认专用),宿主**不再裁剪**,
    /// 直接把这张图入库+剪贴板。与 regionConfirmed 互斥触发。
    void annotatedConfirmed(const QImage& composed);
    void cancelled();
    /// 用户在本遮罩上有鼠标/键盘动作 —— 会话据此重置空闲超时(S6.1)。
    void activity();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    /// 文字工具的宿主内联输入框:Enter/失焦提交,Esc 取消。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// 8 向手柄 + 选区内部(整体拖动);仅微调模式使用。
    enum class Handle {
        None,
        TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left,
        Inside,
    };

    qreal dpr() const;
    /// 选区(局部逻辑坐标)按 DPR 换算为物理像素(桌面**全局**物理坐标);越界由核心钳制。
    QRect selectionPhysical() const;
    /// 引擎场景矩形 = 选区**局部**物理像素(原点在选区左上)。
    /// ⚠️ 二者不是一回事:前者给核心裁剪用,后者给引擎用;混用就是"标注画不准"的根因。
    QRect annotationSceneRect() const;
    /// 命中测试:pos 落在哪个手柄/内部(局部逻辑坐标)。
    Handle handleAt(const QPoint& pos) const;
    /// 8 个手柄中心点(局部逻辑坐标)。
    QList<QPoint> handlePoints(const QRect& rect) const;
    static Qt::CursorShape cursorForHandle(Handle handle);
    /// 按当前拖动的 handle 把选区调整到 pos(局部逻辑坐标)。
    void applyAdjustment(const QPoint& pos);
    /// 选区是否够大、可以确认。
    bool selectionUsable() const;

    QImage m_frozen;          // 该屏冻结底图(物理像素)
    int m_screenIndex;
    QScreen* m_screen = nullptr;
    bool m_autoConfirm = true;
    bool m_rightClickCancel = true;

    QPoint m_origin;          // 新选区的起点(局部逻辑坐标)
    QRect m_selection;        // 当前选区(局部逻辑坐标)
    bool m_selecting = false; // 正在拖出新选区
    bool m_adjusting = false; // 正在微调已有选区
    Handle m_activeHandle = Handle::None;
    QRect m_adjustStartRect;  // 微调开始时的选区
    QPoint m_adjustStartPos;  // 微调起始光标位置

    // ---- 框选后浮出工具栏(docs/14 §16;仅 autoConfirm=false 生效)----
    // 内嵌于遮罩窗口(不新建窗口),挂在选区底部;按钮走既有信号通道。
    bool toolbarVisible() const;
    QRect toolbarRect() const;              // 局部逻辑坐标(跟随选区/翻转)
    void showToolbar();
    /// 确认选区:标注态走**含标注**的合成图(annotatedConfirmed),否则走原裁剪通道。
    /// Enter/双击/「完成」三条确认路径共用 —— 否则"Enter 确认"会把标注丢掉。
    void confirmSelection();
    bool toolbarContains(const QPoint& pos) const;   // mousePress 命中判定
    void handleToolbarClick(const QPoint& pos);      // 分发按钮动作
    void drawToolbar(QPainter& painter) const;       // paintEvent 内绘制
    void bakeSaveDialog(const QImage& image);        // 另存对话框(保存按钮)

    // ---- 标注态(S3):引擎 + 二级条(颜色/粗细/字号/撤销重做)----
    // 「标注」按钮进入标注态:二级条展开,遮罩上的拖拽变成引擎笔画。
    // 标注与手柄微调**互斥**(docs/14 §16.3-5):有标注即隐藏手柄。
    bool annotating() const { return m_engine != nullptr; }
    bool hasAnnotations() const;             // 是否已有标注(隐藏手柄用)
    void enterAnnotating();                  // 进入标注态(建引擎+展开二级条)
    void exitAnnotating();                   // 退出(清理引擎+收起二级条)
    QRect subbarRect() const;                // 二级条几何(浮条上方)
    bool subbarContains(const QPoint& pos) const;
    void drawSubbar(QPainter& painter) const;   // 只画底板(条目在共享控件里)

    /// 切标注工具(含取色器采完自动回退的同步)。
    void setAnnoTool(EditorTool tool);

    // ---- 文字工具的内联输入(浮条没有 QGraphicsView,引擎的 QGraphicsTextItem
    //      收不到键盘事件;文本由宿主 QLineEdit 收,确认后烧成图元)----
    void beginInlineText(const QPoint& widgetPos);
    void commitInlineText();
    void cancelInlineText();
    QLineEdit* m_textEdit = nullptr;         // 正在输入的文本框(空 = 没在输入)
    QPoint m_textAnchor;                     // 落点(局部逻辑坐标)

    AnnotationEngine* m_engine = nullptr;    // 进入标注态才建(平时零开销)
    bool m_subbarVisible = false;
    QRect m_subbarRect;
    /// 二级标注条 = **共享控件**(§17.3 A,与编辑器同一个 AnnotationToolbar)。
    /// 浮条只画它的底板(视觉随主浮条),条目绘制/命中测试全由控件负责 ——
    /// 自绘版的"绘制与命中测试各写一份"是反复出 bug 的来源,不再保留。
    /// 生命周期随标注态:enterAnnotating 建,exitAnnotating 销毁(平时零开销)。
    QPointer<AnnotationToolbar> m_subbar;
    /// 控件命中:鼠标在共享控件上时,遮罩不得把它当成"选区外点击"去取消。
    bool subbarWidgetContains(const QPoint& pos) const;
    int m_penSize = 4;                       // 二级条选中的粗细档位
    int m_fontSize = 36;                     // 二级条选中的字号
    EditorTool m_annoTool = EditorTool::Pen; // 当前标注工具
    QColor m_annoColor = QColor(0xE0, 0x31, 0x31);
    bool m_draggingStroke = false;           // 标注笔画拖拽中

    int m_barAlpha = kBarAlphaHalf;   // 浮条底板不透明度(默认半透;设置可覆盖)
    QRect m_toolbarRect;        // 当前浮条几何(缓存,翻转后更新)
    QString m_ocrPending;       // 非空 = OCR 进行中(浮条按钮置灰语义)
};

} // namespace nb
