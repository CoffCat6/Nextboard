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

#include "capture/stitch_hud.hpp"
#include "editor/annotation_engine.hpp"

class QLineEdit;
class QScreen;
class QTimer;
class QJsonObject;

namespace nb {

struct OcrLineBox;   // host/core_bridge.hpp(图中取字的行数据;此处只需前向声明)

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
    /// 从已读到的设置对象里取标注偏好(颜色/粗细/字号)。
    /// **由会话层在构造后立刻调用**(每块遮罩都要),故必须是 public。
    void applyAnnotationPrefs(const QJsonObject& data);

    /// 极简档下底板仅 70% 不透明 → 普通按钮文字需更高对比度,否则"透到看不清"。
    /// **只影响"未激活"按钮的前景色**:语义色(完成/取消/激活)已由主题保证对比度。
    bool barNeedsContrastBoost() const { return m_barAlpha <= kBarAlphaMinimal; }

    /// OCR 是否正在进行(验收断言"连点不会并发"用;也供宿主置灰按钮)。
    bool ocrPending() const { return m_ocrPending; }

    /// 本遮罩所属屏幕(验收与调试用;会话层用不到,它自己持有 QScreen*)。
    QScreen* screen() const { return m_screen; }

    /// 长截图的进度更新(总高/缺口/结论)。**由会话层回调** ——
    /// 采集与拼接在会话层(它才持有屏与核心句柄),遮罩只负责显示。
    /// 之所以放 public 而非 private:会话层的 lambda 不是本类的成员。
    void updateStitchProgress(int totalHeight, int gaps, int bands, const QString& reason);

    /// 会话层在投递每一帧前,把"这一帧相对上一帧的变化度与帧间隔"交给遮罩,
    /// 供失败提示分流使用(见 updateStitchProgress 的 no-overlap 分支)。
    void setStitchFrameStats(int changedPctX100, qint64 dtMs) {
        m_stitchLastChangedPct = changedPctX100;
        m_stitchLastDtdMs = dtMs;
    }

    /// 标记"上一拍已返回",允许节拍器投下一帧。
    ///
    /// 供会话层在**所有**出口调用(含捕获失败、核心拒绝)。**必须**用 RAII 守卫
    /// 或在每个 return 前调 —— 漏一次节拍器就永久停摆:页面还在滚,长图再也不更新。
    void markSampleReturned() { m_stitchSampling = false; }

    /// 当前是否处于长截图态 / 采集中(供会话层判断,避免重复投递)。
    bool stitching() const { return m_stitch != StitchState::Off; }
    bool stitchCapturing() const { return m_stitch == StitchState::Capturing; }

    /// 采集选区在**本屏内的设备像素矩形**(相对屏左上角)。
    /// 进入采集时固化,采集期只读 —— 这是长截图唯一的坐标真值。
    QRect stitchRegionDevice() const { return m_stitchRegionDevice; }

    /// 采集期是否已隐藏遮罩(遮罩不可见时它不能画自己的面板)。
    bool stitchOverlayHidden() const { return m_stitchOverlayHidden; }

    /// 会话层在合成成功后回调本遮罩,转发 stitchFinished(信号必须由本类发出)。
    ///
    /// 为什么需要这个 public 转发:合成发生在会话层的 lambda 里(它持有核心句柄),
    /// 而 emit 只能在本类作用域内做。验收(--stitch-live-flow)靠这条信号拿到
    /// 最终长图并断言"高度 > 选区高度",否则只能从日志肉眼核对。
    void reportStitchFinished(const QImage& composed) { emit stitchFinished(composed); }

    /// 采集期由 HUD 的「结束」走**同一条**收尾路径(验收用同一入口,不另造捷径)。
    void requestStitchFinishForTest() {
        if (m_stitch == StitchState::Capturing) toggleStitchCapture();
    }

    /// 采集期隐藏/恢复本遮罩。
    ///
    /// ## 为什么要整块藏起来(docs/19 v2 §3.1)
    ///
    /// 旧实现在遮罩上"挖洞"(SetWindowRgn 把选区从窗口区域里去掉),既能看见页面
    /// 又能点面板 —— 但那条路有两个死结:
    /// - 窗口区域是**设备像素**,而 m_selection 是 Qt **逻辑像素**;150% 屏上洞会
    ///   按 1:1 挖出来,选区实际被盖住一部分 → 每帧采到的都是遮罩上的静止画面
    ///   → 核心判 duplicate → 长图只有一屏高(真机实测现象);
    /// - 选区 = 整屏时"整窗减选区"是空区域 → 遮罩整块消失,面板也一起消失。
    ///
    /// v2 改法:采集期**整个遮罩隐藏**,页面完全可见可交互;选区描边与「结束/取消」
    /// 面板由会话层的独立浮窗承担(见 capture_session.cpp 的 StitchHud)。
    void setStitchOverlayHidden(bool hidden);

signals:
    /// 选区确认:**本屏物理像素**坐标,相对该屏左上角(直接供 nb_capture_crop)。
    void regionConfirmed(const QRect& physicalRect);
    /// 带标注的确认:合成图已含标注(浮条标注态确认专用),宿主**不再裁剪**,
    /// 直接把这张图入库+剪贴板。与 regionConfirmed 互斥触发。
    void annotatedConfirmed(const QImage& composed);
    void cancelled();
    /// 用户在本遮罩上有鼠标/键盘动作 —— 会话据此重置空闲超时(S6.1)。
    void activity();

