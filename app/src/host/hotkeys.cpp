#include "hotkeys.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"
#include "host/core_bridge.hpp"
#include "capture/capture_session.hpp"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QDateTime>
#include <QTimer>
#include <QDebug>

#include <atomic>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
// Linux(M3)X11 热键:**专用线程自有 X 连接**(K8 定稿,docs/29 §9.1)——
// Fedora 6.8 QtGui 无 QNativeInterface::QX11Application,拿不到 Qt 的 xcb
// 连接;而 XGrabKey 的 KeyPress 事件只会送达**抓取方连接**,自建连接 +
// 线程内 XNextEvent 循环是最干净的等价物(Wayland 原生会话后续走 portal)。
// 结构:命令队列(Grab/Ungrab,同步等待结果,保留"失败项"toast 语义)
// + select 轮询(X 事件 / 命令 / 100ms 心跳)。
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <sys/select.h>

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSharedPointer>
#include <QWaitCondition>
#include <mutex>
#include <thread>
#endif

namespace nb {

namespace {

// ---- 热键 id 分配 ----
//
// ⚠️ 必须是**进程级**计数器,不能挂在 Hotkeys 实例上(2026-11-03 修)。
// 原实现是 `kHotkeyIdBase + m_nextId++`,m_nextId 是成员,于是每个 Hotkeys 实例
// 都从 0xB001 开始。产品路径只有单实例,看不出问题;但验收流会 new/delete 一个
// 临时 Hotkeys —— 它去注册 0xB001 时主实例正占着该 id(RegisterHotKey 失败,
// 日志刷出 GetLastError=1409),而 delete 时它的析构又把**主实例的** 0xB001…
// 一并 UnregisterHotKey(同线程同 id 互斥,注销不看属主)—— 验收跑完产品热键
// 全部失效,直到下次 applyFromSettings。
// 计数器提为进程级原子量后,每个实例拿到的 id 区间天然不重叠。
std::atomic<int> g_nextHotkeyId{1};

constexpr int kHotkeyIdBase = 0xB000;

#if defined(Q_OS_WIN)
bool parseComboImpl(const QString& combo, unsigned* modifiers, unsigned* vk) {
    // 形如 "Ctrl+Shift+V";与第一代 hotkey.rs 的解析语义对齐(宽松:大小写/顺序不敏感)
    UINT mods = 0;
    UINT key = 0;
    const QStringList parts = combo.split('+', Qt::SkipEmptyParts);
    for (QString part : parts) {
        part = part.trimmed().toLower();
        if (part == "ctrl") mods |= MOD_CONTROL;
        else if (part == "shift") mods |= MOD_SHIFT;
        else if (part == "alt") mods |= MOD_ALT;
        else if (part == "win") mods |= MOD_WIN;
        else if (part.length() == 1) {
            // VkKeyScanW 失败返回 -1,LOBYTE(-1) == 0xFF —— 一个"看起来合法"的
            // 虚拟键码,于是 parseCombo 通过、RegisterHotKey 失败,最终文案变成
            // 「快捷键不可用:Ctrl+?(被其他程序占用)」:用户按提示去关别的程序,
            // 永远解决不了(2026-11-03 修)。这里显式判定,走"格式非法"分支。
            const SHORT scan = VkKeyScanW(part.at(0).toUpper().unicode());
            if (scan == -1) return false;
            key = LOBYTE(scan);
        }
        else if (part.startsWith("f") && part.mid(1).toInt() >= 1 && part.mid(1).toInt() <= 24)
            key = VK_F1 + part.mid(1).toInt() - 1;
    }
    if (mods == 0 || key == 0) return false;
    *modifiers = mods;
    *vk = key;
    return true;
}
#else
// Linux(M3):解析为 X11 修饰掩码 + keysym(vk 参数承载 keysym)。
// 修饰符映射:Ctrl=ControlMask Shift=ShiftMask Alt=Mod1Mask Win=Mod4Mask。
bool parseComboImpl(const QString& combo, unsigned* modifiers, unsigned* vk) {
    unsigned mods = 0;
    unsigned keysym = 0;
    const QStringList parts = combo.split('+', Qt::SkipEmptyParts);
    for (QString part : parts) {
        part = part.trimmed().toLower();
        if (part == "ctrl") mods |= ControlMask;
        else if (part == "shift") mods |= ShiftMask;
        else if (part == "alt") mods |= Mod1Mask;
        else if (part == "win") mods |= Mod4Mask;
        else if (part.length() == 1) {
            // 与 Windows 分支同一防坑:解析不出的字符走"格式非法",不留模糊键
            const KeySym sym = XStringToKeysym(part.toUpper().toUtf8().constData());
            if (sym == NoSymbol) return false;
            keysym = static_cast<unsigned>(sym);
        }
        else if (part.startsWith("f") && part.mid(1).toInt() >= 1 && part.mid(1).toInt() <= 24)
            keysym = static_cast<unsigned>(XK_F1) + static_cast<unsigned>(part.mid(1).toInt() - 1);
        else {
            // 具名键(Home/End/PgUp…):直接查 keysym 表,查不到 = 格式非法
            const KeySym sym = XStringToKeysym(part.toUpper().toUtf8().constData());
            if (sym == NoSymbol) return false;
            keysym = static_cast<unsigned>(sym);
        }
    }
    if (mods == 0 || keysym == 0) return false;
    *modifiers = mods;
    *vk = keysym;
    return true;
}

// ---- X11 热键线程(K8):自有连接 + 命令队列 + 事件循环 --------------------
//
// 为什么是线程:XGrabKey 的 KeyPress 只送达**抓取方连接**;Qt 的 xcb 连接
// 在本环境拿不到(QNativeInterface 头缺失),故热键线程自建连接、自读事件,
// 命中的动作经 invokeMethod 队列投递回宿主(跨线程信号)。
//
// 生命周期:首个注册动作经 std::call_once 拉起,常驻到进程结束(XCloseDisplay
// 即自动解除本连接的全部抓键);Hotkeys 实例析构只清**自己的**注册与 owner。

struct X11HotkeyCmd {
    enum Kind { Grab, UngrabAll } kind = Grab;
    unsigned keysym = 0;   // Grab:目标 keysym
    unsigned mods = 0;     // Grab:X11 修饰掩码
    int id = 0;            // Grab:热键 id
    bool done = false;     // 线程完成后置位(registerAction 同步等待)
    bool ok = false;
};

QMutex g_x11Mutex;
QWaitCondition g_x11Cond;
QQueue<QSharedPointer<X11HotkeyCmd>> g_x11Cmds;
/// id → (keycode, mods):解抓与事件反查的注册表(进程级,抓键本就是服务器全局)。
QHash<int, QPair<unsigned, unsigned>> g_x11Grabs;
/// (keycode<<16 | mods) → id:KeyPress 反查表。
QHash<unsigned long, int> g_x11ByKey;
Hotkeys* g_x11Owner = nullptr;   // 事件回调目标(活实例;析构时置空)
std::thread g_x11Thread;
std::once_flag g_x11Once;

constexpr unsigned kLockCombos[] = {0u, Mod2Mask, LockMask, Mod2Mask | LockMask};
constexpr unsigned kModsMask = ControlMask | ShiftMask | Mod1Mask | Mod4Mask;

/// 锁位组合全抓(NumLock/CapsLock 开着时组合键才不失灵);任一被占即整体失败。
bool grabWithLocks(Display* d, unsigned kc, unsigned mods) {
    bool ok = true;
    for (const unsigned lock : kLockCombos) {
        if (XGrabKey(d, static_cast<int>(kc), mods | lock,
                     DefaultRootWindow(d), True, GrabModeAsync, GrabModeAsync) == 0)
            ok = false;
    }
    XSync(d, False);
    if (!ok) {
        for (const unsigned lock : kLockCombos)
            XUngrabKey(d, static_cast<int>(kc), mods | lock, DefaultRootWindow(d));
        XSync(d, False);
    }
    return ok;
}

void ungrabWithLocks(Display* d, unsigned kc, unsigned mods) {
    for (const unsigned lock : kLockCombos)
        XUngrabKey(d, static_cast<int>(kc), mods | lock, DefaultRootWindow(d));
    XSync(d, False);
}

void x11_hotkey_thread() {
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        QMutexLocker l(&g_x11Mutex);
        qWarning() << "X11 热键线程: XOpenDisplay 失败,全局热键不可用";
        // 失联状态下排空队列,让等待方拿到失败结果
        while (!g_x11Cmds.isEmpty()) {
            auto cmd = g_x11Cmds.dequeue();
            cmd->done = true;
            cmd->ok = false;
        }
        g_x11Cond.wakeAll();
        return;
    }
    while (true) {
        // 1) 命令(Grab / UngrabAll)
        {
            QMutexLocker l(&g_x11Mutex);
            while (!g_x11Cmds.isEmpty()) {
                auto cmd = g_x11Cmds.dequeue();
                if (cmd->kind == X11HotkeyCmd::Grab) {
                    const KeyCode kc = XKeysymToKeycode(d, static_cast<KeySym>(cmd->keysym));
                    if (kc == 0) {
                        cmd->ok = false;
                    } else if (grabWithLocks(d, kc, cmd->mods)) {
                        g_x11Grabs.insert(cmd->id, {kc, cmd->mods});
                        g_x11ByKey.insert((static_cast<unsigned long>(kc) << 16) | cmd->mods,
                                          cmd->id);
                        cmd->ok = true;
                    } else {
                        cmd->ok = false;
                    }
                } else {
                    // UngrabAll:解掉本连接的全部抓键并清注册表
                    for (auto it = g_x11Grabs.constBegin(); it != g_x11Grabs.constEnd(); ++it)
                        ungrabWithLocks(d, it.value().first, it.value().second);
                    g_x11Grabs.clear();
                    g_x11ByKey.clear();
                    cmd->ok = true;
                }
                cmd->done = true;
            }
            g_x11Cond.wakeAll();
        }
        // 2) 事件:抓取方连接才会收到 XGrabKey 的 KeyPress
        while (XPending(d) > 0) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type != KeyPress) continue;
            const unsigned kc = static_cast<unsigned>(ev.xkey.keycode);
            const unsigned mods = ev.xkey.state & kModsMask;
            int id = -1;
            Hotkeys* owner = nullptr;
            {
                QMutexLocker l(&g_x11Mutex);
                auto it = g_x11ByKey.find((kc << 16) | mods);
                if (it != g_x11ByKey.end()) id = it.value();
                owner = g_x11Owner;
            }
            if (id >= 0 && owner) {
                // 队列投递回宿主线程(hotkeyFired 的订阅在 Hotkeys 构造里)
                QMetaObject::invokeMethod(
                    owner, [owner, id] { emit owner->hotkeyFired(id); },
                    Qt::QueuedConnection);
            }
        }
        // 3) 心跳等待:X 事件 / 命令唤醒管道(命令由 100ms 轮询兜底处理)
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ConnectionNumber(d), &rf);
        timeval tv{0, 100 * 1000};
        ::select(ConnectionNumber(d) + 1, &rf, nullptr, nullptr, &tv);
    }
}

