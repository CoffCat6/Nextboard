#include "capture/smart_capture.hpp"

#include <QDebug>
#include <QHash>
#include <limits>
#include <algorithm>
#include <QElapsedTimer>
#include <QImage>
#include <QScreen>
#include <QStringList>

#if defined(Q_OS_WIN)
#include <dwmapi.h>
#include <windows.h>

// UIA 元素级(S3)。只在 probeElementAt 里用到;窗口级(S1)完全不碰 COM ——
// 这样"元素探测失败/超时"永远不会影响"至少还能按窗口框"。
#include <UIAutomation.h>
#else
// Linux(M3):窗口级枚举用 Xlib(EWMH)。GUI 线程一次性短连接;热键线程
// 另有自己的连接。Xlib 多线程约定:XInitThreads 必须先于任何 Xlib 调用 ——
// Qt xcb 平台插件启动时已调,此处幂等重入只作显式声明。
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <unistd.h>
#endif

namespace nb::smart {

// Windows 段一:窗口枚举助手(isShellClass/可见矩形/cloaked/EnumCtx/ComGuard)
// 与 probeWindows —— 全部依赖 Win32/DWM/COM,Linux 走文件尾的降级桩。
#if defined(Q_OS_WIN)
namespace {

/// 候选窗口的最小边长(物理像素)。
///
/// 取 60:实测托盘残片是 24x24,而正常应用窗口最小也有 100+。取 48 会漏进
/// 一部分 32x32 的状态指示窗,取 80 又会吃掉小工具窗(如计算器 260x180 没问题,
/// 但某些便签类小窗只有 70 宽)。
constexpr int kMinCandidateEdge = 60;

/// 候选窗口允许的最大宽高比(max(w/h, h/w))。
///
/// 取 12:任务栏变体实测是 3072x70(43.9)与 1920x40(48),正常窗口极少超过 12。
/// 极窄长窗口(比如某些工具的侧边栏)确实存在,但框住它的价值远低于"悬停在
/// 任务栏上却框住一整条"的干扰。
constexpr qreal kMaxCandidateAspect = 12.0;

/// 我们不希望被当成"可截图目标"的 shell 类名。
///
/// 判据是"框住它没有意义":桌面/任务栏/输入法候选窗/Alt-Tab 覆盖层这类要么
/// 铺满全屏(等于全屏截图)、要么是转瞬即逝的 UI 装饰。**刻意不用
/// WS_EX_TOOLWINDOW / WS_EX_NOACTIVATE 过滤** —— 大量正常应用窗口(含本项目的
/// 贴图)带着这些标志,按标志过滤会误杀。
bool isShellClass(const QString& cls) {
    static const QStringList kDeny = {
        QStringLiteral("Progman"),                  // 桌面
        QStringLiteral("WorkerW"),                  // 桌面(壁纸宿主)
        QStringLiteral("Shell_TrayWnd"),            // 任务栏
        QStringLiteral("Shell_SecondaryTrayWnd"),   // 副屏任务栏
        QStringLiteral("ApplicationManager_DesktopShellWindow"),
        QStringLiteral("MultitaskingViewFrame"),    // 任务视图
        QStringLiteral("TaskListThumbnailWnd"),     // 任务栏预览缩略图
        QStringLiteral("XamlExplorerHostIslandWindow"),  // Alt-Tab / 开始菜单宿主
        QStringLiteral("ForegroundStaging"),
        QStringLiteral("SysShadow"),                // 菜单/提示的投影层
        QStringLiteral("IME"),                      // 输入法候选
        QStringLiteral("MSCTFIME UI"),
        QStringLiteral("EdgeUiInputTopWndClass"),
    };
    return kDeny.contains(cls);
}

/// 窗口的**可见**边界(物理像素)。
///
/// 优先 DWM 的 extended frame bounds:Win10 起 GetWindowRect 会把那圈**不可见**
/// 的 7~8px 拖拽边框算进去,照它裁出来的图四周带一条桌面残影。DWM 不可用
/// (Win7 / 某些无 DWM 会话)时回退 GetWindowRect。
QRect windowVisibleRect(HWND hwnd) {
    RECT rc{};
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc,
                                          sizeof(rc)))) {
        const QRect r(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
        if (!r.isEmpty()) return r;
    }
    if (::GetWindowRect(hwnd, &rc)) {
        return QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    }
    return {};
}

/// 窗口是否被 DWM 隐藏(UWP 的"幽灵窗口":IsWindowVisible 为真但屏幕上看不见)。
bool isCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

QString windowClass(HWND hwnd) {
    wchar_t buf[256] = {0};
    const int n = ::GetClassNameW(hwnd, buf, 255);
    if (n <= 0) return {};
    return QString::fromWCharArray(buf, n);
}

QString windowTitle(HWND hwnd) {
    wchar_t buf[512] = {0};
    const int n = ::GetWindowTextW(hwnd, buf, 511);
    if (n <= 0) return {};
    return QString::fromWCharArray(buf, n);
}

struct EnumCtx {
    QVector<Candidate>* out = nullptr;
    DWORD selfPid = 0;
};

