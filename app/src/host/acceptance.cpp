#include "host/acceptance.hpp"

#include "host/core_bridge.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
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

void reportTodoGroups(PanelWindow& panel) {
    // 只读自检:切待办视图 → 读回各行分组键。配合"脚本先播种各分组待办"使用,
    // 断言组标题的分界点(键序变化处即标题出现处)。
    // 之后切回「全部」视图再读一次筛选条 —— 来源条在两视图的显示规则不同,
    // 一次跑完两个视图的状态才能证明"待办视图不再多出待办 chip"。
    QTimer::singleShot(3000, &panel, [&panel] {
        panel.invokeAcceptance("acceptanceShowTodoGroups");
        QTimer::singleShot(800, &panel, [&panel] {
            const QVariantMap st = panel.rootProperty("acceptanceState").toMap();
            qInfo() << "GROUPS viewId:" << st.value("viewId").toString()
                    << "keys:" << st.value("rowGroups").toString()
                    << "| sourceBar:" << st.value("sourceBarVisible").toBool()
                    << "sourceChips:" << st.value("sourceChips").toString();
            panel.invokeAcceptance("acceptanceShowAll");
            QTimer::singleShot(800, &panel, [&panel] {
                const QVariantMap st2 = panel.rootProperty("acceptanceState").toMap();
                qInfo() << "GROUPS viewId:" << st2.value("viewId").toString()
                        << "| sourceBar:" << st2.value("sourceBarVisible").toBool()
                        << "sourceChips:" << st2.value("sourceChips").toString()
                        << "| tagChips:" << st2.value("tagChips").toString();
                QApplication::quit();
            });
        });
    });
}

void reportMonitors() {
    // 多屏配对自检:把两端清单并排打出来,并逐屏核对"按名能否配对 + 底图是否自洽"。
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
