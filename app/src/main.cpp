// Nextboard 入口:Qt 拥有进程与事件循环,Qt Quick 承载主面板,业务在 nbcore.dll。
//
// 启动顺序:单实例检查 → nbcore init → QML 装配 → 主面板(Quick)→ 托盘/热键 → 消息循环。
// 失焦隐藏等窗口行为见 panel_window.cpp。

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QDebug>

#include <cstdlib>
#include <exception>

#include <windows.h>

// S2 阶段:qDebug/qInfo 全量落文件(发布期移除);WIN32 子程序无控制台,这是唯一可观测手段
static void fileMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    static QFile log(QCoreApplication::applicationDirPath() + "/nextboard.log");
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
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"
#include "panel/qml_bootstrap.hpp"
#include "translate/translate_window.hpp"

// ---- 崩溃兜底 ----
//
// 本应用是托盘程序、没有控制台,而崩溃常常表现为**静默进程死亡**(日志停在最后一条
// 正常记录上),事后完全无从下手。这里把未处理异常与 std::terminate 也写进同一份日志,
// 至少留下异常码与地址。必须在 QApplication 之后安装(applicationDirPath 依赖它)。
namespace {

void appendCrashLog(const QString& text) {
    QFile log(QCoreApplication::applicationDirPath() + "/nextboard.log");
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
    // 隐藏自启参数(与第一代 --hidden 语义一致)
    bool startHidden = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--hidden")) startHidden = true;
    }

    SingleInstance instance;
    if (!instance.ensure("NextboardSingleInstance")) {
        // 已有实例:激活它的窗口后退出
        instance.activateExisting();
        return 0;
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
    qInstallMessageHandler(fileMessageHandler);
    ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::set_terminate(onTerminate);
    qInfo() << "Nextboard starting, appDir =" << QCoreApplication::applicationDirPath();
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

    // 翻译窗口:独立顶层窗口,但**按需创建、关闭即销毁**(架构 D12)。
    // 它自带一个 QQuickWidget,即第二个 QQmlEngine + 场景图 —— 只要它存在就要多付
    // 一份 QML 运行时(具体数值看 mem[translate-open] 与 mem[translate-closed] 的差)。
    // 翻译是低频操作,重建成本远小于常驻代价。
    // 三条打开链路(面板按钮 / 条目右键 / 全局热键)统一走 translatePrefill。
    QPointer<nb::TranslateWindow> translate;
    QObject::connect(&nb::CoreFacade::instance(), &nb::CoreFacade::translatePrefill,
                     &panel, [&panel, &translate](const QString& text) {
                         // 两个窗口不并存:打开翻译即收起面板,避免互相遮挡
                         panel.hide();
                         if (!translate) {
                             translate = new nb::TranslateWindow();
                             nb::logMemory("translate-open");   // 量化第二个 QML 引擎的代价
                         }
                         translate->openWith(text);
                     });

    // --screenshot <path>:启动后延时自截(grab 渲染结果),自动化验收用。
    // --query <text>:与 --screenshot 联动,先注入搜索词再截图(验收搜索链路)。
    QString shotPath, queryText;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--screenshot") && i + 1 < argc)
            shotPath = QString::fromLocal8Bit(argv[++i]);
        else if (arg == QLatin1String("--query") && i + 1 < argc)
            queryText = QString::fromLocal8Bit(argv[++i]);
    }
    if (!shotPath.isEmpty()) {
        QTimer::singleShot(5000, &panel, [path = shotPath, queryText, &panel] {
            if (!queryText.isEmpty()) panel.applySearchText(queryText);
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

    // nbcore 通知 HWND = 主面板窗口;WM_APP → PanelWindow::clipChanged
    // → ClipEntryModel::refresh(200ms 去抖)。
    core.setNotifyHwnd(panel.nativeHandle());

    const int rc = QApplication::exec();

    core.shutdown();
    return rc;
}