void x11EnsureThread() {
    std::call_once(g_x11Once, [] {
        g_x11Thread = std::thread(x11_hotkey_thread);
        // 常驻守护线程:static std::thread 在静态析构时若仍 joinable 会
        // std::terminate —— 进程级生命周期,显式 detach。
        g_x11Thread.detach();
    });
}

#endif  // Linux 线程机制

/// 截图防抖窗口:上一次热键触发后这么久内不再起新会话。
///
/// 为什么需要(2026-11-03,外部审查 #27):原实现只有 150ms 的延迟定时器,
/// 没有"已经在路上"的概念 —— 连按两次热键会排两个定时器,两次都执行
/// panel.hide() 和 startCaptureSession();第二个会话虽被 pruneOverlays() 拦住,
/// 但第一次的 hide() 已经无条件生效,且用户看到的是"按了没反应"。
/// 250ms 略大于 150ms 的延迟,覆盖"定时器还没到就又按一次"这段真实窗口。
constexpr int kCaptureDebounceMs = 250;
qint64 g_lastCaptureRequestMs = 0;

/// 截图前收起主面板并等窗口真正消失,否则面板会被拍进画面。
/// onCaptured:入库后接续的动作(截图翻译/取字);为空则只入库 + 写剪贴板。
void hidePanelThenCapture(PanelWindow& panel, CaptureIntent intent,
                          CaptureDone onCaptured = {}) {
    // 防抖:见 kCaptureDebounceMs。判定与占位必须在同一处,否则两次并发调用
    // 都能通过"看时间戳"这一关。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - g_lastCaptureRequestMs < kCaptureDebounceMs) {
        qInfo() << "热键触发过快,忽略本次(before" << now - g_lastCaptureRequestMs << "ms)";
        return;
    }
    g_lastCaptureRequestMs = now;
    panel.hide();
    // 带 context(qApp):应用销毁后队列里的这次触发会一并作废,不会在
    // 一个已析构的世界里 startCaptureSession。
    QTimer::singleShot(150, qApp, [intent, onCaptured] {
        startCaptureSession(intent, onCaptured);
    });
}

} // namespace

