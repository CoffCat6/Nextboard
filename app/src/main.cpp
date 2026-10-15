// Nextboard 入口:Qt 拥有进程与事件循环,Qt Quick 承载主面板,业务在 nbcore.dll。
//
// 启动顺序:单实例检查 → nbcore init → QML 装配 → 主面板(Quick)→ 托盘/热键 → 消息循环。
// 失焦隐藏等窗口行为见 panel_window.cpp。

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QJsonObject>
#include <QMessageBox>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QDebug>

#include <cstdlib>
#include <exception>

#include <windows.h>

// 数据/日志目录(exe 同级 data/)的单一来源;下面的日志处理器要用,故先于代码段引入
#include "host/app_paths.hpp"

// S2 阶段:qDebug/qInfo 全量落文件(发布期移除);WIN32 子程序无控制台,这是唯一可观测手段。
// 日志与数据同处:`<exe 同级>\data\nextboard.log`(便携式布局,见 host/app_paths.hpp)
static void fileMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    static QFile log(nb::appDataFile(QStringLiteral("nextboard.log")));
    if (!log.isOpen()) log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    const char* level = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO"
        : type == QtWarningMsg ? "WARN" : type == QtCriticalMsg || type == QtFatalMsg ? "CRIT" : "?";
    if (log.isOpen()) {
        log.write(QStringLiteral("[%1] %2\n").arg(level, msg).toUtf8());
        log.flush();
    }
}

#include "host/single_instance.hpp"
#include "host/core_bridge.hpp"
#include "host/memory_probe.hpp"
#include "host/tray.hpp"
#include "host/hotkeys.hpp"
#include "host/acceptance.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"
#include "panel/qml_bootstrap.hpp"

// ---- 崩溃兜底 ----
//
// 本应用是托盘程序、没有控制台,而崩溃常常表现为**静默进程死亡**(日志停在最后一条
// 正常记录上),事后完全无从下手。这里把未处理异常与 std::terminate 也写进同一份日志,
// 至少留下异常码与地址。必须在 QApplication 之后安装(applicationDirPath 依赖它)。
namespace {

void appendCrashLog(const QString& text) {
    QFile log(nb::appDataFile(QStringLiteral("nextboard.log")));
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        log.write(QStringLiteral("[CRASH] %1\n").arg(text).toUtf8());
}

/// 常见异常码:0xC0000005 访问违例 / 0xC0000409 栈溢出或 fail-fast /
/// 0xE06D7363 未捕获的 C++ 异常 / 0xC0000094 整数除零。
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    const DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    const void* address =
        (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionAddress : nullptr;
    appendCrashLog(QStringLiteral("未处理异常 code=0x%1 address=%2")
                       .arg(static_cast<quint32>(code), 8, 16, QLatin1Char('0'))
                       .arg(reinterpret_cast<quintptr>(address), 0, 16));
    return EXCEPTION_EXECUTE_HANDLER;   // 记完即终止,不再弹系统崩溃框
}

void onTerminate() {
    appendCrashLog(QStringLiteral("std::terminate(未捕获的 C++ 异常 / noexcept 违反)"));
    std::abort();
}

} // namespace

