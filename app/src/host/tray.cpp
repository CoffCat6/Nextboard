#include "tray.hpp"
#include "capture/capture_session.hpp"
#include "../app_font.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
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
            m_panel.showCentered();
    });
    // 气泡点击 → 弹面板并切到"待办"视图(§4.3)。⚠️ messageClicked 在部分
    // Win10/11 环境不触发(系统通知中心设置/免打扰);那时气泡本身已起到提示作用,
    // 用户从托盘图标也能打开面板看到角标 —— 冒烟清单覆盖这条降级路径。
    connect(this, &QSystemTrayIcon::messageClicked, this, [] {
        CoreFacade::instance().showCenteredOnTodos();
    });
    show();
}

void Tray::buildMenu() {
    m_menu = new QMenu();
    // 托盘菜单用**独立的、更小的**字号(见 app_font.hpp 的 kTrayMenuFontPixelSize):
    // 它是系统级弹层,与桌面其它右键菜单并排,用界面正文的 14px 会明显偏大。
    //
    // ⚠️ 原先这里写的是 `if (pointSizeF() <= 0.0) setPointSizeF(9.0)`,靠
    //    "应用字体没有磅值"兜底 —— 而应用字体是 setPixelSize 设的,pointSizeF()
    //    恒为 -1,该分支**必然触发**,字号实际由这条隐式路径决定。现在字号显式,
    //    不再依赖意外行为。
    m_menu->setFont(nb::uiFont(kTrayMenuFontPixelSize));
    // Windows 原生菜单样式不跟随 QPalette 的深色化,深色系统下文字会变得极浅;
    // 显式 QSS 固定浅色底 + 深色字(theme.md §7 浅色配色),与系统菜单观感一致
    //
    // ⚠️ 限定到本菜单(`QMenu#nbTrayMenu`):样式表会作用于**目标窗口及其全部子窗口**,
    // 而托盘菜单的弹出子菜单是其子窗口。若不加 selector,深色主题下未打开过设置抽屉时
    // (无人重置 QSS)这段浅色规则会残留并套到后续弹出的子菜单上。
    //
    // 内边距:条目 4px 上下 / 左右各 8px,菜单 2px。
    // 收紧过程是量测驱动的(见 docs/progress.md「四.2」):
    //   · 右内边距原 24px 是给**子菜单箭头**留的,而本菜单没有子菜单 → 纯空占;
    //   · 左内边距原 12px 是给**勾选列**留的,勾选列已随 setCheckable 一起去掉;
    //   · 左右改成等宽:不对称原本只为让出勾选列,现在没有理由,等宽更稳。
    // ⚠️ 随之删掉了 `::indicator { width: 14px; height: 14px; }` 规则 ——
    //    本菜单已无可勾选项,该规则无用。**若日后又加回可勾选项**,必须同时
    //    补回 indicator 规则,否则勾选标记会走 QSS 默认样式,深色下可能看不清。
    m_menu->setObjectName(QStringLiteral("nbTrayMenu"));
    m_menu->setStyleSheet(QStringLiteral(
        "QMenu#nbTrayMenu { background: #FFFFFF; border: 1px solid #D9E0E7; padding: 2px; }"
        "QMenu#nbTrayMenu::item { color: #17202A; padding: 4px 8px; border-radius: 4px; }"
        "QMenu#nbTrayMenu::item:selected { background: #E8F1FF; color: #17202A; }"
        "QMenu#nbTrayMenu::item:disabled { color: #B6BEC8; }"
        "QMenu#nbTrayMenu::separator { height: 1px; background: #D9E0E7; margin: 4px 8px; }"));
    m_menu->addAction("打开主面板", [this] { m_panel.showCentered(); });
    m_menu->addAction("打开翻译", [] { CoreFacade::instance().openTranslateCentered(); });
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
    });
    // 口径与热键一致(默认"最近复制",可在设置里切换为"图片优先")
    m_menu->addAction("贴图（最近内容）", [] { CoreFacade::instance().pinLatest(); });
    m_menu->addSeparator();
    m_pauseAction = m_menu->addAction(QStringLiteral("暂停监听"));
    // ⚠️ 刻意**不设** setCheckable:暂停/恢复的状态已由文案本身表达
    //    ("暂停监听" ⇄ "恢复监听"),勾选标记是冗余的 —— 而 QMenu 只要存在
    //    任何一个可勾选项,就会给**所有**条目预留一列勾选列宽(实测 18px),
    //    这是本菜单偏宽的最大单项。去掉后整菜单立减 18px。
    const QJsonObject currentSettings = m_core.call("get_settings").value("data").toObject();
    const bool monitoring = currentSettings.value("monitorEnabled").toBool(true);
    m_pauseAction->setText(monitoring ? QStringLiteral("暂停监听")
                                     : QStringLiteral("恢复监听"));
    connect(m_pauseAction, &QAction::triggered, this, [this] {
        // 监听开关持久化在 nbcore 设置里;这里读-改-写。
        // 状态从设置现读,不再依赖 QAction 的勾选态(已不用勾选表达状态)。
        const QJsonObject resp = m_core.call("get_settings");
        QJsonObject settings = resp.value("data").toObject();
        const bool nowMonitoring = settings.value("monitorEnabled").toBool(true);
        settings.insert("monitorEnabled", !nowMonitoring);
        m_core.call("save_settings", settings);
        m_pauseAction->setText(nowMonitoring ? QStringLiteral("恢复监听")
                                             : QStringLiteral("暂停监听"));
    });
    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("退出"), [] {
        QApplication::quit();
    });
}

void Tray::onTodoDue() {
    // 拉取"已到点但还没播报过"的待办(宿主确认制:气泡被用户关掉也不再重复弹,
    // 但面板角标仍会亮 —— 两层去重的第二层,见 docs/05-todo.md §4.2)。
    const QJsonObject resp = m_core.call("todo_due_pending");
    if (!resp.value("ok").toBool()) return;
    const QJsonArray items = resp.value("data").toArray();
    if (items.isEmpty()) return;

    QStringList shownIds;
    for (const QJsonValue& v : items) {
        const QJsonObject item = v.toObject();
        const QString id = item.value("id").toString();
        const QString text = item.value("text").toString();
        const qint64 at = static_cast<qint64>(item.value("at").toDouble());
        const bool missed = item.value("missed").toBool();
        // 标题 = 正文首行截 24 字(多行正文塞进气泡标题会被系统截断成乱码观感)
        QString title = text.section(QLatin1Char('\n'), 0, 0).left(24);
        if (missed) title = QStringLiteral("错过的提醒:") + title;
        const QString body = QStringLiteral("截止 %1,点击查看")
                                 .arg(QDateTime::fromMSecsSinceEpoch(at)
                                          .toString(QStringLiteral("MM-dd HH:mm")));
        showMessage(title, body, QSystemTrayIcon::Information, 8000);
        shownIds.append(id);
    }
    if (!shownIds.isEmpty()) {
        QJsonArray ids;
        for (const QString& id : shownIds) ids.append(id);
        m_core.call("todo_due_mark_shown", QJsonObject{{"ids", ids}});
    }
}

} // namespace nb