BOOL CALLBACK enumTopLevel(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumCtx*>(param);
    if (!ctx || !ctx->out) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->selfPid) return TRUE;              // 本进程窗口(遮罩/面板/贴图/便签)

    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (::IsIconic(hwnd)) return TRUE;                 // 最小化:屏幕上没有它的内容
    if (isCloaked(hwnd)) return TRUE;
    if (isShellClass(windowClass(hwnd))) return TRUE;

    const QRect rect = windowVisibleRect(hwnd);
    if (rect.width() < 8 || rect.height() < 8) return TRUE;

    // ---- 碎片过滤(实测数据驱动,docs/23 §4.4)----
    //
    // 真机枚举出来的噪声有两类,框住它们**没有意义**且会挡住真正想框的窗口:
    //   ① 托盘/输入法残片:24x24 的无标题小窗(实测 SwitchHosts / Pilo 的
    //      "com.xxx-siw" 都属此类),它们常驻 z 序最上层;
    //   ② 屏幕边缘条:形如 3072x70 / 1920x40 的无标题横条(任务栏变体/挂件)。
    //
    // 判据用"最小边长 + 宽高比"而不是"有没有标题":无标题窗口里有正常应用
    // (自绘 UI 的客户端很常见),按标题过滤会误杀一大片。
    if (rect.width() < kMinCandidateEdge || rect.height() < kMinCandidateEdge) return TRUE;
    const qreal ratio = static_cast<qreal>(rect.width()) / qMax(1, rect.height());
    if (ratio > kMaxCandidateAspect || ratio < 1.0 / kMaxCandidateAspect) return TRUE;

    Candidate c;
    c.physical = rect;
    c.title = windowTitle(hwnd);
    c.id = reinterpret_cast<quintptr>(hwnd);
    c.windowId = c.id;
    c.level = 0;
    ctx->out->append(c);
    return TRUE;
}

/// UIA 的 RAII 包裹:无论从哪个 return 出去都 Release,且只负责 COM 的初/终。
template <typename T>
void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct ComGuard {
    bool initialized = false;
    ComGuard() {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // 调用线程已是 STA(例如被某处 CoInitialize 过):按 STA 再试。
            hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        }
        initialized = SUCCEEDED(hr);
    }
    ~ComGuard() { if (initialized) ::CoUninitialize(); }
    ComGuard(const ComGuard&) = delete;
    ComGuard& operator=(const ComGuard&) = delete;
};

} // namespace

QVector<Candidate> probeWindows() {
    QVector<Candidate> out;
    EnumCtx ctx;
    ctx.out = &out;
    ctx.selfPid = ::GetCurrentProcessId();
    ::EnumWindows(&enumTopLevel, reinterpret_cast<LPARAM>(&ctx));
    // EnumWindows 本身就是 z 序(顶层在前),正是命中测试想要的顺序。
    return out;
}
#endif  // Q_OS_WIN(段一:枚举助手 + probeWindows)

QSize screenPhysicalSize(QScreen* screen, const QImage& frozen) {
    if (!frozen.isNull()) return frozen.size();
    if (screen) return screen->geometry().size() * screen->devicePixelRatio();
    return {};
}

QPoint screenPhysicalOrigin(QScreen* screen) {
    if (!screen) return {};
    return screen->geometry().topLeft();
}

QRect screenPhysicalRect(QScreen* screen, const QImage& frozen) {
    return QRect(screenPhysicalOrigin(screen), screenPhysicalSize(screen, frozen));
}

QRect toLocalPhysical(const QRect& globalPhysical, const QRect& screenPhysical) {
    if (globalPhysical.isEmpty() || screenPhysical.isEmpty()) return {};
    const QRect rel = globalPhysical.translated(-screenPhysical.topLeft());
    const QRect clamped = rel.intersected(QRect(QPoint(0, 0), screenPhysical.size()));
    if (clamped.isEmpty()) return {};
    return clamped;
}

QRect toLocalLogical(const QRect& globalPhysical, const QRect& screenPhysical, qreal dpr) {
    if (globalPhysical.isEmpty() || screenPhysical.isEmpty() || dpr <= 0.01) return {};
    const QRect rel = globalPhysical.translated(-screenPhysical.topLeft());
    const QRect clamped = rel.intersected(QRect(QPoint(0, 0), screenPhysical.size()));
    if (clamped.isEmpty()) return {};
    // 四舍五入而不是截断:与 selectionPhysical() 的 toAlignedRect 口径靠近,
    // 来回换算的误差恒定在 1px 内(截断会随 dpr 累积成"框小一圈")。
    return QRect(QPoint(qRound(clamped.x() / dpr), qRound(clamped.y() / dpr)),
                 QSize(qRound(clamped.width() / dpr), qRound(clamped.height() / dpr)));
}

// candidatesAt 的定义在本函数之后(candidateAt 复用它的排序),先声明。
QVector<int> candidatesAt(const QVector<Candidate>& list, const QRect& screenPhysical,
                          const QPoint& cursorLocalLogical, qreal dpr, int minSize);

int candidateAt(const QVector<Candidate>& list, const QRect& screenPhysical,
                const QPoint& cursorLocalLogical, qreal dpr, int minSize, int levelOnly) {
    // 与 candidatesAt 同一度量:先找**前台**窗口,再看它内部的元素。
    // 直接顺序扫会命中后台窗口的元素 —— 正是"全屏应用占住后再框不出别的"。
    const QVector<int> ordered =
        candidatesAt(list, screenPhysical, cursorLocalLogical, dpr, minSize);
    if (ordered.isEmpty()) return -1;
    if (levelOnly < 0) return ordered.first();
    for (int idx : ordered) {
        if (list.at(idx).level == levelOnly) return idx;
    }
    return -1;
}

