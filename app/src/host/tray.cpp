#include "tray.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QMenu>
#include <QStyle>

namespace nb {

Tray::Tray(PanelWindow& panel, CoreBridge& core, QObject* parent)
    : QSystemTrayIcon(parent), m_panel(panel), m_core(core) {
    setIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));
    setToolTip("Nextboard");
    buildMenu();
    setContextMenu(m_menu);
    connect(this, &QSystemTrayIcon::activated, this, [this](ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            m_panel.showAtCursor();
    });
    show();
}

void Tray::buildMenu() {
    m_menu = new QMenu();
    m_menu->addAction("打开主面板", [this] { m_panel.showAtCursor(); });
    m_pauseAction = m_menu->addAction("暂停监听");
    m_pauseAction->setCheckable(true);
    connect(m_pauseAction, &QAction::toggled, this, [this](bool paused) {
        // 监听开关持久化在 nbcore 设置里;这里读-改-写
        const QJsonObject resp = m_core.call("get_settings");
        QJsonObject settings = resp.value("data").toObject();
        settings.insert("monitorEnabled", !paused);
        m_core.call("save_settings", settings);
        m_pauseAction->setText(paused ? "恢复监听" : "暂停监听");
    });
    m_menu->addSeparator();
    m_menu->addAction("退出", [] {
        QApplication::quit();
    });
}

} // namespace nb