int main(int argc, char* argv[]) {
    // 隐藏自启参数(与第一代 --hidden 语义一致);
    // --software / --hardware:强制 Qt Quick 用软件(CPU 光栅)/ 硬件(D3D11)后端,
    //   用于对比渲染后端的内存与手感(软件是默认,硬件用于 A/B 与显卡充足场景)。
    // --data-dir <path>:覆盖数据目录,验收/多实例调试用 —— **必须在任何
    //   appDataDir() 调用之前解析并生效**,否则会先落到真实数据目录。
    bool startHidden = false;
    bool softwareBackend = false;
    bool hardwareBackend = false;
    bool ownDataDir = false;   // 显式 --data-dir:验收/调试,不参与单实例门禁
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--hidden")) startHidden = true;
        else if (arg == QLatin1String("--software")) softwareBackend = true;
        else if (arg == QLatin1String("--hardware")) hardwareBackend = true;
        else if (arg == QLatin1String("--data-dir") && i + 1 < argc) {
            nb::setAppDataDirOverride(QString::fromLocal8Bit(argv[++i]));
            ownDataDir = true;
        }
    }

    // 单实例门禁:同一数据目录只允许一个实例。
    // ⚠️ 指定了 --data-dir 时**跳过**(验收/调试常与用户实例并存,且用的是
    //    独立数据目录,不存在竞争;否则第二次验收启动会被门禁静默挡掉)。
    // ⚠️ 验收模式(各种 --*-flow / --todo-groups)会**写库或改状态**,
    //    若调用方忘了配 --data-dir,默认落到真实数据目录里 —— 这里兜底切到独立目录,
    //    保证验收不污染用户数据。目录固定在 **exe 同级**的 `acceptance-data`(不用临时
    //    目录:临时目录受 TMP 环境变量影响,位置不确定,排查时反而找不到数据)。
    //    --todo-groups 只是读回,但同样要隔离:它靠脚本**先播种**数据才有效,
    //    而播种会写库。
    if (!ownDataDir) {
        for (int i = 1; i < argc; ++i) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            if (arg == QLatin1String("--todo-flow")
                || arg == QLatin1String("--todo-groups")
                || arg == QLatin1String("--clear-done-flow")
                || arg == QLatin1String("--settings-flow")
                || arg == QLatin1String("--restore-flow")
                || arg == QLatin1String("--clear-all-flow")
                || arg == QLatin1String("--dump-monitors")) {
                const QString dir = QDir(QCoreApplication::applicationDirPath())
                                        .filePath(QStringLiteral("acceptance-data"));
                const bool created = QDir().mkpath(dir);
                nb::setAppDataDirOverride(dir);
                ownDataDir = true;
                qWarning() << arg << "未指定 --data-dir,已改用" << dir
                           << "(建目录成功:" << created << ")以免污染真实数据";
                break;
            }
        }
    }
    if (!ownDataDir) {
        SingleInstance instance;
        if (!instance.ensure("NextboardSingleInstance")) {
            // 已有实例:激活它的窗口后退出
            instance.activateExisting();
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("Nextboard");
    QApplication::setOrganizationName("Nextboard");
    // 应用图标:主面板标记(AppMark)的 ico 版,已打进 Qt 资源(不依赖工作目录)。
    // 设为全局窗口图标 → 任务栏 / 窗口左上角 / QMessageBox 都用它;
    // 托盘图标也取这里的值(见 tray.cpp),保证同源。
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/resources/nextboard.ico")));
    // 常驻托盘(第一代语义):关闭/隐藏最后一个窗口不退出,退出仅走托盘菜单
    QApplication::setQuitOnLastWindowClosed(false);
    // 数据目录(exe 同级 data/):日志、设置、数据库、截图 blob 全部落在这一处。
    // 必须在装日志处理器之前建好,否则首条日志无处可写(表现为"静默无日志")。
    const bool dataDirReady = nb::ensureAppDataDir();
    qInstallMessageHandler(fileMessageHandler);
    ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(onTerminate);
    qInfo() << "Nextboard starting, appDir =" << QCoreApplication::applicationDirPath();
    if (dataDirReady) qInfo() << "data dir =" << nb::appDataDir();
    else qWarning() << "数据目录创建失败(安装目录需可写):" << nb::appDataDir();
    // 启动分段读数:基线高时据此定位是"Qt 运行时 / nbcore / QML 装配 / 面板 QML 树"
    // 哪一段吃掉的,而不是靠猜。
    nb::logMemory("app-init");   // Qt 运行时 + 平台插件,尚未加载任何 QML

    // 1. nbcore 初始化(数据目录、通知窗口稍后由面板提供;先 0 = 仅全局通知)。
    nb::CoreBridge& core = nb::CoreBridge::instance();
    QString initError;
    if (!core.init(startHidden, &initError)) {
        qCritical() << "nbcore init failed:" << initError;
        QMessageBox::critical(nullptr, "Nextboard", QStringLiteral("核心初始化失败:%1").arg(initError));
        return 1;
    }
    qInfo() << "nbcore ready";
    nb::logMemory("core-ready");   // + nbcore.dll(Rust 运行时 + SQLite)

    // 渲染后端:必须在**构造第一个 QQuickWindow 之前**设定(官方文档),运行期不可切换,
    // 故为"重启生效"设置。**默认软件(CPU 光栅)**:实测比硬件(D3D11)后端省
    // ~55MB 首帧 / ~101MB 稳态(隐藏态 231MB → 130MB),是唯一"开箱即超预算"的来源
    // (docs/06-memory-optimization.md §1)。设置项或 --hardware 可切回硬件后端。
    // 命令行开关优先级高于设置,便于 A/B 与排障。
    bool softwareFromSettings = false;
    {
        const QJsonObject s = core.call("get_settings").value("data").toObject();
        softwareFromSettings = s.value("softwareRender").toBool(true);
    }
    const bool useSoftware = hardwareBackend ? false : (softwareBackend || softwareFromSettings);
    if (useSoftware) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        qInfo() << "Qt Quick graphics API = Software"
                << (softwareBackend ? "(--software 覆盖)" : "(设置项/默认)");
    } else {
        qInfo() << "Qt Quick graphics API = Hardware (D3D11)"
                << (hardwareBackend ? "(--hardware 覆盖)" : "(设置项)");
    }

    // 2. QML 装配(类型/单例注册)→ 主面板
    nb::setupQmlEnvironment();
    nb::logMemory("qml-env");      // + Qt Quick Controls 风格与全局字体
    nb::PanelWindow panel(core);
    nb::logMemory("panel-built");  // + 主面板 QQuickWidget 与整棵 QML 树(含列表)
    nb::qmlAttachPanel(&panel);
    panel.showAtCursor();
    if (startHidden) panel.hide();
    // 内存基线:此后每个关键事件都有一条读数,便于做前后对比(S6.5 验收)
    nb::logMemory("startup");

    // 翻译已并入主面板,做成「整窗模式切换」(不再是独立顶层窗口)。
    // 三条打开链路(面板按钮 / 条目右键 / 全局热键)统一走 translatePrefill →
    // panel.showTranslate(跟随光标);托盘链路走 translatePrefillCentered(居中屏幕)。
    // 于是不再有第二个 QQmlEngine 与它自己的场景图/渲染栈 —— 这是内存收口的主要收益。
    QObject::connect(&nb::CoreFacade::instance(), &nb::CoreFacade::translatePrefill,
                     &panel, [&panel](const QString& text) {
        panel.showTranslate(text, nb::PanelWindow::Orientation::AtCursor);
    });
    QObject::connect(&nb::CoreFacade::instance(), &nb::CoreFacade::translatePrefillCentered,
                     &panel, [&panel](const QString& text) {
        panel.showTranslate(text, nb::PanelWindow::Orientation::ScreenCenter);
    });

    // --screenshot <path>:启动后延时自截(grab 渲染结果),自动化验收用。
    // --query <text>:与 --screenshot 联动,先注入搜索词再截图(验收搜索链路)。
    // --settings:与 --screenshot 联动,先打开设置抽屉再截图(验收懒构建链路)。
    // --translate <text>:与 --screenshot 联动,切到翻译页并预填原文再截图
    //   (验收翻译页;原文非空会自动翻译,顺带覆盖译文区)。
    QString shotPath, queryText, translateText;
    bool openSettings = false;
    bool todoFlow = false;
    bool clearDoneFlow = false;
    bool settingsFlow = false;
    bool restoreFlow = false;
    bool clearAllFlow = false;
    bool todoGroups = false;
    bool dumpMonitors = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--screenshot") && i + 1 < argc)
            shotPath = QString::fromLocal8Bit(argv[++i]);
        else if (arg == QLatin1String("--query") && i + 1 < argc)
            queryText = QString::fromLocal8Bit(argv[++i]);
        else if (arg == QLatin1String("--translate") && i + 1 < argc)
            translateText = QString::fromLocal8Bit(argv[++i]);
        else if (arg == QLatin1String("--settings"))
            openSettings = true;
        // 待办交互验收:走"新建卡片 → 开浮层 → 拨轮 → 确定 → 保存"整条链路,
        // 结果(浮层几何/字段值/是否关闭)打日志供脚本断言。
        else if (arg == QLatin1String("--todo-flow"))
            todoFlow = true;
        // 清空已完成待办验收:播种归档 → 切待办视图 → 真实点击「清空已完成」
        // → 点确认 → 读库断言(见 acceptance.cpp 的 runClearDoneFlow)。
        else if (arg == QLatin1String("--clear-done-flow"))
            clearDoneFlow = true;
        // 设置抽屉交互验收:手输数值保存 / 抽屉模态与三种关闭路径 / 待办页签
        // 恒显示(见 acceptance.cpp 的 runSettingsFlow)。
        else if (arg == QLatin1String("--settings-flow"))
            settingsFlow = true;
        // 恢复/导入验收:确认真实点击驱动"备份 zip 的消费端"(见 runRestoreFlow)。
        else if (arg == QLatin1String("--restore-flow"))
            restoreFlow = true;
        // 清空全部数据验收:播种剪贴板 + 待办 → 真实点击「清空全部数据」→ 点确认
        // → 读库断言全清(见 runClearAllFlow)。
        else if (arg == QLatin1String("--clear-all-flow"))
            clearAllFlow = true;
        // 待办分组自检:只读回各行分组键(配合脚本播种数据验证组标题分界点)。
        else if (arg == QLatin1String("--todo-groups"))
            todoGroups = true;
        // 多屏配对自检:打印两端显示器清单并核对按名配对/底图自洽性。
        else if (arg == QLatin1String("--dump-monitors"))
            dumpMonitors = true;
    }
    // 待办交互验收(--todo-flow):真实点击驱动整条链路,逻辑见 host/acceptance.cpp
    if (todoFlow) nb::runTodoAcceptanceFlow(panel);
    if (clearDoneFlow) nb::runClearDoneFlow(panel);
    if (settingsFlow) nb::runSettingsFlow(panel);
    if (restoreFlow) nb::runRestoreFlow(panel);
    if (clearAllFlow) nb::runClearAllFlow(panel);
    if (todoGroups) nb::reportTodoGroups(panel);
    if (dumpMonitors) nb::reportMonitors();
    if (!shotPath.isEmpty()) {
        QTimer::singleShot(5000, &panel, [path = shotPath, queryText, openSettings,
                                          translateText, &panel] {
            if (!queryText.isEmpty()) panel.applySearchText(queryText);
            if (openSettings) panel.openSettingsPanel();
            if (!translateText.isEmpty()) {
                // 翻译页要"停留"(失焦不隐藏),居中摆放也让自截位置确定
                panel.showTranslate(translateText, nb::PanelWindow::Orientation::ScreenCenter);
            }
            // 再给一拍防抖 + 刷新时间
            QTimer::singleShot(800, &panel, [path, &panel] {
                qInfo() << "self-screenshot ->" << path;
                panel.grab().save(path);
                QApplication::quit();
            });
        });
    }

    // 3. 托盘与热键
    nb::Tray tray(panel, core);
    nb::Hotkeys hotkeys(panel, core);
    // 设置里改完快捷键后重新注册(信号由 CoreFacade 发出,避免它依赖 Hotkeys 实例)
    QObject::connect(&nb::CoreFacade::instance(), &nb::CoreFacade::hotkeysChanged,
                     &hotkeys, &nb::Hotkeys::applyFromSettings);

    // 待办到点(remind/notify):托盘气泡 + 列表/角标刷新(docs/05-todo.md §4.3)。
    // 面板**不自动弹出** —— 气泡点击才弹(§6.5,避免打扰)。
    // ⚠️ 连接必须在 tray 构造之后(tray 是栈对象,先声明先构造)。
    QObject::connect(&panel, &nb::PanelWindow::todoDue, &tray, &nb::Tray::onTodoDue);
    // 气泡点击(QSystemTrayIcon::messageClicked)→ 弹面板并切"待办"视图
    QObject::connect(&nb::CoreFacade::instance(), &nb::CoreFacade::todosRequested,
                     &panel, &nb::PanelWindow::showCenteredOnTodos);
    // 托盘「今日待办」子菜单点击某条 → 弹面板定位高亮该条
    QObject::connect(&nb::CoreFacade::instance(), &nb::CoreFacade::todoOpenRequested,
                     &panel, &nb::PanelWindow::showCenteredOnTodo);

    // nbcore 通知 HWND = 主面板窗口;WM_APP → PanelWindow::clipChanged
    // → ClipEntryModel::refresh(200ms 去抖)。
    core.setNotifyHwnd(panel.nativeHandle());

    const int rc = QApplication::exec();

    core.shutdown();
    return rc;
}