QVector<int> candidatesAt(const QVector<Candidate>& list, const QRect& screenPhysical,
                          const QPoint& cursorLocalLogical, qreal dpr, int minSize) {
    QVector<int> hits;
    for (int i = 0; i < list.size(); ++i) {
        const QRect r = toLocalLogical(list[i].physical, screenPhysical, dpr);
        if (r.isEmpty()) continue;
        if (r.width() < minSize || r.height() < minSize) continue;
        if (cursorLocalLogical.x() >= r.left() && cursorLocalLogical.x() <= r.right() &&
            cursorLocalLogical.y() >= r.top() && cursorLocalLogical.y() <= r.bottom()) {
            hits.append(i);
        }
    }
    // ---- 排序:先按 z 序的**属主窗口**,再在窗口内部让元素优先 ----
    //
    // 第一版写成"level 降序"就完了,结果是**后台窗口的元素压过了前台窗口**:
    // 全屏应用的 UIA 根元素会覆盖整个屏幕,于是"先框到全屏应用,再框另一个
    // 非全屏应用就框不出来"(用户 2026-10-26 实测反馈)。
    //
    // 正确的度量是**所属窗口的 z 序**,不是元素自身的 level:
    //   * 一个元素属于哪个窗口(候选的 windowId)决定它"在哪一层";
    //   * 窗口的 z 序 = 该窗口在候选表里的位置(枚举顺序,顶层在前);
    //   * 只有**同一个窗口内部**,元素才优先于窗口本身。
    //
    // 于是:光标压着小窗口时,即使后台全屏窗口有一个覆盖全屏的元素,小窗口
    // (z 序更前)仍然胜出 —— 这正是用户的直觉。
    if (hits.size() > 1) {
        // 窗口 z 序:windowId → 该窗口在候选表里的最小下标(level==0 的那条)。
        // 若某元素的窗口不在表里(理论上不会),退化为"排到最后"。
        QHash<quintptr, int> windowZ;
        for (int i = 0; i < list.size(); ++i) {
            if (list.at(i).level == 0) windowZ.insert(list.at(i).windowId, i);
        }
        // ⚠️ max() 在 <windows.h> 之后是**宏**(windef.h),必须加括号才能取到
        //    std::numeric_limits 的成员 —— 否则报一堆莫名其妙的语法错。
        const int kNoWindow = (std::numeric_limits<int>::max)();
        const auto zOf = [&list, &windowZ, kNoWindow](int idx) {
            const quintptr win = list.at(idx).windowId;
            return win ? windowZ.value(win, kNoWindow) : kNoWindow;
        };
        std::stable_sort(hits.begin(), hits.end(), [&list, &zOf](int a, int b) {
            const int za = zOf(a);
            const int zb = zOf(b);
            if (za != zb) return za < zb;                    // 前台窗口优先
            return list.at(a).level > list.at(b).level;      // 同窗口内元素更细
        });
    }
    return hits;
}

// Windows 段二:元素级 UIA(worthProbing / probeElementAt / dumpElementTree /
// buildElementSnapshot)—— 依赖 COM/CUIAutomation。
#if defined(Q_OS_WIN)
bool worthProbing(quintptr windowId) {
    if (!windowId) return false;
    // 注意:这里**只按类名排除已知无 UIA 树的实现**,不做"是否响应"的探测 ——
    // 真探测一次的要价就是几十毫秒,而探测本身就该在工作线程里发生。
    const QString cls = windowClass(reinterpret_cast<HWND>(windowId));
    if (cls.isEmpty()) return false;
    // 已知"UIA 树为空或只有根"的实现:探测它们只是白花几十毫秒。
    static const QStringList kDeny = {
        QStringLiteral("UnityWndClass"),      // Unity 游戏
        QStringLiteral("UnrealWindow"),       // UE 游戏
        QStringLiteral("CryENGINE"),          // CryEngine
        QStringLiteral("GameMaker"),          // GameMaker
        QStringLiteral("SDL_app"),            // SDL
        QStringLiteral("LWJGL"),              // Java 游戏
        QStringLiteral("ConsoleWindowClass"), // 控制台(元素无意义,窗口本身已够)
    };
    return !kDeny.contains(cls);
}


