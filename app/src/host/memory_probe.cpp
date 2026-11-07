#include "memory_probe.hpp"

#include <QDebug>
#include <QFile>
#include <QTextStream>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

namespace nb {
namespace {

QString toMb(unsigned long long bytes) {
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1);
}

#if !defined(Q_OS_WIN)
/// 从 /proc 文本里取 "Key:\t <n> kB" 的 n(单位 kB),缺失返回 0。
unsigned long long procKb(const QByteArray& text, const char* key) {
    const QByteArray needle = QByteArray(key) + ':';
    const int at = text.indexOf(needle);
    if (at < 0) return 0;
    const int lineEnd = text.indexOf('\n', at);
    const int len = (lineEnd < 0 ? text.size() : lineEnd) - at;
    const QByteArray line = text.mid(at, len);
    // 形如 "VmRSS:\t  123456 kB" —— 取第二列数字
    const QList<QByteArray> parts = line.simplified().split(' ');
    if (parts.size() < 2) return 0;
    return parts.at(1).toULongLong() * 1024ull;
}
#endif

} // namespace

MemorySnapshot memorySnapshot() {
    MemorySnapshot snapshot;
#if defined(Q_OS_WIN)
    // EX 版才带 PrivateUsage;结构体首部与 PROCESS_MEMORY_COUNTERS 布局兼容,
    // 故可把指针转成基类型传入(cb 仍填 EX 的大小)。
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters))) {
        snapshot.workingSet = counters.WorkingSetSize;
        snapshot.privateBytes = counters.PrivateUsage;
        snapshot.peakWorkingSet = counters.PeakWorkingSetSize;
    }
#else
    // Linux(M0b):VmRSS/VmHWM(/proc/self/status)与 WorkingSet/Peak 同义;
    // 私有内存取 /proc/self/smaps_rollup 的 Private_Clean+Private_Dirty
    // (口径与 Win32 PrivateUsage 最接近;读不到时保持 0,日志侧自动省略)。
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray text = status.readAll();
        snapshot.workingSet = procKb(text, "VmRSS");
        snapshot.peakWorkingSet = procKb(text, "VmHWM");
    }
    QFile rollup(QStringLiteral("/proc/self/smaps_rollup"));
    if (rollup.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray text = rollup.readAll();
        snapshot.privateBytes =
            procKb(text, "Private_Clean") + procKb(text, "Private_Dirty");
    }
#endif
    return snapshot;
}

QString memorySummary() {
    const MemorySnapshot snapshot = memorySnapshot();
    return QStringLiteral("工作集 %1 MB · 私有 %2 MB")
        .arg(toMb(snapshot.workingSet), toMb(snapshot.privateBytes));
}

void logMemory(const char* tag) {
    const MemorySnapshot snapshot = memorySnapshot();
    qInfo().noquote() << QStringLiteral("mem[%1] 工作集 %2 MB · 私有 %3 MB · 峰值 %4 MB")
                             .arg(QString::fromUtf8(tag), toMb(snapshot.workingSet),
                                  toMb(snapshot.privateBytes),
                                  toMb(snapshot.peakWorkingSet));
}

} // namespace nb
