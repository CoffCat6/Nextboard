#include "auto_start.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <QVarLengthArray>
#endif

namespace nb::auto_start {

/// 开机自启必须静默:带 `--hidden` 启动,主面板不弹出、不抢焦点(docs/04 §8 D11)。
/// Windows 与安装器写 Run 键的形态保持一致(tools/installer.nsi 的 SecAutoStart),
/// Linux 与 XDG autostart 的 Exec 形态保持一致 —— 否则"安装器开的自启不弹面板、
/// 应用内开的自启弹面板"——同一功能两种行为。
constexpr char kHiddenArg[] = " --hidden";

#if defined(Q_OS_WIN)

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Nextboard";

QString commandLine() {
    // 引号包裹:路径含空格时 Run 键按整条命令解析,不包裹会被拆成 exe + 参数。
    // `--hidden` 必须在引号**外**,否则会被当成路径的一部分。
    return QStringLiteral("\"%1\"%2")
        .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()),
             QString::fromLatin1(kHiddenArg));
}

/// 读回 Run 键里当前登记的命令行(不存在 / 读失败 → 空串)。
QString registeredCommandLine() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return QString();
    DWORD type = 0;
    DWORD bytes = 0;
    QString value;
    if (::RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS
        && type == REG_SZ && bytes > 0) {
        // REG_SZ 的 bytes 含结尾 NUL;按 wchar_t 步长读取后去掉尾部 NUL。
        QVarLengthArray<wchar_t, 512> buffer(static_cast<int>(bytes / sizeof(wchar_t)) + 1, L'\0');
        if (::RegQueryValueExW(key, kValueName, nullptr, &type,
                               reinterpret_cast<LPBYTE>(buffer.data()), &bytes) == ERROR_SUCCESS) {
            value = QString::fromWCharArray(buffer.constData());
        }
    }
    ::RegCloseKey(key);
    return value;
}

} // namespace

bool enabled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    const LSTATUS rc = ::RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, nullptr);
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool setEnabled(bool enable) {
    HKEY key = nullptr;
    // ⚠️ 启用与停用都要 KEY_SET_VALUE:RegDeleteValue **同样**要求键句柄带 KEY_SET_VALUE
    //    权限(它删的是"值",不是"子键")。早先停用时传 DELETE —— 那是 RegDeleteKey 删
    //    整个子键的权限 —— 于是 RegDeleteValue 返回 ERROR_ACCESS_DENIED(5),功能表现为
    //    "勾上开机自启后就再也取消不掉"(设置页回读注册表后把勾选态弹回去)。
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        qWarning() << "auto_start: 打开 Run 注册表键失败(需 KEY_SET_VALUE), error ="
                   << ::GetLastError();
        return false;
    }
    const QString command = commandLine();
    const LSTATUS rc = enable
        ? ::RegSetValueExW(key, kValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(command.utf16()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
        : ::RegDeleteValueW(key, kValueName);
    ::RegCloseKey(key);
    // 删除时值不存在也算成功(幂等);其余失败打日志并返回 false
    if (rc != ERROR_SUCCESS && !(rc == ERROR_FILE_NOT_FOUND && !enable)) {
        qWarning() << "auto_start: 写/删注册表值失败, rc =" << rc;
        return false;
    }
    // 回读复核:注册表被策略/安全软件拦截时,写入可能"成功"但实际未生效
    if (enabled() != enable) {
        qWarning() << "auto_start: 操作后回读不一致(期望" << enable << ",实际" << enabled()
                   << ")—— 注册表可能被安全软件或组策略拦截";
        return false;
    }
    return true;
}

bool refreshCommandLineIfStale() {
    if (!enabled()) return false;   // 没开自启:无事可做
    const QString current = registeredCommandLine();
    if (current == commandLine()) return false;   // 已是最新形态

    // ⚠️ 只升级**指向本 exe** 的自启项。
    //    典型场景:用户把正式版装在 %LOCALAPPDATA%\Programs\Nextboard,
    //    而开发时从 app\build 跑同一份代码 —— 若不设守卫,跑一次 dev 构建就会把
    //    用户的自启改指向 build 目录,重装/清理构建后自启静默失效。
    //    指向别处的值留给那个安装自身去修(它运行时会走到同一个函数)。
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (!current.contains(exePath, Qt::CaseInsensitive)) {
        qInfo() << "auto_start: Run 键指向其它安装,跳过命令行升级:" << current;
        return false;
    }

    // 老版本(0.2.0 及更早)的应用内开关写的是不带 --hidden 的命令行 → 开机弹面板。
    // 这里就地改写,用户无需去设置页关一次再开一次。
    if (!setEnabled(true)) {
        qWarning() << "auto_start: 自启命令行升级失败(仍为旧值,开机可能弹出面板)";
        return false;
    }
    qInfo() << "auto_start: 已把 Run 键升级为带 --hidden 的形态(修开机弹面板)";
    return true;
}

QString debugRegisteredCommandLine() {
    return registeredCommandLine();
}

QString debugExpectedCommandLine() {
    return commandLine();
}

bool debugWriteRawCommandLine(const QString& command) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        qWarning() << "auto_start: 伪造命令行时打开 Run 键失败, error =" << ::GetLastError();
        return false;
    }
    const LSTATUS rc = ::RegSetValueExW(
        key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.utf16()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

#else  // ---- Linux(M0b):XDG autostart(~/.config/autostart/nextboard.desktop)----

namespace {

/// 自启入口文件;enabled() = 文件存在且未被 Hidden 禁用。
QString desktopFilePath() {
    return QDir::home().filePath(QStringLiteral(".config/autostart/nextboard.desktop"));
}

QString commandLine() {
    // Exec 形态与 Windows 同构:路径引号包裹,--hidden 在引号外;
    // Linux 路径分隔符本就是 '/',toNativeSeparators 为恒等变换,这里显式不调用。
    return QStringLiteral("\"%1\"%2")
        .arg(QCoreApplication::applicationFilePath(), QString::fromLatin1(kHiddenArg));
}

/// 写入标准 Desktop Entry(Enabled/Disabled 共用一个模板,Hidden 字段区分)。
bool writeDesktopEntry(const QString& execCommand) {
    const QString path = desktopFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "auto_start: 写 autostart .desktop 失败:" << path
                   << file.errorString();
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=Nextboard\n"
                          "Comment=Clipboard history & screenshot board\n"
                          "Exec=%1\n"
                          "Hidden=false\n"
                          "X-GNOME-Autostart-enabled=true\n")
               .arg(execCommand);
    out.flush();
    return file.error() == QFileDevice::NoError;
}

/// 读回当前登记的 Exec(文件不存在 / 无 Exec → 空串)。
QString registeredCommandLine() {
    QFile file(desktopFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (line.startsWith("Exec=")) {
            return QString::fromUtf8(line.mid(5)).trimmed();
        }
    }
    return QString();
}

} // namespace

