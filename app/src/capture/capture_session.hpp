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

/// 截图会话的**意图**,决定"框完之后去哪儿"。
///
/// 与 regionAutoConfirm(设置项)的关系:**Scrolling 优先于它**。
/// 设了 regionAutoConfirm=true(松手即入库)的用户同样能用长截图 ——
/// 长截图意图本来就是"我明确要长截图",不该被一个通用的"框完立刻入库"
/// 偏好否掉。这也是把意图单独做出来的原因(旧写作方式只能靠 regionAutoConfirm
/// 区分 Direct / Toolbar,没有第三种出口)。
enum class CaptureIntent {
    /// 普通框选:按 regionAutoConfirm 决定"松手即入库"或"浮出工具栏"。
    Region,
    /// 整屏直通:不建遮罩,直接入库。
    Fullscreen,
    /// **滚动长截图**:框完即进长截图就绪态(选区锁定 + 「开始」面板),
    /// 与 regionAutoConfirm 无关。热键 Ctrl+Shift+L 走这条。
    Scrolling,
};

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
///
/// 返回值:
/// - **条目 id**(非空):正常入库并写剪贴板;
/// - **空串**:入库失败或超大小上限(此时仍会写剪贴板),具体原因经
///   `outOversize` 与 `outNotice` 告知调用方 —— 调用方据此决定是否提示用户。
///
/// 为什么要把"超限"单列出来:截一张普通区域超限几乎不可能,但**滚动长截图**
/// 极易撞上 `maxImageMb`(30000px 长图 RGBA 就是几百 MB,PNG 压完仍可能超),
/// 若只写日志,用户会以为"存下来了",过后在列表里找不到 —— 静默丢数据比报错更糟。
///
/// 顺序遵循第一代 docs/phase5.md §3.6:**先入库、后写剪贴板** —— 剪贴板监听线程
/// 读到同一张图时按 hash 命中去重,只 touch 不新建,不会产生重复条目。
/// outOversize *非空时*回填"是否因超限未入库";outNotice 回填给用户看的提示文案。
/// 两者都可传 nullptr(不需要区分的调用方,如编辑器链路)。
QString ingestImageAndCopy(const QImage& image, bool* outOversize, QString* outNotice);

/// 遮罩窗口:铺满**指定**屏幕,绘制冻结底图 + 半透明遮罩 + 选区。
class OverlayWidget : public QWidget {
    Q_OBJECT
public:
    /// screen:本遮罩所属屏幕(frozen 即该屏的物理像素底图)。
    /// autoConfirm:对应设置 regionAutoConfirm —— true = 松开鼠标即确认(默认);
    ///              false = 选区保留,可用 8 向手柄微调,Enter/双击确认。
    /// rightClickCancel:对应设置 regionRightClickCancel —— 右键是否取消本次截图。
    /// autoStartStitching:框完**直接进长截图就绪态**(CaptureIntent::Scrolling 用)。
    /// 此时 autoConfirm 的语义只剩"点空是否取消",不再决定"松手即入库" ——
    /// 否则设了 regionAutoConfirm=true 的用户一松手就入库了,永远进不了长截图。
    OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, bool autoConfirm,
                  bool rightClickCancel, bool autoStartStitching = false,
                  QWidget* parent = nullptr);

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

    /// 本遮罩所属屏幕(验收与调试用;会话层用不到,它自己持有 QScreen*)。
    QScreen* screen() const { return m_screen; }

    /// 长截图的进度更新(总高/缺口/结论)。**由会话层回调** ——
    /// 采集与拼接在会话层(它才持有屏与核心句柄),遮罩只负责显示。
    /// 之所以放 public 而非 private:会话层的 lambda 不是本类的成员。
    void updateStitchProgress(int totalHeight, int gaps, const QString& reason);

    /// 标记"上一拍已返回",允许节拍器投下一帧。
    ///
    /// 供会话层在**所有**出口调用(含捕获失败、核心拒绝)。**必须**用 RAII 守卫
    /// 或在每个 return 前调 —— 漏一次节拍器就永久停摆:页面还在滚,长图再也不更新。
    void markSampleReturned() { m_stitchSampling = false; }

    /// 当前是否处于长截图态 / 采集中(供会话层判断,避免重复投递)。
    bool stitching() const { return m_stitch != StitchState::Off; }
    bool stitchCapturing() const { return m_stitch == StitchState::Capturing; }