// 静态成员实现(头文件里声明为 static;自由函数接到成员签名)
bool Hotkeys::parseCombo(const QString& combo, unsigned* modifiers, unsigned* vk) {
    return parseComboImpl(combo, modifiers, vk);
}

/// WM_HOTKEY 分发器:QAbstractNativeEventFilter 挂 QApplication。
///
/// ⚠️ 生命周期必须严格配对(2026-11-02 修,崩溃现场见 docs/29)。
///
/// 原实现有两个致命点:
///   ① `new HotkeyFilter(this)` **从不释放**,而且析构时不 `removeNativeEventFilter`;
///   ② 成员是**裸指针** `Hotkeys* m_owner`,而 Hotkeys 在 main() 里是**栈对象**。
///
/// 于是程序退出时 Hotkeys 先析构,过滤器仍留在 qGuiApp 的事件过滤链上;此后任何
/// WM_HOTKEY 到达,就经由悬垂的 m_owner 访问已析构对象 → 0xC0000005。
/// 实测崩溃日志里 nextboard.exe offset=0xa2fdb 反复出现,正是这条路径。
///
/// 修法:
///   * 用 **QPointer** 而非裸指针 —— 对象没了就自动变 null,访问前判空;
///   * 析构里 `removeNativeEventFilter` —— 过滤器不能比宿主活得久;
///   * 由 Hotkeys 自己持有(成员,不 new),生命周期天然跟随。
class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    explicit HotkeyFilter(Hotkeys* owner) : m_owner(owner) {}
    ~HotkeyFilter() override {
        // 冗余保险:install 侧在 Hotkeys 构造里做,remove 也在这里配对。
        if (qGuiApp) qGuiApp->removeNativeEventFilter(this);
    }
    bool nativeEventFilter(const QByteArray& eventType, void* message, qint64* result) override {
        Q_UNUSED(result);
        // 宿主已析构(Hotkeys 是栈对象,退出期先走):直接放行,绝不碰 m_owner。
        if (m_owner.isNull()) return false;
#if defined(Q_OS_WIN)
        if (eventType == "windows_generic_MSG") {
            auto* msg = static_cast<MSG*>(message);
            if (msg->message == WM_HOTKEY) {
                emit m_owner->hotkeyFired(static_cast<int>(msg->wParam));
                return true;
            }
        }
#else
        // Linux(M3):X11 事件在**热键线程自己的连接**上处理(K8 设计 ——
        // XGrabKey 的 KeyPress 只送达抓取方连接),本过滤器对 X11 事件直通。
        Q_UNUSED(eventType);
        Q_UNUSED(message);
#endif
        return false;
    }
