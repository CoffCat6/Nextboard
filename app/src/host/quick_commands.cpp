#include "quick_commands.hpp"

#include "app_paths.hpp"
#include "capture/capture_session.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QHash>
#include <QTimer>
#include <QRegularExpression>
#include <QVariantMap>
#include <QVector>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <objbase.h>

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
    {"bluetooth",      "蓝牙设备",    "ms-settings:bluetooth"},
    {"apps-features",  "应用与功能",  "ms-settings:appsfeatures"},
    {"storage",        "存储",        "ms-settings:storagesense"},
    {"mouse",          "鼠标设置",    "main.cpl"},
    {"datetime",       "日期和时间",  "timedate.cpl"},
    {"credman",        "凭据管理器",  "control.exe keymgr.dll,KRShowKeyMgr"},
    {"cleanmgr",       "磁盘清理",    "cleanmgr.exe"},
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

/// 清理用户粘贴的目标:Windows "复制文件地址" 自带**首尾引号**;另有零宽
/// 字符与首尾空白。不去引号的话,粘贴路径永远过不了扩展名检查(实测反馈:
/// 删除后重新添加同一 exe 报"路径无效")。
QString cleanTarget(const QString& target) {
    QString t = target.trimmed();
    while (t.startsWith('"') && t.endsWith('"') && t.length() >= 2)
        t = t.mid(1, t.length() - 2).trimmed();
    // 去零宽与 BOM 类字符
    static const QRegularExpression invisible(
        QStringLiteral("[\u200B\u200C\u200D\uFEFF]"));
    t.remove(invisible);
    return t;
}

/// app 类型允许的目标形态:可执行扩展名或已存在目录(目录 = 资源管理器打开)。
bool isValidAppTarget(const QString& target) {
    static const QRegularExpression ext(
        QStringLiteral("\\.(exe|lnk|bat|cmd)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString t = cleanTarget(target);
    if (ext.match(t).hasMatch()) return true;
    return QFileInfo::exists(t) && QFileInfo(t).isDir();
}

/// HICON → QImage(Qt6 无 QtWinExtras;ICONINFO + GetDIBits 手动转换)。
/// alpha 预乘:PNG 保存需要非预乘;GetDIBits 的 32bpp 顶层位图在带 alpha 的
/// 现代图标上已是预乘 ARGB —— 逐像素除回 alpha(未带 alpha 的旧图标 mask 全 1,
/// 除法退化为原值,安全)。
QImage hIconToImage(HICON icon) {
    ICONINFO info = {};
    if (!::GetIconInfo(icon, &info)) return QImage();
    BITMAP bmp = {};
    if (!::GetObjectW(info.hbmColor, sizeof(bmp), &bmp)) {
        ::DeleteObject(info.hbmColor); ::DeleteObject(info.hbmMask);
        return QImage();
    }
    const int w = bmp.bmWidth, h = bmp.bmHeight;
    QImage color(w, h, QImage::Format_ARGB32_Premultiplied);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = ::GetDC(nullptr);
    const int got = ::GetDIBits(dc, info.hbmColor, 0, h,
                                color.bits(), &bmi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    QImage out;
    if (got == h) {
        // mask 检查:全 0 透明区?简化:若 color 有非零 alpha 视为带 alpha;
        // 否则用 mask 合成(老图标)。
        bool hasAlpha = false;
        for (int y = 0; y < h && !hasAlpha; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(color.constScanLine(y));
            for (int x = 0; x < w; ++x)
                if (qAlpha(row[x]) != 0) { hasAlpha = true; break; }
        }
        out = color.convertToFormat(QImage::Format_ARGB32);
        if (!hasAlpha) {
            // 用 mask 填 alpha:mask 1bpp,0 = 不透明
            QImage mask(w, h, QImage::Format_Mono);
            BITMAPINFO mbmi = bmi;
            mbmi.bmiHeader.biBitCount = 1;
            HDC dc2 = ::GetDC(nullptr);
            ::GetDIBits(dc2, info.hbmMask, 0, h, mask.bits(), &mbmi, DIB_RGB_COLORS);
            ::ReleaseDC(nullptr, dc2);
            for (int y = 0; y < h; ++y) {
                QRgb* row = reinterpret_cast<QRgb*>(out.scanLine(y));
                const uchar* mrow = mask.constScanLine(y);
                for (int x = 0; x < w; ++x) {
                    const bool transparent = (mrow[x / 8] >> (7 - (x % 8))) & 1;
                    if (transparent) row[x] = 0;
                }
            }
        }
    }
    ::DeleteObject(info.hbmColor);
    ::DeleteObject(info.hbmMask);
    return out;
}

/// .lnk 解引用:IShellLinkW 读目标路径(docs/24 Q2 收尾)。
/// 返回目标绝对路径(lnk 指向文件夹时为文件夹路径);失败返回空串。
/// COM 初始化:Qt 主线程通常已初始化(托盘/对话框);此处用 Ensure 级别兜底。
QString resolveLnkTarget(const QString& lnkPath) {
    // COM 就绪兜底:Qt 不自动初始化;已初始化时返回 S_FALSE/RPC_E_CHANGED_MODE,
    // 均不重复 Uninitialize(引用计数模型,此处自增无副作用)。
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const QString native = QDir::toNativeSeparators(lnkPath);
    IShellLinkW* link = nullptr;
    const HRESULT hrCreate = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_IShellLinkW,
                                                reinterpret_cast<void**>(&link));
    if (FAILED(hrCreate) || !link) return QString();
    QString out;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                       reinterpret_cast<void**>(&file))) && file) {
        if (SUCCEEDED(file->Load(reinterpret_cast<const wchar_t*>(native.constData()),
                                 STGM_READ))) {
            wchar_t path[MAX_PATH] = {};
            if (SUCCEEDED(link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH))
                    && path[0] != L'\0') {
                out = QString::fromWCharArray(path);
            }
        }
        file->Release();
    }
    link->Release();
    return out;
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

