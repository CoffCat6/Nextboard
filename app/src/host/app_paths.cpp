#include "app_paths.hpp"

#include <QCoreApplication>
#include <QDir>

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

bool ensureAppDataDir() {
    const QString dir = appDataDir();
    return QDir().mkpath(dir);
}

QString appDataFile(const QString& name) {
    return QDir(appDataDir()).filePath(name);
}

} // namespace nb
