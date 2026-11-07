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
// Linux(M0b):组合键解析暂无消费者(注册为桩);保留平台对称签名。
bool parseComboImpl(const QString& combo, unsigned* modifiers, unsigned* vk) {
    Q_UNUSED(combo);
    Q_UNUSED(modifiers);
    Q_UNUSED(vk);
    return false;
}
#endif

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
        // Linux(M0b):X11 XCB 事件分派随 M3 热键接入(docs/29 §5.2);当前为直通。
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
    // Linux(M0b):全局热键暂未接入 —— X11 XGrabKey / portal GlobalShortcuts 在
    // M3(docs/29 §5.2/§8)。走同一"失败项"通道给可读提示(降级契约),注册表
    // 语义与 Windows 保持对称:该动作不可用,不影响其他动作。
    Q_UNUSED(action);
    m_failures.append(QStringLiteral("%1(Linux 全局热键暂未接入)").arg(combo));
    return;
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
