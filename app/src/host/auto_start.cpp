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
    const REGSAM access = enable ? KEY_SET_VALUE : DELETE;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, access, &key) != ERROR_SUCCESS) {
        qWarning() << "auto_start: 打开 Run 注册表键失败, error =" << ::GetLastError();
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
    return true;
}

} // namespace nb::auto_start
