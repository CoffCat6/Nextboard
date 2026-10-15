#include "host/acceptance.hpp"

#include "host/core_bridge.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QScreen>
#include <QTime>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>

namespace nb {

void runTodoAcceptanceFlow(PanelWindow& panel) {
    // 待办交互验收:分步驱动,每步读回状态打日志(人工/脚本据此断言)。
    // 目的:回归三个交互缺陷 —— ①浮层打不开(几何溢出面板)
    // ②时间设了却保存不上(字段没写回)③必须点别处才关轮(确定无效)。
    QTimer::singleShot(4000, &panel, [&panel] {
        // 目标时间:明天 09:30(与"现在"明显不同,便于识别"没写回")
        const QDateTime target = QDateTime(QDate::currentDate().addDays(1), QTime(9, 30));
        const qint64 ms = target.toMSecsSinceEpoch();

        // ① 打开卡片 + 点「截止」字段(浮层保持打开以便观测几何)
        panel.invokeAcceptance("acceptanceOpenPicker");
        QTimer::singleShot(600, &panel, [&panel, ms, target] {
            const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
            const bool insideX = st.value("pickerX").toDouble() >= 0
                && st.value("pickerX").toDouble() + st.value("pickerW").toDouble()
                   <= st.value("overlayW").toDouble();
            const bool insideY = st.value("pickerY").toDouble() >= 0
                && st.value("pickerY").toDouble() + st.value("pickerH").toDouble()
                   <= st.value("overlayH").toDouble();
            qInfo() << "STEP1 card:" << st.value("cardVisible").toBool()
                    << "pickerVisible:" << st.value("pickerVisible").toBool()
                    << "geom x/y/w/h/contentH:" << st.value("pickerX").toDouble()
                    << st.value("pickerY").toDouble() << st.value("pickerW").toDouble()
                    << st.value("pickerH").toDouble() << st.value("pickerContentH").toDouble()
                    << "overlay w/h:" << st.value("overlayW").toDouble()
                    << st.value("overlayH").toDouble()
                    << "insideX:" << insideX << "insideY:" << insideY;

            // ② 拨轮 → **真实点击「确定」** → 值应写回字段且浮层关闭
            //    ⚠️ 三处关键:
            //    a. 拨轮必须改 currentIndex(走 onCurrentIndexChanged →
            //       rebuildFromTumblers → accepted 的真实链路),不能用
            //       setFromMs(那是预填路径)—— 早先多版验收都用了预填
            //       路径或直调 commit,把真 bug 掩盖了。
            //    b. 点击必须投真实鼠标事件(QQuickWidget 层),直接调
            //       onClicked 或 sendEvent 给 QQuickWindow 都绕过命中路径。
            //    c. 预期值 = **今天 + 目标小时 + 打开时归位的分钟**
            //       (拨的是"时"列;分/秒/毫秒仍是打开归位时的当前值),
            //       不是原始 targetMs(明天 09:30)。
            panel.setRootProperty("acceptanceTargetMs", QVariant(static_cast<double>(ms)));
            panel.invokeAcceptance("acceptanceSetWheel");
            const QDateTime now0 = QDateTime::currentDateTime();
            // ⚠️ 分钟粒度截断(设计 §3:UI 按分钟选择):归位保留到分钟,
            //    秒/毫秒清零 —— 预期值必须同口径,否则差几秒恒不匹配。
            const QDateTime wheelExpected(
                QDate::currentDate(),
                QTime(target.time().hour(), now0.time().minute(), 0, 0));
            const qint64 wheelMs = wheelExpected.toMSecsSinceEpoch();
            const bool clicked = panel.clickByObjectName(QStringLiteral("todoPickerConfirm"));
            qInfo() << "STEP2 clicked confirm:" << clicked
                    << "expected wheelMs:" << wheelMs;
            QTimer::singleShot(500, &panel, [&panel, wheelMs, target] {
                const QVariantMap st2 = panel.rootProperty("acceptanceState").toMap();
                const qint64 fieldMs =
                    static_cast<qint64>(st2.value("dueFieldMs").toDouble());
                qInfo() << "STEP2 pickerClosed:" << !st2.value("pickerVisible").toBool()
                        << "dueFieldMs:" << fieldMs << "expected:" << wheelMs
                        << "match:" << (fieldMs == wheelMs && fieldMs > 0)
                        << "remindAutoFilled(=due-10min):" << static_cast<qint64>(
                               st2.value("remindFieldMs").toDouble());

                // ③ 输入正文 → **真实点击「保存」** → 库中应有该条目
                panel.setRootProperty("acceptanceInputText",
                                      QVariant(QStringLiteral("验收待办")));
                panel.invokeAcceptance("acceptanceSetText");
                const bool saved =
                    panel.clickByObjectName(QStringLiteral("todoComposerSave"));
                qInfo() << "STEP3 clicked save:" << saved;
                QTimer::singleShot(1200, &panel, [&panel, fieldMs] {
                    const QVariantMap st3 = panel.rootProperty("acceptanceState").toMap();
                    qInfo() << "STEP3 composerClosed:"
                            << !st3.value("cardVisible").toBool()
                            << "errorText:" << st3.value("errorText").toString();
                    const QVariantMap resp = CoreFacade::instance().call(
                        "todo_list", QVariantMap{{"done", "all"}});
                    const QVariantList items =
                        resp.value("data").toMap().value("items").toList();
                    qInfo() << "STEP4 persisted items:" << items.size();
                    if (!items.isEmpty()) {
                        const QVariantMap row = items.first().toMap();
                        const qint64 due = static_cast<qint64>(row.value("dueAt").toDouble());
                        // 期望值 = STEP2 读回的**字段值**(滚轮组合出的时刻),
                        // 而不是原始 targetMs —— 拨轮只拨了"时"列,
                        // 日期部分是打开时归位的"今天",两者本就不同。
                        qInfo() << "STEP4 text:" << row.value("textContent").toString()
                                << "dueAt:" << due << "fieldMs:" << fieldMs
                                << "match:" << (due == fieldMs && fieldMs > 0)
                                << "remindAt:" << static_cast<qint64>(
                                       row.value("remindAt").toDouble());
                    }

                    // ⑤ 待办视图智能分组自检(§6.3):切到待办视图读回各行分组键 ——
                    //    组标题的显示边界 = "本行键 != 上一行键",键序本身正确即边界正确。
                    //    (真实渲染无法在此断言,故只验证键序列;渲染由真机目视。)
                    panel.invokeAcceptance("acceptanceShowTodoGroups");
                    QTimer::singleShot(600, &panel, [&panel] {
                        const QVariantMap st4 = panel.rootProperty("acceptanceState").toMap();
                        qInfo() << "STEP5 viewId:" << st4.value("viewId").toString()
                                << "rowGroups:" << st4.value("rowGroups").toString();
                        QApplication::quit();
                    });
                });
            });
        });
    });
}

void runClearDoneFlow(PanelWindow& panel) {
    // 清空已完成待办验收(破坏性操作,值得一条常驻回归钩子):
    //   播种 2 条已完成 + 1 条未完成 → 切待办视图 → 真实点击组头「清空已完成」
    //   → 真实点击确认「清空」→ 读库断言:归档全部清除、未完成不受影响。
    // ⚠️ 只断言"没有 done 残留 + 未完成条目仍在",不断言总行数 ——
    //    验收数据目录可能残留其它待办,数量断言会误报。
    QTimer::singleShot(3500, &panel, [&panel] {
        CoreFacade& core = CoreFacade::instance();
        // 清掉前次运行留下的验收条目(按前缀识别):保证数据量恒定,
        // 目标行必在首屏可视区内 —— 委托没被创建时点击探针会找不到元素。
        const QVariantMap pre = core.call("todo_list", QVariantMap{{"done", "all"}});
        for (const QVariant& v : pre.value("data").toMap().value("items").toList()) {
            const QVariantMap m = v.toMap();
            if (m.value("textContent").toString().startsWith(QStringLiteral("验收-")))
                core.call("delete_entry", QVariantMap{{"id", m.value("id").toString()}});
        }
        auto create = [&core](const QString& text, bool done) {
            const QVariantMap r = core.call("todo_create", QVariantMap{{"text", text}});
            if (!r.value("ok").toBool()) return false;
            const QString id = r.value("data").toMap().value("id").toString();
            if (!done || id.isEmpty()) return !done;
            const QVariantMap d = core.call("todo_set_done", QVariantMap{{"id", id}, {"done", true}});
            return d.value("ok").toBool();
        };
        const bool seeded = create(QStringLiteral("验收-已完成A"), true)
                            && create(QStringLiteral("验收-已完成B"), true)
                            && create(QStringLiteral("验收-未完成C"), false);
        qInfo() << "CLEAR-DONE STEP1 seeded:" << seeded;

        panel.invokeAcceptance("acceptanceShowTodoGroups");   // 切待办视图(入口只在待办视图出现)
        QTimer::singleShot(900, &panel, [&panel] {
            const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
            qInfo() << "CLEAR-DONE STEP2 viewId:" << st.value("viewId").toString()
                    << "rowGroups:" << st.value("rowGroups").toString();
            // 真实点击「已完成」组头的清空入口(委托内元素,靠 objectName 递归找)
            const bool clickedLink = panel.clickByObjectName(QStringLiteral("clearDoneLink"));
            qInfo() << "CLEAR-DONE STEP3 clickedLink:" << clickedLink;
            QTimer::singleShot(500, &panel, [&panel] {
                const QVariantMap st2 = panel.rootProperty("acceptanceState").toMap();
                qInfo() << "CLEAR-DONE STEP4 confirmCardVisible:"
                        << st2.value("clearDoneVisible").toBool();
                const bool clickedConfirm =
                    panel.clickByObjectName(QStringLiteral("clearDoneConfirm"));
                qInfo() << "CLEAR-DONE STEP5 clickedConfirm:" << clickedConfirm;
                QTimer::singleShot(900, &panel, [&panel] {
                    const QVariantMap resp = CoreFacade::instance().call(
                        "todo_list", QVariantMap{{"done", "all"}});
                    const QVariantList items =
                        resp.value("data").toMap().value("items").toList();
                    int doneLeft = 0, openKept = 0;
                    for (const QVariant& v : items) {
                        const QVariantMap m = v.toMap();
                        if (m.value("todo").toMap().value("done").toBool()) ++doneLeft;
                        else if (m.value("textContent").toString()
                                 == QStringLiteral("验收-未完成C")) ++openKept;
                    }
                    qInfo() << "CLEAR-DONE STEP6 doneLeft:" << doneLeft
                            << "openKept:" << (openKept > 0)
                            << "| 判据:doneLeft==0 且 openKept==true";
                    QApplication::quit();
                });
            });
        });
    });
}

void runClearAllFlow(PanelWindow& panel) {
    // 清空全部数据验收(docs/05-todo.md §1.3 语义调整:清空 = 全量重置,含待办):
    //   播种 1 条剪贴板 + 2 条待办(1 未完成 1 已完成)→ 真实点击设置按钮开抽屉
    //   → 把「清空全部数据」滚进可视区 → 真实点击按钮 → 真实点击「确认清空」
    //   → 读库断言:counts 全口径归零、待办视图清空(todo_meta 无孤儿由单测兜底)。
    QTimer::singleShot(3500, &panel, [&panel] {
        CoreFacade& core = CoreFacade::instance();
        const bool clip = core.call("save_text_entry",
                                    QVariantMap{{"text", QStringLiteral("验收-清空-剪贴板")},
                                                {"source", "clipboard"}})
                              .value("ok").toBool();
        const QVariantMap t1 =
            core.call("todo_create", QVariantMap{{"text", QStringLiteral("验收-清空-未完成")}});
        const QVariantMap t2 =
            core.call("todo_create", QVariantMap{{"text", QStringLiteral("验收-清空-已完成")}});
        const bool done = core.call(
            "todo_set_done",
            QVariantMap{{"id", t2.value("data").toMap().value("id").toString()}, {"done", true}})
                              .value("ok").toBool();
        qInfo() << "CLEAR-ALL STEP1 seeded clip:" << clip
                << "todos:" << (t1.value("ok").toBool() && t2.value("ok").toBool())
                << "doneMarked:" << done;

        const bool opened = panel.clickByObjectName(QStringLiteral("settingsButton"));
        qInfo() << "CLEAR-ALL STEP2 clickedOpen:" << opened;
        QTimer::singleShot(800, &panel, [&panel] {
            const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
            qInfo() << "CLEAR-ALL STEP2 drawerOpen:" << st.value("settingsOpen").toBool();
            // 危险区在表单尾部:不先滚进可视区,按钮点击会落空
            panel.invokeAcceptance("acceptanceRevealClearAll");
            QTimer::singleShot(700, &panel, [&panel] {
                const bool clickedClear = panel.clickByObjectName(QStringLiteral("clearAllBtn"));
                qInfo() << "CLEAR-ALL STEP3 clickedClear:" << clickedClear;
                QTimer::singleShot(700, &panel, [&panel] {
                    const bool clickedConfirm =
                        panel.clickByObjectName(QStringLiteral("clearAllConfirmBtn"));
                    qInfo() << "CLEAR-ALL STEP4 clickedConfirm:" << clickedConfirm;
                    QTimer::singleShot(900, &panel, [&panel] {
                        const QVariantMap counts = CoreFacade::instance().call("entry_counts");
                        const QVariantMap todos = CoreFacade::instance().call(
                            "todo_list", QVariantMap{{"done", "all"}});
                        const int all = counts.value("data").toMap().value("all").toInt();
                        const int todosLeft =
                            todos.value("data").toMap().value("items").toList().size();
                        qInfo() << "CLEAR-ALL STEP5 counts.all:" << all
                                << "todosLeft:" << todosLeft
                                << "| 判据:all==0 且 todosLeft==0";
                        QApplication::quit();
                    });
                });
            });
        });
    });
}

void reportTodoGroups(PanelWindow& panel) {
    // 只读自检:切待办视图 → 读回各行分组键。配合"脚本先播种各分组待办"使用,
    // 断言组标题的分界点(键序变化处即标题出现处)。
    // 之后切回「全部」视图,顺带验证筛选浮层的数据源(方案 A:来源/标签已并入
    // 浮层,旧的 sourceBar 不再存在)。
    QTimer::singleShot(3000, &panel, [&panel] {
        panel.invokeAcceptance("acceptanceShowTodoGroups");
        QTimer::singleShot(800, &panel, [&panel] {
            const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
            qInfo() << "GROUPS viewId:" << st.value("viewId").toString()
                    << "keys:" << st.value("rowGroups").toString()
                    << "| filterActive:" << st.value("filterActive").toBool()
                    << "sourceChips:" << st.value("sourceChips").toString();
            panel.invokeAcceptance("acceptanceShowAll");
            QTimer::singleShot(800, &panel, [&panel] {
                const QVariantMap st2 = panel.rootProperty("acceptanceState").toMap();
                qInfo() << "GROUPS viewId:" << st2.value("viewId").toString()
                        << "| sourceChips:" << st2.value("sourceChips").toString()
                        << "| tagChips:" << st2.value("tagChips").toString()
                        << "| filterActive:" << st2.value("filterActive").toBool();
                // 打开筛选浮层 → 读回状态(验证浮层可用且角标口径正确)
                panel.invokeAcceptance("acceptanceOpenFilter");
                QTimer::singleShot(400, &panel, [&panel] {
                    const QVariantMap st3 = panel.rootProperty("acceptanceState").toMap();
                    qInfo() << "FILTER popupOpen:" << st3.value("filterPopupOpen").toBool()
                            << "active:" << st3.value("filterActive").toBool()
                            << "count:" << st3.value("filterCount").toInt();
                    // 验证"仅打开、鼠标未进入"时**不自动收回**(自动收回要求先进过
                    // 浮层):开浮层后停 1s 再读,仍开着才算通过 —— 否则用户点开
                    // 去看选项、手在别处,浮层立刻被关,选项都来不及看。
                    QTimer::singleShot(1000, &panel, [&panel] {
                        const QVariantMap kept = panel.rootProperty("acceptanceState").toMap();
                        qInfo() << "FILTER autoHideKept popupOpen:"
                                << kept.value("filterPopupOpen").toBool();
                        // 设一个来源筛选 → 角标应变 1、列表应收窄、popup 仍开着(浮层里选完
                        // 就关要再开一次,太啰嗦 —— 选完不关,由用户看效果后自行关闭)。
                        panel.invokeAcceptance("acceptanceSetSource");
                        QTimer::singleShot(600, &panel, [&panel] {
                            const QVariantMap st4 = panel.rootProperty("acceptanceState").toMap();
                            const QVariantMap resp = CoreFacade::instance().call(
                                "list_entries", QVariantMap{{"source", "capture"},
                                                            {"limit", 50}, {"offset", 0}});
                            qInfo() << "FILTER afterSet active:" << st4.value("filterActive").toBool()
                                    << "count:" << st4.value("filterCount").toInt()
                                    << "activeSource:" << st4.value("activeSource").toString()
                                    << "| listTotal:" << resp.value("data").toMap().value("total").toInt();
                            // 清掉筛选收尾,并读回确认回到未筛选态
                            panel.invokeAcceptance("acceptanceClearSource");
                            QTimer::singleShot(500, &panel, [&panel] {
                                const QVariantMap st5 = panel.rootProperty("acceptanceState").toMap();
                                qInfo() << "FILTER afterClear active:" << st5.value("filterActive").toBool()
                                        << "count:" << st5.value("filterCount").toInt();
                                QApplication::quit();
                            });
                        });
                    });
                });
            });
        });
    });
}

void runSettingsFlow(PanelWindow& panel) {
    // 设置抽屉交互验收,回归三组用户实测缺陷。全部走"真实点击/按键",不直调
    // QML 函数 —— 直调会把"事件没送到"这类缺陷(命中路径/焦点/遮挡)全部掩盖。
    //
    // ⚠️ 键盘注入(手输 999 + 回车、Esc)依赖 QML 场景的活动焦点项,而
    //    QQuickWidget 的 QML activeFocus 要求面板窗口是**系统前台窗口**
    //    (isRenderWindowFor:widget 顶层窗口 == focusWindow;见 qquickwidget.cpp)。
    //    故流程开头先把面板激活到前台,否则键会"没反应"而非报错。
    QTimer::singleShot(3000, &panel, [&panel] {
        panel.activateWindow();
        panel.raise();

        // 播种一条文本条目:STEP "点抽屉外的列表区域"要有一个真实卡片可点,
        // 且用 save_text_entry(不进剪贴板)播种 —— 剪贴板全程保持原样,
        // "穿透会复制"的判据才干净。
        const QVariantMap seeded = CoreFacade::instance().call(
            "save_text_entry", QVariantMap{{"text", QStringLiteral("验收-设置屏蔽-卡片")},
                                           {"source", "clipboard"}});
        qInfo() << "SETTINGS STEP1 seeded:" << seeded.value("ok").toBool();

        QTimer::singleShot(1000, &panel, [&panel] {
            // ---- A. 手输数值保存(此前:改得动、存不上) ----
            // 用一个与默认值(5000)明显不同的数,便于识别"没写回"。
            const bool opened = panel.clickByObjectName(QStringLiteral("settingsButton"));
            qInfo() << "SETTINGS STEP2 clickedOpen:" << opened;
            QTimer::singleShot(800, &panel, [&panel] {
                const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
                qInfo() << "SETTINGS STEP2 drawerOpen:" << st.value("settingsOpen").toBool();
                const bool focused = panel.clickByObjectName(QStringLiteral("historyLimitSpin"));
                qInfo() << "SETTINGS STEP3 clickedSpin:" << focused;
                QTimer::singleShot(600, &panel, [&panel] {
                    // Ctrl+A 全选 → 键入 999(替换) → 回车走 QQuickSpinBox 原生提交
                    // (keyRelease → updateValue(modified=true) → valueModified → 自动保存)
                    panel.sendKey(Qt::Key_A, Qt::ControlModifier);
                    panel.sendKey(Qt::Key_9, Qt::NoModifier, QStringLiteral("9"));
                    panel.sendKey(Qt::Key_9, Qt::NoModifier, QStringLiteral("9"));
                    panel.sendKey(Qt::Key_9, Qt::NoModifier, QStringLiteral("9"));
                    panel.sendKey(Qt::Key_Return);
                    qInfo() << "SETTINGS STEP3 typed 999 + Enter";
                    QTimer::singleShot(1500, &panel, [&panel] {
                        const QVariantMap resp = CoreFacade::instance().call("get_settings");
                        const int limit = resp.value("data").toMap()
                                              .value("historyLimit").toInt();
                        qInfo() << "SETTINGS STEP4 persisted historyLimit:" << limit
                                << "expected 999 match:" << (limit == 999);

                        // ---- B1. 滚到底后点 ✕(固定头部:滚动不再挡关闭按钮) ----
                        panel.invokeAcceptance("acceptanceSettingsToBottom");
                        QTimer::singleShot(700, &panel, [&panel] {
                            const QVariantMap st2 = panel.rootProperty("acceptanceState").toMap();
                            qInfo() << "SETTINGS STEP5 scrolled y:"
                                    << st2.value("settingsScrollY").toDouble()
                                    << "scrolledDown:" << (st2.value("settingsScrollY").toDouble() > 0);
                            const bool closed =
                                panel.clickByObjectName(QStringLiteral("settingsClose"));
                            qInfo() << "SETTINGS STEP5 clickedClose:" << closed;
                            QTimer::singleShot(800, &panel, [&panel] {
                                const QVariantMap st3 =
                                    panel.rootProperty("acceptanceState").toMap();
                                qInfo() << "SETTINGS STEP6 closeBtn drawerOpen:"
                                        << st3.value("settingsOpen").toBool() << "(期望 false)";

                                // ---- B2. 点抽屉外(列表卡片):应被 modal 吞掉,不穿透 ----
                                panel.clickByObjectName(QStringLiteral("settingsButton"));
                                QTimer::singleShot(800, &panel, [&panel] {
                                    const QVariantMap st4 =
                                        panel.rootProperty("acceptanceState").toMap();
                                    const QString clipBefore =
                                        QApplication::clipboard()->text();
                                    // xFrac=0.25:卡片宽到面板右半区,正中点会落在打开的
                                    // 抽屉底下 —— 必须把点击点挪进左半区才是"抽屉外"。
                                    const bool clickedCard = panel.clickByObjectName(
                                        QStringLiteral("entryCard0"), 0.25);
                                    qInfo() << "SETTINGS STEP7 drawerOpen:"
                                            << st4.value("settingsOpen").toBool()
                                            << "clickedCard(抽屉外):" << clickedCard;
                                    QTimer::singleShot(1000, &panel,
                                                       [&panel, clipBefore] {
                                        const QVariantMap st5 =
                                            panel.rootProperty("acceptanceState").toMap();
                                        const QString clipAfter =
                                            QApplication::clipboard()->text();
                                        qInfo() << "SETTINGS STEP8 outsideClick drawerOpen:"
                                                << st5.value("settingsOpen").toBool()
                                                << "(期望 false)"
                                                << "panelStillVisible:" << panel.isVisible()
                                                << "(期望 true)"
                                                << "clipboardUnchanged:" << (clipAfter == clipBefore);

                                        // ---- B3. Esc 关闭(interactive:false 时被 Qt 吞掉) ----
                                        panel.clickByObjectName(
                                            QStringLiteral("settingsButton"));
                                        QTimer::singleShot(800, &panel, [&panel] {
                                            const QVariantMap st6 =
                                                panel.rootProperty("acceptanceState").toMap();
                                            panel.sendKey(Qt::Key_Escape);
                                            qInfo() << "SETTINGS STEP9 drawerOpen:"
                                                    << st6.value("settingsOpen").toBool()
                                                    << "sent Esc";
                                            QTimer::singleShot(900, &panel, [&panel] {
                                                const QVariantMap st7 =
                                                    panel.rootProperty("acceptanceState").toMap();
                                                qInfo() << "SETTINGS STEP10 escClosed drawerOpen:"
                                                        << st7.value("settingsOpen").toBool()
                                                        << "(期望 false)";

                                                // ---- C. 待办页签恒显示(零未完成待办时) ----
                                                qInfo() << "SETTINGS STEP11 todoTabVisible:"
                                                        << st7.value("todoTabVisible").toBool()
                                                        << "(期望 true,零未完成待办)";
                                                panel.clickByObjectName(
                                                    QStringLiteral("todoTab"));
                                                QTimer::singleShot(800, &panel, [&panel] {
                                                    const QVariantMap st8 =
                                                        panel.rootProperty("acceptanceState")
                                                            .toMap();
                                                    qInfo() << "SETTINGS STEP12 viewId:"
                                                            << st8.value("viewId").toString()
                                                            << "(期望 todo)";
                                                    QApplication::quit();
                                                });
                                            });
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}

void runRestoreFlow(PanelWindow& panel) {
    // 恢复 / 导入验收:确认卡**真实点击**驱动两条"备份消费端"链路。
    //   ① 恢复(替换式):播种(A1 文本 + A2 图片)→ 备份 zip1 → 变更(加 C、
    //      删 A1、删掉 A2 的图片文件)→ UI 确认恢复 → 断言数据与图片文件都回到
    //      备份时刻 + 险备份 zip 落在恢复源同目录;
    //   ② 导入(合并式):再备 zip2 → 删掉 zip2 独有的条目 D → UI 确认导入 →
    //      断言 D 回来、重复的跳过;再导一次断言幂等(全跳过)。
    // ⚠️ 唯一跳过的一步是"原生文件对话框选文件"(进程内无法交互)——路径经
    //    acceptanceBackupPath 写入;确认卡 → 真实点击确认 → 真实 Core 调用
    //    → 异步完成回投这一整段都走产品链路。
    QTimer::singleShot(3000, &panel, [&panel] {
        panel.activateWindow();
        panel.raise();

        const QString dataDir = CoreFacade::instance()
                                    .call("data_dir").value("data").toMap()
                                    .value("path").toString();
        const QString zip1 = dataDir + QStringLiteral("/zip1.zip");
        const QString zip2 = dataDir + QStringLiteral("/zip2.zip");
        // 1x1 合法 PNG:图片链路的播种/恢复载体(基64)。
        const QString pngB64 = QStringLiteral(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAE"
            "hQGAhKmMIQAAAABJRU5ErkJggg==");

        // ---- STEP1 播种:A1 文本 + A2 图片(图片文件随后会被手删,验证恢复还原) ----
        const QVariantMap r1 = CoreFacade::instance().call(
            "save_text_entry",
            QVariantMap{{"text", QStringLiteral("验收-恢复-A")}, {"source", "clipboard"}});
        const QVariantMap r2 = CoreFacade::instance().call(
            "ingest_capture_png", QVariantMap{{"png_base64", pngB64}});
        const QString a1Id = r1.value("data").toMap().value("entry").toMap()
                                 .value("id").toString();
        const QVariantMap a2Entry = r2.value("data").toMap().value("entry").toMap();
        const QString a2Blob = a2Entry.value("blobPath").toString();
        qInfo() << "RESTORE STEP1 seeded text:" << r1.value("ok").toBool()
                << "image:" << r2.value("ok").toBool() << "a2Blob:" << a2Blob;

        QTimer::singleShot(900, &panel, [&panel, zip1, zip2, dataDir, a1Id, a2Blob] {
            // ---- STEP2 备份 zip1 → 制造变更(加 C、删 A1、删 A2 的图片文件) ----
            const QVariantMap bk = CoreFacade::instance().call(
                "backup_create", QVariantMap{{"path", zip1}});
            CoreFacade::instance().call(
                "save_text_entry",
                QVariantMap{{"text", QStringLiteral("验收-恢复-C")}, {"source", "clipboard"}});
            CoreFacade::instance().call("delete_entry", QVariantMap{{"id", a1Id}});
            const bool blobRemoved =
                QFile::remove(dataDir + QStringLiteral("/blobs/") + a2Blob);
            qInfo() << "RESTORE STEP2 backup1:" << bk.value("ok").toBool()
                    << "deletedA1:true" << "blobFileRemoved:" << blobRemoved;

            // ---- STEP3 从 UI 打开恢复确认卡(真实点击设置按钮开抽屉) ----
            QTimer::singleShot(900, &panel, [&panel, zip1, zip2, dataDir, a2Blob] {
                panel.clickByObjectName(QStringLiteral("settingsButton"));
                QTimer::singleShot(800, &panel, [&panel, zip1, zip2, dataDir, a2Blob] {
                    panel.setRootProperty("acceptanceBackupPath", zip1);
                    panel.invokeAcceptance("acceptanceOpenRestore");
                    QTimer::singleShot(700, &panel, [&panel, zip1, zip2, dataDir, a2Blob] {
                        const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
                        qInfo() << "RESTORE STEP3 restoreCardVisible:"
                                << st.value("restoreCardVisible").toBool();
                        const bool clicked =
                            panel.clickByObjectName(QStringLiteral("restoreConfirmBtn"));
                        qInfo() << "RESTORE STEP3 clickedConfirm:" << clicked;

                        // ---- STEP4 断言:数据与图片文件都回到备份时刻 + 险备份存在 ----
                        QTimer::singleShot(2300, &panel, [&panel, dataDir, a2Blob, zip2] {
                            const QVariantMap resp = CoreFacade::instance().call(
                                "list_entries", QVariantMap{{"limit", 200}});
                            const QVariantList items =
                                resp.value("data").toMap().value("items").toList();
                            bool hasA1 = false;
                            bool hasC = false;
                            for (const QVariant& v : items) {
                                const QString t = v.toMap().value("textContent").toString();
                                if (t == QStringLiteral("验收-恢复-A")) hasA1 = true;
                                if (t == QStringLiteral("验收-恢复-C")) hasC = true;
                            }
                            const bool imgBack =
                                QFile::exists(dataDir + QStringLiteral("/blobs/") + a2Blob);
                            const int safetyZips = QDir(dataDir)
                                .entryList(QStringList{QStringLiteral("nextboard-pre-restore-*.zip")},
                                           QDir::Files)
                                .size();
                            qInfo() << "RESTORE STEP4 total:" << items.size()
                                    << "(期望 2)A1Back:" << hasA1
                                    << "Cgone:" << !hasC
                                    << "blobFileBack:" << imgBack
                                    << "safetyZips:" << safetyZips << "(期望 >=1)";

                            // ---- STEP5 导入预置:加 D → 备份 zip2 → 删 D ----
                            CoreFacade::instance().call(
                                "save_text_entry",
                                QVariantMap{{"text", QStringLiteral("验收-导入-D")},
                                            {"source", "clipboard"}});
                            const QVariantMap bk2 = CoreFacade::instance().call(
                                "backup_create", QVariantMap{{"path", zip2}});
                            // 找到 D 的 id 并删除(模拟"只有备份里还有 D")
                            const QVariantList cur = CoreFacade::instance()
                                                         .call("list_entries", QVariantMap{{"limit", 200}})
                                                         .value("data").toMap().value("items").toList();
                            for (const QVariant& v : cur) {
                                const QVariantMap m = v.toMap();
                                if (m.value("textContent").toString()
                                    == QStringLiteral("验收-导入-D")) {
                                    CoreFacade::instance().call(
                                        "delete_entry", QVariantMap{{"id", m.value("id")}});
                                }
                            }
                            qInfo() << "RESTORE STEP5 backup2:" << bk2.value("ok").toBool()
                                    << "D deleted(只在 zip2 里)";

                            // ---- STEP6 UI 确认导入(真实点击) ----
                            QTimer::singleShot(800, &panel, [&panel, zip2] {
                                panel.setRootProperty("acceptanceBackupPath", zip2);
                                panel.invokeAcceptance("acceptanceOpenImport");
                                QTimer::singleShot(700, &panel, [&panel] {
                                    const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
                                    qInfo() << "RESTORE STEP6 importCardVisible:"
                                            << st.value("importCardVisible").toBool();
                                    const bool clicked =
                                        panel.clickByObjectName(QStringLiteral("importConfirmBtn"));
                                    qInfo() << "RESTORE STEP6 clickedConfirm:" << clicked;

                                    // ---- STEP7 断言合并结果 + STEP8 幂等再导一次 ----
                                    QTimer::singleShot(1800, &panel, [&panel] {
                                        const QVariantList items = CoreFacade::instance()
                                            .call("list_entries", QVariantMap{{"limit", 200}})
                                            .value("data").toMap().value("items").toList();
                                        bool hasD = false;
                                        for (const QVariant& v : items) {
                                            if (v.toMap().value("textContent").toString()
                                                == QStringLiteral("验收-导入-D")) hasD = true;
                                        }
                                        qInfo() << "RESTORE STEP7 total:" << items.size()
                                                << "(期望 3)D_back:" << hasD;

                                        panel.invokeAcceptance("acceptanceOpenImport");
                                        QTimer::singleShot(700, &panel, [&panel] {
                                            panel.clickByObjectName(QStringLiteral("importConfirmBtn"));
                                            QTimer::singleShot(1800, &panel, [&panel] {
                                                const QVariantList items = CoreFacade::instance()
                                                    .call("list_entries", QVariantMap{{"limit", 200}})
                                                    .value("data").toMap().value("items").toList();
                                                qInfo() << "RESTORE STEP8 reimport total:"
                                                        << items.size()
                                                        << "(期望 3,全跳过幂等)";
                                                QApplication::quit();
                                            });
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    });
}

void reportMonitors() {    // 多屏配对自检:把两端清单并排打出来,并逐屏核对"按名能否配对 + 底图是否自洽"。
    // 这是"多屏位置/分辨率混乱"的回归抓手 —— 该缺陷无法靠单测覆盖(需要真实多屏),
    // 但可以用日志断言:每个 Qt 屏都应能在核心清单里按名找到,且底图尺寸
    // ≈ 几何 × DPR(若把 A 屏的图配给 B 屏,这两者立刻对不上)。
    const QJsonArray coreMonitors = CoreBridge::instance().listMonitors();
    qInfo() << "MONITORS qt screens:" << QGuiApplication::screens().size()
            << "core monitors:" << coreMonitors.size();

    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* s : screens) {
        const QRect geo = s->geometry();
        const qreal dprVal = s->devicePixelRatio();
        qInfo().nospace() << "MONITORS qt name=" << s->name()
                          << " geo=" << geo.x() << "," << geo.y() << " "
                          << geo.width() << "x" << geo.height()
                          << " dpr=" << dprVal;

        // 按屏描述配对取图,核对底图尺寸与"几何 × DPR"是否自洽。
        // ⚠️ 这里**不**按 name 比对核心清单:Qt 的 QScreen::name() 是 EDID 友好名
        //    ("S24 PRO"),与核心的 `\\.\DISPLAYn` 不同源,配不上是**预期**的
        //    (实测三屏全配不上);配对主力键是物理原点,由核心 match_monitor 处理。
        //    真正该断言的是下面这条:取到的底图尺寸是否与该屏几何 × DPR 自洽 ——
        //    若配错屏,这一条会立刻失败。
        const QImage shot = CoreBridge::instance().captureMonitorMatched(s);
        if (shot.isNull()) {
            qWarning().nospace() << "MONITORS   ⚠️ 配对捕获失败: " << s->name();
            continue;
        }
        const int expectW = qRound(geo.width() * dprVal);
        const int expectH = qRound(geo.height() * dprVal);
        const bool ok = qAbs(shot.width() - expectW) <= 2 && qAbs(shot.height() - expectH) <= 2;
        qInfo().nospace() << "MONITORS   shot=" << shot.width() << "x" << shot.height()
                          << " expect=" << expectW << "x" << expectH
                          << " self-consistent=" << ok;
        if (!ok) {
            qWarning().nospace() << "MONITORS   ⚠️ 底图尺寸与几何不符(可能配到了别屏): "
                                 << s->name();
        }
    }
    QApplication::quit();
}

} // namespace nb
