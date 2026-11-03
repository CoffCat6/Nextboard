// 智能框选:会话开始前枚举"可预选的目标"(顶层窗口 / UIA 元素),让遮罩上的鼠标
// 移动能自动框住"光标下的那块内容",单击即采纳。
//
// ## 为什么单独成文件
//
// 1) **坐标**:遮罩上原本已有三套坐标系(逻辑像素 / 桌面绝对物理像素 / 选区局部
//    物理像素)。智能框选引入**第四套** —— Win32 与 UIA 给的都是"虚拟桌面绝对
//    物理像素"。历史上两次"标注画不准"全是第四套这种"换算散写"导致的,故这里
//    把 4→1 的换算固定成**纯函数**,与 overlay_geometry 同一口径、可被验收直接断言。
// 2) **隔离 Win32/COM**:capture_session.cpp 已 2300 行,把窗口枚举与 UIA 塞进去
//    会让"框选状态机"彻底不可读;而枚举本身是可单测的纯数据生产。
//
// ## 坐标口径(务必与 overlay_geometry.hpp 头注释对照读)
//
//   Candidate.physical = **虚拟桌面绝对物理像素**(Win32 RECT 原样,可为负坐标 ——
//   多屏时左/上方的屏是负的);
//   OverlayWidget 内部仍是局部逻辑像素;换算由 toLocalLogical 承担。
//
// ## 降级契约
//
// 任何一步失败(枚举为空 / COM 未就绪 / UIA 超时)**都返回空表**,不报错、不阻塞
// 会话:智能框选是**增益**能力,拿不到候选时必须退化成原有的纯手动框选,
// 绝不能因此让截图用不了。
#pragma once

#include <QRect>
#include <QString>
#include <QVector>

class QImage;
class QScreen;

