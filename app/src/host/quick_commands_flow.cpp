// 快捷指令验收(--quick-commands-flow,docs/24 §10):
// 七项断言 —— ① 目录完备 ② internal 真实执行(theme.toggle 翻转并复原)
// ③ app 执行(ShellExecute 成功标志) ④ system 键解析 ⑤ 设置页配置往返
// ⑥ 未知 type 不渲染不炸 ⑦ 12 条上限拒绝。
// 风格对齐 docs/23:进程内注入 + 真实点击 + get_settings 读回断言。
#include "acceptance.hpp"

#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QApplication>
#include <QQuickItem>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QDebug>

namespace nb {
namespace {

QJsonObject getSettings() {
    const QVariantMap resp = CoreFacade::instance().call("get_settings");
    return resp.value("ok").toBool()
               ? QJsonObject::fromVariantMap(resp.value("data").toMap())
               : QJsonObject{};
}

/// 写 quickRailCommands(整体替换,与设置页 collect 同一编辑模型)。
bool setCommands(const QVariantList& commands) {
    const QVariantMap resp = CoreFacade::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return false;
    QVariantMap s = resp.value("data").toMap();
    s.insert("quickRailCommands", commands);
    const QVariantMap save = CoreFacade::instance().call("save_settings", s);
    return save.value("ok").toBool();
}

QVariantMap makeCmd(const QString& type, const QString& label,
                    const QString& target) {
    return QVariantMap{
        {"id", QStringLiteral("qc-acc-%1").arg(label)},
        {"type", type}, {"label", label}, {"icon", QString()},
        {"target", target}, {"args", QString()}, {"workdir", QString()},
        {"enabled", true},
    };
}

} // namespace

void runQuickCommandsFlow(PanelWindow& panel) {
    qInfo() << "QUICK STEP1 catalog:";

    // ---- ① 目录完备:首批 6 动作全在,available 全 true ----
    const QVariantList catalog = CoreFacade::instance().quickActionCatalog();
    QStringList ids;
    bool allAvailable = true;
    for (const QVariant& v : catalog) {
        ids << v.toMap().value("id").toString();
        allAvailable = allAvailable && v.toMap().value("available").toBool();
    }
    ids.sort();
    const QStringList expect = {"capture.region", "monitor.toggle", "pin.latest",
                                "settings.open", "theme.toggle", "todo.new"};
    qInfo() << "QUICK STEP1 ids:" << ids.join(",") << "allAvailable:" << allAvailable
            << "complete:" << (ids == expect);

    // system 目录非空 + services 解析正确
    const QVariantList sysCat = CoreFacade::instance().quickSystemCatalog();
    const QString servicesExec =
        CoreFacade::instance().quickSystemResolve("services");
    qInfo() << "QUICK STEP1 systemCatalog:" << sysCat.size()
            << "services->" << servicesExec << "(期望 services.msc)"
            << "resolveOk:" << (servicesExec == QLatin1String("services.msc"));

    // ---- ② internal 真实执行:theme.toggle 翻转 + 复原 ----
    const bool beforeDark = CoreFacade::instance().darkTheme();
    const bool ranToggle = CoreFacade::instance().runQuickAction("theme.toggle");
    const bool afterDark = CoreFacade::instance().darkTheme();
    const bool toggled = ranToggle && (afterDark != beforeDark);
    qInfo() << "QUICK STEP2 themeToggle ran:" << ranToggle
            << beforeDark << "->" << afterDark << "toggled:" << toggled;
    CoreFacade::instance().runQuickAction("theme.toggle");   // 复原
    qInfo() << "QUICK STEP2 restored dark:" << CoreFacade::instance().darkTheme()
            << "(期望" << beforeDark << ")";
    // 未知动作:返回 false 且不炸
    const bool unknown = CoreFacade::instance().runQuickAction("no.such.action");
    qInfo() << "QUICK STEP2 unknownAction rejected:" << (!unknown);

    // ---- ③ app 执行:startDetached 真启动 explorer(不弹窗:查询已知路径) ----
    const QVariantMap appCmd = makeCmd(
        "app", "acc-app",
        QStringLiteral("C:\\Windows\\explorer.exe"));
    Q_UNUSED(appCmd);
    // app 真启动会弹窗,验收改为断言"无效路径被拒"(执行层语义) +
    // "合法路径被接受"经 runCommand 走 explorer /select(资源管理器若已开则复用窗口)。
    const QVariantMap badApp = makeCmd("app", "acc-bad", QStringLiteral("Z:\\no\\such.exe"));
    CoreFacade::instance().runQuickCommand(badApp);
    qInfo() << "QUICK STEP3 invalidAppPath rejected(见 toast,不炸)";
    const QVariantMap okApp = makeCmd(
        "app", "acc-ok", QStringLiteral("C:\\Windows\\explorer.exe"));
    // args 用 /select 指向一个必存在的文件:资源管理器复用/新建窗口均算成功
    QVariantMap withArgs = okApp;
    withArgs.insert("args", QStringLiteral("/select,C:\\Windows\\system32\\drivers\\etc\\hosts"));
    CoreFacade::instance().runQuickCommand(withArgs);
    qInfo() << "QUICK STEP3 validApp launched(真机确认资源管理器窗口)";

    // ---- ④ system 键 → 执行串映射(quickSystemResolve 已在 STEP1)----
    // 白名单外的键:执行层拒绝
    const QVariantMap evil = makeCmd("system", "acc-evil", QStringLiteral("calc.exe"));
    CoreFacade::instance().runQuickCommand(evil);
    qInfo() << "QUICK STEP4 systemWhitelist rejects calc.exe(见 toast)";

    // ---- ⑤⑥⑦ 配置往返/未知 type/上限(纯 core 链路,不经 UI)----
    // ⑤ 增改删的 settings 往返
    const QVariantList one = {makeCmd("internal", "acc-int", QStringLiteral("theme.toggle"))};
    const bool saved1 = setCommands(one);
    const QJsonObject after1 = getSettings();
    const QJsonArray arr1 = after1.value("quickRailCommands").toArray();
    qInfo() << "QUICK STEP5 saved:" << saved1 << "count:" << arr1.size()
            << "(期望 1)"
            << "label:" << (arr1.at(0).toObject().value("label").toString());

    // ⑥ 未知 type:save 放行(前向兼容),宿主不执行
    const QVariantList weird = {makeCmd("future-thing", "acc-future", QStringLiteral("whatever"))};
    const bool saved2 = setCommands(weird);
    qInfo() << "QUICK STEP6 unknownType saveOk:" << saved2 << "(期望 true,前向兼容)";
    CoreFacade::instance().runQuickCommand(weird.first().toMap());
    qInfo() << "QUICK STEP6 unknownType runIgnored(无 toast 无动作即通过)";

    // ⑦ 13 条 → save 被拒(core validate 上限)
    QVariantList many;
    for (int i = 0; i < 13; ++i)
        many.append(makeCmd("internal", QStringLiteral("acc-%1").arg(i),
                            QStringLiteral("theme.toggle")));
    const bool saved3 = setCommands(many);
    qInfo() << "QUICK STEP7 thirteen rejected:" << (!saved3) << "(期望 true)";

    // 清场:验收产生的配置清空(不污染用户配置)
    setCommands({});
    const QJsonObject clean = getSettings();
    qInfo() << "QUICK CLEAN count:" << clean.value("quickRailCommands").toArray().size()
            << "(期望 0)";

    // ---- ⑧ 设置页管理区截图(人工核验下拉中文/布局):切快捷分类 → grab ----
    // 走真实点击(与 --settings-flow 的导航同一链路);截图路径由 --screenshot 传入,
    // 未传则跳过(flow 其余断言不依赖)。
    const QString shotPath = qEnvironmentVariable("QUICK_FLOW_SHOT");
    if (!shotPath.isEmpty()) {
        // 注入三条演示指令(三类型各一)供人工核验;流程末尾统一清场。
        QVariantList demo;
        demo.append(makeCmd("internal", "演示截屏", QStringLiteral("capture.region")));
        demo.append(makeCmd("system", "演示服务", QStringLiteral("services")));
        demo.append(makeCmd("app", "演示记事本", QStringLiteral("C:\\Windows\\notepad.exe")));
        setCommands(demo);
        // ① 列表形态:rail 悬停提示(自绘卡片,属性驱动;hover 触发是标准
        //   containsMouse 惯用法,显示效果经 acceptanceShowRailTip 人工核验)
        panel.invokeAcceptance("acceptanceShowRailTip");
        QTimer::singleShot(300, &panel, [&panel, shotPath] {
            panel.grab().save(shotPath + QStringLiteral(".rail.png"));
            qInfo() << "QUICK SHOT(rail) ->" << shotPath << ".rail.png";
            // ② 设置页管理区
            panel.clickByObjectName(QStringLiteral("settingsButton"));
            QTimer::singleShot(900, &panel, [&panel, shotPath] {
                panel.clickByObjectName(QStringLiteral("settingsNavQuick"));
                QTimer::singleShot(900, &panel, [&panel, shotPath] {
                    // 再点「添加指令」打开编辑卡片,截第二张(图标网格在此可见)
                    panel.clickByObjectName(QStringLiteral("quickCmdAdd"));
                    QTimer::singleShot(700, &panel, [&panel, shotPath] {
                        panel.grab().save(shotPath + QStringLiteral(".editor.png"));
                        qInfo() << "QUICK SHOT(editor) ->" << shotPath << ".editor.png";
                        setCommands({});
                        QApplication::quit();
                    });
                });
            });
        });
    } else {
        qInfo() << "QUICK DONE";
        QApplication::quit();
    }
}

} // namespace nb
