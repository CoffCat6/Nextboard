#include "single_instance.hpp"

#include <QDebug>
#include <QDir>

#if defined(Q_OS_WIN)
#include <windows.h>

#include "host/core_bridge.hpp"   // nb::WM_APP_ACTIVATE
#else
// Linux(M0b):门禁 = QLockFile(数据目录内,按 PID 判陈旧锁);
// 唤起通道 = QLocalServer/QLocalSocket(socket 名 = 数据目录 FNV-1a 摘要)。
// 语义与 Windows 版一一对应:不同数据目录互不阻挡;二次启动定点唤起。
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#endif

namespace {

#if defined(Q_OS_WIN)
/// 共享节名:存放"已有实例的主面板 HWND"(见 activateExisting)。
constexpr wchar_t kShmName[] = L"NextboardSingleInstanceHwnd";
#endif

/// 数据目录 → 门禁/通道名后缀(FNV-1a 32 位,8 位十六进制)。
///
/// 用例是"不同数据目录必须拿到不同门禁",不是密码学 —— FNV-1a 足够且没有依赖。
/// 先做路径规范化(小写 + 本地分隔符),否则 "D:/a/b" 与 "D:\a\b" 会算出两个名字,
/// 而它们是同一个目录。
QString dirHash(const QString& dir) {
#if defined(Q_OS_WIN)
    const QChar nativeSep = QLatin1Char('\\');
#else
    const QChar nativeSep = QLatin1Char('/');
#endif
    const QString norm = QDir::cleanPath(dir).toLower().replace(QLatin1Char('/'), nativeSep);
    quint32 h = 2166136261u;
    for (const QChar ch : norm) {
        h ^= static_cast<quint32>(ch.unicode());
        h *= 16777619u;
    }
    return QStringLiteral("%1").arg(h, 8, 16, QLatin1Char('0'));
}

#if !defined(Q_OS_WIN)
/// ensure() 时写入的门禁键(publishWindow/activateExisting 复用);
/// 进程级生命周期,static 存放与 Windows 版"句柄活到进程结束"的口径一致。
QString g_instanceChannel;
#endif

}  // namespace

// 唤起的接收方:Windows = **PanelWindow::nativeEvent**(WM_APP_ACTIVATE 分支);
// Linux = SingleInstance::activateRequested() 信号 → main.cpp → showAtCursor。

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() {
#if defined(Q_OS_WIN)
    if (m_mutex) ::ReleaseMutex(static_cast<HANDLE>(m_mutex));
    if (m_mutex) ::CloseHandle(static_cast<HANDLE>(m_mutex));
#else
    // QLockFile 析构即解锁;实例随进程退出,残锁由 PID 校验自动视为陈旧。
    delete static_cast<QLockFile*>(m_mutex);
#endif
}

bool SingleInstance::ensure(const QString& key, unsigned long* lastError) {
#if defined(Q_OS_WIN)
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
#else
    // Linux(M0b):锁文件放数据目录内(key 即数据目录),目录可能尚未创建。
    // staleLockTime(0) = 永不按时间判陈旧 —— 只信 PID 校验(QLockFile 在 Unix
    // 侧把 PID+主机名写进锁文件,崩溃残留锁会被可靠识别),避免"慢启动 30s 内
    // 被误判陈旧"的双开窗口。
    QDir().mkpath(key);
    auto* lock =
        new QLockFile(key + QStringLiteral("/nextboard-single-instance.lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        if (lastError) *lastError = 1;
        delete lock;
        return false;
    }
    m_mutex = lock;
    g_instanceChannel = QStringLiteral("nextboard-si-%1").arg(dirHash(key));
    if (lastError) *lastError = 0;
    return true;
#endif
}

void SingleInstance::publishWindow(void* rawHwnd) {
#if defined(Q_OS_WIN)
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
#else
    Q_UNUSED(rawHwnd);
    // K7(2026-11-07 修):`--data-dir` 实例的 ensure() 被跳过(不参与单实例
    // 门禁,Windows 同语义)→ 通道名为空。此前直接 listen("") 报 Name error。
    // 此类实例本就没有"被唤起"的入口(门禁未拒绝过任何人),跳过即正确。
    if (g_instanceChannel.isEmpty()) {
        qInfo() << "single-instance: --data-dir 实例不参与单实例门禁,跳过通道发布";
        return;
    }
    // Linux(M0b):QLocalServer 承担唤起通道。先清残留 socket 文件
    // (崩溃后的 unix domain socket 会残留,不清则 listen 失败)。
    QLocalServer::removeServer(g_instanceChannel);
    auto* server = new QLocalServer(this);
    if (!server->listen(g_instanceChannel)) {
        qWarning() << "single-instance: QLocalServer 监听失败,二次启动唤起不可用:"
                   << server->errorString();
        return;
    }
    connect(server, &QLocalServer::newConnection, this, [this, server] {
        while (QLocalSocket* conn = server->nextPendingConnection()) {
            conn->readAll();  // 唤起信号不带 payload(与 WM_APP 消息同哲学)
            conn->disconnectFromServer();
            emit activateRequested();
        }
    });
#endif
}

void SingleInstance::activateExisting() {
#if defined(Q_OS_WIN)
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
#else
    // Linux(M0b):连已有实例的唤起通道;连不上 = 实例未发布通道(旧版本)
    // 或恰好退出,与 Windows 广播兜底同样"不退化成崩溃"。
    QLocalSocket socket;
    socket.connectToServer(g_instanceChannel);
    if (socket.waitForConnected(300)) {
        socket.write("a");
        socket.waitForBytesWritten(300);
        socket.disconnectFromServer();
        return;
    }
    qInfo() << "single-instance: 未发现可唤起的实例通道,直接退出";
#endif
}
