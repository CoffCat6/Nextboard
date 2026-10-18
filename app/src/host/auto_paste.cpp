#include "auto_paste.hpp"

#include "core_bridge.hpp"

#include <QDebug>
#include <QJsonObject>
#include <QTimer>

#include <windows.h>

namespace nb {
namespace {

/// 投一次 Ctrl+V。
///
/// 用**扫描码**(KEYEVENTF_SCANCODE)而不是虚拟键码:SendInput 文档明确要求在
/// 可能时给出扫描码,否则某些输入法/远程桌面/游戏窗口收不到(虚拟键码路径要由
/// 系统反查键盘布局,反查失败就静默丢键)。0x1D = LeftCtrl,0x2F = V(Set 1)。
///
/// 按下与抬起之间**必须**有小间隔:一次性投 4 个事件时,部分应用(实测 Electron
/// 系与部分远程桌面客户端)只能看到"已按下"而看不到配对抬起,结果是 Ctrl 卡住。
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
    const UINT sent = ::SendInput(4, inputs, sizeof(INPUT));
    if (sent != 4) {
        // 最常见原因:前台窗口以更高完整性级别运行(UIPI)→ 事件被静默丢弃。
        // 只记日志:此时"什么都没发生"与用户没开该功能无从区分,但弹窗更糟。
        qWarning() << "auto-paste: SendInput 只投出" << sent << "个事件(可能被 UIPI 拦截)"
                   << "lasterror =" << ::GetLastError();
    } else {
        qInfo() << "auto-paste: 已投出 Ctrl+V";
    }
}

} // namespace

void pasteToForegroundIfEnabled(int delayMs) {
    // 设置每次现读:低频动作(用户点一次复制),不值得为它做缓存与失效管理;
    // 反而缓存会让"刚在设置里打开开关 → 立刻复制没生效"看起来像 bug。
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return;
    if (!resp.value("data").toObject().value("copyAutoPaste").toBool()) return;

    // 延后一拍:此刻面板可能还在收起过程中(hide() 之后前台窗口需要一两拍才拿到
    // 焦点)。面板仍是前台时投键 = 粘到面板自己身上(搜索框多一段文本)。
    QTimer::singleShot(delayMs > 0 ? delayMs : 1, [] {
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
    });
}

} // namespace nb
