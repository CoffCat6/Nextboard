#include "auto_paste.hpp"

#include "core_bridge.hpp"

#include <QDebug>
#include <QCoreApplication>
#include <QJsonObject>
#include <QTimer>

#include <chrono>
#include <thread>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
// Linux(M3):XTest 注入 + 前台 PID 检查(EWMH)。fail-safe 原则见
// pasteToForegroundIfEnabled 的 Linux 分支注释。
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <unistd.h>
#endif

namespace nb {
namespace {

/// 投一次 Ctrl+V。
///
/// 用**扫描码**(KEYEVENTF_SCANCODE)而不是虚拟键码:SendInput 文档明确要求在
/// 可能时给出扫描码,否则某些输入法/远程桌面/游戏窗口收不到(虚拟键码路径要由
/// 系统反查键盘布局,反查失败就静默丢键)。0x1D = LeftCtrl,0x2F = V(Set 1)。
///
/// 按下与抬起**分两批**投递,中间让出一拍(实现见下)。
/// 一次性投 4 个事件时,部分应用(实测 Electron 系与部分远程桌面客户端)只能看到
/// "已按下"而看不到配对抬起,结果是 Ctrl 卡住 —— 这里必须真的有两批,否则注释在撒谎。
#if defined(Q_OS_WIN)
void sendCtrlV() {
    constexpr WORD kScanCtrl = 0x1D;
    constexpr WORD kScanV = 0x2F;
    INPUT inputs[4] = {};
    auto key = [](INPUT& in, WORD scan, bool up) {
        in.type = INPUT_KEYBOARD;
        // KEYEVENTF_KEYUP 表示抬起;扫描码路径必须带 KEYEVENTF_SCANCODE
        in.ki.wScan = scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
        in.ki.wVk = 0;
        in.ki.time = 0;
        in.ki.dwExtraInfo = 0;
    };
    key(inputs[0], kScanCtrl, false);
    key(inputs[1], kScanV, false);
    key(inputs[2], kScanV, true);
    key(inputs[3], kScanCtrl, true);

    // ⚠️ **分两次 SendInput,中间让出一拍**(2026-11-03 修,外部审查 #6 的
    //    "注释与实现互相矛盾":注释写"必须有小间隔",代码却一把梭 4 个事件)。
    //    注释描述的现象是真实观测过的(Electron 系 / 远程桌面只看到按下、
    //    看不到抬起 → Ctrl 卡住),所以是改实现,不是删注释。
    //    为什么必须拆成两个调用:同一批次里的 4 个事件会被目标进程**连续取出**,
    //    系统不保证它在其间泵过消息 —— 按下与抬起落在同一批就等于"没有间隔"。
    //    3ms 是保守取值:低于输入栈的合批窗口几乎等于没拆,高于 20ms 会让
    //    "复制后立刻粘贴"的连续操作出现可感知粘滞。
    const UINT down = ::SendInput(2, inputs, sizeof(INPUT));
    if (down != 2) {
        // 最常见原因:前台窗口以更高完整性级别运行(UIPI)→ 事件被静默丢弃。
        // 只记日志:此时"什么都没发生"与用户没开该功能无从区分,但弹窗更糟。
        // 按下都没成功就不再投抬起,否则会留下一对"没按下却抬起"的事件。
        qWarning() << "auto-paste: Ctrl+V 的按下事件只投出" << down
                   << "个(可能被 UIPI 拦截)lasterror =" << ::GetLastError();
        return;
    }
    ::Sleep(3);
    const UINT raised = ::SendInput(2, &inputs[2], sizeof(INPUT));
    if (raised != 2) {
        // 抬起投失败比按下失败**更糟**:目标应用会认为 Ctrl/V 一直按着。
        // 这是唯一值得主动补救的分支 —— 补投一次抬起。
        qWarning() << "auto-paste: 抬起事件只投出" << raised
                   << "个,补投一次 lasterror =" << ::GetLastError();
        ::SendInput(2, &inputs[2], sizeof(INPUT));
    } else {
        qInfo() << "auto-paste: 已投出 Ctrl+V(按下/抬起分两批)";
    }
}
#else
    // Linux(M3):XTest 注入(自有 X 连接;Wayland 原生会话无注入能力 → 跳过)。
    void sendCtrlV(Display* d) {
        const KeyCode vkc = XKeysymToKeycode(d, XK_v);
        const KeyCode ckc = XKeysymToKeycode(d, XK_Control_L);
        if (vkc == 0 || ckc == 0) {
            qWarning() << "auto-paste: 键码解析失败(V/Control_L),跳过注入";
            return;
        }
        // 按下/抬起**分两批 + 中间让一拍**:与 Windows 分支同一实测依据
        // (Electron 系/远程桌面只看到按下看不到抬起 → Ctrl 卡住)。
        XTestFakeKeyEvent(d, ckc, True, CurrentTime);
        XTestFakeKeyEvent(d, vkc, True, CurrentTime);
        XSync(d, False);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        XTestFakeKeyEvent(d, vkc, False, CurrentTime);
        XTestFakeKeyEvent(d, ckc, False, CurrentTime);
        XSync(d, False);
        qInfo() << "auto-paste: 已投出 Ctrl+V(XTest,按下/抬起分两批)";
    }

