// 全局热键:Windows = RegisterHotKey;Linux(M3)= X11 XGrabKey(XWayland 兼容,
// Wayland 原生会话走 XDG portal,后续按需接入 —— docs/29 §5.2/§8)。
// 接口保持平台无关(不引平台头)。
#pragma once

#include <QObject>
#include <QStringList>

#include <QMap>

#include <memory>

namespace nb {

class PanelWindow;
class CoreBridge;
class HotkeyFilter;

class Hotkeys : public QObject {
    Q_OBJECT
public:
    /// 动作 id(与设置里的 hotkeys 表一致)。
    /// ⚠️ 截图翻译/截图取字/滚动长截图三动作已移除(2026-11-01):
    /// 功能由截图工具栏按钮承担(OCR取字/翻译/长截图),不再有全局热键。
    enum class Action {
        ToggleMain,
        CaptureRegion,
        CaptureFullscreen,
        PinLatest,
        OpenTranslate,
        CaptureDelayed,   ///< 延时截图(F16):倒计时后再框选
    };

    Hotkeys(PanelWindow& panel, CoreBridge& core, QObject* parent = nullptr);
    ~Hotkeys() override;

signals:
    /// WM_HOTKEY / X11 KeyPress 到达(hotkey id)。
    void hotkeyFired(int id);

public slots:
    /// 从 nbcore 设置读取 hotkeys 表并注册;失败的动作不影响其他动作,
    /// 但会汇总成一条 toast 告知用户(否则"设置成功"的假象无人察觉)。
    void applyFromSettings();

private:
    void registerAction(Action action, const QString& combo);
    // unsigned(= Win32 UINT,ABI 一致):接口层不引平台头的做法。
    // Linux:modifiers = X11 修饰掩码,vk = keysym。
    static bool parseCombo(const QString& combo, unsigned* modifiers, unsigned* vk);

    PanelWindow& m_panel;
    CoreBridge& m_core;
    /// 事件过滤器。**由本对象持有**(成员而非裸 new),生命周期严格跟随 ——
    /// 详见 hotkeys.cpp 里 HotkeyFilter 的注释(2026-11-02 闪退修复)。
    /// 用 std::unique_ptr 而不是值成员:HotkeyFilter 的完整定义在本文件之外,
    /// 值成员需要完整类型。
    std::unique_ptr<HotkeyFilter> m_filter;
    // key = 注册 id,value = action。id 由**进程级**计数器分配(hotkeys.cpp),
    // 不与实例生命周期绑定 —— 否则验收流的临时实例会注销掉主实例的热键。
    QMap<int, Action> m_registered;
    /// 本轮 applyFromSettings 的失败项("组合键(原因)"),轮末汇总提示后清空。
    QStringList m_failures;
};

} // namespace nb