namespace {

/// 探测预算(docs/26 §4 S1)。**实测驱动**,别按直觉调小。
///
/// 依据(2026-11-02,Chrome 156 / Edge 156 / 三屏混合 DPI):
///   * ControlView DFS:Chromium 探针页 21~30ms / Telegram Web 136ms;
///   * RawView DFS:41~56ms / 108ms;
///   * 旧值 350ms 在"每个命中兄弟都展开"之后余量不够:一次真实页面的
///     RawView 深挖就要 100ms+,而超时是**硬截断**(直接返回当前 best,
///     可能停在很粗的一层)。
///
/// 对外的默认值在头文件(probeElementAt 的默认实参),这里只放内部常量。

/// 最大深度。实测最深 20(Telegram Web 的 RawView),再往下是文本 run,
/// 框住单个字符没有意义。
constexpr int kProbeMaxDepth = 20;
#endif  // 段二常量(仅 Windows UIA 用)

// "明显小于窗口"的判据(物理像素)。差得比这还小就等于没细化:
// Chromium 的外壳 Pane 常只比窗口小 2px(2077x1388 vs 2078x1390),
// 旧实现用严格 `>=` 判"与窗口等大",恰好漏掉这种。
// ⚠️ 放在平台门控外:elementAtSnapshot(跨平台纯函数)也用它。
constexpr int kMinShrinkPhys = 8;

#if defined(Q_OS_WIN)
/// 一次 DFS 的产出。
struct DfsResult {
    QRect best;          // 最深且明显小于窗口的命中(绝对物理像素)
    QString bestName;
    int depth = 0;       // best 的深度(0 = 什么都没找到)
    int visited = 0;     // 访问过的节点数
};

/// 沿 `walker` 做受预算约束的 DFS,返回**最深且明显小于窗口**的、包含光标的元素。
///
/// ## 为什么不是"每层只挑一个兄弟"(原实现)
///
/// Chromium 顶层是两个**同尺寸** Pane:`[0]` 空壳(0 子节点)、`[1]` 才是网页内容。
/// 原实现在同层多个命中里取"面积最小" —— 而 `[0]` 恰好小 2px,必然选中空壳,
/// 于是永远停在第 1 层,元素级恒等于窗口级。症状极具迷惑性:功能"看起来接上了"。
/// 实测(2026-11-02):改成同层全展开后,同一位置从 depth=1 到 depth=9~11
/// (Chromium 探针页)/ 20(Telegram Web)。
///
/// ## 为什么不是"整树 FindAll"
///
/// 浏览器 raw 视图几万节点,一次 FindAll 就能卡几百毫秒。DFS + 预算截断
/// 既保证深度,又把代价压在实测 20~140ms。
DfsResult dfsDeepest(IUIAutomationTreeWalker* walker, IUIAutomationElement* root,
                     const QRect& winRect, const QPoint& cursor, qint64 minPhysical,
                     int maxNodes, int maxDepth, int budgetMs) {
    DfsResult r;
    if (!walker || !root) return r;
    QElapsedTimer timer;
    timer.start();

    // 所有权规则(全局唯一一条,别在循环里各写一套):
    //   * QVector<Item> stack 里的每个元素**持有 1 份引用**,出栈即 Release;
    //   * 本层收集到的 kids 数组**持有 1 份引用**,本层处理完统一 Release;
    //   * 要下沉时先 AddRef 再 append —— 栈与 kids 各持一份,互不干扰。
    // 这样 GetNextSiblingElement 永远在元素仍有效时调用,不会 use-after-release
    // (第一版在这里踩过:入栈转移所有权后再拿它取兄弟)。
    struct Item {
        IUIAutomationElement* node;
        int depth;
    };
    QVector<Item> stack;
    root->AddRef();                 // 调用方仍持有它自己的引用,这里多拿一份
    stack.append({root, 0});

    while (!stack.isEmpty()) {
        if (timer.elapsed() > budgetMs || r.visited > maxNodes) break;
        const Item cur = stack.takeLast();
        if (cur.depth >= maxDepth) {
            IUIAutomationElement* n = cur.node;
            release(n);
            continue;
        }

        // ---- 先把本节点的子元素全部收集起来,再统一判断 ----
        QVector<IUIAutomationElement*> kids;
        IUIAutomationElement* child = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(cur.node, &child)) && child) {
            while (child) {
                kids.append(child);
                IUIAutomationElement* sib = nullptr;
                if (FAILED(walker->GetNextSiblingElement(child, &sib))) sib = nullptr;
                child = sib;
            }
        }
        {
            IUIAutomationElement* n = cur.node;
            release(n);
        }

        for (IUIAutomationElement* kid : kids) {
            if (timer.elapsed() > budgetMs || r.visited > maxNodes) break;
            ++r.visited;

            RECT rc{};
            const bool ok = SUCCEEDED(kid->get_CurrentBoundingRectangle(&rc)) &&
                            rc.right > rc.left && rc.bottom > rc.top;
            const qint64 w = ok ? rc.right - rc.left : 0;
            const qint64 h = ok ? rc.bottom - rc.top : 0;
            // 坐标口径:UIA 的 bounding rectangle 是**物理屏幕坐标**(与 Win32 一致)。
            const bool contains = ok && cursor.x() >= rc.left && cursor.x() < rc.right &&
                                  cursor.y() >= rc.top && cursor.y() < rc.bottom;
            const bool bigEnough = ok && w >= minPhysical && h >= minPhysical;
            if (!contains || !bigEnough) continue;

            // 只认"明显小于窗口"的元素:Chromium 的外壳 Pane 常只比窗口小 2px,
            // 把它记成 best 等于没细化(旧实现用严格 `>=` 判"与窗口等大",
            // 恰好漏掉这种 —— docs/26 那个缺陷的另一半)。
            const bool smaller = w < winRect.width() - kMinShrinkPhys ||
                                 h < winRect.height() - kMinShrinkPhys;
            const int childDepth = cur.depth + 1;
            const qint64 area = w * h;
            const qint64 bestArea = static_cast<qint64>(r.best.width()) * r.best.height();
            if (smaller && (childDepth > r.depth ||
                            (childDepth == r.depth && area < bestArea))) {
                r.depth = childDepth;
                r.best = QRect(rc.left, rc.top, static_cast<int>(w), static_cast<int>(h));
                BSTR name = nullptr;
                if (SUCCEEDED(kid->get_CurrentName(&name)) && name) {
                    r.bestName = QString::fromWCharArray(name);
                    ::SysFreeString(name);
                } else {
                    r.bestName.clear();
                }
            }

            // 下沉 —— 同层可以有**多个**兄弟都包含光标,全都要试。
            // 这正是原实现("每层只挑一个")失效的地方,见函数头注释。
            kid->AddRef();                   // 栈自己那一份
            stack.append({kid, childDepth});
        }

        for (IUIAutomationElement* kid : kids) {
            IUIAutomationElement* k = kid;
            release(k);
        }
    }

    // 预算/上限截断时栈里还有活着的元素:必须全部 Release,否则跨进程 COM 泄漏。
    for (const Item& it : stack) {
        IUIAutomationElement* p = it.node;
        release(p);
    }
    return r;
}

} // namespace

