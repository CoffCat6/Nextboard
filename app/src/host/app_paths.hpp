// 应用路径:数据目录的**唯一来源**。
//
// 布局(便携式):`<nextboard.exe 同级>\data\` 下集中存放
//   clipboard.db(+wal/shm) · blobs\<YYYY-MM>\*.png · app_config.json · nextboard.log · nbcore.log
//
// 为什么抽成独立模块:C++ 侧有多处需要它(nbcore 初始化、应用日志、崩溃兜底日志、
// 界面错误提示),各写一份 `applicationDirPath() + "data"` 迟早漂移。
//
// ⚠️ 约束:exe 所在目录必须可写 —— 安装到 `%LOCALAPPDATA%\Programs\Nextboard`
//    (安装器默认,单用户免 UAC)满足;若用户手工装到 Program Files 等只读位置,
//    数据目录创建会失败,由启动链路给出可读错误(见 core_bridge init)。
#pragma once

#include <QString>

namespace nb {

/// 数据根目录绝对路径(exe 同级 `data`)。依赖 QCoreApplication 已构造。
/// 若已用 `setAppDataDirOverride()` 指定(命令行 `--data-dir`,验收/多实例调试用),
/// 返回该覆盖值。
QString appDataDir();

/// 覆盖数据目录(必须在任何 `appDataDir()` 调用**之前**,即 main 早期解析参数时)。
/// 用途:自动化验收跑在独立临时目录,**绝不写用户真实数据**;
/// 空串 = 恢复默认(exe 同级 data)。
void setAppDataDirOverride(const QString& dir);

/// 确保数据目录存在;成功或已存在返回 true。
bool ensureAppDataDir();

/// 数据目录下某个文件的绝对路径(如 `nextboard.log`)。
QString appDataFile(const QString& name);

} // namespace nb