    /// 单窗口的 _NET_WM_PID;没有/拿不到返回 0。
    pid_t pidOfWindow(Display* d, Window w) {
        if (w == None) return 0;
        const Atom pidAtom = XInternAtom(d, "_NET_WM_PID", True);
        if (pidAtom == None) return 0;
        Atom type = None;
        int fmt = 0;
        unsigned long n = 0, rest = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(d, w, pidAtom, 0, 1, False, XA_CARDINAL,
                               &type, &fmt, &n, &rest, &data) != Success || !data) {
            return 0;
        }
        pid_t pid = 0;
        if (n > 0 && fmt == 32) pid = static_cast<pid_t>(*(reinterpret_cast<unsigned long*>(data)));
        XFree(data);
        return pid;
    }

    /// 解析"前台窗口的 PID"。
    ///
    /// ⚠️ **不能只看 XGetInputFocus**:面板 hide 后 X 焦点按 RevertToPointerRoot
    /// 回退,WM(xfwm4 实测 2026-11-08)可能整秒不设显式焦点 —— 期间
    /// XGetInputFocus 返回 PointerRoot,而事件实际按**指针所在窗口**投递
    /// (xdotool getwindowfocus 正是这么解析的,所以探针看得到 mousepad、
    /// 我们的单发检查却是"未知")。两级解析:
    /// ① _NET_ACTIVE_WINDOW(WM 权威口径,客户端窗,必带 _NET_WM_PID);
    /// ② 回退 XGetInputFocus —— PointerRoot 时取指针下窗口,再沿框架向上爬
    ///    找带 _NET_WM_PID 的客户端窗(框架窗本身不带)。
    /// 都拿不到返回 0 = fail-safe 拒注(绝不盲注)。
    pid_t resolveForegroundPid(Display* d) {
        // ① EWMH 活动窗口
        const Atom awAtom = XInternAtom(d, "_NET_ACTIVE_WINDOW", True);
        if (awAtom != None) {
            Window root = DefaultRootWindow(d);
            Atom type = None;
            int fmt = 0;
            unsigned long n = 0, rest = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(d, root, awAtom, 0, 1, False, XA_WINDOW,
                                   &type, &fmt, &n, &rest, &data) == Success && data) {
                Window active = None;
                if (n > 0 && fmt == 32) active = *(reinterpret_cast<Window*>(data));
                XFree(data);
                const pid_t pid = pidOfWindow(d, active);
                if (pid != 0) return pid;
            }
        }
        // ② 焦点回退
        Window focus = None;
        int revert = 0;
        XGetInputFocus(d, &focus, &revert);
        if (focus == None) return 0;
        Window w = focus;
        if (w == PointerRoot) {
            Window root = DefaultRootWindow(d), child = None;
            int rx = 0, ry = 0, wx = 0, wy = 0;
            unsigned mask = 0;
            if (!XQueryPointer(d, root, &root, &child, &rx, &ry, &wx, &wy, &mask)) {
                return 0;   // 指针不在本屏:无人有焦点,拒注
            }
            w = child;
        }
        for (int hops = 0; w != None && hops < 8; ++hops) {
            const pid_t pid = pidOfWindow(d, w);
            if (pid != 0) return pid;
            Window qroot = None, qparent = None;
            Window* kids = nullptr;
            unsigned n = 0;
            if (!XQueryTree(d, w, &qroot, &qparent, &kids, &n)) return 0;
            if (kids) XFree(kids);
            if (qparent == None || qparent == qroot) return 0;
            w = qparent;
        }
        return 0;
    }

    /// Linux(M3):等焦点就绪再投 Ctrl+V。
    ///
    /// 为什么不能单发:面板收起后 **WM 重设焦点有可感延迟** —— xfwm4 实测
    /// (2026-11-08 Ubuntu VM):面板 hide 后 +80ms 时 XGetInputFocus 仍是 None
    /// (fail-safe 拒注),+0.5s 时焦点已落在目标窗口且带 _NET_WM_PID。于是
    /// 100ms 步进轮询、至多 ~1s:期间焦点回到本进程 → 跳过(防粘到自己);
    /// 超时仍拿不到 → 拒绝注入。fail-safe 硬约束不变:**绝不盲注**。
    void x11PasteWithFocusRetry(int attempt) {
        constexpr int kMaxAttempts = 10;
        Display* d = XOpenDisplay(nullptr);
        if (!d) {
            qWarning() << "auto-paste: XOpenDisplay 失败,跳过注入";
            return;
        }
        const pid_t foreground = resolveForegroundPid(d);
        if (foreground == ::getpid()) {
            qInfo() << "auto-paste: 前台仍是本进程窗口,跳过";
            XCloseDisplay(d);
            return;
        }
        if (foreground == 0) {
            XCloseDisplay(d);
            if (attempt + 1 >= kMaxAttempts) {
                qInfo() << "auto-paste: 前台 PID 未知(等待约1s仍拿不到焦点/EWMH),跳过注入(fail-safe)";
                return;
            }
            QTimer::singleShot(100, qApp, [attempt] { x11PasteWithFocusRetry(attempt + 1); });
            return;
        }
        sendCtrlV(d);
        XCloseDisplay(d);
    }
