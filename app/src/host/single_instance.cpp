#include "single_instance.hpp"

#include <QDebug>
#include <QDir>

#include <windows.h>

#include "host/core_bridge.hpp"   // nb::WM_APP_ACTIVATE

namespace {

/// 共享节名:存放"已有实例的主面板 HWND"(见 activateExisting)。
constexpr wchar_t kShmName[] = L"NextboardSingleInstanceHwnd";

/// 数据目录 → 互斥量名后缀(FNV-1a 32 位,8 位十六进制)。
///
/// 用例是"不同数据目录必须拿到不同互斥量",不是密码学 —— FNV-1a 足够且没有依赖。
/// 先做路径规范化(小写 + 本地分隔符),否则 "D:/a/b" 与 "D:\a\b" 会算出两个名字,
/// 而它们是同一个目录。
QString dirHash(const QString& dir) {
    const QString norm = QDir::cleanPath(dir).toLower().replace(QLatin1Char('/'),
                                                              QLatin1Char('\\'));
    quint32 h = 2166136261u;
    for (const QChar ch : norm) {
        h ^= static_cast<quint32>(ch.unicode());
        h *= 16777619u;
    }
    return QStringLiteral("%1").arg(h, 8, 16, QLatin1Char('0'));
}

}  // namespace

// 唤起消息的接收方是 **PanelWindow::nativeEvent**(2026-11-03 接通)。
// 此前这里有一个 ActivateFilter,但全仓从未实例化它 —— 消息广播出去无人认领,
// 二次启动静默 return 0,用户看到的是"点了图标没反应"。改为在 PanelWindow 里
// 直接认领:少一个对象、少一处生命周期配对(本模块此前已因生命周期问题崩过)。

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() {
    if (m_mutex) ::ReleaseMutex(static_cast<HANDLE>(m_mutex));
    if (m_mutex) ::CloseHandle(static_cast<HANDLE>(m_mutex));
}

bool SingleInstance::ensure(const QString& key, unsigned long* lastError) {
    // ⚠️ 互斥量名必须**按数据目录**区分(2026-11-03,外部审查 #15)。
    // 原实现 `Q_UNUSED(key)` + 硬编码名字:"同一数据目录只允许一个实例"这个语义
    // 从来没实现过 —— 两个用**不同** --data-dir 的合法实例(验收 vs 用户实例,
    // 或双便携副本)会互相挡掉,而它们本来各写各的库、没有任何竞争。
    //
    // 命名约束(实测踩过):CreateMutexW 对含反斜杠的非保留前缀名在本沙箱/作业对象
    // 环境下返回 ERROR_PATH_NOT_FOUND —— 所以**绝不能**把路径本身编进名字。
    // 用 blake3 风格的做法不可行(C++ 侧没有依赖),这里用 FNV-1a 32 位摘要:
    // 目标只是"不同目录不同名字",不是抗碰撞密码学。折叠成 8 位十六进制后,
    // 名字里只剩 [A-Za-z0-9],不触碰任何路径前缀规则。
    ::SetLastError(0);
    const QString name = QStringLiteral("NextboardSingleInstance-%1").arg(dirHash(key));
    m_mutex = ::CreateMutexW(nullptr, TRUE, reinterpret_cast<LPCWSTR>(name.utf16()));
    const DWORD gle = ::GetLastError();
    if (lastError) *lastError = gle;
    return m_mutex && gle != ERROR_ALREADY_EXISTS;
}

void SingleInstance::publishWindow(void* rawHwnd) {
    const HWND hwnd = static_cast<HWND>(rawHwnd);
    if (!hwnd) return;
    // 把本实例的主面板 HWND 写进命名共享节,供"被拒绝的第二个实例"定点投递。
    // 命名互斥量保证同一时刻只有一个实例在执行发布,故不需要额外并发保护。
    // ⚠️ 为什么要存:见 activateExisting 的注释 —— 广播到 HWND_BROADCAST 对
    //    Qt 的**工具窗口**(Qt::Tool,主面板即是)实测收不到。
    const HANDLE mapping = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HWND), kShmName);
    if (!mapping) {
        qWarning() << "single-instance: 创建共享节失败,二次启动唤起将退化为广播";
        return;
    }
    if (void* view = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(HWND))) {
        *static_cast<HWND*>(view) = hwnd;
        ::UnmapViewOfFile(view);
    }
    // ⚠️ **不能**现在 CloseHandle(2026-11-03 实测踩到):命名节对象在
    // **最后一个句柄关闭时即销毁**,关了之后第二个实例 OpenFileMapping 会失败、
    // 只能退回广播(而广播对 Qt::Tool 窗口无效)—— 表现就是"唤起仍然没反应"。
    // 句柄必须活到进程结束:交给一个函数级 static 持有(不主动释放,进程退出时
    // 由内核回收)。
    static const HANDLE kKept = mapping;
    Q_UNUSED(kKept);
}

void SingleInstance::activateExisting() {
    // 唤起已有实例:优先**定点投递**。
    //
    // ⚠️ 不要用 PostMessageW(HWND_BROADCAST, …)(2026-11-03 实测):
    //    HWND_BROADCAST 只投给**顶层、非消息-only**窗口,而主面板是
    //    Qt::Tool | FramelessWindowHint。实测向广播发 WM_APP+10 后主面板
    //    IsWindowVisible 仍为 false;改为向具体 HWND PostMessage 后立刻可见。
    //    原实现写着"广播即可",其实这条链路从未真正工作过(而且当时也没有接收方,
    //    见文件头的 ActivateFilter 说明)—— 两个缺陷叠在一起,表现为
    //    "再次点图标完全没反应"。
    const HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
    if (mapping) {
        if (const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(HWND))) {
            const HWND target = *static_cast<const HWND*>(view);
            ::UnmapViewOfFile(view);
            if (target && ::IsWindow(target)) {
                ::PostMessageW(target, nb::WM_APP_ACTIVATE, 0, 0);
                ::CloseHandle(mapping);
                return;
            }
        }
        ::CloseHandle(mapping);
    }
    // 兜底:共享节不存在(旧实例尚未发布 / 旧版本实例)或窗口已失效 ——
    // 仍然广播一次,不退化成"什么都不做"。
    ::PostMessageW(HWND_BROADCAST, nb::WM_APP_ACTIVATE, 0, 0);
}