    /// 长截图:请求采集一帧。
    ///
    /// ⚠️ v2 起**不再带矩形参数**:选区在进入采集时已固化成
    /// (屏, 设备像素矩形) 并交给核心 begin;每帧都用同一份,不允许中途重算
    /// (重算就是混合 DPI 下"采到别处"的来源)。参数留空是为了让信号签名保持
    /// 简单可读:接收方从 stitchRegionDevice() 取。
    void stitchFrameRequested();
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
    void resizeEvent(QResizeEvent* e) override;
    /// 鼠标离开窗口:清掉浮条按钮悬停态(否则高亮会"卡"在最后悬停的按钮上)。
    void leaveEvent(QEvent* e) override;
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
    /// 第 index 个按钮的矩形(局部逻辑坐标)。**绘制与命中测试共用这一份** ——
    /// 各写一份算式是"点到的位置和看到的字对不上"的老来源。
    QRect toolbarButtonRect(int index) const;
    /// 按钮下标(命中不到返回 -1)。悬停高亮与点击分发共用。
    int toolbarButtonAt(const QPoint& pos) const;
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
    /// 面板「取消」:丢会话 + 退出长截图态(选区保留)。
    void abortStitchFromPanel();

    /// 采集中宿主投递的一帧(物理像素,相对本屏)。宿主不采集时不调用。
    QRect stitchCaptureRect() const;





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
    QRect m_stitchPanelRect;      // 就绪态面板缓存(翻转/夹屏后)
    QString m_stitchNotice;       // 面板上的一行状态文案(空 = 不显示)
    /// 框完直接进长截图就绪态(CaptureIntent::Scrolling)。见构造函数注释。
    bool m_autoStartStitching = false;
    int m_stitchTotalHeight = 0;  // 已拼接总高(设备像素)
    int m_stitchGaps = 0;         // 累计缺口数(供宿主提示"有内容漏掉")
    int m_stitchBands = 0;        // 已插入产物的警示条数
    /// 采集期固化的选区(本屏设备像素,相对屏左上角)。采集期只读。
    QRect m_stitchRegionDevice;
    /// 采集期遮罩是否已隐藏(隐藏后自己不画面板,面板由 HUD 承担)。
    bool m_stitchOverlayHidden = false;
    /// 采集期 UI(选区描边 + 结束/取消面板)。采集结束/取消时销毁。
    QPointer<StitchHud> m_stitchHud;
    /// 最近一帧的变化度(0.01% 为单位)与帧间隔(ms);-1 = 未知。
    /// 仅用于把 no-overlap 的提示分流到真实原因,不参与拼接判定。
    int m_stitchLastChangedPct = -1;
    qint64 m_stitchLastDtdMs = -1;
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
    // 标注偏好:初值与 core settings 的默认值一致(default_annotation_*),
    // 真正生效的值在 startCaptureSession 里从设置覆盖(见 loadAnnotationPrefs)。
    int m_penSize = 4;                       // 二级条选中的粗细档位
    int m_fontSize = 36;                     // 二级条选中的字号
    EditorTool m_annoTool = EditorTool::Pen; // 当前标注工具
    QColor m_annoColor = QColor(0xE0, 0x31, 0x31);
    bool m_draggingStroke = false;           // 标注笔画拖拽中
    /// 把当前标注偏好写回设置(与 cycleBarAlpha 同一口吻:改了即记,不点确定)。
    /// 只在**用户主动改变**时调用(颜色/粗细/字号),不在读取时写。
    void saveAnnotationPrefs();


    int m_barAlpha = kBarAlphaHalf;   // 浮条底板不透明度(默认半透;设置可覆盖)
    QRect m_toolbarRect;        // 当前浮条几何(缓存,翻转后更新)
    /// 鼠标悬停的浮条按钮下标(-1 = 无)。只影响高亮,不参与命中判定。
    int m_hoverButton = -1;
    /// 悬停变化时只重绘浮条及其上方一条边(整屏重绘在 4K 上很吃帧)。
    void setHoverButton(int index);

    /// 悬停**停留**后才弹的按钮说明(见 cpp 里 kHoverTipDelayMs 的理由)。
    /// 自绘浮条拿不到 QHelpEvent(QToolTip 事件),所以提示得自己起表。
    QTimer* m_hoverTipTimer = nullptr;
    int m_hoverTipShownFor = -1;   // 已经为哪个按钮弹过提示(避免重复弹)
    /// 按钮的说明文案(含快捷键)。空串 = 该按钮无说明。
    QString toolbarButtonTip(int index) const;
    void scheduleHoverTip(int index);
    void hideHoverTip();
    /// OCR 正在识别(非空 = 进行中)。用途:**防重入** —— 识别是 0.4~5s 的异步任务,
    /// 连点 N 次会并发起 N 个识别,每个完成还会各自 emit cancelled()
    /// (表现为"点一次取字,截图会话被取消好几次")。同时用于按钮置灰。
    bool m_ocrPending = false;
    /// 弹出识别结果驻留面板(与编辑器共用 ui/ocr_result_panel)。
    /// **必须用顶层窗口**:本方法调用后会话立即 cancelled → 遮罩销毁,
    /// 子控件会跟着消失(见 ocr_result_panel.hpp 的说明)。
    /// @param source 被识别的原图(选区图),行框高亮的底图。
    /// @param lines 每行识别结果及其在 source 中的包围盒(图中取字);
    ///        可为空(引擎没给出框时,面板降级为纯文本)。
    /// @param anchorGlobal 面板定位基准 = 选区在桌面上的全局矩形。
    ///        由调用方在**点击那一刻**算好传进来,而不是在这里现算 ——
    ///        回调发生在识别完成后,那时遮罩可能已在关闭链路上,窗口几何不再可靠。
    void showOcrResultPanel(const QString& text, const QString& reason,
                            const QImage& source, const QRect& anchorGlobal,
                            const QVector<OcrLineBox>& lines = {});
};

} // namespace nb
