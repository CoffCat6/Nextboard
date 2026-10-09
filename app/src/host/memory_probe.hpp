// 进程内存探测:内存收口(S6.1)与内存验收(S6.5)的可观测手段。
//
// 为什么需要它:贴图/遮罩/编辑器各自持有大位图,任何"优化"若不落到实测数字上
// 都只是猜测。这里给出统一口径的读数(与任务管理器"内存"列一致的工作集),
// 关键事件处各写一条日志,就能把内存变化挂到具体动作上做前后对比。
#pragma once

#include <QString>

namespace nb {

/// 进程内存快照(单位:字节)。
struct MemorySnapshot {
    unsigned long long workingSet = 0;   ///< 工作集:物理内存常驻(任务管理器「内存」列口径)
    unsigned long long privateBytes = 0; ///< 私有提交:本进程独占的虚拟内存
    unsigned long long peakWorkingSet = 0;
};

/// 读取当前进程内存;失败时各字段为 0。
MemorySnapshot memorySnapshot();

/// `工作集 128.4 MB · 私有 96.2 MB` —— 日志与托盘显示共用同一格式。
QString memorySummary();

/// 按 tag 写一条内存日志,用于把内存变化挂到具体事件上(启动/显隐/会话始末/贴图增删)。
void logMemory(const char* tag);

} // namespace nb
