#include "core_bridge.hpp"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QStandardPaths>

#include <windows.h>

namespace nb {

CoreBridge& CoreBridge::instance() {
    static CoreBridge bridge;
    return bridge;
}

bool CoreBridge::init(bool /*startHidden*/, QString* error) {
    qInfo() << "bridge: loading nbcore.dll";
    m_dll = ::LoadLibraryW(L"nbcore.dll");
    if (!m_dll) {
        if (error) *error = QStringLiteral("加载 nbcore.dll 失败 (GetLastError=%1)").arg(::GetLastError());
        return false;
    }
    qInfo() << "bridge: resolving exports";
    auto resolve = [this](const char* name) {
        return reinterpret_cast<QFunctionPointer>(::GetProcAddress(m_dll, name));
    };
    m_init = reinterpret_cast<NbInit>(resolve("nb_init"));
    m_callFn = reinterpret_cast<NbCallFn>(resolve("nb_call"));
    m_freeString = reinterpret_cast<NbFreeString>(resolve("nb_free_string"));
    m_freeBuffer = reinterpret_cast<NbFreeBuffer>(resolve("nb_free_buffer"));
    m_abi = reinterpret_cast<NbAbiVersion>(resolve("nb_abi_version"));
    m_shutdown = reinterpret_cast<NbShutdown>(resolve("nb_shutdown"));
    m_captureMonitor = reinterpret_cast<NbCaptureMonitor>(resolve("nb_capture_monitor"));
    m_captureCrop = reinterpret_cast<NbCaptureCrop>(resolve("nb_capture_crop"));
    if (!m_init || !m_callFn || !m_freeString || !m_shutdown || !m_abi) {
        if (error) *error = QStringLiteral("nbcore.dll 导出缺失");
        return false;
    }
    if (m_abi() != 1u) {
        if (error) *error = QStringLiteral("nbcore ABI 版本不匹配(%1),请重新构建核心").arg(m_abi());
        return false;
    }

    // 数据目录:%LOCALAPPDATA%/Nextboard
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    m_dataDir = dataDir;

    // 通知窗口:传 0(仅全局 PostMessage;v1 面板轮询/事件驱动再接入专属 HWND)
    const QJsonObject cfg{
        {"data_dir", QDir::toNativeSeparators(dataDir)},
        {"notify_hwnd", 0},
    };
    const QByteArray json = QJsonDocument(cfg).toJson(QJsonDocument::Compact);
    qInfo() << "bridge: calling nb_init" << json;
    const int rc = m_init(json.constData());
    qInfo() << "bridge: nb_init returned" << rc;
    if (rc != 0) {
        if (error) *error = QStringLiteral("nb_init 失败(rc=%1)").arg(rc);
        return false;
    }
    m_initialized = true;
    return true;
}

void CoreBridge::shutdown() {
    if (m_initialized && m_shutdown) {
        m_shutdown();
        m_initialized = false;
    }
    // 注意:不 FreeLibrary——面板/贴图窗口可能在 shutdown 后仍持有回调;
    // 进程退出时随进程回收(与 v1 单例生命周期一致)。
}

QJsonObject CoreBridge::call(const QString& method, const QJsonObject& req) {
    if (!ok()) {
        qWarning() << "nb_call" << method << "-> core not ok";
        return {{"ok", false}};
    }
    const QByteArray methodUtf8 = method.toUtf8();
    const QByteArray reqUtf8 =
        QJsonDocument(req).toJson(QJsonDocument::Compact);
    char* resp = nullptr;
    const int rc = m_callFn(methodUtf8.constData(),
                            req.isEmpty() ? nullptr : reqUtf8.constData(), &resp);
    QJsonObject out;
    if (resp) {
        const QByteArray raw(resp);
        out = QJsonDocument::fromJson(raw).object();
        if (out.isEmpty())
            qWarning() << "nb_call" << method << "rc=" << rc
                       << "resp unparseable:" << raw.left(300);
        m_freeString(resp);
    } else {
        qWarning() << "nb_call" << method << "rc=" << rc << "resp=nullptr";
    }
    if (rc != 0 && !out.contains("error")) {
        out = {{"ok", false}, {"error", QStringLiteral("nb_call rc=%1").arg(rc)}};
    }
    return out;
}

void CoreBridge::setNotifyHwnd(HWND hwnd) {
    if (!m_callFn) return;
    // 走 save_settings? 不 —— 通知 HWND 是运行期参数,用专门导出最干净;
    // v1 借道 settings 通道太重,直接加一个 nb_set_notify_hwnd 导出。
    auto fn = reinterpret_cast<void(__cdecl*)(unsigned long long)>(
        ::GetProcAddress(m_dll, "nb_set_notify_hwnd"));
    if (fn) fn(reinterpret_cast<unsigned long long>(hwnd));
}

QString CoreBridge::blobsDirAbs() {
    return instance().m_dataDir + "/blobs";
}

QImage CoreBridge::captureMonitor(unsigned index) {
    if (!m_captureMonitor || !m_freeBuffer) return {};
    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    if (m_captureMonitor(index, &rgba, &w, &h) != 0 || !rgba || w == 0 || h == 0) {
        if (rgba) m_freeBuffer(rgba, 0);
        return {};
    }
    // QImage 构造拷贝像素,随后立即释放核心缓冲
    QImage image(rgba, w, h, QImage::Format_RGBA8888);
    QImage copy = image.copy();
    m_freeBuffer(rgba, static_cast<unsigned long long>(w) * h * 4);
    return copy;
}

} // namespace nb