QVector<Candidate> probeElementAt(quintptr windowId, const QPoint& cursorGlobalPhysical,
                                  qreal dpr, int minSize, int maxNodes, int budgetMs,
                                  ProbeStats* stats) {
    QVector<Candidate> out;
    if (stats) *stats = ProbeStats{};
    if (!windowId || dpr <= 0.01) return out;

    ComGuard com;
    if (!com.initialized) return out;   // COM 不可用:静默降级为窗口级

    IUIAutomation* uia = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&uia))) ||
        !uia) {
        return out;
    }

    IUIAutomationElement* node = nullptr;
    if (FAILED(uia->ElementFromHandle(reinterpret_cast<UIA_HWND>(windowId), &node)) || !node) {
        release(uia);
        return out;
    }

    RECT winRect{};
    if (FAILED(node->get_CurrentBoundingRectangle(&winRect)) || winRect.right <= winRect.left ||
        winRect.bottom <= winRect.top) {
        release(node);
        release(uia);
        return out;
    }
    const QRect winQ(winRect.left, winRect.top, static_cast<int>(winRect.right - winRect.left),
                     static_cast<int>(winRect.bottom - winRect.top));
    const qint64 minPhysical = qMax<qint64>(1, static_cast<qint64>(qRound(minSize * dpr)));

    QElapsedTimer timer;
    timer.start();

    // ---- 第一发:ControlView(快、语义干净)----
    IUIAutomationTreeWalker* ctrlWalker = nullptr;
    uia->get_ControlViewWalker(&ctrlWalker);
    DfsResult r = dfsDeepest(ctrlWalker, node, winQ, cursorGlobalPhysical, minPhysical,
                             maxNodes, kProbeMaxDepth, budgetMs);
    int viewUsed = 0;

    // ---- 第二发:第一发没细化时退到 RawView(S2)----
    //
    // RawView 比 ControlView 细 2~9 层(实测:Chromium 探针页 9 → 11 层,
    // 能到单行文本/单列),但节点数多一个量级。只在"第一发停在很粗的层"时补,
    // 让常见情况快、复杂情况细。
    if (r.depth <= 2) {
        const int remain = budgetMs - static_cast<int>(timer.elapsed());
        if (remain > 80) {   // 余量太小别投:必然超时,徒增延迟
            IUIAutomationTreeWalker* rawWalker = nullptr;
            uia->get_RawViewWalker(&rawWalker);
            const int left = maxNodes - r.visited;
            if (left > 64) {
                const DfsResult r2 = dfsDeepest(rawWalker, node, winQ, cursorGlobalPhysical,
                                                minPhysical, left, kProbeMaxDepth, remain);
                if (r2.depth > r.depth) {
                    r = r2;
                    viewUsed = 1;
                }
            }
            release(rawWalker);
        }
    }

    release(ctrlWalker);

    if (r.best.isEmpty() == false) {
        Candidate c;
        c.physical = r.best;
        c.title = r.bestName;
        c.id = windowId;          // 元素不单独给 id:同一次会话内由 windowId + 矩形区分
        c.windowId = windowId;
        c.level = 1;
        out.append(c);
    }

    if (stats) {
        stats->depth = r.depth;
        stats->visited = r.visited;
        stats->elapsedMs = static_cast<int>(timer.elapsed());
        stats->viewUsed = viewUsed;
        stats->refined = !r.best.isEmpty();
    }

    release(node);
    release(uia);
    return out;
}

QStringList dumpElementTree(quintptr windowId, int depth, int maxPerLevel) {
    QStringList out;
    if (!windowId) return out;
    ComGuard com;
    if (!com.initialized) return out;
    IUIAutomation* uia = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&uia))) ||
        !uia) {
        return out;
    }
    IUIAutomationElement* node = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->ElementFromHandle(reinterpret_cast<UIA_HWND>(windowId), &node)) || !node) {
        release(uia);
        return out;
    }
    uia->get_ControlViewWalker(&walker);
    // 与 buildElementSnapshot 同一条守卫:空 walker 不得进循环(否则 571/601 处解引用)。
    if (!walker) {
        release(node);
        release(uia);
        return out;
    }

    // ---- 每层打印**全部**子元素,并把"该子元素自己的子节点数"一并打出来 ----
    //
    // ⚠️ 旧实现只沿第 0 个孩子继续下沉(`if (i == 0)`)。Chromium 的第 0 个孩子
    //    恰好是空壳 Pane,于是诊断输出永远是 `L1 n=0` —— 它与主算法的同一个错误
    //    互相印证,把自身缺陷写成了"Chromium 惰性无障碍"(docs/26 §2.5)。
    //    诊断工具的错误比算法的错误代价更高:它会误导归因。
    //
    //    "→N" 是子元素自己的子节点数:**空的支路一眼可见**。
    for (int d = 0; d < depth; ++d) {
        // 先把本层子元素收齐(同一个所有权理由:取兄弟需要前一个仍有效)
        QVector<IUIAutomationElement*> kids;
        IUIAutomationElement* child = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(node, &child)) && child) {
            while (child) {
                kids.append(child);
                IUIAutomationElement* sib = nullptr;
                if (FAILED(walker->GetNextSiblingElement(child, &sib))) sib = nullptr;
                child = sib;
            }
        }
        if (kids.isEmpty()) {
            out << QStringLiteral("L%1 n=0 <空:到叶子了>").arg(d);
            break;
        }

        QString arms;
        // 沿"子节点最多"的支路继续下沉(而不是第 0 个);空壳支路不该占预算。
        IUIAutomationElement* next = nullptr;
        int bestCount = 0;
        for (int i = 0; i < kids.size(); ++i) {
            IUIAutomationElement* kid = kids.at(i);
            RECT rc{};
            kid->get_CurrentBoundingRectangle(&rc);
            BSTR name = nullptr;
            QString nm;
            if (SUCCEEDED(kid->get_CurrentName(&name)) && name) {
                nm = QString::fromWCharArray(name);
                ::SysFreeString(name);
            }
            // 该子元素自己的子节点数 —— 诊断的关键信息("空壳"在这里现形)
            int grand = 0;
            IUIAutomationElement* gc = nullptr;
            if (SUCCEEDED(walker->GetFirstChildElement(kid, &gc)) && gc) {
                ++grand;
                while (true) {
                    IUIAutomationElement* sib = nullptr;
                    if (FAILED(walker->GetNextSiblingElement(gc, &sib)) || !sib) break;
                    IUIAutomationElement* g = gc;
                    release(g);
                    gc = sib;
                    ++grand;
                }
            }
            {
                IUIAutomationElement* g = gc;
                release(g);
            }

            if (i < maxPerLevel) {
                arms += QStringLiteral(" [%1][%2 %3x%4 @%5,%6]→%7")
                            .arg(i)
                            .arg(nm.left(18))
                            .arg(rc.right - rc.left)
                            .arg(rc.bottom - rc.top)
                            .arg(rc.left)
                            .arg(rc.top)
                            .arg(grand);
            }
            if (grand > bestCount) {
                bestCount = grand;
                IUIAutomationElement* old = next;
                release(old);
                kid->AddRef();
                next = kid;      // next 自己持一份
            }
        }
        out << (QStringLiteral("L%1 n=%2").arg(d).arg(kids.size()) + arms +
                QStringLiteral(" [最深支路子节点数=%1]").arg(bestCount));

        for (IUIAutomationElement* kid : kids) {
            IUIAutomationElement* k = kid;
            release(k);
        }
        release(node);
        node = next;
        if (!node) break;
    }

    release(walker);
    release(node);
    release(uia);
    return out;
}

