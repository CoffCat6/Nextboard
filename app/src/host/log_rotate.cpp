#include "log_rotate.hpp"

#include <QFile>
#include <QFileInfo>

namespace nb {

namespace {
/// 滚动后的归档文件名(`x.log` → `x.log.1`)。
QString rotatedPath(const QString& path) {
    return path + QStringLiteral(".1");
}

/// 把 `path` 改名为归档位(覆盖旧归档)。
///
/// 先删旧归档再改名:Windows 上 rename 到已存在的目标会失败,而旧归档删除失败
/// (被别的进程 / 杀软占着)也不该让调用方崩 —— 此时改名失败,本次放弃滚动。
/// 日志写不下去比丢一份旧归档更糟,故这里全部吞掉返回值,由调用方决定后续。
bool rotateFile(const QString& path) {
    const QString target = rotatedPath(path);
    if (QFile::exists(target)) QFile::remove(target);
    return QFile::rename(path, target);
}
} // namespace

bool rotateLogIfNeeded(const QString& path, qint64 writtenBytes, qint64 limitBytes) {
    if (writtenBytes < limitBytes) return false;
    if (!QFileInfo::exists(path)) return false;
    return rotateFile(path);
}

bool openLogFileAppend(QFile& file, const QString& path, qint64 limitBytes) {
    // 打开前先判一次:上次运行可能是强杀(没走到滚动分支),文件已超限。
    const QFileInfo info(path);
    if (info.exists() && info.size() >= limitBytes) rotateFile(path);
    file.setFileName(path);
    // ⚠️ 必须 Unbuffered(2026-11-08):Qt 6.9 的 QIODevice 未加该标志时自带
    // 用户态写缓冲,write() 不产生 write(2) 系统调用 —— Info 级日志要等 Warn+
    // 的 flush() 或正常退出析构才落 OS。Ubuntu VM 实测:干净启动零 WARN + 强杀
    // 退出 = nextboard.log 0 字节(strace:openat 与时间戳格式化俱在、write 为零;
    // Rust 侧同款 8KB BufWriter 病因,docs/29 §9.1 K9)。"Info 交给 OS 页缓存"
    // 的设计意图只有 Unbuffered 才真正成立。
    return file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text
                     | QIODevice::Unbuffered);
}

} // namespace nb
