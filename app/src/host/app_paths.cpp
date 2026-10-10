#include "app_paths.hpp"

#include <QCoreApplication>
#include <QDir>

namespace nb {

QString appDataDir() {
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
