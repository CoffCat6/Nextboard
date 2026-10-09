#include "memory_probe.hpp"

#include <QDebug>

#include <windows.h>
#include <psapi.h>

namespace nb {
namespace {

QString toMb(unsigned long long bytes) {
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1);
}

} // namespace

MemorySnapshot memorySnapshot() {
    MemorySnapshot snapshot;
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