private:
    QPointer<Hotkeys> m_owner;
};

Hotkeys::Hotkeys(PanelWindow& panel, CoreBridge& core, QObject* parent)
    : QObject(parent), m_panel(panel), m_core(core) {
    // 过滤器由本对象**持有**:构造时装、析构时摘(见 HotkeyFilter 的注释)。
    // 原先 `new HotkeyFilter(this)` 是裸泄漏 —— 退出时宿主先析构,过滤器仍挂在
    // qGuiApp 上,WM_HOTKEY 一来就通过悬垂指针访问已析构对象 → 闪退。
    m_filter = std::make_unique<HotkeyFilter>(this);
    qGuiApp->installNativeEventFilter(m_filter.get());
    connect(this, &Hotkeys::hotkeyFired, this, [this](int id) {
        if (!m_registered.contains(id)) return;
        qInfo() << "热键触发: id =" << id
                << "action =" << static_cast<int>(m_registered.value(id));
        switch (m_registered.value(id)) {
            case Action::ToggleMain:
                m_panel.isVisible() ? m_panel.hide() : m_panel.showAtCursor();
                break;
            case Action::CaptureRegion:
                hidePanelThenCapture(m_panel, nb::CaptureIntent::Region);
                break;
            case Action::CaptureFullscreen:
                hidePanelThenCapture(m_panel, nb::CaptureIntent::Fullscreen);
                break;
            case Action::PinLatest:
                // 贴图逻辑统一收在 CoreFacade(面板右键「贴到屏幕」复用同一条路径)
                CoreFacade::instance().pinLatest();
                break;
            case Action::OpenTranslate:
                // 进入面板的翻译页;显示/切形态由 translatePrefill 的订阅方负责
                CoreFacade::instance().openTranslate();
                break;
            case Action::CaptureDelayed:
                // 延时截图(F16):等 N 秒再框选(等待秒数取设置,与托盘入口同一取值口径)
                m_panel.hide();
                QTimer::singleShot(150, qApp, [] {
                    startDelayedCapture(captureDelaySeconds(), CaptureIntent::Region);
                });
                break;
            // ⚠️ 截图翻译/截图取字/滚动长截图三 case 已移除(2026-11-01):
            //    工具栏按钮承担这三项(OCR取字/翻译/长截图),全局热键一并退役。
            default:
                qInfo() << "hotkey action" << static_cast<int>(m_registered.value(id));
                break;
        }
    });
    applyFromSettings();
}

