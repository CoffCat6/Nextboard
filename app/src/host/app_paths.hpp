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
//
// 📌 **已评审的取舍(2026-10-26,用户拍板"保持现状")**:开发构建下本目录位于
//    `app\build\data\` —— 在构建产物目录里,手动清 build 会连数据一起删。
//    不迁移的理由:便携语义(数据跟着 exe 走)优先,且开发树已有启动自动备份兜底
//    (CoreFacade::maybeAutoBackup,每日一份保留 3 份,`<data>\backups\`)。
//    任何人要清 `app\build` 前,先确认 `backups\` 里有新备份或手动挪走 `data\`。
#pragma once

#include <QString>

namespace nb {

/// 数据根目录绝对路径(exe 同级 `data`)。依赖 QCoreApplication 已构造。
/// 若已用 `setAppDataDirOverride()` 指定(命令行 `--data-dir`,验收/多实例调试用),
/// 返回该覆盖值。
QString appDataDir();

/// 数据目录的**早期**版本:不依赖 QCoreApplication(用 Win32 取 exe 路径)。
///
/// 唯一用途是"QApplication 构造之前"的场景 —— 目前只有单实例门禁(main.cpp
/// 在 QApplication 之前就要算互斥量名,而 appDataDir() 依赖 applicationDirPath()、
/// 此时返回空串)。行为与 appDataDir() 保持一致:有 --data-dir 覆盖就用覆盖值,
/// 否则 exe 同级 data。
/// ⚠️ 正常的运行期代码一律用 appDataDir(),不要用这个 —— 两处口径分开维护
/// （一个走 Qt、一个走 Win32)只是为了让 QApplication 之前的调用点有正确值。
QString appDataDirEarly();

/// 覆盖数据目录(必须在任何 `appDataDir()` 调用**之前**,即 main 早期解析参数时)。
/// 用途:自动化验收跑在独立临时目录,**绝不写用户真实数据**;
/// 空串 = 恢复默认(exe 同级 data)。
void setAppDataDirOverride(const QString& dir);

/// 确保数据目录存在;成功或已存在返回 true。
bool ensureAppDataDir();

/// 数据目录下某个文件的绝对路径(如 `nextboard.log`)。
QString appDataFile(const QString& name);

} // namespace nb
