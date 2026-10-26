#include "capture/smart_capture.hpp"

#include <QHash>
#include <limits>
#include <algorithm>
#include <QElapsedTimer>
#include <QImage>
#include <QScreen>
#include <QStringList>

#include <dwmapi.h>
#include <windows.h>

// UIA 元素级(S3)。只在 probeElementAt 里用到;窗口级(S1)完全不碰 COM ——
// 这样"元素探测失败/超时"永远不会影响"至少还能按窗口框"。
#include <UIAutomation.h>

namespace nb::smart {

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

QVector<Candidate> probeElementAt(quintptr windowId, const QPoint& cursorGlobalPhysical,
                                  qreal dpr, int minSize, int maxNodes, int budgetMs) {
    QVector<Candidate> out;
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
    IUIAutomationCondition* cond = nullptr;
    const HRESULT hrWin = uia->ElementFromHandle(reinterpret_cast<UIA_HWND>(windowId), &node);
    if (FAILED(hrWin) || !node) {
        release(uia);
        return out;
    }
    if (FAILED(uia->CreateTrueCondition(&cond)) || !cond) {
        release(node);
        release(uia);
        return out;
    }

    const RECT winRect = [&] {
        RECT r{};
        node->get_CurrentBoundingRectangle(&r);
        return r;
    }();
    const qint64 minPhysical = qMax<qint64>(1, static_cast<qint64>(qRound(minSize * dpr)));

    QElapsedTimer timer;
    timer.start();
    int visited = 0;
    /// 连续"与窗口同尺寸"的层数(见下方下沉逻辑;防死循环)。
    int sameSizeStreak = 0;
    QRect best;   // 目前找到的"最深处、仍包含光标"的元素
    QString bestName;

    // ---- 贪心下沉:每层只在**子元素里挑包含光标的那几个**,再下钻一层 ----
    //
    // 为什么不 ElementFromPoint:遮罩自己全屏置顶,系统命中测试只会返回遮罩;
    // 为什么不整树 FindAll:浏览器的整树是几万个节点,一次 FindAll 就能卡住
    //   好几百毫秒。贪心下沉的代价是 O(深度 × 每层兄弟数),通常 < 40 次 COM 调用。
    for (int depth = 0; depth < 32; ++depth) {
        if (timer.elapsed() > budgetMs || visited > maxNodes) break;

        IUIAutomationElementArray* children = nullptr;
        if (FAILED(node->FindAll(TreeScope_Children, cond, &children)) || !children) break;

        int len = 0;
        children->get_Length(&len);
        visited += len;

        IUIAutomationElement* next = nullptr;
        QRect nextRect;
        QString nextName;
        qint64 nextArea = 0;

        for (int i = 0; i < len; ++i) {
            if (timer.elapsed() > budgetMs || visited > maxNodes) break;
            IUIAutomationElement* child = nullptr;
            if (FAILED(children->GetElement(i, &child)) || !child) continue;

            RECT r{};
            if (FAILED(child->get_CurrentBoundingRectangle(&r)) || r.right <= r.left ||
                r.bottom <= r.top) {
                release(child);
                continue;
            }
            const qint64 w = r.right - r.left;
            const qint64 h = r.bottom - r.top;
            // 坐标口径:UIA 的 bounding rectangle 是**物理屏幕坐标**(与 Win32 一致)。
            const bool contains = cursorGlobalPhysical.x() >= r.left &&
                                  cursorGlobalPhysical.x() < r.right &&
                                  cursorGlobalPhysical.y() >= r.top &&
                                  cursorGlobalPhysical.y() < r.bottom;
            if (!contains || w < minPhysical || h < minPhysical) {
                release(child);
                continue;
            }
            // 同层可能多个子元素都包含光标(重叠):取**面积最小**的那个 ——
            // 它就是视觉上"光标正压着的内容"。
            const qint64 area = w * h;
            if (!next || area < nextArea) {
                release(next);
                next = child;
                nextRect = QRect(r.left, r.top, static_cast<int>(w), static_cast<int>(h));
                nextArea = area;
                BSTR name = nullptr;
                if (SUCCEEDED(child->get_CurrentName(&name)) && name) {
                    nextName = QString::fromWCharArray(name);
                    ::SysFreeString(name);
                } else {
                    nextName.clear();
                }
            } else {
                release(child);
            }
        }
        release(children);

        if (!next) break;   // 这一层没有包含光标的子元素:已到最深处

        // ⚠️ 元素与窗口同尺寸**不等于"到底了"**:实测 Chrome 的 UIA 树上,
        //    根节点就是一个与窗口等大的容器,真正的正文区块在**它下面几层**。
        //    早先版本在这里 break,结果"元素级"永远只会框住整个窗口 ——
        //    功能看起来接上了,实际上没有任何细化。
        //
        //    正确做法:同尺寸就**继续下沉**,只是不把它记成 best。
        //    用 sameSizeStreak 兜住"一连几十层都同尺寸"的死循环。
        const LONG winW = winRect.right - winRect.left;
        const LONG winH = winRect.bottom - winRect.top;
        const bool sameAsWindow =
            nextRect.width() >= winW && nextRect.height() >= winH;
        if (!sameAsWindow) {
            best = nextRect;
            bestName = nextName;
            sameSizeStreak = 0;
        } else if (++sameSizeStreak > 6) {
            // 连续 6 层都和窗口一样大:这个应用的 UIA 只报容器,再钻没有意义
            release(node);
            node = next;
            break;
        }
        release(node);
        node = next;
    }

    if (!best.isEmpty()) {
        Candidate c;
        c.physical = best;
        c.title = bestName;
        c.id = windowId;          // 元素不单独给 id:同一次会话内由 windowId + 矩形区分
        c.windowId = windowId;
        c.level = 1;
        out.append(c);
    }

    release(cond);
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
    IUIAutomationCondition* cond = nullptr;
    if (FAILED(uia->ElementFromHandle(reinterpret_cast<UIA_HWND>(windowId), &node)) || !node ||
        FAILED(uia->CreateTrueCondition(&cond)) || !cond) {
        release(node);
        release(uia);
        return out;
    }
    for (int d = 0; d < depth; ++d) {
        IUIAutomationElementArray* children = nullptr;
        if (FAILED(node->FindAll(TreeScope_Children, cond, &children)) || !children) {
            out << QStringLiteral("L%1 <FindAll 失败>").arg(d);
            break;
        }
        int len = 0;
        children->get_Length(&len);
        QString line = QStringLiteral("L%1 n=%2").arg(d).arg(len);
        for (int i = 0; i < len && i < maxPerLevel; ++i) {
            IUIAutomationElement* child = nullptr;
            if (FAILED(children->GetElement(i, &child)) || !child) continue;
            RECT r{};
            child->get_CurrentBoundingRectangle(&r);
            BSTR name = nullptr;
            QString nm;
            if (SUCCEEDED(child->get_CurrentName(&name)) && name) {
                nm = QString::fromWCharArray(name);
                ::SysFreeString(name);
            }
            line += QStringLiteral(" [%1 %2x%3 @%4,%5]")
                        .arg(nm.left(18))
                        .arg(r.right - r.left)
                        .arg(r.bottom - r.top)
                        .arg(r.left)
                        .arg(r.top);
            if (i == 0) {
                // 沿第 0 个孩子继续下沉(与 probeElementAt 的贪心路径同向)
                release(node);
                node = child;
            } else {
                release(child);
            }
        }
        out << line;
        release(children);
        if (!node) break;
    }
    release(cond);
    release(node);
    release(uia);
    return out;
}

} // namespace nb::smart
