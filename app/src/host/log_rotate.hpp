// 日志轮转:单代滚动(`x.log` → `x.log.1`),防止长期运行把磁盘写满。
//
// 为什么需要:宿主日志是**全量落文件**且只 append(main.cpp 的 fileMessageHandler),
// 托盘常驻程序一开就是几周,Info 级日志(剪贴板事件/待办 tick/内存读数)源源不断,
// 没有任何上限。用户磁盘被一个排障文件吃掉是最不该发生的事故。
//
// 设计取舍(与"按日期分文件/保留 N 代"对比):
//  - **单代**只保留 `x.log` 与 `x.log.1`,总量恒定 2×阈值,不随运行时长增长;
//    排障需要的是"最近发生了什么",一代足够,多了反而要找哪一份。
//  - **阈值判断走已写字节数**(写入后累计),不做每条的 stat:stat 每条一次系统调用,
//    而日志最热的路径是剪贴板事件。代价是单文件可超过阈值约一条日志的长度。
//  - 滚动发生在**跨过阈值的那次写入之后**:先把这条写进去(保住它),
//    再整体换文件 —— 崩溃前的最后一条日志不会因为滚动而丢。
#pragma once

#include <QString>

class QFile;

namespace nb {

/// 单份日志的大小上限(字节)。两代合计约为它的两倍。
constexpr qint64 kLogRotateBytes = 2 * 1024 * 1024;   // 2MB

/// 若 `path` 已达/超过阈值则滚动:`path` → `path.1`(覆盖旧 .1)。
/// 返回是否滚动成功;未达阈值/文件不存在/改名失败都返回 false。
/// 滚动失败**不影响写日志**:下次再试,不让排障工具本身变成故障源。
bool rotateLogIfNeeded(const QString& path, qint64 writtenBytes,
                       qint64 limitBytes = kLogRotateBytes);

/// 打开日志文件准备追加;若已存在且超过阈值,**先**滚动再打开。
///
/// 为什么打开前也要判一次:程序可能在上次退出前刚好越过阈值(或上次是强杀,
/// 没走到滚动分支),下次启动若直接 append 就会继续往一个已经超限的文件里写,
/// 直到本次运行再次越线 —— 表现为"日志长期大于阈值"。
/// 打开失败不抛异常,返回 false(调用方自行降级)。
bool openLogFileAppend(QFile& file, const QString& path,
                       qint64 limitBytes = kLogRotateBytes);

} // namespace nb
