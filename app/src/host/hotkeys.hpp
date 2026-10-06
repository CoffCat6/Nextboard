// 全局热键:RegisterHotKey 管理(多动作 → 多组合键,冲突回滚沿用第一代语义)。
#pragma once

#include <QObject>

#include <windows.h>

#include <QMap>

namespace nb {

class PanelWindow;
class CoreBridge;

class Hotkeys : public QObject {
    Q_OBJECT
public:
    /// 动作 id(与设置里的 hotkeys 表一致)。
    enum class Action { ToggleMain, CaptureRegion, CaptureFullscreen, PinLatest, OpenTranslate, CaptureTranslate };

    Hotkeys(PanelWindow& panel, CoreBridge& core, QObject* parent = nullptr);
    ~Hotkeys() override;

signals:
    /// WM_HOTKEY 到达(hotkey id)。
    void hotkeyFired(int id);

public slots:
    /// 从 nbcore 设置读取 hotkeys 表并注册;失败的动作记日志不阻塞。
    void applyFromSettings();

private:
    // QWidget 无关:用 QAbstractNativeEventFilter 收 WM_HOTKEY 更直接
    bool nativeEvent(const QByteArray& eventType, void* message, qint64* result);

private:
    void registerAction(Action action, const QString& combo);
    static bool parseCombo(const QString& combo, UINT* modifiers, UINT* vk);

    PanelWindow& m_panel;
    CoreBridge& m_core;
    // hotkey id 从 1 递增;key = 注册 id,value = action
    QMap<int, Action> m_registered;
    int m_nextId = 1;
};

} // namespace nb
