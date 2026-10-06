#include "hotkeys.hpp"
#include "panel/panel_window.hpp"
#include "host/core_bridge.hpp"
#include "capture/capture_session.hpp"

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
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
                nb::startCaptureSession(nb::CaptureIntent::Region);
                break;
            case Action::CaptureFullscreen:
                nb::startCaptureSession(nb::CaptureIntent::Fullscreen);
                break;
            case Action::PinLatest:
                // S2.5 PoC:贴最近图片条目;完整逻辑在 S3 数据联通后补
                qInfo() << "pinLatest (PoC stub)";
                break;
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
    const QJsonObject resp = m_core.call("get_settings");
    const QJsonObject hotkeys = resp.value("data").toObject().value("hotkeys").toObject();
    const QMap<Action, QString> names{
        {Action::ToggleMain, "toggleMain"},
        {Action::CaptureRegion, "captureRegion"},
        {Action::CaptureFullscreen, "captureFullscreen"},
        {Action::PinLatest, "pinLatest"},
        {Action::OpenTranslate, "openTranslate"},
        {Action::CaptureTranslate, "captureTranslate"},
    };
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        const QString combo = hotkeys.value(it.value()).toString();
        if (!combo.isEmpty()) registerAction(it.key(), combo);
    }
}

void Hotkeys::registerAction(Action action, const QString& combo) {
    UINT mods = 0, vk = 0;
    if (!parseCombo(combo, &mods, &vk)) {
        qWarning() << "热键格式非法,跳过:" << combo;
        return;
    }
    const int id = kHotkeyIdBase + m_nextId++;
    if (!::RegisterHotKey(nullptr, id, mods, vk)) {
        qWarning() << "热键注册失败(可能被占用):" << combo << "GetLastError =" << ::GetLastError();
        return; // 冲突回滚语义:该动作不可用,不影响其他动作(第一代 hotkey.rs)
    }
    m_registered.insert(id, action);
    qInfo() << "热键已注册:" << combo;
}

} // namespace nb
