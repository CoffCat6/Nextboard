#include "quick_commands.hpp"

#include "capture/capture_session.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QTimer>
#include <QRegularExpression>
#include <QVariantMap>
#include <QVector>
#include <windows.h>
#include <shellapi.h>

namespace nb {
namespace {

using SystemEntry = struct { const char* key; const char* label; const char* exec; };

/// system 精选目录(docs/24 §5.1):target 存 key,执行串在这里 ——
/// 新增系统页 = 表里加一行。ms-settings: 前缀的手填项不经本表,直接 ShellExecute。
const SystemEntry kSystemEntries[] = {
    {"env-vars",       "环境变量",    "rundll32.exe sysdm.cpl,EditEnvironmentVariables"},
    {"services",       "服务",        "services.msc"},
    {"network",        "网络连接",    "ncpa.cpl"},
    {"firewall",       "防火墙",      "wf.msc"},
    {"devmgmt",        "设备管理器",  "devmgmt.msc"},
    {"diskmgmt",       "磁盘管理",    "diskmgmt.msc"},
    {"taskmgr",        "任务管理器",  "taskmgr.exe"},
    {"control",        "控制面板",    "control.exe"},
    {"display",        "显示设置",    "ms-settings:display"},
    {"sound",          "声音设置",    "mmsys.cpl"},
    {"power",          "电源和睡眠",  "ms-settings:powersleep"},
    {"windows-update", "Windows 更新", "ms-settings:windowsupdate"},
};

/// ms-settings: 手填的白名单前缀(docs/24 §3.2:不开放任意 URI,
/// protocol handler 是注册表面)。
bool isAllowedSystemTarget(const QString& target) {
    if (target.startsWith(QLatin1String("ms-settings:")))
        return true;
    for (const auto& e : kSystemEntries)
        if (target == QLatin1String(e.key)) return true;
    return false;
}

/// app 类型允许的目标形态:可执行扩展名或已存在目录(目录 = 资源管理器打开)。
bool isValidAppTarget(const QString& target) {
    static const QRegularExpression ext(
        QStringLiteral("\\.(exe|lnk|bat|cmd)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString t = target.trimmed();
    if (ext.match(t).hasMatch()) return true;
    return QFileInfo::exists(t) && QFileInfo(t).isDir();
}

} // namespace

QuickCommands::QuickCommands(PanelWindow& panel, CoreBridge& core, QObject* parent)
    : QObject(parent), m_panel(panel), m_core(core) {
    registerActions();
}

void QuickCommands::registerActions() {
    // ---- 首批 6 动作(docs/24 §4.3):全部复用现有链路,零新逻辑 ----
    // 新增动作 = 这里加一条 ActionEntry;设置页挑选列表自动跟随(catalog)。
    m_actions.insert(QStringLiteral("capture.region"),
                     {QStringLiteral("智能截屏"), QStringLiteral("capture"), true,
                      [this] {
                          // 与热键 Ctrl+Shift+A 同源:收面板 → 等窗口真消失 → 框选
                          m_panel.hide();
                          QTimer::singleShot(150, [] {
                              startCaptureSession(nb::CaptureIntent::Region);
                          });
                          return true;
                      }});
    m_actions.insert(QStringLiteral("pin.latest"),
                     {QStringLiteral("贴图(最近内容)"), QStringLiteral("pin"), true,
                      [] {
                          CoreFacade::instance().pinLatest();
                          return true;
                      }});
    m_actions.insert(QStringLiteral("monitor.toggle"),
                     {QStringLiteral("暂停/恢复监听"), QStringLiteral("pause"), true,
                      [this] {
                          // 与托盘「暂停监听」同一条 get→改→save 链路(tray.cpp 同源)
                          const QVariantMap resp = CoreFacade::instance().call("get_settings");
                          if (!resp.value("ok").toBool()) return false;
                          QVariantMap settings = resp.value("data").toMap();
                          const QVariant mon = settings.value("monitorEnabled");
                          const bool now = mon.isValid() ? mon.toBool() : true;
                          settings.insert("monitorEnabled", !now);
                          CoreFacade::instance().call("save_settings", settings);
                          CoreFacade::instance().showToast(
                              now ? QStringLiteral("已暂停剪贴板监听")
                                  : QStringLiteral("已恢复剪贴板监听"));
                          emit quickCatalogChanged();
                          return true;
                      }});
    m_actions.insert(QStringLiteral("theme.toggle"),
                     {QStringLiteral("深浅色切换"), QStringLiteral("theme"), true,
                      [this] {
                          // 取反当前明暗:system 模式下按解析结果落到显式 light/dark
                          const bool dark = CoreFacade::instance().darkTheme();
                          const QString next = dark ? QStringLiteral("light")
                                                    : QStringLiteral("dark");
                          const QVariantMap resp = CoreFacade::instance().call("get_settings");
                          if (!resp.value("ok").toBool()) return false;
                          QVariantMap settings = resp.value("data").toMap();
                          settings.insert("theme", next);
                          CoreFacade::instance().call("save_settings", settings);
                          CoreFacade::instance().applyTheme(next);
                          return true;
                      }});
    m_actions.insert(QStringLiteral("todo.new"),
                     {QStringLiteral("新建待办"), QStringLiteral("todo"), true,
                      [this] {
                          m_panel.showAtCursor();
                          m_panel.openTodoComposerForAcceptance();
                          return true;
                      }});
    m_actions.insert(QStringLiteral("settings.open"),
                     {QStringLiteral("打开设置"), QStringLiteral("settings"), true,
                      [this] {
                          m_panel.showSettings();
                          return true;
                      }});
}

QVariantList QuickCommands::quickActionCatalog() const {
    QVariantList out;
    // 固定输出序:按 id 排序,目录展示稳定(不随 QHash 遍历序漂移)
    QList<QString> ids = m_actions.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString& id : ids) {
        const ActionEntry& e = m_actions.value(id);
        out.append(QVariantMap{
            {"id", id},
            {"label", e.label},
            {"iconKind", e.iconKind},
            {"available", e.available},
        });
    }
    return out;
}

QVariantList QuickCommands::quickSystemCatalog() const {
    QVariantList out;
    for (const auto& e : kSystemEntries)
        out.append(QVariantMap{
            {"key", QLatin1String(e.key)},
            // label 是中文字面量:必须经 fromUtf8 转换 —— QLatin1String 会把
            // UTF-8 字节逐个当 Latin1 字符,中文全成乱码(实测踩过)。
            {"label", QString::fromUtf8(e.label)},
        });
    return out;
}

QString QuickCommands::quickSystemResolve(const QString& key) const {
    for (const auto& e : kSystemEntries)
        if (key == QLatin1String(e.key)) return QLatin1String(e.exec);
    return QString();
}

bool QuickCommands::shellOpen(const QString& target, const QString& args,
                              const QString& workdir) {
    const QString t = QDir::toNativeSeparators(target.trimmed());
    const QString a = args.trimmed();
    // 工作目录空 → target 所在目录;目录目标本身 → target(打开的就是它)
    QString wd = workdir.trimmed();
    if (wd.isEmpty()) {
        const QFileInfo fi(t);
        wd = fi.isDir() ? t : fi.absolutePath();
    }
    const std::wstring wTarget = t.toStdWString();
    const std::wstring wArgs = a.toStdWString();
    const std::wstring wDir = QDir::toNativeSeparators(wd).toStdWString();
    HINSTANCE inst = ::ShellExecuteW(nullptr, L"open", wTarget.c_str(),
                                     wArgs.c_str(), wDir.c_str(), SW_SHOWNORMAL);
    // SehellExecute 返回值 > 32 = 成功(Win32 约定);31 = 无关联等失败
    return reinterpret_cast<qintptr>(inst) > 32;
}

bool QuickCommands::runApp(const QVariantMap& cmd) {
    const QString target = cmd.value("target").toString();
    const QString label = cmd.value("label").toString();
    if (!isValidAppTarget(target)) {
        CoreFacade::instance().showToast(
            QStringLiteral("无法启动:%1(路径无效)").arg(label));
        return false;
    }
    if (!shellOpen(target, cmd.value("args").toString(),
                   cmd.value("workdir").toString())) {
        CoreFacade::instance().showToast(
            QStringLiteral("无法启动:%1").arg(label));
        return false;
    }
    return true;
}

bool QuickCommands::runSystem(const QVariantMap& cmd) {
    const QString target = cmd.value("target").toString().trimmed();
    const QString label = cmd.value("label").toString();
    // 语义校验在执行前再过一道(core 只查语法;配置可能来自导入/手改)
    if (!isAllowedSystemTarget(target)) {
        CoreFacade::instance().showToast(
            QStringLiteral("不支持的系统目标:%1").arg(target));
        return false;
    }
    // 目录键 → 执行串;ms-settings: 前缀原样
    QString exec = quickSystemResolve(target);
    if (exec.isEmpty()) exec = target;
    // msc/cpl/exe 走 ShellExecute(系统关联);URI 同样吃得到
    const int sp = exec.indexOf(QLatin1Char(' '));
    const QString exe = sp < 0 ? exec : exec.left(sp);
    const QString args = sp < 0 ? QString() : exec.mid(sp + 1);
    if (!shellOpen(exe, args, QString())) {
        CoreFacade::instance().showToast(
            QStringLiteral("无法打开:%1").arg(label));
        return false;
    }
    return true;
}

bool QuickCommands::runInternal(const QVariantMap& cmd) {
    return runQuickAction(cmd.value("target").toString());
}

bool QuickCommands::runQuickAction(const QString& actionId) {
    const auto it = m_actions.constFind(actionId);
    if (it == m_actions.constEnd()) {
        // 配置可能来自更高版本(新动作)或已废弃:明示而非静默
        CoreFacade::instance().showToast(
            QStringLiteral("未知动作:%1").arg(actionId));
        return false;
    }
    if (!it->available) {
        CoreFacade::instance().showToast(
            QStringLiteral("动作当前不可用:%1").arg(it->label));
        return false;
    }
    return it->run();
}

void QuickCommands::runCommand(const QVariantMap& cmd) {
    const QString type = cmd.value("type").toString();
    if (type == QLatin1String("app")) runApp(cmd);
    else if (type == QLatin1String("system")) runSystem(cmd);
    else if (type == QLatin1String("internal")) runInternal(cmd);
    // 未知 type:静默忽略(docs/24 §7.2 前向兼容契约 —— 高版本配置在
    // 低版本宿主上不炸、不报错,数据保留)
}

} // namespace nb
