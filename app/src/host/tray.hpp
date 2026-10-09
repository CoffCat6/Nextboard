// 托盘:打开主面板 / 截图 / 暂停监听 / 退出。
#pragma once

#include <QSystemTrayIcon>
#include <QAction>

#include "host/core_bridge.hpp"

namespace nb {

class PanelWindow;

class Tray : public QSystemTrayIcon {
    Q_OBJECT
public:
    Tray(PanelWindow& panel, CoreBridge& core, QObject* parent = nullptr);

private:
    void buildMenu();

    PanelWindow& m_panel;
    CoreBridge& m_core;
    class QMenu* m_menu = nullptr;
    QAction* m_pauseAction = nullptr;
    /// 「延时截图」菜单项:展开菜单时把当前等待秒数写进标题。
    QAction* m_delayAction = nullptr;
    /// 内存读数(禁用态,仅展示):展开菜单时刷新,与日志 mem[...] 同一口径。
    QAction* m_memoryAction = nullptr;
};

} // namespace nb