// ---- 元素快照(docs/27:进窗预取 + 查表,取代"每次悬停重探")----

QVector<ElementNode> buildElementSnapshot(quintptr windowId, int maxNodes, int budgetMs,
                                         ProbeStats* stats) {
    QVector<ElementNode> out;
    if (stats) *stats = ProbeStats{};
    if (!windowId) return out;

    ComGuard com;
    if (!com.initialized) return out;

    IUIAutomation* uia = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&uia))) ||
        !uia) {
        return out;
    }
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(reinterpret_cast<UIA_HWND>(windowId), &root)) || !root) {
        release(uia);
        return out;
    }
    RECT winRect{};
    if (FAILED(root->get_CurrentBoundingRectangle(&winRect))) {
        release(root);
        release(uia);
        return out;
    }
    IUIAutomationTreeWalker* walker = nullptr;
    uia->get_ControlViewWalker(&walker);
    // walker 拿不到 = COM 侧异常(理论上不会,但 dfsDeepest 有同样的 null 守卫):
    // 不能带着空 walker 进循环 —— 那会在 GetFirstChildElement 上解引用空指针,
    // 又是一个 0xC0000005。拿不到就按"该窗口没有可用树"降级。
    if (!walker) {
        release(root);
        release(uia);
        return out;
    }

    QElapsedTimer timer;
    timer.start();

    // 与 dfsDeepest 同一套所有权规则:栈持有引用,出栈即 Release;
    // 本层 kids 数组各持一份,本层统一 Release。
    struct Item {
        IUIAutomationElement* node;
        int parentIdx;   // out 数组里的父索引(-1 = 根)
        int depth;
    };
    QVector<Item> stack;
    root->AddRef();
    stack.append({root, -1, 0});

    int visited = 0;
    int maxDepthSeen = 0;
    // 诊断:矩形无效(不入快照)的子元素数量。UIA 树里这类节点不罕见(浏览器
    // 的 virtualized/离屏内容),也是上面那段 double-Release 崩溃的触发面 ——
    // 留一条日志,复发时能一眼看出"这次是否走过了那条分支"。
    int invalidRect = 0;

    while (!stack.isEmpty()) {
        if (timer.elapsed() > budgetMs || visited > maxNodes) break;
        const Item cur = stack.takeLast();
        if (cur.depth >= kProbeMaxDepth) {
            IUIAutomationElement* n = cur.node;
            release(n);
            continue;
        }

        QVector<IUIAutomationElement*> kids;
        IUIAutomationElement* child = nullptr;
        if (SUCCEEDED(walker->GetFirstChildElement(cur.node, &child)) && child) {
            while (child) {
                kids.append(child);
                IUIAutomationElement* sib = nullptr;
                if (FAILED(walker->GetNextSiblingElement(child, &sib))) sib = nullptr;
                child = sib;
            }
        }
        {
            IUIAutomationElement* n = cur.node;
            release(n);
        }

        // ⚠️ 引用所有权(2026-11-03 闪退修复,别改回去):
        //
        //   kids 数组对每条子元素持有 1 份引用,**统一由下面第二个循环释放**。
        //   循环体内绝不能 release()。旧实现在"矩形无效"分支里额外放了一次,
        //   紧接着的统一释放又 Release 同一条指针 —— 同一个 COM 对象被释放两次,
        //   第二次是 use-after-free;崩溃栈实测就落在 Release 的 vtable 调用上
        //   (用户在 Chrome 中截图时 0xC0000005,Chrome 的 UIA 树里 bounding rect
        //    为空/非正的节点最多,最容易踩到;崩溃时刻与 400ms 预算耗尽的清理路径
        //    完全吻合)。
        //
        //   第二层保险:统一释放循环用**引用**遍历,release() 会把槽位置空,
        //   将来即使有人再误加一次释放,也只会是空指针(release 内部跳过),
        //   不会退化成 UAF。
        for (IUIAutomationElement*& kid : kids) {
            if (timer.elapsed() > budgetMs || visited > maxNodes) break;
            ++visited;

            RECT rc{};
            const bool ok = SUCCEEDED(kid->get_CurrentBoundingRectangle(&rc)) &&
                            rc.right > rc.left && rc.bottom > rc.top;
            if (!ok) {
                ++invalidRect;
                continue;   // 不入快照,也就不再下沉;引用仍由下面的统一循环释放
            }

            ElementNode n;
            n.physical = QRect(rc.left, rc.top, static_cast<int>(rc.right - rc.left),
                               static_cast<int>(rc.bottom - rc.top));
            BSTR name = nullptr;
            if (SUCCEEDED(kid->get_CurrentName(&name)) && name) {
                n.title = QString::fromWCharArray(name);
                ::SysFreeString(name);
            }
            n.parent = cur.parentIdx;
            n.depth = cur.depth + 1;
            if (n.depth > maxDepthSeen) maxDepthSeen = n.depth;

            const int idx = out.size();
            out.append(n);
            kid->AddRef();      // 栈自己那一份(kids 数组那一份仍旧由下面统一释放)
            stack.append({kid, idx, n.depth});
        }

        for (IUIAutomationElement*& kid : kids) release(kid);
    }

    for (const Item& it : stack) {
        IUIAutomationElement* p = it.node;
        release(p);
    }
    release(walker);
    release(root);
    release(uia);

    if (invalidRect > 0) {
        qDebug().nospace() << "元素快照: 窗口=" << Qt::hex << windowId << Qt::dec
                           << " 节点=" << out.size() << " 无效矩形=" << invalidRect
                           << " 用时=" << timer.elapsed() << "ms";
    }

    if (stats) {
        stats->depth = maxDepthSeen;
        stats->visited = visited;
        stats->elapsedMs = static_cast<int>(timer.elapsed());
        stats->viewUsed = 0;
        stats->refined = !out.isEmpty();
    }
    return out;
}
#else
// Linux(M3):窗口级枚举 = EWMH/Xlib(本段);元素级(AT-SPI2)二期,仍走
// 降级桩。契约与 Windows 段一一致:失败/未接入返回**空表**,不报错不阻塞,
// 退化成纯手动框选。Wayland 原生会话无 X 枚举能力:XOpenDisplay 失败 → 空表。
namespace {

// 与 Windows 段一的同名常量同值(docs/23 §4.4 碎片过滤判据)。它们在 Windows
// 匿名命名空间里,平台门控够不着,这里按同值另立(改一处须改两处)。
constexpr int kMinCandidateEdge = 60;
constexpr qreal kMaxCandidateAspect = 12.0;

/// Xlib 错误默认 handler 会 exit 整个进程 —— 枚举期间目标窗口随时可能被
/// 销毁(BadWindow 是正常竞态不是故障),局部吞掉。Xlib 错误 handler 是
/// **线程级**的,只影响本线程的 Xlib 调用,不碰 Qt xcb 主连接(它不走 Xlib)。
int ignoreXErrors(Display*, XErrorEvent*) { return 0; }

/// 读 32-bit 格式属性,按 Xlib 约定落到 long 数组(ATOM/WINDOW/CARDINAL 通用)。
QVector<long> readLongAtoms(Display* d, Window w, const char* name, unsigned long maxItems) {
    QVector<long> out;
    if (w == None) return out;
    const Atom atom = XInternAtom(d, name, True);
    if (atom == None) return out;
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, rest = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(d, w, atom, 0, maxItems, False, AnyPropertyType,
                           &type, &fmt, &n, &rest, &data) != Success) {
        return out;
    }
    if (data) {
        if (fmt == 32) {
            const auto* items = reinterpret_cast<const long*>(data);
            out.reserve(int(qMin<unsigned long>(n, maxItems)));
            for (unsigned long i = 0; i < n && i < maxItems; ++i) out.append(items[i]);
        }
        XFree(data);
    }
    return out;
}

