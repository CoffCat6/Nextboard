#pragma once

#include <QString>

namespace nb {

/// 开机自启:HKCU\Software\Microsoft\Windows\CurrentVersion\Run 写/删
/// `Nextboard` 键值(当前用户,无需管理员权限)。
namespace auto_start {

/// 当前是否已启用(注册表存在该键值即视为启用)。
bool enabled();

/// 启用/停用;返回是否成功(失败原因写 qWarning)。
bool setEnabled(bool enable);

} // namespace auto_start
} // namespace nb
