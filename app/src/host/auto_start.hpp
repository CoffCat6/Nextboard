#pragma once

#include <QString>

namespace nb {

/// 开机自启:HKCU\Software\Microsoft\Windows\CurrentVersion\Run 写/删
/// `Nextboard` 键值(当前用户,无需管理员权限)。
///
/// **命令行形态必须带 `--hidden`**:开机自启的语义是"静默驻留托盘",
/// 面板抢焦点弹出会打断用户开机后的第一件事。安装器(tools/installer.nsi
/// 的 SecAutoStart)与这里的应用内开关写同一形态,行为才一致 —— docs/04 §8 D11。
namespace auto_start {

/// 当前是否已启用(注册表存在该键值即视为启用)。
bool enabled();

/// 启用/停用;返回是否成功(失败原因写 qWarning)。
/// 启用时写入的命令行为 `"<exe 路径>" --hidden`。
bool setEnabled(bool enable);

/// 把**旧版本留下的**自启命令行升级为带 `--hidden` 的形态(启动时调用)。
///
/// 老版本(0.2.0 及更早)的应用内开关只写 `"<exe>"`,于是"设置里开了自启 →
/// 每次开机弹面板"。本函数在自启已启用但命令行不含 `--hidden` 时就地改写,
/// 用户不必去设置页关了再开。返回是否发生了改写(未启用 / 已是最新 → false)。
bool refreshCommandLineIfStale();

// ---- 验收辅助(--autostart-flow) ----
//
// 存在的理由:自启形态的唯一可见后果发生在**下次开机**,人工回归一次就要重启一次,
// 而这段逻辑全是注册表读写,完全可以进程内断言。三个钩子让验收能
// "读回实际值 / 拿到期望值 / 伪造旧值" —— 与 docs/04 §6 的安装冒烟清单互补:
// 冒烟验证"安装器写的值对不对",这里验证"应用内开关写的值对不对"。

/// 读回 Run 键当前登记的命令行(无值 → 空串)。
QString debugRegisteredCommandLine();

/// 期望的命令行形态(`"<exe 路径>" --hidden`),供验收比对。
QString debugExpectedCommandLine();

/// 原样写入一条命令行(不做任何加工),用于伪造旧版本留下的值。
bool debugWriteRawCommandLine(const QString& command);

} // namespace auto_start
} // namespace nb