Hotkeys::~Hotkeys() {
#if defined(Q_OS_WIN)
    for (int id : m_registered.keys()) ::UnregisterHotKey(nullptr, id);
#else
    // Linux(M3):解掉本实例注册的全部抓键(命令 + 同步等待),并撤销回调目标。
    // 若本实例就是登记的 owner,先等解抓完成再摘 owner —— 避免解抓期间事件
    // 投递到将死对象(QPointer 兜底在,但时序上先清更干净)。
    bool wasOwner = false;
    {
        QMutexLocker l(&g_x11Mutex);
        wasOwner = (g_x11Owner == this);
    }
    if (wasOwner) {
        auto cmd = QSharedPointer<X11HotkeyCmd>::create();
        cmd->kind = X11HotkeyCmd::UngrabAll;
        {
            QMutexLocker l(&g_x11Mutex);
            g_x11Cmds.enqueue(cmd);
            QDeadlineTimer dl(2000);
            while (!cmd->done && !dl.hasExpired()) g_x11Cond.wait(&g_x11Mutex, dl);
            if (g_x11Owner == this) g_x11Owner = nullptr;
        }
    }
#endif
    // ⚠️ 顺序很重要:显式摘掉过滤器,**再**让 m_filter 析构。
    //    HotkeyFilter 的析构里也做了一次(冗余保险),但这里显式摘更清楚 ——
    //    读者不该靠"某个成员析构顺带做的事"来理解退出安全性。
    if (qGuiApp && m_filter) qGuiApp->removeNativeEventFilter(m_filter.get());
}

void Hotkeys::applyFromSettings() {
    // 先撤销既有注册:改设置后重新应用时必须先清干净 —— registerAction 每次分配
    // 新 id,不清理的话旧组合键会一直留在系统里(改了等于没改,还多占一个组合键)。
#if defined(Q_OS_WIN)
    for (int id : m_registered.keys()) ::UnregisterHotKey(nullptr, id);
#else
    // 重应用:解掉本实例全部旧抓键(命令 + 同步等待),注册表由线程清空
    {
        QMutexLocker l(&g_x11Mutex);
        bool wasOwner = (g_x11Owner == this);
        if (wasOwner) {
            auto cmd = QSharedPointer<X11HotkeyCmd>::create();
            cmd->kind = X11HotkeyCmd::UngrabAll;
            g_x11Cmds.enqueue(cmd);
            QDeadlineTimer dl(2000);
            while (!cmd->done && !dl.hasExpired()) g_x11Cond.wait(&g_x11Mutex, dl);
        }
    }
#endif
    m_registered.clear();
    m_failures.clear();

    const QJsonObject resp = m_core.call("get_settings");
    const QJsonObject hotkeys = resp.value("data").toObject().value("hotkeys").toObject();
    const QMap<Action, QString> names{
        {Action::ToggleMain, "toggleMain"},
        {Action::CaptureRegion, "captureRegion"},
        {Action::CaptureFullscreen, "captureFullscreen"},
        {Action::PinLatest, "pinLatest"},
        {Action::OpenTranslate, "openTranslate"},
        {Action::CaptureDelayed, "captureDelayed"},
    };
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        const QString combo = hotkeys.value(it.value()).toString();
        if (!combo.isEmpty()) registerAction(it.key(), combo);
    }
    // 注册失败/格式非法不能只进日志:用户刚在设置里改完,不提示就会以为已生效
    // (冲突回滚语义保持:失败项不影响其余动作,只是明确告诉用户哪几个没成)。
    if (!m_failures.isEmpty()) {
        CoreFacade::instance().showToast(
            QStringLiteral("快捷键不可用:%1").arg(m_failures.join(QStringLiteral("、"))));
    }
}

