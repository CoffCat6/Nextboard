// 热键生命周期验收(--hotkey-lifecycle-flow)。
//
// ## 为什么单开一条
//
// 2026-11-02 用户报告"有些应用下快捷键冲突导致 Nextboard 闪退"。崩溃日志里
// nextboard.exe offset=0xa2fdb 反复出现,根因是 WM_HOTKEY 过滤器的生命周期:
//   * `new HotkeyFilter(this)` 裸 new,从不释放,析构也不摘过滤器;
//   * 成员是裸指针 `Hotkeys* m_owner`,而 Hotkeys 在 main() 里是**栈对象**。
// 于是退出时 Hotkeys 先析构,过滤器仍在 qGuiApp 的事件链上 → 悬垂 → 0xC0000005。
//
// 这条 flow 的作用:**把"退出期收到 WM_HOTKEY 不崩"变成一个可重复的断言**。
// 它构造一个短命的 Hotkeys,让其析构后**继续向事件循环投递模拟的 WM_HOTKEY**,
// 然后断言进程仍然活着、且过滤器确实被摘掉了。
//
// 真正的崩溃只在"过滤器没摘 + 宿主已死"时发生,所以:
//   - 断言 ①:Hotkeys 析构后,事件过滤器数量回到构造前的值(摘干净了);
//   - 断言 ②:析构后投一条 WM_HOTKEY,进程不崩、且返回 false(没人吞它)。
#include "host/acceptance.hpp"

#include "host/core_bridge.hpp"
#include "host/hotkeys.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QTimer>

#include <windows.h>

namespace nb {
namespace {

int g_hkFailures = 0;

void hkCheck(bool ok, const QString& what) {
    if (ok) {
        qInfo().noquote() << "HOTKEY-LIFECYCLE  PASS" << what;
    } else {
        ++g_hkFailures;
        qWarning().noquote() << "HOTKEY-LIFECYCLE  FAIL" << what;
    }
}

/// 数一数当前挂了多少个原生事件过滤器。
///
/// QCoreApplication 没有公开计数 API,但 `nativeEventFilter` 链是私有的 ——
/// 所以改用**行为探针**:自己装一个过滤器并观察它是否被调用。
/// 这里换一个更直接的做法:记录"我们的哨兵过滤器"在 WM_HOTKEY 时是否被调用过。
/// 过滤器是链式的,Qt 会依次调用直到有人返回 true。若 HotkeyFilter 已被摘掉,
/// 哨兵就能收到消息。
class SentinelFilter : public QAbstractNativeEventFilter {
public:
    bool sawHotkey = false;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qint64*) override {
        if (eventType == "windows_generic_MSG") {
            auto* msg = static_cast<MSG*>(message);
            if (msg->message == WM_HOTKEY) {
                sawHotkey = true;
                return false;   // 不吞,让链继续
            }
        }
        return false;
    }
};

/// 直接向事件循环投一条 WM_HOTKEY(id 用我们的基数,确保看着像真的)。
void postNativeHotkey(int id) {
    MSG msg{};
    msg.hwnd = nullptr;
    msg.message = WM_HOTKEY;
    msg.wParam = static_cast<WPARAM>(id);
    // QCoreApplication::sendEvent 不管原生消息;走 qApp 的过滤器链要经
    // QAbstractEventDispatcher 的 nativeEventFilter 通道 —— 最直接的等效做法是
    // 自己调一遍过滤器链的入口。Qt 内部是 QCoreApplicationPrivate::sendThroughObjectEventFilters,
    // 不公开,故这里用"手工投递到 HotkeyFilter 会看到的位置":
    // 走 ::PostThreadMessage 太慢且要真正的热键注册,反而引入不确定性。
    //
    // 实际做法:用一个直接调用 —— QGuiApplication::notify 不覆盖原生消息,
    // 所以我们调用的是"和 Qt 内部同一入口"的公开替代:QCoreApplication::instance()
    // 的 eventDispatcher 的 filterNativeEvent。
    QAbstractEventDispatcher* d = QCoreApplication::eventDispatcher();
    if (d) {
        qintptr r = 0;
        d->filterNativeEvent(QByteArrayLiteral("windows_generic_MSG"), &msg, &r);
    }
}

} // namespace

void runHotkeyLifecycleFlow(PanelWindow& panel) {
    QTimer::singleShot(1200, qApp, [&panel] {
        CoreBridge& core = CoreBridge::instance();

        // 哨兵:挂在链尾部,用来观察 WM_HOTKEY 是否被前一个过滤器吞掉。
        SentinelFilter sentinel;
        qApp->installNativeEventFilter(&sentinel);

        // ---- ① 宿主活着时:WM_HOTKEY 应被 HotkeyFilter 吞掉(哨兵看不到)----
        {
            auto* hotkeys = new Hotkeys(panel, core);
            Q_UNUSED(hotkeys);
            sentinel.sawHotkey = false;
            postNativeHotkey(0xB001);
            qInfo() << "HOTKEY-LIFECYCLE  宿主存活时哨兵是否收到 =" << sentinel.sawHotkey;
            hkCheck(!sentinel.sawHotkey,
                    QStringLiteral("宿主存活时 WM_HOTKEY 被 HotkeyFilter 接管(哨兵未收到)"));

            // ---- ② 析构宿主 → 过滤器必须被摘掉 ----
            delete hotkeys;
            sentinel.sawHotkey = false;
            postNativeHotkey(0xB001);
            qInfo() << "HOTKEY-LIFECYCLE  宿主析构后哨兵是否收到 =" << sentinel.sawHotkey;
            // 这是本次崩溃的直接回归点:
            //   修复前 —— HotkeyFilter 仍挂在链上且 m_owner 悬垂,
            //             访问已析构对象 = 0xC0000005(进程直接消失,断言根本没机会跑);
            //   修复后 —— 过滤器已被摘掉,消息落到哨兵,进程安然无恙。
            hkCheck(sentinel.sawHotkey,
                    QStringLiteral("宿主析构后过滤器已摘除,WM_HOTKEY 落到哨兵(无悬垂访问)"));
        }

        // ---- ③ 反复建/删:不泄漏过滤器(链不会越挂越长)----
        //
        // 判据:多轮之后,哨兵仍然能在宿主析构后收到消息。
        // 若过滤器泄漏,链上会积累多个悬垂的 HotkeyFilter,第一个就可能崩溃。
        {
            bool survived = true;
            for (int i = 0; i < 5 && survived; ++i) {
                auto* hk = new Hotkeys(panel, core);
                delete hk;
                sentinel.sawHotkey = false;
                postNativeHotkey(0xB001);
                if (!sentinel.sawHotkey) survived = false;
            }
            hkCheck(survived, QStringLiteral("连续 5 轮建/删后无泄漏过滤器残留"));
        }

        qApp->removeNativeEventFilter(&sentinel);

        qInfo() << "HOTKEY-LIFECYCLE  RESULT failures =" << g_hkFailures;
        QApplication::quit();
    });
}

} // namespace nb
