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
} // namespace

void setAppDataDirOverride(const QString& dir) {
    g_dataDirOverride = dir;
}

QString appDataDir() {
    if (!g_dataDirOverride.isEmpty()) {
        return QDir::cleanPath(g_dataDirOverride);
    }
    // 便携式布局:数据跟着 exe 走,便于整目录拷贝迁移/绿色部署
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
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
    return QDir(QFileInfo(exePath).absolutePath()).filePath(QStringLiteral("data"));
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