void Hotkeys::registerAction(Action action, const QString& combo) {
#if !defined(Q_OS_WIN)
    // Linux(M3):X11 XGrabKey,经**热键线程的连接**执行(命令 + 同步等待)。
    // 失败走同一"失败项"通道,语义与 Windows 对称:该动作不可用,不影响其他动作。
    unsigned mods = 0, keysym = 0;
    if (!parseCombo(combo, &mods, &keysym)) {
        m_failures.append(QStringLiteral("%1(格式非法)").arg(combo));
        qWarning() << "热键格式非法,跳过:" << combo;
        return;
    }
    x11EnsureThread();
    const int id = kHotkeyIdBase + g_nextHotkeyId.fetch_add(1);
    auto cmd = QSharedPointer<X11HotkeyCmd>::create();
    cmd->kind = X11HotkeyCmd::Grab;
    cmd->keysym = keysym;
    cmd->mods = mods;
    cmd->id = id;
    {
        QMutexLocker l(&g_x11Mutex);
        g_x11Owner = this;   // 活实例登记(事件回调目标)
        g_x11Cmds.enqueue(cmd);
        QDeadlineTimer dl(2000);
        while (!cmd->done && !dl.hasExpired()) g_x11Cond.wait(&g_x11Mutex, dl);
    }
    if (!cmd->done) {
        m_failures.append(QStringLiteral("%1(热键通道超时)").arg(combo));
        qWarning() << "热键注册等待超时:" << combo;
        return;
    }
    if (!cmd->ok) {
        // 被占 = 已被别的客户端抓走(其他热键软件);文案与 Windows 对称
        const QString why = QStringLiteral("组合键已被注册(本进程或其它程序)");
        m_failures.append(QStringLiteral("%1(%2)").arg(combo, why));
        qWarning() << "热键抓取失败(" << why << "):" << combo;
        return;
    }
    m_registered.insert(cmd->id, action);
    qInfo() << "热键已注册(X11):" << combo;
#else
    unsigned mods = 0, vk = 0;
    if (!parseCombo(combo, &mods, &vk)) {
        m_failures.append(QStringLiteral("%1(格式非法)").arg(combo));
        qWarning() << "热键格式非法,跳过:" << combo;
        return;
    }
    // 进程级分配(见文件头注释);每个 id 只属于注册它的那个实例。
    const int id = kHotkeyIdBase + g_nextHotkeyId.fetch_add(1);
    if (!::RegisterHotKey(nullptr, id, mods, vk)) {
        // ⚠️ 报原因要分清「本进程内已被占用」与「别的程序占用」:前者是**我们的**
        // 缺陷(哪个实例还在用这个 id),后者才该让用户去关别的程序。混为一谈会让
        // 真问题伪装成用户环境问题 —— 2026-11-03 的验收日志里正是 1409 刷屏而
        // 文案写着"被其他程序占用"。
        const DWORD gle = ::GetLastError();
        // ⚠️ 1409 (ERROR_HOTKEY_ALREADY_REGISTERED) 两种情形**共用一个错误码**:
        // 本进程另一处已注册同一组合键,或别的程序占着。GetLastError 区分不出来,
        // 文案就不该断言是哪一种(上一版写成"本进程内重复注册",反而误导排查)。
        const QString why = gle == ERROR_HOTKEY_ALREADY_REGISTERED
            ? QStringLiteral("组合键已被注册(本进程或其它程序)")
            : QStringLiteral("注册被系统拒绝");
        m_failures.append(QStringLiteral("%1(%2)").arg(combo, why));
        qWarning() << "热键注册失败(" << why << "):" << combo << "id =" << Qt::hex << id
                   << Qt::dec << "GetLastError =" << gle;
        return; // 冲突回滚语义:该动作不可用,不影响其他动作(第一代 hotkey.rs)
    }
    m_registered.insert(id, action);
    qInfo() << "热键已注册:" << combo;
#endif  // Q_OS_WIN
}

} // namespace nb