signals:
    /// 选区确认:**本屏物理像素**坐标,相对该屏左上角(直接供 nb_capture_crop)。
    void regionConfirmed(const QRect& physicalRect);
    /// 带标注的确认:合成图已含标注(浮条标注态确认专用),宿主**不再裁剪**,
    /// 直接把这张图入库+剪贴板。与 regionConfirmed 互斥触发。
    void annotatedConfirmed(const QImage& composed);
    void cancelled();
    /// 用户在本遮罩上有鼠标/键盘动作 —— 会话据此重置空闲超时(S6.1)。
    void activity();

    /// 长截图:请求采集一帧(由会话层在内容变化时投递,宿主侧据此调核心拼接)。
    ///
    /// 参数是**本屏物理像素**的选区矩形,相对该屏左上角 —— 与 regionConfirmed 同口径。
    /// 宿主收到后重新捕获该屏并裁这个矩形(不能用 m_frozen:那是框选时的静止画面)。
    void stitchFrameRequested(const QRect& physicalRect);
    /// 长截图:用户点了「结束」,请求会话层收尾(核心 finish → 入库 → 剪贴板)。
    /// 与 stitchFrameRequested 分开:**这件事只发生一次**,不复用帧通道。
    void stitchFinishRequested();
    /// 长截图完成:交出拼接后的长图(含标注)。走与 annotatedConfirmed 相同的
    /// 「已合成图 → 入库 + 剪贴板」通道,故会话层不需要为它单开一条出口。
    void stitchFinished(const QImage& composed);
    /// 长截图放弃(用户点「取消」):会话应 abort 核心侧会话并收遮罩。
    /// 刻意**不复用 cancelled** —— cancelled 的语义是"取消整次截图",
    /// 而长截图的取消只需丢掉已采集的帧,两者在日志与提示上要能分开。
    void stitchAborted();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    /// 窗口尺寸变化时重算长截图的挖洞区域(见 refreshStitchHole)。
    void resizeEvent(QResizeEvent* e) override;
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

    // ---- 滚动长截图态(docs/19)----
    //
    // 三态:关闭 → 就绪(选区锁定、面板显示「开始」)→ 采集中(面板显示「结束」+ 进度条)。
    // 选区在就绪态**锁定**:它是采集窗口,中途变尺寸会让已采帧与新帧宽度不一致,
    // 拼接必然错位(docs/19 §3.3 I1)。
    bool stitchActive() const { return m_stitch != StitchState::Off; }
    /// 进入长截图态(从浮条「长截图」按钮)。已进入则不重复。
    void enterStitching();
    /// 退出长截图态(不结束会话):回到普通框选态,选区仍保留。
    void exitStitching();
    /// 开始/停止采集。返回操作后是否处于采集中。
    bool toggleStitchCapture();
    QRect stitchPanelRect() const;
    bool stitchPanelContains(const QPoint& pos) const;
    void handleStitchPanelClick(const QPoint& pos);
    void drawStitchPanel(QPainter& painter) const;

    /// 采集中宿主投递的一帧(物理像素,相对本屏)。宿主不采集时不调用。
    QRect stitchCaptureRect() const;



    /// 长截图采集时的"挖洞":把选区**设为鼠标穿透**,让滚轮/点击直达底下的真实窗口。
    ///
    /// ## 为什么必须挖洞 —— 这是长截图能否成立的前提
    ///
    /// 遮罩是铺满整屏的置顶窗口,它会**吞掉所有鼠标事件**。用户在选区上滚动时,
    /// 滚轮被遮罩接收,底下的浏览器/记事本根本不动 —— 页面不滚,采到的每一帧
    /// 都是同一张图,长截图等于不工作(docs/19 §7 R2)。
    ///
    /// ## 三种方案的取舍
    ///
    /// | 方案 | 效果 | 问题 |
    /// | --- | --- | --- |
    /// | ① 转发滚轮(SendInput 给下层) | 能滚 | 用户看不到结果 —— 遮罩上仍是冻结画面,等于**盲滚**;且要猜目标窗口 |
    /// | ② 整个遮罩 WA_TransparentForMouseEvents | 能滚 | 连「结束」按钮都点不到,无法收尾 |
    /// | ③ **选区挖洞**(本方案) | 能滚且**看得见** | 需要按选区实时更新窗口区域 |
    ///
    /// 实现走 ③:用 `SetWindowRgn` 把窗口区域设成"整屏减去选区"?不行 ——
    /// 那样选区外的遮罩也没了。正确做法反过来:采集期间把**选区那块**从窗口区域
    /// 里挖掉,再把遮罩的鼠标穿透打开;**同时**用一个独立的小窗口显示"冻结的选区
    /// 首帧"作为视觉参照,它本身也穿透。
    ///
    /// 简化实现(实际采用):采集期间只把选区**挖掉窗口区域**(下层真实窗口因此
    /// 可见、可交互),遮罩其余部分保持。用户看到的是**真实的页面**,滚动即时可见。
    /// 面板与进度条在挖洞**之外**,仍然可点。
    void applyStitchHole(bool enable);
    /// 按当前选区重算挖洞(选区在长截图态是锁定的,故只在进入/退出时调用)。
    void refreshStitchHole();

    /// 采集节拍:定时截一帧并交给会话层拼接(docs/19 §3.3 I4)。
    ///
    /// 为什么是**定时**而不是"内容变化才采":判断"内容变了"本身就要先截一帧
    /// 再比 —— 那等于每拍都截。折中是**定频轻采样 + 核心侧去重**:
    /// 核心用行指纹判 `duplicate`(一帧比对只是几行整数,远低于一次整屏捕获),
    /// 重复帧被丢弃、不占内存预算。用户滚一下就出结果,停手时也不会堆帧。
    ///
    /// 间隔取 [`kStitchSampleIntervalMs`]:太快会在 4K 上把 CPU 吃满(每次整屏
    /// 捕获 + 裁剪 ~10ms),太慢则快速滚动时会漏内容(重叠不够被判 no-overlap)。
    void startStitchSampling();
    void stopStitchSampling();

    enum class StitchState { Off, Ready, Capturing };
    StitchState m_stitch = StitchState::Off;
    QRect m_stitchPanelRect;      // 缓存(翻转/夹屏后)
    QString m_stitchNotice;       // 面板下方的一行状态文案(空 = 不显示)
    /// 框完直接进长截图就绪态(CaptureIntent::Scrolling)。见构造函数注释。
    bool m_autoStartStitching = false;
    int m_stitchTotalHeight = 0;  // 已拼接总高(物理像素;文案按 dpr 折算显示)
    int m_stitchGaps = 0;         // 累计缺口数(供宿主提示"有内容漏掉")
    bool m_stitchHoleApplied = false;  // 采集期间窗口区域已挖洞(退出时必须还原)
    /// 采集节拍定时器(只在 Capturing 态运行)。
    QTimer* m_stitchTimer = nullptr;
    /// 上一拍是否还在采集(防止定时器在合成中又投一帧)。
    bool m_stitchSampling = false;
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