/// 窗口标题:_NET_WM_NAME(UTF-8)优先,WM_NAME 兜底;可空(同 Windows)。
QString readWindowName(Display* d, Window w) {
    for (const char* name : { "_NET_WM_NAME", "WM_NAME" }) {
        const Atom atom = XInternAtom(d, name, True);
        if (atom == None) continue;
        Atom type = None;
        int fmt = 0;
        unsigned long n = 0, rest = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(d, w, atom, 0, 4096, False, AnyPropertyType,
                               &type, &fmt, &n, &rest, &data) == Success && data) {
            QString s;
            if (fmt == 8 && n > 0) {
                s = QString::fromUtf8(reinterpret_cast<const char*>(data), int(n));
            }
            XFree(data);
            if (!s.isEmpty()) return s;
        }
    }
    return {};
}

/// 窗口**外框**的绝对物理矩形:客户区(XGetGeometry + XTranslateCoordinates)
/// 按 _NET_FRAME_EXTENTS(左/右/上/下)外扩 —— X11 版的
/// DWMWA_EXTENDED_FRAME_BOUNDS(排除值是"装饰遮住的客户区"而非"不可见拖拽带",
/// 语义对齐:候选框应覆盖用户看见的整扇窗,含标题栏)。没有 extents 的 WM
/// 就退回客户区 —— 对齐 Windows 的 GetWindowRect 兜底。
QRect windowOuterRect(Display* d, Window w) {
    Window rootRet = None;
    int x = 0, y = 0;
    unsigned wu = 0, hu = 0, bw = 0, depth = 0;
    if (!XGetGeometry(d, w, &rootRet, &x, &y, &wu, &hu, &bw, &depth)) return {};
    int ax = 0, ay = 0;
    Window child = None;
    if (!XTranslateCoordinates(d, w, rootRet, 0, 0, &ax, &ay, &child)) return {};
    QRect r(ax, ay, int(wu), int(hu));
    const QVector<long> ext = readLongAtoms(d, w, "_NET_FRAME_EXTENTS", 4);
    if (ext.size() == 4) {
        r.adjust(int(-ext.at(0)), int(-ext.at(2)), int(ext.at(1)), int(ext.at(3)));
    }
    return r;
}

} // namespace

