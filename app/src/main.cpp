// Nextboard 入口:Qt 拥有进程与事件循环,Qt Quick 承载主面板,业务在 nbcore.dll。
//
// 启动顺序:单实例检查 → nbcore init → QML 装配 → 主面板(Quick)→ 托盘/热键 → 消息循环。
// 失焦隐藏等窗口行为见 panel_window.cpp。

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonObject>
#include <QMessageBox>
#include <QPainter>
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
        // 仅 Warning 及以上才强制刷盘(flush 含 _commit 同步盘);Debug/Info 交给
        // OS 页缓存。注意 QtMsgType 枚举序(Info=4 排在 Warning 之后),不可用 >= 比较。
        // Info/Debug 不刷盘的代价:进程被强杀时尾部普通日志可能丢,崩溃诊断(WARN+)不受影响。
        if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) log.flush();
    }
}

#include "host/single_instance.hpp"
#include "host/core_bridge.hpp"
#include "host/memory_probe.hpp"
#include "host/tray.hpp"
#include "note/note_registry.hpp"
#include "host/hotkeys.hpp"
#include "host/acceptance.hpp"
#include "host/auto_start.hpp"
#include "panel/core_facade.hpp"
#include "pin/pin_window.hpp"
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
    // 出错地址落在哪个模块(+偏移):地址本身因 ASLR 每次都变,模块名才是
    // 能对上号的。反查不到(堆上执行等)就只报地址。
    QString where;
    MEMORY_BASIC_INFORMATION mb{};
    if (address && VirtualQuery(address, &mb, sizeof(mb)) && mb.Type == MEM_IMAGE) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(address), &mod) && mod) {
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(mod, path, MAX_PATH)) {
                const QString file = QFileInfo(QString::fromWCharArray(path)).fileName();
                const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
                const uintptr_t at = reinterpret_cast<uintptr_t>(address);
                where = QStringLiteral(" module=%1 offset=0x%2")
                            .arg(file)
                            .arg(at - base, 0, 16);
            }
        }
    }
    appendCrashLog(QStringLiteral("未处理异常 code=0x%1 address=%2%3")
                       .arg(static_cast<quint32>(code), 8, 16, QLatin1Char('0'))
                       .arg(reinterpret_cast<quintptr>(address), 0, 16)
                       .arg(where));
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
    QString updateUrlOverride; // --update-url:验收注入的更新检查地址(必须早于 QML 装配)
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--hidden")) startHidden = true;
        else if (arg == QLatin1String("--software")) softwareBackend = true;
        else if (arg == QLatin1String("--hardware")) hardwareBackend = true;
        else if (arg == QLatin1String("--update-url") && i + 1 < argc)
            updateUrlOverride = QString::fromLocal8Bit(argv[++i]);
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
                || arg == QLatin1String("--update-flow")
                || arg == QLatin1String("--dump-monitors")
                || arg == QLatin1String("--capture-toolbar-flow")
                || arg == QLatin1String("--editor-flow")
                || arg == QLatin1String("--autostart-flow")
                || arg == QLatin1String("--stitch-flow")
                || arg == QLatin1String("--stitch-settings-flow")
                || arg == QLatin1String("--stitch-live-flow")
                || arg == QLatin1String("--note-flow")
                || arg == QLatin1String("--todo-layout-flow")) {
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
    // 开机自启的命令行形态升级:0.2.0 及更早的应用内开关只写 `"<exe>"`,不带
    // --hidden,于是"设置里开了自启 → 每次开机主面板抢焦点弹出"。已启用但命令行
    // 是旧形态时就地改写(用户不必去设置页关了再开)。安装器自己的 SecAutoStart
    // 从一开始就带 --hidden,这里补齐应用内开关这条路径 —— docs/04 §8 D11。
    if (nb::auto_start::refreshCommandLineIfStale())
        qInfo() << "auto_start: 自启命令行已升级为带 --hidden(开机静默驻留托盘)";

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
    // 更新检查地址的验收覆盖必须落在 QML 装配**之前**:`updateCheckUrl` 是 CONSTANT 属性,
    // 绑定时读一次就定型 —— 晚设不会生效(表现为"验收仍在打编译期那个空地址")。
    if (!updateUrlOverride.isEmpty())
        nb::CoreFacade::setUpdateCheckUrlOverride(updateUrlOverride);
    // 验收模式:任一 --*-flow / --todo-groups 出现即置位(必须早于 PanelWindow
    // 构造,QML 绑定初始化时要读到)。Main.qml 据此门控 acceptanceState ——
    // 产品路径不再为断言维持一串活绑定。
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.endsWith(QStringLiteral("-flow")) || arg == QLatin1String("--todo-groups")) {
            nb::PanelWindow::setAcceptanceModeForNextInstance();
            break;
        }
    }
    nb::setupQmlEnvironment();
    nb::logMemory("qml-env");      // + Qt Quick Controls 风格与全局字体
    nb::PanelWindow panel(core);
    nb::logMemory("panel-built");  // + 主面板 QQuickWidget 与整棵 QML 树(含列表)
    nb::qmlAttachPanel(&panel);
    if (startHidden) {
        // 开机自启(--hidden):**不上屏**,而不是"显示后立刻隐藏"(后者会闪一帧)。
        panel.prepareHiddenStart();
    } else {
        panel.showAtCursor();
    }
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
    bool updateFlow = false;
    bool todoGroups = false;
    bool dumpMonitors = false;
    bool captureToolbarFlow = false;
    bool editorFlow = false;
    bool autoStartFlow = false;
    bool stitchFlow = false;
    bool stitchSettingsFlow = false;
    bool stitchLiveFlow = false;
    bool noteFlow = false;
    bool todoLayoutFlow = false;
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
        // 更新检查验收:真实点击「检查更新」(检查地址由 --update-url 指向本地 fixture)。
        else if (arg == QLatin1String("--update-flow"))
            updateFlow = true;
        // 待办分组自检:只读回各行分组键(配合脚本播种数据验证组标题分界点)。
        else if (arg == QLatin1String("--todo-groups"))
            todoGroups = true;
        // 多屏配对自检:打印两端显示器清单并核对按名配对/底图自洽性。
        else if (arg == QLatin1String("--dump-monitors"))
            dumpMonitors = true;
        // 框选浮条 + 标注态验收:真遮罩 + 真鼠标事件,断言标注落点与合成图
        // (见 host/capture_toolbar_flow.cpp)。不写库,故不进 --data-dir 门禁。
        else if (arg == QLatin1String("--capture-toolbar-flow"))
            captureToolbarFlow = true;
        // 编辑器裁剪/旋转验收:引擎级 + 真窗口端到端(见 host/editor_flow.cpp)。
        // 不写库,故不进 --data-dir 门禁。
        else if (arg == QLatin1String("--editor-flow"))
            editorFlow = true;
        // 自启形态验收:真实读写 Run 键,断言应用内开关写的命令行带 --hidden
        // (与安装器同形态)。会临时改写用户的 Run 键,结束前必定还原。
        else if (arg == QLatin1String("--autostart-flow"))
            autoStartFlow = true;
        // 长截图验收:构造帧跑通 append→finish→abort,断言位移/缺口/会话隔离。
        // 纯内存图像运算,不写库,故不进 --data-dir 门禁。
        else if (arg == QLatin1String("--stitch-flow"))
            stitchFlow = true;
        // 长截图设置往返验收:写读比 + 非法值拒绝。会改设置,**必须**配 --data-dir。
        else if (arg == QLatin1String("--stitch-settings-flow"))
            stitchSettingsFlow = true;
        // 长截图真实采集验收:真窗口 + 真滚动 + 真采屏。会移动鼠标抓屏,故隔离数据目录。
        else if (arg == QLatin1String("--stitch-live-flow"))
            stitchLiveFlow = true;
        // 桌面便签交互验收(docs/20 §10.2):会移动真实光标,故隔离数据目录。
        else if (arg == QLatin1String("--note-flow"))
            noteFlow = true;
        // 编辑卡片字段顺序验收(docs/21):纯读回,不写库
        else if (arg == QLatin1String("--todo-layout-flow"))
            todoLayoutFlow = true;
    }
    // 待办交互验收(--todo-flow):真实点击驱动整条链路,逻辑见 host/acceptance.cpp
    if (todoFlow) nb::runTodoAcceptanceFlow(panel);
    if (clearDoneFlow) nb::runClearDoneFlow(panel);
    if (settingsFlow) nb::runSettingsFlow(panel);
    if (restoreFlow) nb::runRestoreFlow(panel);
    if (clearAllFlow) nb::runClearAllFlow(panel);
    if (updateFlow) {
        // 「打开下载页」在验收里降级为日志:真起浏览器会抢焦点、干扰断言,也没法反复跑
        // (docs/07-auto-update.md §11.2)。真实打开由发布前人工冒烟覆盖。
        nb::CoreFacade::setBrowserLaunchDisabledForAcceptance(true);
        nb::runUpdateFlow(panel);
    }
    if (todoGroups) nb::reportTodoGroups(panel);
    if (dumpMonitors) nb::reportMonitors();
    if (captureToolbarFlow) nb::runCaptureToolbarFlow();
    if (editorFlow) nb::runEditorFlow();
    if (autoStartFlow) nb::runAutoStartFlow();
    if (stitchFlow) nb::runStitchFlow();
    if (stitchSettingsFlow) nb::runStitchSettingsFlow();
    if (stitchLiveFlow) nb::runStitchLiveFlow();
    if (noteFlow) nb::runNoteAcceptanceFlow();
    if (todoLayoutFlow) nb::runTodoLayoutFlow(panel);
    // 贴图 DPI 探针(--pin-probe <path>):以四角色块的测试图建一枚贴图,渲染
    // 稳定后 grab() 存档再退出。回归件:2026-10-26 缩放缓存误用 DPR 画布,
    // 高分屏上贴图内容放大 1/d² 且裁边 —— 本探针在任何缩放比下都可断言
    // "四个角色块完整在框内"。
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--pin-probe")
            && i + 1 < argc) {
            const QString probePath = QString::fromLocal8Bit(argv[++i]);
            QTimer::singleShot(800, qApp, [probePath] {
                QImage img(480, 320, QImage::Format_ARGB32_Premultiplied);
                img.fill(0xFFF5F5F5);
                {
                    QPainter p(&img);
                    p.fillRect(0, 0, 60, 60, QColor(220, 30, 30));        // 左上 红
                    p.fillRect(420, 0, 60, 60, QColor(30, 160, 30));      // 右上 绿
                    p.fillRect(0, 260, 60, 60, QColor(30, 30, 220));      // 左下 蓝
                    p.fillRect(420, 260, 60, 60, QColor(220, 200, 30));   // 右下 黄
                    p.fillRect(200, 120, 80, 80, QColor(20, 20, 20));     // 中央 黑
                }
                nb::PinContent content;
                content.idPrefix = QStringLiteral("pinprobe");
                content.image = img;
                auto* pin = new nb::PinWindow(content, nb::PinOptions{});
                pin->setAttribute(Qt::WA_DeleteOnClose);
                pin->show();
                QTimer::singleShot(600, pin, [pin, probePath] {
                    qInfo() << "pin-probe ->" << probePath;
                    pin->grab().save(probePath);
                    QApplication::quit();
                });
            });
            break;
        }
    }
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

    // ---- 桌面便签(docs/20-todo-sticky-note.md)----
    // 启动恢复:只为 todo_note.visible=1 的行建窗(visible=0 是"几何记忆")。
    // ⚠️ 必须在 setNotifyHwnd 之后:恢复时会读库,而库通知目标此刻才就位。
    nb::NoteRegistry::instance().restoreAll();
    // 启动自动备份(24h 节流,后台线程,保留最近 3 份):静默数据安全网。
    // 放在恢复之后:此时核心就绪、且启动高峰的写库(恢复/迁移)已落定。
    nb::CoreFacade::instance().maybeAutoBackup();
    // 面板改动(改标题/备注/完成/删除)→ 便签同步。
    //
    // ⚠️ 必须接 **PanelWindow::clipChanged**,不是 CoreFacade::clipChanged
    //    (2026-10-18 修"编辑待办后便签不刷新,要关掉重贴才变"):
    //    core 的 WM_APP_CLIP_CHANGED 由 PanelWindow::nativeEvent 转发成
    //    PanelWindow::clipChanged —— **所有** sqlite 写入(含 todo_update /
    //    todo_set_done / todo_delete)都走这一条;
    //    而 CoreFacade::clipChanged 只在宿主 C++ 主动调 notifyClipChanged() 的
    //    两处(截图入库、编辑器提交)发,面板内的编辑根本不经过它。
    //    接错信号的症状正是"便签永远停在打开那一刻的内容"。
    // 同步本身 200ms 去抖:clipChanged 由**所有** sqlite 写入触发(含每一次
    // 剪贴板入库),连续复制粘贴时每次复制都会全量拉一次 todo_note_list FFI。
    // syncFromCore 有"无便签早退/内容不变不回灌"兜底,这里只合并突发。
    auto* noteSyncDebounce = new QTimer(&panel);
    noteSyncDebounce->setSingleShot(true);
    noteSyncDebounce->setInterval(200);
    QObject::connect(noteSyncDebounce, &QTimer::timeout, &panel,
                     [] { nb::NoteRegistry::instance().syncFromCore(); });
    QObject::connect(&panel, &nb::PanelWindow::clipChanged, &panel,
                     [noteSyncDebounce] { noteSyncDebounce->start(); });
    // 屏幕增删/分辨率变化:全部重夹回各自可用区(拔副屏后不留虚空的窗口)
    QObject::connect(qApp, &QGuiApplication::screenAdded, &panel,
                     [] { nb::NoteRegistry::instance().reclampAll(); });
    QObject::connect(qApp, &QGuiApplication::screenRemoved, &panel,
                     [] { nb::NoteRegistry::instance().reclampAll(); });

    const int rc = QApplication::exec();

    core.shutdown();
    return rc;
}