#endif  // Q_OS_WIN

} // namespace

void pasteToForegroundIfEnabled(int delayMs) {
    // 设置每次现读:低频动作(用户点一次复制),不值得为它做缓存与失效管理;
    // 反而缓存会让"刚在设置里打开开关 → 立刻复制没生效"看起来像 bug。
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return;
    if (!resp.value("data").toObject().value("copyAutoPaste").toBool()) return;

    // 延后一拍:此刻面板可能还在收起过程中(hide() 之后前台窗口需要一两拍才拿到
    // 焦点)。面板仍是前台时投键 = 粘到面板自己身上(搜索框多一段文本)。
    QTimer::singleShot(delayMs > 0 ? delayMs : 1, qApp, [] {
#if defined(Q_OS_WIN)
        // 再确认一次:若这期间又有本进程的窗口拿到前台(例如用户手快又按了热键),
        // 就不该投键 —— 否则粘到的是面板自己。
        const HWND foreground = ::GetForegroundWindow();
        DWORD pid = 0;
        ::GetWindowThreadProcessId(foreground, &pid);
        if (pid == ::GetCurrentProcessId()) {
            qInfo() << "auto-paste: 前台仍是本进程窗口,跳过";
            return;
        }
        sendCtrlV();
#else
        // Linux(M3):前台检查与 Windows 同语义,但多一层**焦点就绪轮询** ——
        // 面板收起后 WM(xfwm4)重设焦点有可感延迟,+80ms 单发必踩"焦点未知"
        // fail-safe(2026-11-08 VM 实测)。详见 x11PasteWithFocusRetry 注释:
        // 就绪即注、焦点回本进程即跳过、超时仍未知即拒注 —— 绝不盲注。
#ifdef NB_HAVE_XTEST
        x11PasteWithFocusRetry(0);
#else
        // 无 libXtst 的构建:自动粘贴不可用(设置项保留,行为=仅复制)
        qInfo() << "auto-paste: 本构建无 libXtst,自动粘贴不可用,仅保留复制";
#endif
#endif
    });
}

} // namespace nb
