// Nextboard 入口:Qt 拥有进程与事件循环,Qt Quick 承载主面板,业务在 nbcore.dll。
//
// 启动顺序:单实例检查 → nbcore init → QML 装配 → 主面板(Quick)→ 托盘/热键 → 消息循环。
// 失焦隐藏等窗口行为见 panel_window.cpp。

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QTimer>
#include <QDebug>

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
#include "host/tray.hpp"
#include "host/hotkeys.hpp"
#include "panel/panel_window.hpp"
#include "panel/qml_bootstrap.hpp"

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
    // 常驻托盘(第一代语义):关闭/隐藏最后一个窗口不退出,退出仅走托盘菜单
    QApplication::setQuitOnLastWindowClosed(false);
    qInstallMessageHandler(fileMessageHandler);
    qInfo() << "Nextboard starting, appDir =" << QCoreApplication::applicationDirPath();

    // 1. nbcore 初始化(数据目录、通知窗口稍后由面板提供;先 0 = 仅全局通知)。
    nb::CoreBridge& core = nb::CoreBridge::instance();
    QString initError;
    if (!core.init(startHidden, &initError)) {
        qCritical() << "nbcore init failed:" << initError;
        QMessageBox::critical(nullptr, "Nextboard", QStringLiteral("核心初始化失败:%1").arg(initError));
        return 1;
    }
    qInfo() << "nbcore ready";

    // 2. QML 装配(类型/单例注册)→ 主面板
    nb::setupQmlEnvironment();
    nb::PanelWindow panel(core);
    nb::qmlAttachPanel(&panel);
    panel.showAtCursor();
    if (startHidden) panel.hide();

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

    // nbcore 通知 HWND = 主面板窗口;WM_APP → PanelWindow::clipChanged
    // → ClipEntryModel::refresh(200ms 去抖)。
    core.setNotifyHwnd(panel.nativeHandle());

    const int rc = QApplication::exec();

    core.shutdown();
    return rc;
}
