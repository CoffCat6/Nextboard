#include "tray.hpp"
#include "capture/capture_session.hpp"
#include "../app_font.hpp"
#include "note/note_registry.hpp"
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

namespace {
// 托盘「今日待办」子菜单的条目上限(§6.6):8×~30px 条目单屏从容,
// 也是"一眼可扫读"的合理上限;超出部分显示"…共 N 条"(total 由 core 精确 COUNT)。
constexpr int kTrayTodoMax = 8;
} // namespace

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
    // ⚠️ 选择器必须同时覆盖**二级菜单**:QSS 作用于目标窗口及其全部子窗口,
    //    但 #nbTrayMenu 只匹配一级菜单本体,子菜单(独立窗口)会退回原生样式
    //    (原生灰底/灰字,与这里的浅色主题割裂)。子菜单显式命名后并列进选择器。
    //    另:一级菜单现在**有了子菜单箭头**,当初"右内边距 24px 是给箭头预留、
    //    无子菜单时纯空占"的历史结论不再成立 —— 箭头列已必要,内边距保持 8px
    //    会让文字与箭头贴得很近,属可接受(条目短),不再为它单独加规则。
    m_menu->setStyleSheet(QStringLiteral(
        "QMenu#nbTrayMenu, QMenu#nbTrayTodoMenu "
        "{ background: #FFFFFF; border: 1px solid #D9E0E7; padding: 2px; }"
        "QMenu#nbTrayMenu::item, QMenu#nbTrayTodoMenu::item "
        "{ color: #17202A; padding: 4px 8px; border-radius: 4px; }"
        "QMenu#nbTrayMenu::item:selected, QMenu#nbTrayTodoMenu::item:selected "
        "{ background: #E8F1FF; color: #17202A; }"
        "QMenu#nbTrayMenu::item:disabled, QMenu#nbTrayTodoMenu::item:disabled "
        "{ color: #B6BEC8; }"
        "QMenu#nbTrayMenu::separator, QMenu#nbTrayTodoMenu::separator "
        "{ height: 1px; background: #D9E0E7; margin: 4px 8px; }"));
    m_menu->addAction("打开主面板", [this] { m_panel.showCentered(); });

    // ---- 「今日待办」二级菜单(§6.6):不打开面板就能看到今天还有什么 ----
    // 一级项只做入口+计数(具体待办放不下,既定决策);条目在 refreshTodoSubmenu
    // 里于每次菜单展开时重建(aboutToShow 钩子,与「延时截图」动态文案同点)。
    m_todoMenu = new QMenu(QStringLiteral("今日待办"), m_menu);
    m_todoMenu->setObjectName(QStringLiteral("nbTrayTodoMenu"));
    m_todoMenu->addAction(QStringLiteral("今天没有待办"))->setEnabled(false);
    QAction* const todoEntry = m_menu->addMenu(m_todoMenu);
    Q_UNUSED(todoEntry);
    connect(m_todoMenu, &QMenu::triggered, this, [this](QAction* action) {
        // 条目 action 的 data = 条目 id;"查看全部"是空 id → 不定位只切视图
        const QString id = action->data().toString();
        if (id.isEmpty()) CoreFacade::instance().showCenteredOnTodos();
        else CoreFacade::instance().openTodoEntry(id);
    });

    // ---- 「便签」二级菜单(docs/20 §7.2)----
    // ⚠️ 这是**必需的兜底**,不是锦上添花:便签是 Qt::Tool 窗口,不占任务栏、
    //    不进 Alt+Tab,被别的置顶窗口盖住时就再也点不到。core_facade.cpp 里
    //    "漏一次用户就再也找不回来"的教训对便签同样成立。
    m_noteMenu = new QMenu(QStringLiteral("桌面便签"), m_menu);
    m_noteMenu->setObjectName(QStringLiteral("nbTrayTodoMenu"));
    m_noteMenu->addAction(QStringLiteral("暂无便签"))->setEnabled(false);
    m_menu->addMenu(m_noteMenu);
    connect(m_noteMenu, &QMenu::triggered, this, [this](QAction* action) {
        const QString cmd = action->data().toString();
        if (cmd == QLatin1String("expand-all")) {
            CoreFacade::instance().expandAllTodoNotes();
        } else if (cmd == QLatin1String("collapse-all")) {
            CoreFacade::instance().collapseAllTodoNotes();
        } else if (!cmd.isEmpty()) {
            // 点某张便签:展开并前置(已开着的窗口由注册表负责抬到最前)
            CoreFacade::instance().openTodoNote(cmd);
        }
    });

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
    // 滚动长截图(docs/19):与热键 Ctrl+Shift+L 同一条意图 —— 框完直接进长截图就绪态。
    // 托盘也放一个入口,是因为"有热键但用户不知道"与"没热键"一样够不到功能。
    m_menu->addAction("滚动长截图", [capture] { capture(CaptureIntent::Scrolling); });
    // 延时截图(F16):按下后先去展开菜单/等悬浮提示,倒计时结束才开始框选。
    // 等待秒数**点击时**才读(改设置即时生效,不必重启);与快捷键入口同一取值口径。
    m_delayAction = m_menu->addAction(QStringLiteral("延时截图"));
    connect(m_delayAction, &QAction::triggered, this, [this] {
        m_panel.hide();   // 与即时截图同理:面板必须先收起,否则会被拍进画面
        QTimer::singleShot(150, [] {
            startDelayedCapture(captureDelaySeconds(), CaptureIntent::Region);
        });
    });
    // 菜单每次展开时把当前等待秒数写进标题,并重建「今日待办」子菜单:
    // 不搞后台轮询,打开菜单才拉数(§6.6);两条动态内容共用这一个钩子。
    connect(m_menu, &QMenu::aboutToShow, this, [this] {
        m_delayAction->setText(
            QStringLiteral("延时截图（%1 秒）").arg(captureDelaySeconds()));
        refreshTodoSubmenu();
        refreshNoteSubmenu();
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

void Tray::refreshNoteSubmenu() {
    m_noteMenu->clear();
    const QVariantList notes = NoteRegistry::instance().describe();
    if (notes.isEmpty()) {
        m_noteMenu->addAction(QStringLiteral("暂无便签"))->setEnabled(false);
        m_noteMenu->addSeparator();
        QAction* const hint = m_noteMenu->addAction(
            QStringLiteral("（在待办卡片右键「贴到桌面」）"));
        hint->setEnabled(false);
        return;
    }
    // 逐张列出:id 作为 data,点击即展开前置(即便窗口被盖住也能找回来)
    for (const QVariant& raw : notes) {
        const QVariantMap note = raw.toMap();
        const QString id = note.value(QStringLiteral("id")).toString();
        // 摘要从 core 现取(便签窗口自身不缓存正文给托盘看):一次主键查询,
        // 且能顺带发现"条目已被删"的残留(那种情况下面会显示占位文案)。
        const QVariantMap got = CoreFacade::instance().call(
            QStringLiteral("todo_get"), {{QStringLiteral("id"), id}});
        QString label = QStringLiteral("(已失效)");
        if (got.value(QStringLiteral("ok")).toBool()) {
            const QString text = got.value(QStringLiteral("data")).toMap()
                                     .value(QStringLiteral("textContent")).toString();
            if (!text.isEmpty()) {
                label = text.section(QLatin1Char('\n'), 0, 0).left(20);
                if (text.section(QLatin1Char('\n'), 1).size() > 0) label += QStringLiteral("…");
            }
        }
        if (note.value(QStringLiteral("collapsed")).toBool()) {
            label = QStringLiteral("〔收起〕") + label;
        }
        QAction* const entry = m_noteMenu->addAction(label);
        entry->setData(id);
    }
    m_noteMenu->addSeparator();
    m_noteMenu->addAction(QStringLiteral("全部展开"))->setData(QStringLiteral("expand-all"));
    m_noteMenu->addAction(QStringLiteral("全部收起"))->setData(QStringLiteral("collapse-all"));
}

void Tray::refreshTodoSubmenu() {
    // 口径(docs/05-todo.md §6.6):未完成 且 (已逾期 或 今天到期)。
    // core 侧 due=today 过滤(等价 due_at ≤ 今天末尾,逾期是其子集),
    // 排序沿用 §5(逾期>今日>未来>无期限);total 精确,是"…共 N 条"的前提。
    const QJsonObject resp = m_core.call("todo_list", QJsonObject{
        {"done", QJsonValue(QStringLiteral("open"))},
        {"due", QJsonValue(QStringLiteral("today"))},
        {"limit", QJsonValue(static_cast<double>(kTrayTodoMax))},
        {"offset", QJsonValue(0)},
    });
    // ⚠️ 重建前先断开 triggered 连接再清空条目:clear() 只删 action,
    //    不删 lambda 里对 this 的隐式捕获关系,但被删 action 的连接随对象
    //    销毁自动断开 —— 这里直接重建即可,无需手工断连(注释防止后人怀疑)。
    m_todoMenu->clear();

    if (!resp.value("ok").toBool()) {
        // FFI 失败(核心未起/被占用):给出禁用态提示,不弹错误打扰
        m_todoMenu->addAction(QStringLiteral("暂不可用"))->setEnabled(false);
        m_todoMenu->addSeparator();
        m_todoMenu->addAction(QStringLiteral("查看全部"),
                              [this] { CoreFacade::instance().showCenteredOnTodos(); });
        return;
    }
    const QJsonArray items = resp.value("data").toObject().value("items").toArray();
    const int total = resp.value("data").toObject().value("total").toInt();

    if (items.isEmpty()) {
        m_todoMenu->addAction(QStringLiteral("今天没有待办"))->setEnabled(false);
    } else {
        for (const QJsonValue& v : items) {
            const QJsonObject item = v.toObject();
            const QString id = item.value("id").toString();
            const QString text = item.value("textContent").toString();
            const qint64 dueAt = static_cast<qint64>(
                item.value("dueAt").toDouble());
            const QJsonObject todo = item.value("todo").toObject();
            const bool overdue = todo.value("overdue").toBool();
            // 状态前缀:逾期 → "已逾期";今天到期 → "今天 HH:mm"(与卡片徽标同口径)
            QString status = overdue ? QStringLiteral("已逾期")
                : QStringLiteral("今天 %1").arg(
                      QDateTime::fromMSecsSinceEpoch(dueAt).toString(
                          QStringLiteral("HH:mm")));
            // 正文首行截 20 字:QMenu 无横向滚动,长文会把菜单撑到离谱
            QString line = text.section(QLatin1Char('\n'), 0, 0).left(20);
            if (text.section(QLatin1Char('\n'), 1).size() > 0) line += QStringLiteral("…");
            QAction* const entry = m_todoMenu->addAction(
                QStringLiteral("%1 · %2").arg(status, line));
            entry->setData(id);
        }
        if (total > items.size()) {
            QAction* const more = m_todoMenu->addAction(
                QStringLiteral("…共 %1 条").arg(total));
            more->setEnabled(false);
        }
    }
    m_todoMenu->addSeparator();
    m_todoMenu->addAction(QStringLiteral("查看全部"),
                          [this] { CoreFacade::instance().showCenteredOnTodos(); });
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
