#include "app_paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace nb {

namespace {
/// 数据目录覆盖值(命令行 --data-dir)。空串 = 用默认。
QString g_dataDirOverride;

/// Linux(M4):数据目录解析 —— exe 同级 data/(便携布局:开发构建、绿色部署)
/// **可写则优先**;不可写(/opt 与 /usr 系统目录安装、AppImage 只读挂载)时
/// 回落 $XDG_DATA_HOME/Nextboard(默认 ~/.local/share/Nextboard)。
///
/// ⚠️ 两个入口(appDataDir/appDataDirEarly)必须解析到**同一目录**:单实例
/// 门禁键 = 数据目录(appDataDirEarly 在 QApplication 之前取),若两处结果
/// 不同,同一安装会算出两个门禁键,"同目录双实例"随之而来。故共用本函数,
/// 并各自用函数级 static 缓存(解析含 mkpath/写探针,不能每调用一次)。
/// 判据用**写探针**而不是路径猜测:/opt 可能有用户可写目录的非常规布局,
/// AppImage 挂载点是只读 squashfs(mkpath 直接失败)。
QString resolveLinuxDataDir(const QString& exeDir) {
    const QString portable = QDir(exeDir).filePath(QStringLiteral("data"));
    if (QDir().mkpath(portable)) {
        QFile probe(QDir(portable).filePath(QStringLiteral(".write-probe")));
        if (probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            probe.remove();
            return portable;
        }
    }
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    const QString base = dataHome.isEmpty()
                             ? QDir::home().filePath(QStringLiteral(".local/share"))
                             : dataHome;
    const QString xdg = QDir(base).filePath(QStringLiteral("Nextboard"));
    // ⚠️ 回落目录**必须在这里建**:单实例门禁(appDataDirEarly)在主流程的
    // ensureAppDataDir 之前就要往里写 QLockFile —— 目录不存在时门禁误判
    // "已有实例"直接退出(2026-11-08 AppImage 冒烟实测踩中)。
    QDir().mkpath(xdg);
    return xdg;
}
} // namespace

void setAppDataDirOverride(const QString& dir) {
    g_dataDirOverride = dir;
}

QString appDataDir() {
    if (!g_dataDirOverride.isEmpty()) {
        return QDir::cleanPath(g_dataDirOverride);
    }
#if defined(Q_OS_WIN)
    // 便携式布局:数据跟着 exe 走,便于整目录拷贝迁移/绿色部署
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
#else
    static const QString resolved =
        resolveLinuxDataDir(QCoreApplication::applicationDirPath());
    return resolved;
#endif
}

QString appDataDirEarly() {
    if (!g_dataDirOverride.isEmpty()) {
        return QDir::cleanPath(g_dataDirOverride);
    }
    // ⚠️ 不能用 QCoreApplication::applicationDirPath():QApplication 尚未构造时
    //    它返回空串(见函数注释)。
#if defined(Q_OS_WIN)
    // Windows:直接问 Win32 要 exe 路径。
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return QString();
    const QString exePath = QString::fromWCharArray(buf, static_cast<int>(n));
    return QDir(QDir(exePath).absolutePath()).filePath(QStringLiteral("data"));
#else
    // Linux(M0b):/proc/self/exe 与 GetModuleFileNameW 同语义(内核解析的
    // 真实可执行文件,不受 PATH/工作目录影响)。
    const QString exePath = QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
    if (exePath.isEmpty()) return QString();
    static const QString resolved = resolveLinuxDataDir(QFileInfo(exePath).absolutePath());
    return resolved;
#endif
}

bool ensureAppDataDir() {
    const QString dir = appDataDir();
    return QDir().mkpath(dir);
}

QString appDataFile(const QString& name) {
    return QDir(appDataDir()).filePath(name);
}

} // namespace nb
