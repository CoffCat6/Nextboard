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
    /// 「暂停/恢复监听」菜单项:文案随状态切换(见 tray.cpp —— 刻意**不用**
    /// 勾选态表达状态,理由在那里)。
    QAction* m_pauseAction = nullptr;
    /// 「延时截图」菜单项:展开菜单时把当前等待秒数写进标题。
    QAction* m_delayAction = nullptr;
};

} // namespace nb
