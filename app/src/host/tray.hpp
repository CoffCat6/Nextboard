// 托盘:打开主面板 / 今日待办 / 截图 / 暂停监听 / 退出。
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

    /// 待办到点(docs/05-todo.md §4.3):拉取待播报列表 → 逐条弹气泡 →
    /// 回写"已播报"标记(宿主确认制去重,气泡只弹一次)。
    void onTodoDue();

private:
    void buildMenu();
    /// 重建「今日待办」二级菜单的条目(docs/05-todo.md §6.6):
    /// 每次一级菜单 aboutToShow 时调用 —— 不搞后台轮询,打开菜单才拉数。
    void refreshTodoSubmenu();

    PanelWindow& m_panel;
    CoreBridge& m_core;
    class QMenu* m_menu = nullptr;
    /// 「今日待办」二级菜单:内容在每次菜单展开时重建(§6.6)。
    class QMenu* m_todoMenu = nullptr;
    /// 「暂停/恢复监听」菜单项:文案随状态切换(见 tray.cpp —— 刻意**不用**
    /// 勾选态表达状态,理由在那里)。
    QAction* m_pauseAction = nullptr;
    /// 「延时截图」菜单项:展开菜单时把当前等待秒数写进标题。
    QAction* m_delayAction = nullptr;
};

} // namespace nb
