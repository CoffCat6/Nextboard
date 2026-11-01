#include "hotkeys.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"
#include "host/core_bridge.hpp"
#include "capture/capture_session.hpp"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QImage>
#include <QTimer>
#include <QDebug>

namespace nb {

namespace {
constexpr int kHotkeyIdBase = 0xB000;

bool parseComboImpl(const QString& combo, UINT* modifiers, UINT* vk) {
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
        else if (part.length() == 1) key = LOBYTE(VkKeyScanW(part.at(0).toUpper().unicode()));
        else if (part.startsWith("f") && part.mid(1).toInt() >= 1 && part.mid(1).toInt() <= 24)
            key = VK_F1 + part.mid(1).toInt() - 1;
    }
    if (mods == 0 || key == 0) return false;
    *modifiers = mods;
    *vk = key;
    return true;
}

/// 截图前收起主面板并等窗口真正消失,否则面板会被拍进画面。
/// onCaptured:入库后接续的动作(截图翻译/取字);为空则只入库 + 写剪贴板。
void hidePanelThenCapture(PanelWindow& panel, CaptureIntent intent,
                          CaptureDone onCaptured = {}) {
    panel.hide();
    QTimer::singleShot(150, [intent, onCaptured] {
        startCaptureSession(intent, onCaptured);
    });
}

} // namespace

// 静态成员实现(头文件里声明为 static;自由函数接到成员签名)
bool Hotkeys::parseCombo(const QString& combo, UINT* modifiers, UINT* vk) {
    return parseComboImpl(combo, modifiers, vk);
}

/// WM_HOTKEY 分发器:QAbstractNativeEventFilter 挂 QApplication。
class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    explicit HotkeyFilter(Hotkeys* owner) : m_owner(owner) {}
    bool nativeEventFilter(const QByteArray& eventType, void* message, qint64* result) override {
        Q_UNUSED(result);
        if (eventType == "windows_generic_MSG") {
            auto* msg = static_cast<MSG*>(message);
            if (msg->message == WM_HOTKEY) {
                emit m_owner->hotkeyFired(static_cast<int>(msg->wParam));
                return true;
            }
        }
        return false;
    }
private:
    Hotkeys* m_owner;
};

Hotkeys::Hotkeys(PanelWindow& panel, CoreBridge& core, QObject* parent)
    : QObject(parent), m_panel(panel), m_core(core) {
    auto* filter = new HotkeyFilter(this);
    qGuiApp->installNativeEventFilter(filter);
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
                QTimer::singleShot(150, [] {
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
    for (int id : m_registered.keys()) ::UnregisterHotKey(nullptr, id);
}

void Hotkeys::applyFromSettings() {
    // 先撤销既有注册:改设置后重新应用时必须先清干净 —— registerAction 每次分配
    // 新 id,不清理的话旧组合键会一直留在系统里(改了等于没改,还多占一个组合键)。
    for (int id : m_registered.keys()) ::UnregisterHotKey(nullptr, id);
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
    UINT mods = 0, vk = 0;
    if (!parseCombo(combo, &mods, &vk)) {
        m_failures.append(QStringLiteral("%1(格式非法)").arg(combo));
        qWarning() << "热键格式非法,跳过:" << combo;
        return;
    }
    const int id = kHotkeyIdBase + m_nextId++;
    if (!::RegisterHotKey(nullptr, id, mods, vk)) {
        m_failures.append(QStringLiteral("%1(被其他程序占用)").arg(combo));
        qWarning() << "热键注册失败(可能被占用):" << combo << "GetLastError =" << ::GetLastError();
        return; // 冲突回滚语义:该动作不可用,不影响其他动作(第一代 hotkey.rs)
    }
    m_registered.insert(id, action);
    qInfo() << "热键已注册:" << combo;
}

} // namespace nb
