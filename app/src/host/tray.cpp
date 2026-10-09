#include "tray.hpp"
#include "capture/capture_session.hpp"
#include "host/memory_probe.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QDebug>
#include <QFont>
#include <QMenu>
#include <QStyle>
#include <QTimer>

namespace nb {

Tray::Tray(PanelWindow& panel, CoreBridge& core, QObject* parent)
    : QSystemTrayIcon(parent), m_panel(panel), m_core(core) {
    // 托盘图标与应用图标同源(main.cpp 已 setWindowIcon)。
    // ico 自带 16/20/24/32… 多尺寸,QIcon 会按通知区域实际大小挑最合适的一档,
    // 不需要在这里再缩放。取不到时退回系统图标,保证托盘不会变成空白占位。
    QIcon appIcon(QStringLiteral(":/resources/nextboard.ico"));
    if (appIcon.isNull())
        appIcon = QApplication::windowIcon();
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
    QFont menuFont = QApplication::font();
    if (menuFont.pointSizeF() <= 0.0)
        menuFont.setPointSizeF(9.0);
    m_menu->setFont(menuFont);
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
    m_menu->addSeparator();
    // 截图 / 贴图入口(对齐第一代托盘):截图前先收起面板,否则面板会被拍进画面;
    // 延时 150ms 等窗口真正消失(与热键链路 hidePanelThenCapture 同一取舍)。
    const auto capture = [this](CaptureIntent intent) {
        m_panel.hide();
        QTimer::singleShot(150, [intent] { startCaptureSession(intent); });
    };
    m_menu->addAction("矩形截图", [capture] { capture(CaptureIntent::Region); });
    m_menu->addAction("全屏截图", [capture] { capture(CaptureIntent::Fullscreen); });
    // 延时截图(F16):按下后先去展开菜单/等悬浮提示,倒计时结束才开始框选。
    // 等待秒数**点击时**才读(改设置即时生效,不必重启);与快捷键入口同一取值口径。
    m_delayAction = m_menu->addAction(QStringLiteral("延时截图"));
    connect(m_delayAction, &QAction::triggered, this, [this] {
        m_panel.hide();   // 与即时截图同理:面板必须先收起,否则会被拍进画面
        QTimer::singleShot(150, [] {
            startDelayedCapture(captureDelaySeconds(), CaptureIntent::Region);
        });
    });
    // 菜单每次展开时把当前等待秒数写进标题:不进设置也能知道会等多久
    connect(m_menu, &QMenu::aboutToShow, this, [this] {
        m_delayAction->setText(
            QStringLiteral("延时截图（%1 秒）").arg(captureDelaySeconds()));
        m_memoryAction->setText(QStringLiteral("内存 %1").arg(memorySummary()));
    });
    // 口径与热键一致(默认"最近复制",可在设置里切换为"图片优先")
    m_menu->addAction("贴图（最近内容）", [] { CoreFacade::instance().pinLatest(); });
    m_menu->addSeparator();
    m_pauseAction = m_menu->addAction("暂停监听");
    m_pauseAction->setCheckable(true);
    const QJsonObject currentSettings = m_core.call("get_settings").value("data").toObject();
    const bool monitoring = currentSettings.value("monitorEnabled").toBool(true);
    m_pauseAction->setChecked(!monitoring);
    m_pauseAction->setText(monitoring ? "暂停监听" : "恢复监听");
    connect(m_pauseAction, &QAction::toggled, this, [this](bool paused) {
        // 监听开关持久化在 nbcore 设置里;这里读-改-写
        const QJsonObject resp = m_core.call("get_settings");
        QJsonObject settings = resp.value("data").toObject();
        settings.insert("monitorEnabled", !paused);
        m_core.call("save_settings", settings);
        m_pauseAction->setText(paused ? "恢复监听" : "暂停监听");
    });
    m_menu->addSeparator();
    // 内存读数(只读):验收内存时不必另开任务管理器。与日志 mem[...] 同一口径。
    m_memoryAction = m_menu->addAction(QStringLiteral("内存 --"));
    m_memoryAction->setEnabled(false);
    m_menu->addSeparator();
    m_menu->addAction("退出", [] {
        QApplication::quit();
    });
}

} // namespace nb
