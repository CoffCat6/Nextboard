#include "single_instance.hpp"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDebug>

#include <windows.h>

#include "panel/panel_window.hpp"

namespace {
constexpr UINT WM_APP_ACTIVATE = 0x8000 + 10; // WM_APP+10:二次启动唤起
}

/// 唤起消息接收:对主面板窗口执行 显示+前置。
class ActivateFilter : public QAbstractNativeEventFilter {
public:
    explicit ActivateFilter(nb::PanelWindow* panel) : m_panel(panel) {}
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qint64* result) override {
        if (eventType == "windows_generic_MSG") {
            auto* msg = static_cast<MSG*>(message);
            if (msg->message == WM_APP_ACTIVATE && m_panel) {
                m_panel->showAtCursor();
                if (result) *result = 0;
                return true;
            }
        }
        return false;
    }
private:
    nb::PanelWindow* m_panel;
};

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() {
    if (m_mutex) ::ReleaseMutex(static_cast<HANDLE>(m_mutex));
    if (m_mutex) ::CloseHandle(static_cast<HANDLE>(m_mutex));
}

bool SingleInstance::ensure(const QString& key, unsigned long* lastError) {
    // 注意:互斥量名不得带 "Nextboard\\" 这类自定义前缀路径(实测在本沙箱/作业对象
    // 环境下 CreateMutexW 对含反斜杠的非保留前缀名返回 ERROR_PATH_NOT_FOUND);
    // 用 plain name + 进程内一次性初始化即可满足 v1 单实例语义。
    Q_UNUSED(key);
    ::SetLastError(0);
    m_mutex = ::CreateMutexW(nullptr, TRUE, L"NextboardSingleInstance");
    const DWORD gle = ::GetLastError();
    if (lastError) *lastError = gle;
    return m_mutex && gle != ERROR_ALREADY_EXISTS;
}

void SingleInstance::activateExisting() {
    // 广播:已有实例的隐藏消息窗口(主面板 QWidget winId)会收到 WM_APP_ACTIVATE。
    // 用 PostMessage(HWND_BROADCAST) 简化 v1;后续可换命名共享内存记录精确 HWND。
    ::PostMessageW(HWND_BROADCAST, WM_APP_ACTIVATE, 0, 0);
}
