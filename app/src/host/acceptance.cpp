#include "host/acceptance.hpp"

#include "host/auto_start.hpp"
#include "host/core_bridge.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
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
            qInfo() << "CLEAR-ALL STEP2 settingsOpen:" << st.value("settingsOpen").toBool();
            // 危险区在「数据」分类表单尾部(acceptanceRevealClearAll 会先切分类
            // 再滚入视区):不先滚,按钮点击会落空
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

void runUpdateFlow(PanelWindow& panel) {
    // 更新检查验收(docs/07-auto-update.md §11.2):真实点击「设置 → 关于 → 检查更新」
    //   → 读回状态行与下载链接 → 真实点击「打开下载页」。
    // 检查地址由 --update-url 指向本地环回 fixture(tools/update_fixture.py),全程不碰外网;
    // 「打开下载页」在验收模式下只记日志(见 CoreFacade::setBrowserLaunchDisabledForAcceptance),
    // 否则每次跑都会弹一个抢焦点的浏览器。
    QTimer::singleShot(3500, &panel, [&panel] {
        qInfo() << "UPDATE STEP1 checkUrl:" << CoreFacade::instance().updateCheckUrl();
        const bool opened = panel.clickByObjectName(QStringLiteral("settingsButton"));
        qInfo() << "UPDATE STEP1 clickedOpen:" << opened;
        QTimer::singleShot(800, &panel, [&panel] {
            const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
            qInfo() << "UPDATE STEP2 settingsOpen:" << st.value("settingsOpen").toBool()
                    << "checkBtnEnabled:" << st.value("updateEnabled").toBool()
                    << "未配置说明可见:" << st.value("updateNoteVisible").toBool()
                    << "(判据:空置态应 enabled=false 且说明=true;配好地址应相反)";
            // 关于分类(acceptanceRevealUpdate 会先切分类再滚入视区):不先滚,
            // 按钮点击会落空
            panel.invokeAcceptance("acceptanceRevealUpdate");
            QTimer::singleShot(700, &panel, [&panel] {
                const bool clickedCheck =
                    panel.clickByObjectName(QStringLiteral("updateCheckBtn"));
                qInfo() << "UPDATE STEP3 clickedCheck:" << clickedCheck;
                // 环回请求本身是毫秒级,这里的裕量是给"点击 → 异步回投 → 绑定刷新"整条链
                QTimer::singleShot(2000, &panel, [&panel] {
                    const QVariantMap st2 = panel.rootProperty("acceptanceState").toMap();
                    const QString hint = st2.value("updateHintText").toString();
                    const QString page = st2.value("updateDownloadUrl").toString();
                    qInfo() << "UPDATE STEP4 hint:" << hint << "downloadUrl:" << page;
                    const bool clickedPage =
                        panel.clickByObjectName(QStringLiteral("updateOpenPageBtn"));
                    qInfo() << "UPDATE STEP5 clickedOpenPage:" << clickedPage
                            << "(判据:STEP4 文案符合该模式的预期;"
                            << "无下载链接的模式此处置 false 才是对的)";
                    QTimer::singleShot(400, &panel, [&panel] { QApplication::quit(); });
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
    // 设置页交互验收(抽屉时代的回归集 + 整窗页面形态的新断言)。全部走"真实
    // 点击/按键",不直调 QML 函数 —— 直调会把"事件没送到"这类缺陷(命中路径/
    // 焦点/遮挡)全部掩盖。
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
                qInfo() << "SETTINGS STEP2 settingsOpen:" << st.value("settingsOpen").toBool();
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
                                qInfo() << "SETTINGS STEP6 closeBtn settingsOpen:"
                                        << st3.value("settingsOpen").toBool() << "(期望 false)";

                                // ---- B2. 误触不退出:设置页内的普通点击既不关页也不穿透 ----
                                // (抽屉时代验证的是"modal 拦截列表穿透";整窗页面没有
                                //  overlay,等价回归点变为:点击页内元素不会把设置页关掉。)
                                panel.clickByObjectName(QStringLiteral("settingsButton"));
                                QTimer::singleShot(800, &panel, [&panel] {
                                    const QVariantMap st4 =
                                        panel.rootProperty("acceptanceState").toMap();
                                    const QString clipBefore =
                                        QApplication::clipboard()->text();
                                    // 点侧栏「通用」(当前已选中,点击是安全的无操作);
                                    // 若点击误关设置页,settingsOpen 会翻成 false。
                                    const bool clickedNav = panel.clickByObjectName(
                                        QStringLiteral("settingsNavGeneral"));
                                    qInfo() << "SETTINGS STEP7 settingsOpen:"
                                            << st4.value("settingsOpen").toBool()
                                            << "clickedNav(页内):" << clickedNav;
                                    QTimer::singleShot(1000, &panel,
                                                       [&panel, clipBefore] {
                                        const QVariantMap st5 =
                                            panel.rootProperty("acceptanceState").toMap();
                                        const QString clipAfter =
                                            QApplication::clipboard()->text();
                                        qInfo() << "SETTINGS STEP8 inPageClick settingsOpen:"
                                                << st5.value("settingsOpen").toBool()
                                                << "(期望 true)"
                                                << "panelStillVisible:" << panel.isVisible()
                                                << "(期望 true)"
                                                << "clipboardUnchanged:" << (clipAfter == clipBefore);

                                        // ---- B3. Esc 返回列表页(设置页 Esc 语义:退一层,
                                        //      不直接收起;再按一次才收起面板) ----
                                        panel.clickByObjectName(
                                            QStringLiteral("settingsButton"));
                                        QTimer::singleShot(800, &panel, [&panel] {
                                            const QVariantMap st6 =
                                                panel.rootProperty("acceptanceState").toMap();
                                            panel.sendKey(Qt::Key_Escape);
                                            qInfo() << "SETTINGS STEP9 settingsOpen:"
                                                    << st6.value("settingsOpen").toBool()
                                                    << "sent Esc";
                                            QTimer::singleShot(900, &panel, [&panel] {
                                                const QVariantMap st7 =
                                                    panel.rootProperty("acceptanceState").toMap();
                                                qInfo() << "SETTINGS STEP10 escClosed settingsOpen:"
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

                                                    // ---- D. OCR 后端选择器 ----
                                                    // 判据两条:① 下拉**真实点击**能改后端并落盘;
                                                    //           ② 可用性提示反映 core 的 ocr_probe 结果。
                                                    // 走真实点击而非直调 QML 函数 —— 后者会掩盖
                                                    // "控件不可见/被遮挡"这类缺陷(分类页内容
                                                    // 只实例化当前分类,若导航/滚动有问题,点击会落空)。
                                                    //
                                                    // ⚠️ 先重新打开设置页(STEP10 的 Esc 已退回列表),
                                                    //    再真实点击侧栏「文字识别」切到 OCR 分类 ——
                                                    //    这一步同时是侧栏导航链路的验收点。
                                                    panel.clickByObjectName(
                                                        QStringLiteral("settingsButton"));
                                                    QTimer::singleShot(900, &panel, [&panel] {
                                                    const QVariantMap stOpen =
                                                        panel.rootProperty("acceptanceState")
                                                            .toMap();
                                                    qInfo() << "SETTINGS STEP13 settingsReopened:"
                                                            << stOpen.value("settingsOpen")
                                                                   .toBool();
                                                    const bool clickedNavOcr =
                                                        panel.clickByObjectName(
                                                            QStringLiteral("settingsNavOcr"));
                                                    qInfo() << "SETTINGS STEP13 clickedNavOcr:"
                                                            << clickedNavOcr;
                                                    QTimer::singleShot(700, &panel, [&panel] {
                                                    // 不能用 acceptanceSettingsToBottom:它滚到**内容底部**,
                                                    // 会把靠上的 OCR 段滚出视口顶部(实测点击坐标 y=-207,
                                                    // 落在窗口外)。改为把目标控件本身滚入视口。
                                                    panel.setRootProperty(
                                                        "acceptanceScrollTarget",
                                                        QVariant(QStringLiteral(
                                                            "ocrBackendCombo")));
                                                    panel.invokeAcceptance(
                                                        "acceptanceSettingsScrollToTarget");
                                                    QTimer::singleShot(700, &panel, [&panel] {
                                                        const QVariantMap before =
                                                            CoreFacade::instance()
                                                                .call("get_settings");
                                                        const QString beforeBackend =
                                                            before.value("data")
                                                                .toMap()
                                                                .value("ocrBackend")
                                                                .toString();
                                                        qInfo() << "SETTINGS STEP13 ocrBackend(before):"
                                                                << beforeBackend;

                                                        const bool clickedOcr = panel.clickByObjectName(
                                                            QStringLiteral("ocrBackendCombo"));
                                                        qInfo() << "SETTINGS STEP13 clickedOcrCombo:"
                                                                << clickedOcr;
                                                        QTimer::singleShot(700, &panel, [&panel,
                                                                                  beforeBackend] {
                                                            // 下拉已弹出 → 用键盘选中**另一项**并回车
                                                            // (比点 popup 里的 delegate 稳,后者依赖
                                                            //  弹窗定位)。
                                                            panel.sendKey(Qt::Key_Down);
                                                            panel.sendKey(Qt::Key_Return);
                                                            qInfo() << "SETTINGS STEP13 chose other"
                                                                    << "backend";
                                                            QTimer::singleShot(1500, &panel, [&panel,
                                                                                      beforeBackend] {
                                                                const QVariantMap after =
                                                                    CoreFacade::instance()
                                                                        .call("get_settings");
                                                                const QString afterBackend =
                                                                    after.value("data")
                                                                        .toMap()
                                                                        .value("ocrBackend")
                                                                        .toString();
                                                                qInfo() << "SETTINGS STEP14"
                                                                        << "ocrBackend(after):"
                                                                        << afterBackend
                                                                        << "changed:"
                                                                        << (afterBackend !=
                                                                            beforeBackend);

                                                                // 可用性提示:core 的 ocr_probe
                                                                // 必须能报出两个后端各自的可用性。
                                                                const QVariantMap probeSys =
                                                                    CoreFacade::instance().call(
                                                                        "ocr_probe",
                                                                        QVariantMap{
                                                                            {"backend",
                                                                             "system"}});
                                                                const QVariantMap probeRapid =
                                                                    CoreFacade::instance().call(
                                                                        "ocr_probe",
                                                                        QVariantMap{
                                                                            {"backend",
                                                                             "rapidocr"}});
                                                                qInfo() << "SETTINGS STEP15"
                                                                        << "probe system.available:"
                                                                        << probeSys.value("data")
                                                                               .toMap()
                                                                               .value("available")
                                                                               .toBool()
                                                                        << "rapidocr.available:"
                                                                        << probeRapid.value("data")
                                                                               .toMap()
                                                                               .value("available")
                                                                               .toBool();
                                                                // 判定:两个后端都必须能被探测
                                                                // (可用与否取决于本机环境,但"探测接口
                                                                //  可用"是硬要求)。
                                                                qInfo() << "SETTINGS STEP15"
                                                                        << "probeOk:"
                                                                        << (probeSys.value("ok")
                                                                                .toBool() &&
                                                                            probeRapid.value("ok")
                                                                                .toBool());
                                                                // ---- E. 快捷功能页(quick_rail 开关) ----
                                                                // 判据四条:
                                                                //   ① 侧栏真实点击能切到「快捷功能」分类(导航链路);
                                                                //   ② 开关初值 = 设置里的 quickRail(默认 true);
                                                                //   ③ 真实点击开关 → 落库翻转(save_settings 链路);
                                                                //   ④ panelWindow.showQuickRail 属性联动翻转(即时生效链路)。
                                                                // 断言后**复原为 true**,不污染后续流程与用户配置。
                                                                const bool clickedNavQuick =
                                                                    panel.clickByObjectName(
                                                                        QStringLiteral("settingsNavQuick"));
                                                                qInfo() << "SETTINGS STEP16 clickedNavQuick:"
                                                                        << clickedNavQuick;
                                                                QTimer::singleShot(700, &panel, [&panel] {
                                                                    const QVariantMap sq =
                                                                        CoreFacade::instance().call("get_settings");
                                                                    const QVariant beforeVal =
                                                                        sq.value("data").toMap()
                                                                            .value("quickRail");
                                                                    const bool beforeRail =
                                                                        beforeVal.isValid() ? beforeVal.toBool() : true;
                                                                    qInfo() << "SETTINGS STEP16 quickRail(before):"
                                                                            << beforeRail;
                                                                    const bool clickedChk =
                                                                        panel.clickByObjectName(
                                                                            QStringLiteral("quickRailCheck"));
                                                                    qInfo() << "SETTINGS STEP16 clickedCheck:" << clickedChk;
                                                                    QTimer::singleShot(900, &panel, [&panel, beforeRail] {
                                                                        const QVariantMap sa =
                                                                            CoreFacade::instance().call("get_settings");
                                                                        const bool afterRail =
                                                                            sa.value("data").toMap()
                                                                                .value("quickRail").isValid()
                                                                                ? sa.value("data").toMap()
                                                                                      .value("quickRail").toBool()
                                                                                : !beforeRail;
                                                                        const QVariant propRail =
                                                                            panel.rootProperty("acceptanceShowQuickRail");
                                                                        qInfo() << "SETTINGS STEP16 quickRail(after):"
                                                                                << afterRail
                                                                                << "changed:" << (afterRail != beforeRail)
                                                                                << "showQuickRailProp:" << propRail.toBool()
                                                                                << "propMatches:" << (propRail.toBool() == afterRail);
                                                                        // 复原:再点一次回到初值(相同链路,顺带验证往返)
                                                                        panel.clickByObjectName(
                                                                            QStringLiteral("quickRailCheck"));
                                                                        QTimer::singleShot(900, &panel, [&panel, beforeRail] {
                                                                            const QVariantMap sr =
                                                                                CoreFacade::instance().call("get_settings");
                                                                            const bool restored =
                                                                                (sr.value("data").toMap()
                                                                                     .value("quickRail").isValid()
                                                                                     ? sr.value("data").toMap()
                                                                                           .value("quickRail").toBool()
                                                                                     : true) == beforeRail;
                                                                            qInfo() << "SETTINGS STEP16 restored:" << restored;
                                                                            QApplication::quit();
                                                                        });
                                                                    });
                                                                });
                                                            });
                                                        });
                                                    });
                                                    });   // 闭合「切到 OCR 分类后的 700ms 层」
                                                    });   // ← 新增:闭合 STEP13 的 drawerReopened 层
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

void runStitchSettingsFlow() {
    // 长截图设置与热键的**往返**验收(docs/19 §6)。
    //
    // 只验"能否存进去、能否读回来、非法值是否被拒",不验 QML 控件像素 ——
    // 控件本身由 --screenshot --settings 的人工/自截覆盖。
    //
    // 为什么值得单开一条:设置字段的典型故障是"QML 写了但 core 不认"(名字拼错/
    // 大小写不符/serde 重命名漏了),表现是**用户改了不生效且没有任何提示**。
    // core_facade 的 save_settings 走字段级合并,拼错的名字会被静默忽略。
    CoreFacade& core = CoreFacade::instance();
    int failures = 0;
    const auto check = [&failures](bool ok, const char* what) {
        if (!ok) { ++failures; qWarning() << "STITCH-SETTINGS ⚠️ 失败:" << what; }
        else qInfo() << "STITCH-SETTINGS   ok:" << what;
    };

    const QVariantMap before = core.call("get_settings").value("data").toMap();
    qInfo() << "STITCH-SETTINGS STEP0 maxHeight:" << before.value("stitchMaxHeight").toInt()
            << "overlap:" << before.value("stitchOverlapRatio").toInt();

    // ① 默认值:上限 30000、门槛 3
    // (captureScrolling 热键已随动作移除,2026-11-01:工具栏「长截图」承担入口)
    check(before.value("stitchMaxHeight").toInt() == 30000,
          "默认上限 = 30000px(docs/19 §9 Q4)");
    check(before.value("stitchOverlapRatio").toInt() == 3, "默认重叠门槛 = 3%");
    // v2 新增两项:采样间隔与字节预算。默认值必须与 settings.rs 一致。
    check(before.value("stitchSampleIntervalMs").toInt() == 150,
          "默认采样间隔 = 150ms(v2 从 500ms 收紧,直抓选区后单帧更便宜)");
    check(before.value("stitchMaxBytes").toLongLong() == 128LL * 1024 * 1024,
          "默认字节预算 = 128MB(v2:只按像素限高会让内存先爆)");
    check(before.value("hotkeys").toMap().value("captureScrolling").toString().isEmpty(),
          "captureScrolling 热键已移除,旧配置残留键读回为空/缺失");

    // ② 写入新值 → 读回(字段级合并必须认这些名字)
    const bool saved = core.call("save_settings", QVariantMap{
        {"stitchMaxHeight", 45000},
        {"stitchOverlapRatio", 7},
        {"stitchSampleIntervalMs", 220},
        {"stitchMaxBytes", 64LL * 1024 * 1024},
    }).value("ok").toBool();
    const QVariantMap after = core.call("get_settings").value("data").toMap();
    qInfo() << "STITCH-SETTINGS STEP1 saved:" << saved
            << "maxHeight:" << after.value("stitchMaxHeight").toInt()
            << "overlap:" << after.value("stitchOverlapRatio").toInt();
    check(saved, "save_settings 成功");
    check(after.value("stitchMaxHeight").toInt() == 45000, "上限读回 45000");
    check(after.value("stitchOverlapRatio").toInt() == 7, "门槛读回 7");
    check(after.value("stitchSampleIntervalMs").toInt() == 220, "采样间隔读回 220ms");
    check(after.value("stitchMaxBytes").toLongLong() == 64LL * 1024 * 1024,
          "字节预算读回 64MB");
    // 合并不能把同级其余快捷键清零
    check(after.value("hotkeys").toMap().value("toggleMain").toString()
              == before.value("hotkeys").toMap().value("toggleMain").toString(),
          "写长截图设置**没有**清掉其它快捷键(字段级合并生效)");

    // ③ 非法值必须被拒(而不是静默存进去)
    const bool badHigh = core.call("save_settings",
                                   QVariantMap{{"stitchMaxHeight", 999999}}).value("ok").toBool();
    const bool badRatio = core.call("save_settings",
                                    QVariantMap{{"stitchOverlapRatio", 99}}).value("ok").toBool();
    qInfo() << "STITCH-SETTINGS STEP2 reject high:" << !badHigh
            << "reject ratio:" << !badRatio;
    check(!badHigh, "上限 999999 被拒绝(超出 2k~100k)");
    check(!badRatio, "门槛 99% 被拒绝(超出 1~20)");
    const bool badMs = core.call("save_settings",
                                 QVariantMap{{"stitchSampleIntervalMs", 5}}).value("ok").toBool();
    const bool badBytes = core.call("save_settings",
                                    QVariantMap{{"stitchMaxBytes", 8}}).value("ok").toBool();
    qInfo() << "STITCH-SETTINGS STEP2b reject ms:" << !badMs << "reject bytes:" << !badBytes;
    check(!badMs, "采样间隔 5ms 被拒绝(低于 80ms 会挤满 CPU)");
    check(!badBytes, "字节预算 8 字节被拒绝(低于 16MB)且 0 才表示不限制");
    // 被拒后原值必须保持不变(不能出现"半个设置写进去了")
    const QVariantMap afterBad = core.call("get_settings").value("data").toMap();
    check(afterBad.value("stitchMaxHeight").toInt() == 45000, "被拒后上限保持 45000");

    // ④ 还原(验收不污染用户配置)
    core.call("save_settings", QVariantMap{
        {"stitchMaxHeight", before.value("stitchMaxHeight").toInt()},
        {"stitchOverlapRatio", before.value("stitchOverlapRatio").toInt()},
        {"stitchSampleIntervalMs", before.value("stitchSampleIntervalMs").toInt()},
        {"stitchMaxBytes", before.value("stitchMaxBytes").toLongLong()},
    });
    qInfo() << "STITCH-SETTINGS DONE failures=" << failures;
    QApplication::quit();
}

void runAutoStartFlow() {
    // 自启注册表自检:核心断言是"应用内开关写的命令行带 --hidden",与安装器
    // (tools/installer.nsi SecAutoStart)同形态。这条缺陷在真机上的唯一可观测后果
    // 发生在**下次开机**,人工回归一次就要重启一次,而它全是注册表读写 ——
    // 完全可以进程内断言,故做成验收入口。
    //
    // ⚠️ 本流程**真实改写** HKCU\Software\Microsoft\Windows\CurrentVersion\Run
    //    的 Nextboard 值:开头快照原值,每个分支（含所有失败路径）必定还原。
    const QString original = auto_start::debugRegisteredCommandLine();
    qInfo() << "AUTOSTART STEP0 original:" << original;

    auto restore = [original] {
        if (original.isEmpty()) {
            auto_start::setEnabled(false);            // 原本没开自启 → 删掉试写值
        } else {
            auto_start::debugWriteRawCommandLine(original);   // 原样写回
        }
        qInfo() << "AUTOSTART STEP5 restored:" << auto_start::debugRegisteredCommandLine();
    };

    // ① 启用:断言写入的命令行 == 期望形态(`"<exe>" --hidden`)
    const bool enabledOk = auto_start::setEnabled(true);
    const QString written = auto_start::debugRegisteredCommandLine();
    const QString expected = auto_start::debugExpectedCommandLine();
    qInfo() << "AUTOSTART STEP1 enabled:" << enabledOk << "value:" << written;
    qInfo() << "AUTOSTART STEP1 expect:" << expected << "match:" << (written == expected);
    if (!enabledOk || written != expected) {
        qWarning() << "AUTOSTART STEP1 ⚠️ 应用内自启与安装器形态不一致(应为 <exe> --hidden)";
        restore();
        QApplication::quit();
        return;
    }

    // ② 旧形态 → 自动升级:伪造 0.2.0 及更早的写法(只有 exe,无 --hidden)。
    //    ⚠️ 必须指向**本 exe**:refreshCommandLineIfStale() 只升级指向本进程的项
    //    (否则跑一次 dev 构建就会把用户正式版的自启改到 build 目录)。
    const QString stale = QStringLiteral("\"%1\"")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    auto_start::debugWriteRawCommandLine(stale);
    qInfo() << "AUTOSTART STEP2 stale:" << auto_start::debugRegisteredCommandLine();
    const bool upgraded = auto_start::refreshCommandLineIfStale();
    const QString after = auto_start::debugRegisteredCommandLine();
    qInfo() << "AUTOSTART STEP2 upgraded:" << upgraded << "now:" << after;
    if (!(upgraded && after == expected)) {
        qWarning() << "AUTOSTART STEP2 ⚠️ 旧形态未被升级(这些用户开机仍会弹面板)";
        restore();
        QApplication::quit();
        return;
    }

    // ③ 幂等:已是新形态时再跑一次不应改写(返回 false)
    const bool again = auto_start::refreshCommandLineIfStale();
    qInfo() << "AUTOSTART STEP3 idempotent(expect false):" << again;
    if (again) qWarning() << "AUTOSTART STEP3 ⚠️ 非幂等:已是最新形态却又改写了一次";

    // ④ 停用:值应消失
    const bool disabledOk = auto_start::setEnabled(false);
    qInfo() << "AUTOSTART STEP4 disabled:" << disabledOk
            << "enabled():" << auto_start::enabled()
            << "value:" << auto_start::debugRegisteredCommandLine();
    if (!disabledOk || auto_start::enabled())
        qWarning() << "AUTOSTART STEP4 ⚠️ 停用后值仍存在";

    restore();
    qInfo() << "AUTOSTART DONE";
    QApplication::quit();
}


// ---- 待办编辑卡片字段顺序(--todo-layout-flow)----
void runTodoLayoutFlow(PanelWindow& panel) {
    // 打开新建卡片(等价点头部「待办」按钮),然后比较两个输入框的纵向位置。
    // ⚠️ 用 mapToScene 而不是 y:两者的父级不同(标题框在 Rectangle 里、备注框在
    //    ColumnLayout 里),直接比 y 是在比两个不同坐标系下的值,毫无意义。
    QTimer::singleShot(3500, &panel, [&panel] {
        panel.openTodoComposerForAcceptance();
        QTimer::singleShot(600, &panel, [&panel] {
            const qreal titleY = panel.acceptanceItemSceneY(QStringLiteral("todoTitleInput"));
            const qreal noteY = panel.acceptanceItemSceneY(QStringLiteral("todoNoteInput"));
            const bool bothPresent = titleY >= 0 && noteY >= 0;
            // ⚠️ 位置断言**测不到可见性**:mapToScene 对不可见的项照样给坐标。
            //    实测缺陷:备注框的 visible 绑到 parent.noteExpanded,而它的 parent 是
            //    ColumnLayout、noteExpanded 定义在兄弟 RowLayout 上 → 取到 undefined
            //    → 恒不可见(备注功能整个用不了),而本用例一直 PASS。
            const bool noteVisible = panel.acceptanceItemVisible(QStringLiteral("todoNoteInput"));
            const bool titleVisible = panel.acceptanceItemVisible(QStringLiteral("todoTitleInput"));
            qInfo() << "TODO-LAYOUT 标题 y:" << titleY << "备注 y:" << noteY
                    << "两者都存在:" << bothPresent
                    << "标题可见:" << titleVisible << "备注可见:" << noteVisible;
            if (!bothPresent) {
                qWarning().noquote()
                    << "TODO-LAYOUT FAIL 找不到标题/备注输入框(objectName 缺失?)";
            } else if (!titleVisible) {
                qWarning().noquote() << "TODO-LAYOUT FAIL 标题输入框不可见(应始终可见)";
            } else if (noteVisible) {
                // ⚠️ 卡片刚打开时备注框**本该收起**(空备注 + 无焦点)。
                // 它若是可见的,说明 visible 绑定没生效 —— QML 里
                // `visible: undefined` 是**赋值失败、保持默认 true**,不是"恒 false"。
                // 这正是 parent.noteExpanded 那个缺陷的表现。
                qWarning().noquote()
                    << "TODO-LAYOUT FAIL 空备注时备注框应收起,实际却展开(visible 绑定失效)";
            } else {
                // 收起态正确 → 再验它能**展开**、能**再收起**。
                // 只验一次展开是不够的:visible 恒 true 也能通过那一条。
                qInfo().noquote() << "TODO-LAYOUT 备注框已收起,点击展开后复验";
                panel.clickByObjectName(QStringLiteral("todoNoteToggle"));
                QTimer::singleShot(300, qApp, [&panel, titleY] {
                    const bool expandedVisible =
                        panel.acceptanceItemVisible(QStringLiteral("todoNoteInput"));
                    const qreal noteY2 =
                        panel.acceptanceItemSceneY(QStringLiteral("todoNoteInput"));
                    qInfo() << "TODO-LAYOUT 展开后备注可见:" << expandedVisible
                            << "备注 y:" << noteY2;
                    if (!expandedVisible) {
                        qWarning().noquote() << "TODO-LAYOUT FAIL 点「展开」后备注框仍不可见";
                        QApplication::quit();
                        return;
                    }
                    if (!(titleY < noteY2)) {
                        qWarning().noquote() << "TODO-LAYOUT FAIL 标题必须排在备注**上面**";
                        QApplication::quit();
                        return;
                    }
                    // 再收起:证明状态是**双向**跟随的,而不是"一旦可见就永远可见"
                    panel.clickByObjectName(QStringLiteral("todoNoteToggle"));
                    QTimer::singleShot(300, qApp, [&panel] {
                        const bool collapsedAgain =
                            !panel.acceptanceItemVisible(QStringLiteral("todoNoteInput"));
                        qInfo() << "TODO-LAYOUT 再收起后备注已隐藏:" << collapsedAgain;
                        if (!collapsedAgain) {
                            qWarning().noquote()
                                << "TODO-LAYOUT FAIL 点「收起」后备注框仍可见(状态不跟随)";
                        } else {
                            qInfo().noquote()
                                << "TODO-LAYOUT PASS 备注可展开/收起且排在标题下方";
                        }
                        QApplication::quit();
                    });
                });
                return;
            }
            // 注意:上面的展开/收起分支自带 return(异步,结果在定时器里断言),
            // 走不到这里;只有 bothPresent==false / titleVisible==false / noteVisible==true
            // 三种失败会落到本行的 quit。
            QApplication::quit();
        });
    });
}

} // namespace nb
