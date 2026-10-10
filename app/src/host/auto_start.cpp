#include "auto_start.hpp"

#include <QCoreApplication>
#include <QDir>

#include <windows.h>

namespace nb::auto_start {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Nextboard";

QString commandLine() {
    // 引号包裹:路径含空格时 Run 键按整条命令解析,不包裹会被拆成 exe + 参数
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(
        QCoreApplication::applicationFilePath()));
}

} // namespace

bool enabled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    const LSTATUS rc = ::RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, nullptr);
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool setEnabled(bool enable) {
    HKEY key = nullptr;
    // ⚠️ 启用与停用都要 KEY_SET_VALUE:RegDeleteValue **同样**要求键句柄带 KEY_SET_VALUE
    //    权限(它删的是"值",不是"子键")。早先停用时传 DELETE —— 那是 RegDeleteKey 删
    //    整个子键的权限 —— 于是 RegDeleteValue 返回 ERROR_ACCESS_DENIED(5),功能表现为
    //    "勾上开机自启后就再也取消不掉"(设置页回读注册表后把勾选态弹回去)。
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        qWarning() << "auto_start: 打开 Run 注册表键失败(需 KEY_SET_VALUE), error ="
                   << ::GetLastError();
        return false;
    }
    const LSTATUS rc = enable
        ? ::RegSetValueExW(key, kValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(commandLine().utf16()),
                           static_cast<DWORD>((commandLine().size() + 1) * sizeof(wchar_t)))
        : ::RegDeleteValueW(key, kValueName);
    ::RegCloseKey(key);
    // 删除时值不存在也算成功(幂等);其余失败打日志并返回 false
    if (rc != ERROR_SUCCESS && !(rc == ERROR_FILE_NOT_FOUND && !enable)) {
        qWarning() << "auto_start: 写/删注册表值失败, rc =" << rc;
        return false;
    }
    // 回读复核:注册表被策略/安全软件拦截时,写入可能"成功"但实际未生效
    if (enabled() != enable) {
        qWarning() << "auto_start: 操作后回读不一致(期望" << enable << ",实际" << enabled()
                   << ")—— 注册表可能被安全软件或组策略拦截";
        return false;
    }
    return true;
}

} // namespace nb::auto_start