bool enabled() {
    QFile file(desktopFilePath());
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        // Hidden=true 是 XDG 的"用户已停用"语义;其它程序可能改写该字段。
        if (line == "Hidden=true") return false;
    }
    return true;
}

bool setEnabled(bool enable) {
    if (!enable) {
        // 删除时文件不存在也算成功(幂等),与 Windows 分支的 ERROR_FILE_NOT_FOUND 口径一致
        const QString path = desktopFilePath();
        if (QFile::exists(path) && !QFile::remove(path)) {
            qWarning() << "auto_start: 删除 autostart .desktop 失败:" << path;
            return false;
        }
    } else if (!writeDesktopEntry(commandLine())) {
        return false;
    }
    // 回读复核:与 Windows 分支同哲学 —— 写"成功"不等于生效(只读文件系统等)
    if (enabled() != enable) {
        qWarning() << "auto_start: 操作后回读不一致(期望" << enable << ",实际" << enabled()
                   << ")—— autostart 文件可能被只读挂载或权限拦截";
        return false;
    }
    return true;
}

bool refreshCommandLineIfStale() {
    if (!enabled()) return false;
    const QString current = registeredCommandLine();
    if (current == commandLine()) return false;   // 已是最新形态

    // ⚠️ 只升级**指向本 exe** 的自启项(与 Windows 分支同一守卫,理由同):
    //    dev 构建跑一次不能把用户安装的自启改指向构建目录。
    const QString exePath = QCoreApplication::applicationFilePath();
    if (!current.contains(exePath, Qt::CaseInsensitive)) {
        qInfo() << "auto_start: autostart 指向其它安装,跳过命令行升级:" << current;
        return false;
    }

    if (!writeDesktopEntry(commandLine())) {
        qWarning() << "auto_start: 自启命令行升级失败(仍为旧值)";
        return false;
    }
    qInfo() << "auto_start: 已把 autostart Exec 升级为当前形态";
    return true;
}

QString debugRegisteredCommandLine() {
    return registeredCommandLine();
}

QString debugExpectedCommandLine() {
    return commandLine();
}

bool debugWriteRawCommandLine(const QString& command) {
    // 验收辅助:把给定命令行写成当前自启项(Linux 侧等价于"直接改 Exec")
    return writeDesktopEntry(command);
}

#endif  // Q_OS_WIN / Linux

} // namespace nb::auto_start
