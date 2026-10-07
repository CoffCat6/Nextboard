#include "tray.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QMenu>
#include <QStyle>

namespace nb {

Tray::Tray(PanelWindow& panel, CoreBridge& core, QObject* parent)
    : QSystemTrayIcon(parent), m_panel(panel), m_core(core) {
    // 托盘图标与应用图标同源(main.cpp 已 setWindowIcon)。
    // ico 自带 16/20/24/32… 多尺寸,QIcon 会按通知区域实际大小挑最合适的一档,
    // 不需要在这里再缩放。取不到时退回系统图标,保证托盘不会变成空白占位。
    QIcon appIcon = QApplication::windowIcon();
    if (appIcon.isNull())
        appIcon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    setIcon(appIcon);
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
    // Windows 原生菜单样式不跟随 QPalette 的深色化,深色系统下文字会变得极浅;
    // 显式 QSS 固定浅色底 + 深色字(theme.md §7 浅色配色),与系统菜单观感一致
    m_menu->setStyleSheet(QStringLiteral(
        "QMenu { background: #FFFFFF; border: 1px solid #D9E0E7; padding: 4px; }"
        "QMenu::item { color: #17202A; padding: 6px 24px 6px 12px; border-radius: 4px; }"
        "QMenu::item:selected { background: #E8F1FF; color: #17202A; }"
        "QMenu::item:disabled { color: #B6BEC8; }"
        "QMenu::separator { height: 1px; background: #D9E0E7; margin: 4px 8px; }"
        "QMenu::indicator { width: 14px; height: 14px; }"));
    m_menu->addAction("打开主面板", [this] { m_panel.showAtCursor(); });
    m_menu->addAction("打开翻译窗口", [] { CoreFacade::instance().openTranslate(); });
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