namespace nb::smart {

/// 一个可预选的目标。
///
/// 数组顺序即 **z 序(从顶到底)**:probeWindows 返回时已排好,命中测试按序取
/// 第一个包含光标的候选 —— 这正是"点在上面那个窗口就选上面那个"的直觉。
struct Candidate {
    /// 目标在**虚拟桌面绝对物理像素**下的矩形(Win32 RECT 原样)。
    /// 多屏时可能为负(屏在主的左上);零面积表示不可用(已被枚举剔除)。
    QRect physical;
    /// 显示用名称:窗口标题 / UIA 元素的 Name(可能为空)。
    QString title;
    /// 稳定标识:窗口 = HWND;元素 = UIA RuntimeId 的哈希。
    quintptr id = 0;
    /// 所属顶层窗口的 HWND(**元素的层级归属**,窗口自己填自己的 HWND)。
    quintptr windowId = 0;
    /// 0 = 顶层窗口;>=1 = 该窗口内部元素(仅 UIA 阶段产出)。
    int level = 0;
};

/// 枚举当前屏幕上所有可预选的**顶层窗口**(z 序从顶到底)。
///
/// ⚠️ **必须在遮罩窗口建立之前调用**(startCaptureSession 里选的是
/// hideAllForCapture() 之后、new OverlayWidget 之前):遮罩一旦全屏置顶,
/// 枚举结果里就全是它自己。
///
/// 已剔除:本进程窗口、不可见、最小化、DWM cloaked(UWP 幽灵窗口)、零面积、
/// 桌面/任务栏/输入法/菜单等 shell 类。失败返回空表。
QVector<Candidate> probeWindows();

/// 屏幕在**虚拟桌面物理坐标系**下的矩形(原点 = 物理原点,尺寸 = 底图像素)。
///
/// 这是 4→1 换算的基准。两个参数各取一半,是刻意的:
/// - **原点**取 QScreen(项目已实测:Qt 在 Windows 手下发原点,与核心
///   `capture.rs` 的 match_monitor 同源,混合 DPI 下也是物理值);
/// - **尺寸**取冻结底图(底图必是物理像素,而 QScreen::geometry().size() 是
///   **逻辑像素**,直接用它会在 150% 屏上把候选缩小三分之一)。
QSize screenPhysicalSize(QScreen* screen, const QImage& frozen);
QPoint screenPhysicalOrigin(QScreen* screen);
QRect screenPhysicalRect(QScreen* screen, const QImage& frozen);

/// 绝对物理矩形 → 本屏**局部物理**矩形(原点 = 本屏左上)。
///
/// 这是 `selectionPhysical()` 的契约口径:核心 `nb_capture_crop` 吃的本屏
/// 底图,矩形必须相对它的左上角。候选在悬停时就要换算并存进
/// `m_lastCandidatePhysical` —— 采纳时原样交回,**不能**等到采纳才换算:
/// 那样存下的是"虚拟桌面绝对"物理,非主屏(原点非 (0,0))上会让裁剪完全越界,
/// 表现为"非主屏智能框选截屏后既不入库也不进剪贴板"。
///
/// 越界部分裁掉(跨屏窗口只取本屏内那块);完全在屏外返回空矩形。纯函数,可单测。
QRect toLocalPhysical(const QRect& globalPhysical, const QRect& screenPhysical);

/// 绝对物理矩形 → 本屏**局部逻辑**矩形(遮罩的命中与绘制都用它)。
///
/// 越出本屏的部分被裁掉;完全在屏外返回空矩形。纯函数,可单测。
QRect toLocalLogical(const QRect& globalPhysical, const QRect& screenPhysical, qreal dpr);

/// 从候选表里挑出"光标所在的那一个",返回其索引(无命中返回 -1)。
///
/// @param cursorLocalLogical 光标的**遮罩局部逻辑**坐标;
/// @param sizeFilter        候选必须至少这么大(逻辑像素)才算数 —— 用来滤掉
///                          任务栏按钮、状态栏这类"框了也没意义"的碎片。
/// @param levelOnly         只接受该层级的候选(-1 = 不限)。层级切换用它。
int candidateAt(const QVector<Candidate>& list, const QRect& screenPhysical,
                const QPoint& cursorLocalLogical, qreal dpr, int minSize = 24,
                int levelOnly = -1);

/// 光标位置上的**重叠栈**(按 z 序从顶到底,同一 level 的全部命中)。
///
/// 用途:滚轮在重叠窗口之间切换。返回的是**索引**,便于调用方在原表里取。
QVector<int> candidatesAt(const QVector<Candidate>& list, const QRect& screenPhysical,
                          const QPoint& cursorLocalLogical, qreal dpr, int minSize = 24);

// ---- UIA 元素级(S3)----

/// 一次元素探测的结果统计(验收与排障用;产品路径不读)。
/// 为什么要有它:**"元素级没生效"这个失效是静默的** —— 预选照常出现,只是永远
/// 等于整窗。没有 depth/visited 就无法区分"目标应用不给树"与"我们停在第 1 层"。
/// 这正是 docs/26 记录的事故:诊断工具与主算法犯了同一个错(每层只认一个兄弟),
/// 于是把自身的缺陷写成了 Chromium 的限制。
struct ProbeStats {
    /// 命中元素的深度(0 = 除了根什么都没命中)。
    int depth = 0;
    /// 遍历过的节点数(预算的度量;按**节点**计数,不是"每层子节点数之和")。
    int visited = 0;
    /// 实际耗时(ms)。
    int elapsedMs = 0;
    /// 0 = 第一发(ControlView);1 = 退到第二发(RawView)。
    int viewUsed = 0;
    /// 是否找到了"明显小于窗口"的元素(false = 退化为窗口级)。
    bool refined = false;
};

/// 一个窗口的**元素矩形快照**(docs/27:把"每次悬停重探"改成"进窗预取 + 查表")。
///
/// ## 为什么需要这个
///
/// 实测(2026-11-02,Chrome 156 / VSCodium):一次全树 DFS 要 **77~136ms**,而
/// 悬停到下一个位置时这些节点**一个都没变** —— 窗口内容在框选期间是冻结的
/// (底图都冻结了,视觉上也不会变)。于是"每动一次鼠标重探一次"是把同一个答案
/// 反复算:同一窗口内移动 10 次 = 1.13s 纯等待。
///
/// 改成"窗口进入时后台建一次快照,之后每次悬停只做内存命中测试":实测查表
/// **0.33ms/次**,比重新探测快 340 倍。
struct ElementNode {
    /// 绝对物理像素(与 Candidate.physical 同口径)。
    QRect physical;
    /// 显示用名称(UIA Name,可能为空)。
    QString title;
    /// 在 nodes 数组里的父索引(-1 = 顶层)。用途:滚轮在"窗口→区块→段落"
    /// 层级链上移动时要知道谁是谁的祖先(docs/26 §4 S5)。
    int parent = -1;
    /// 深度(1 = 窗口的直接子元素,与 ProbeStats.depth 同口径)。
    int depth = 0;
};

/// 一次性抓取 windowId 的整棵元素树(ControlView),供之后无数次查表。
///
/// ⚠️ **阻塞调用(实测 5~140ms),绝不能在 UI 线程调用**。
///
/// 与 probeElementAt 的分工:
///   * probeElementAt  = 单点探测,一次只回答"这个坐标下是什么"(贵,且会被重复问);
///   * buildElementSnapshot = 整窗一次性抓全,之后 questionAt() 是纯内存操作。
///
/// 快照**不是永久的**:窗口内容变化(网页滚动、切标签)后必须重建。
/// 调用方按 (windowId, 屏幕) 缓存并在失效时丢弃,见 capture_session 的
/// m_elementSnapshots。
///
/// @param windowId 目标顶层窗口 HWND;
/// @param maxNodes 节点上限(防御浏览器整树几万节点);
/// @param budgetMs 时间预算,超时返回已抓到的部分(仍是可用的粗结果);
/// @param stats   可选输出。
/// @return 抓到 0 个节点 = 该窗口没有可用树(合法的降级情形,不是错误)。
QVector<ElementNode> buildElementSnapshot(quintptr windowId, int maxNodes = 6000,
                                          int budgetMs = 400, ProbeStats* stats = nullptr);

/// 在**快照**里查"光标处最深且明显小于窗口的那个元素"(纯内存,无 COM)。
///
/// @param nodes        buildElementSnapshot 的产物;
/// @param winPhysical  窗口自身矩形(用于判断"是否明显小于窗口");
/// @param cursor       光标绝对物理坐标;
/// @param minSize      元素最小物理边长;
/// @param allowEqualWin 是否允许返回"与窗口等大"的节点(默认 false:
///                     等于窗口就是没细化)。
/// @return 找到的元素索引(-1 = 没有命中);返回索引而非拷贝,便于调用方
///         顺着 parent 往上走(层级链)。
int elementAtSnapshot(const QVector<ElementNode>& nodes, const QRect& winPhysical,
                      const QPoint& cursor, int minSize, bool allowEqualWin = false);


/// 该目标窗口是否值得做 UIA 元素探测。
///
/// 不是所有窗口都值得:浏览器/IDE/Office 有完整 UIA 树,而游戏、自绘控件返回的
/// 树要么空、要么只有一个根 —— 对它们探测只是白白花掉几十毫秒。
bool worthProbing(quintptr windowId);

/// 在**光标处**探测 windowId 内部最小的 UIA 元素,追加进 out(level=1)。
///
/// ⚠️ **阻塞调用(几十~几百 ms),绝不能在 UI 线程调用**。设计上由遮罩在
/// "鼠标停稳 400ms"时投到工作线程,完成后回 UI 线程合并。
///
/// ## 算法:受预算约束的 DFS(2026-11-02 重写,见 docs/26)
///
/// 原实现是"每层只挑**一个**子节点下沉"(同层多个命中取面积最小)。这在 Chromium
/// 上 100% 失效:Chrome 的顶层恰好是**两个同尺寸 Pane**,`[0]` 是空壳(0 子节点)、
/// `[1]` 才是网页内容 —— "面积最小"必然选中空壳,于是永远停在第 1 层(整窗)。
/// 症状极具迷惑性:功能"看起来接上了",只是元素级永远等于窗口级。
///
/// 现在改成:同层**所有**包含光标的子节点都入栈,逐层记录"最深且明显小于窗口"
/// 的那一个。实测同一位置从 depth=1 到 depth=9~11(Chromium)/ 20(Telegram Web)。
///
/// ## 两级视图(S2)
///
/// 第一发 ControlView(快、语义干净),结果**等于窗口**时才补一发 RawView
/// (更细但节点多)。大多数窗口第一发就够,不必为少数派拖慢全体。
///
/// @param cursorGlobalPhysical 光标的**虚拟桌面绝对物理**坐标;
/// @param minSize              元素最小逻辑边长,过滤无意义的碎片;
/// @param maxNodes             遍历节点上限(防止浏览器整树几万节点卡死);
/// @param budgetMs             时间预算,超时即返回已有结果;
/// @param stats                可选输出:深度/节点数/耗时/视图(验收与排障用)。
/// @return 找到的元素(最多一个);未找到/失败返回空。
QVector<Candidate> probeElementAt(quintptr windowId, const QPoint& cursorGlobalPhysical,
                                  qreal dpr, int minSize = 24, int maxNodes = 4000,
                                  int budgetMs = 500, ProbeStats* stats = nullptr);

/// **诊断专用**:把 windowId 的 UIA 树逐层打出来(前 depth 层,每层前 maxPerLevel 个)。
///
/// 为什么要有它:元素级"没生效"的症状是**静默的** —— 预选仍然正常,只是永远
/// 框住整个窗口。没有树结构就无从判断"是目标应用不提供子树"还是"我们自己下沉
/// 到一半就停了"。验收(--smart-uia-flow)用它留痕;产品路径不调用。
///
/// ⚠️ 每层必须打印**全部**子元素(受 maxPerLevel 限)并标注空壳 —— 原实现只沿
///    第 0 个孩子继续下沉,而 Chromium 的第 0 个孩子正是空壳,于是诊断结论
///    "L1 n=0" 与主算法的同一个错误互相印证,把缺陷写成了目标应用的限制
///    (docs/26 §2.5)。诊断工具的错误比算法的错误代价更高:它会误导归因。
///
/// @return 每层一行:"L<depth> n=<子元素数> [..] …"
QStringList dumpElementTree(quintptr windowId, int depth = 4, int maxPerLevel = 6);

} // namespace nb::smart