bool QuickCommands::appTargetValid(const QString& target) const {
    return isValidAppTarget(target);
}

QString QuickCommands::appIconUrl(const QString& target) {
    // .lnk 先解引用到目标(IShellLink),之后与 .exe 同一条提取链;
    // 解析失败(目标已删/非文件系统目标)回退默认图标。
    QString t = QDir::toNativeSeparators(target.trimmed());
    if (t.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)) {
        if (!QFileInfo::exists(t)) return QString();
        const QString resolved = resolveLnkTarget(t);
        if (resolved.isEmpty()) return QString();
        t = QDir::toNativeSeparators(resolved);
    }
    if (!t.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
        return QString();
    if (!QFileInfo::exists(t)) return QString();
    // 缓存键 = 图标源路径 md5(lnk 场景是解析后的目标路径,目标不变则复用)
    const QString cacheName = QStringLiteral("quick-icons/%1.png")
                                  .arg(QString::fromLatin1(
                                      QCryptographicHash::hash(
                                          t.toUtf8(), QCryptographicHash::Md5).toHex()));
    const QString cachePath = nb::appDataFile(cacheName);
    if (QFileInfo::exists(cachePath))
        return QUrl::fromLocalFile(cachePath).toString();

    SHFILEINFOW sfi = {};
    // SHGFI_LARGEICON = 32x32 系统档;QML Image smooth 缩到 20px
    if (!::SHGetFileInfoW(reinterpret_cast<const wchar_t*>(t.constData()), 0,
                          &sfi, sizeof(sfi),
                          SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon)
        return QString();
    const QImage img = hIconToImage(sfi.hIcon);
    ::DestroyIcon(sfi.hIcon);
    if (img.isNull()) return QString();
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    if (!img.save(cachePath, "PNG")) return QString();
    return QUrl::fromLocalFile(cachePath).toString();
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
    // 执行时同样剥引号:校验与执行必须同一清洗口径,否则"校验过却启动失败"
    const QString target = cleanTarget(cmd.value("target").toString());
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