QVector<Candidate> probeWindows() {
    QVector<Candidate> out;
    XInitThreads();   // 幂等;多线程约定见文件头 include 处注释
    Display* d = XOpenDisplay(nullptr);
    if (!d) return out;
    const auto oldHandler = XSetErrorHandler(ignoreXErrors);
    Window root = DefaultRootWindow(d);

    // 排除用的原子提前 intern(only_if_exists=False:它们只是标识符,不存在就造)。
    const Atom atomHidden = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    const Atom atomDesktop = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    const Atom atomDock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False);
    const quint64 selfPid = quint64(::getpid());

    // z 序:_NET_CLIENT_LIST_STACKING 自**底向顶**(EWMH 规格;_NET_CLIENT_LIST
    // 只是初始映射序,不随堆叠变化)。**反向遍历 = 顶到底**,与 Windows
    // EnumWindows(顶层在前)同序 —— 命中测试"点在上面那个窗口就选上面那个"
    // 依赖这个顺序。
    const QVector<long> stacking = readLongAtoms(d, root, "_NET_CLIENT_LIST_STACKING", 4096);
    for (auto it = stacking.crbegin(); it != stacking.crend(); ++it) {
        const Window w = static_cast<Window>(*it);
        if (w == None) continue;

        // 本进程窗口(面板/遮罩/贴图/便签):同 Windows 的 selfPid 排除。
        const QVector<long> pid = readLongAtoms(d, w, "_NET_WM_PID", 1);
        if (!pid.isEmpty() && quint64(pid.first()) == selfPid) continue;

        // 最小化(_NET_WM_STATE_HIDDEN):屏幕上没有它的内容(同 IsIconic)。
        if (readLongAtoms(d, w, "_NET_WM_STATE", 64).contains(long(atomHidden))) continue;

        // 桌面/任务栏:同 Windows isShellClass 的排除口径(_DESKTOP/_DOCK)。
        const QVector<long> ty = readLongAtoms(d, w, "_NET_WM_WINDOW_TYPE", 64);
        if (ty.contains(long(atomDesktop)) || ty.contains(long(atomDock))) continue;

        // 未映射(Withdrawn)与"枚举中被销毁"竞态:失败即跳过(错误已吞)。
        XWindowAttributes attr {};
        if (!XGetWindowAttributes(d, w, &attr) || attr.map_state != IsViewable) continue;

        const QRect rect = windowOuterRect(d, w);
        if (rect.width() < 8 || rect.height() < 8) continue;
        // 碎片过滤(同 Windows 段一:最小边长 + 宽高比,判据是"框住它没有意义")。
        if (rect.width() < kMinCandidateEdge || rect.height() < kMinCandidateEdge) continue;
        const qreal ratio = static_cast<qreal>(rect.width()) / qMax(1, rect.height());
        if (ratio > kMaxCandidateAspect || ratio < 1.0 / kMaxCandidateAspect) continue;

        Candidate c;
        c.physical = rect;
        c.title = readWindowName(d, w);
        c.id = quintptr(w);   // X Window id:同 HWND 的"稳定标识"角色
        c.windowId = c.id;
        c.level = 0;
        out.append(c);
    }
    XSetErrorHandler(oldHandler);
    XCloseDisplay(d);
    return out;
}
bool worthProbing(quintptr) { return false; }
QVector<Candidate> probeElementAt(quintptr, const QPoint&, qreal, int, int, int,
                                  ProbeStats*) { return {}; }
QStringList dumpElementTree(quintptr, int, int) { return {}; }
QVector<ElementNode> buildElementSnapshot(quintptr, int, int, ProbeStats*) { return {}; }
#endif  // Q_OS_WIN(段二:元素级 UIA)

int elementAtSnapshot(const QVector<ElementNode>& nodes, const QRect& winPhysical,
                      const QPoint& cursor, int minSize, bool allowEqualWin) {
    if (nodes.isEmpty() || winPhysical.isEmpty()) return -1;
    const int minShrink = allowEqualWin ? 0 : kMinShrinkPhys;

    int bestIdx = -1;
    int bestDepth = -1;
    qint64 bestArea = 0;

    for (int i = 0; i < nodes.size(); ++i) {
        const ElementNode& n = nodes.at(i);
        if (!n.physical.contains(cursor)) continue;
        if (n.physical.width() < minSize || n.physical.height() < minSize) continue;
        // "明显小于窗口"的判据与 dfsDeepest 完全一致(Chromium 外壳 Pane 只差 2px)
        const bool smaller = n.physical.width() < winPhysical.width() - minShrink ||
                             n.physical.height() < winPhysical.height() - minShrink;
        if (!smaller) continue;

        const qint64 area = static_cast<qint64>(n.physical.width()) * n.physical.height();
        if (n.depth > bestDepth || (n.depth == bestDepth && area < bestArea)) {
            bestDepth = n.depth;
            bestArea = area;
            bestIdx = i;
        }
    }
    return bestIdx;
}

} // namespace nb::smart
