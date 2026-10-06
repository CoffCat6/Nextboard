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
};

} // namespace nb
