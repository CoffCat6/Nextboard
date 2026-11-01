#include "core_bridge.hpp"

#include "app_paths.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>

#include <windows.h>

namespace nb {
namespace {

/// 核心缓冲的托管信息:交给 QImage 的清理回调保管。
///
/// 动机(内存):`nb_capture_monitor` 返回的整屏 RGBA 由核心分配(4K 约 33MB)。
/// 早先的写法是先 `image.copy()` 深拷贝一份再释放原缓冲 —— 峰值因此翻倍。
/// 改为让 QImage **直接沿用**核心缓冲,最后一份引用析构时经本结构交还 `nb_free_buffer`。
struct CoreBuffer {
    unsigned char* data;
    unsigned long long size;
    NbFreeBuffer free;
};

/// QImageCleanupFunction:不带上下文,故指针与长度经 CoreBuffer 传递。
void releaseCoreBuffer(void* info) {
    auto* buffer = static_cast<CoreBuffer*>(info);
    if (buffer->free && buffer->data) buffer->free(buffer->data, buffer->size);
    delete buffer;
}

} // namespace

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
    m_version = reinterpret_cast<NbVersion>(resolve("nb_version"));
    m_shutdown = reinterpret_cast<NbShutdown>(resolve("nb_shutdown"));
    m_captureMonitor = reinterpret_cast<NbCaptureMonitor>(resolve("nb_capture_monitor"));
    // 按屏描述配对捕获:可选导出(旧 DLL 没有)→ 取不到时调用方回退按序号,
    // 故**不**加入下面的"致命缺失"检查,避免旧核心直接起不来。
    m_captureMonitorMatched =
        reinterpret_cast<NbCaptureMonitorMatched>(resolve("nb_capture_monitor_matched"));
    m_captureCrop = reinterpret_cast<NbCaptureCrop>(resolve("nb_capture_crop"));
    // 直抓选区(长截图逐帧用):可选导出 —— 旧核心没有时宿主仍可回退
    // "整屏 + 裁剪",但那条路径更慢,故 hasRegionCapture() 会给出提示。
    m_captureMonitorRegion =
        reinterpret_cast<NbCaptureMonitorRegionMatched>(
            resolve("nb_capture_monitor_region_matched"));
    m_ocrRgba = reinterpret_cast<NbOcrRgba>(resolve("nb_ocr_rgba"));
    // 二维码解码:可选导出(旧核心没有)→ qrDecodeRgba 返回 found=false,不崩。
    m_qrDecodeRgba =
        reinterpret_cast<NbQrDecodeRgba>(resolve("nb_qr_decode_rgba"));
    // 长截图导出:同样是**可选**导出(旧核心没有)→ 不进"致命缺失"检查。
    // 功能侧用 hasStitch() 判断是否可用,不可用时热键给出可读提示而非静默无反应。
    m_stitchBegin = reinterpret_cast<NbStitchBegin>(resolve("nb_stitch_begin"));
    m_stitchAppend = reinterpret_cast<NbStitchAppend>(resolve("nb_stitch_append"));
    m_stitchFinish = reinterpret_cast<NbStitchFinish>(resolve("nb_stitch_finish"));
    m_stitchFinishRgba =
        reinterpret_cast<NbStitchFinishRgba>(resolve("nb_stitch_finish_rgba"));
    m_stitchAbort = reinterpret_cast<NbStitchAbort>(resolve("nb_stitch_abort"));
    // 截图入库的 RGBA 直入通道:同样是**可选**导出 —— 旧核心没有时宿主回退
    // 到 ingest_capture_png(见 hasRgbaIngest),不会因为新宿主配旧 DLL 就崩。
    m_ingestCaptureRgba =
        reinterpret_cast<NbIngestCaptureRgba>(resolve("nb_ingest_capture_rgba"));
    // v2(带来源应用):旧核心没有该导出时保持 nullptr,调用处回退 v1。
    m_ingestCaptureRgba2 =
        reinterpret_cast<NbIngestCaptureRgba2>(resolve("nb_ingest_capture_rgba2"));
    m_copyRgbaToClipboard =
        reinterpret_cast<NbCopyRgbaToClipboard>(resolve("nb_copy_rgba_to_clipboard"));
    if (!m_init || !m_callFn || !m_freeString || !m_shutdown || !m_abi) {
        if (error) *error = QStringLiteral("nbcore.dll 导出缺失");
        return false;
    }
    if (m_abi() != 1u) {
        if (error) *error = QStringLiteral("nbcore ABI 版本不匹配(%1),请重新构建核心").arg(m_abi());
        return false;
    }

    // 数据目录:exe 同级 data/(便携式布局,见 host/app_paths.hpp)。
    // 目录不可写时**立即报错**——否则后续会以"核心初始化失败"等含混现象暴露。
    const QString dataDir = appDataDir();
    if (!ensureAppDataDir()) {
        if (error) *error = QStringLiteral("数据目录不可写:%1(安装目录需可写)").arg(dataDir);
        return false;
    }
    m_dataDir = dataDir;
    qInfo() << "bridge: data dir =" << dataDir;

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

    // 生效数据目录:设置里可能启用了自定义目录(与默认目录不同)。
    // blobs 绝对路径、面板 file:// URL 都以它为准;查询失败回退默认目录。
    m_effectiveDataDir = dataDir;
    if (m_callFn) {
    const QJsonObject resp = call("data_dir");
    const QString path = resp.value("data").toObject().value("path").toString();
    if (resp.value("ok").toBool() && !path.isEmpty()) {
        m_effectiveDataDir = path;
        qInfo() << "bridge: effective data dir =" << m_effectiveDataDir;
    }
    // 配置加载警告:核心侧的回退会把**所有设置**打回默认(实测最坏是渲染后端从
    // 软件切回硬件 D3D11,稳态 +100MB),而 Rust 的日志出口是 nbcore.log ——
    // 排障第一现场是这份日志,所以在这里取走并落一条。
    const QJsonObject warnResp = call("settings_warnings");
    if (warnResp.value("ok").toBool()) {
        for (const QJsonValue& v : warnResp.value("data").toArray())
            qWarning().noquote() << "配置警告:" << v.toString();
    }
    }
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
    // 生效目录:自定义数据目录启用后,默认目录下没有 blobs
    return instance().m_effectiveDataDir + "/blobs";
}

QString CoreBridge::thumbnailPathFor(const QString& absoluteBlobPath) {
    // 与 core 的 clip::thumbnail_path() 同一规则:xxx.png → xxx.thumb<ver>.png
    // (在**最后一个点**前插入,无扩展名同样安全)
    const int slash = absoluteBlobPath.lastIndexOf('/');
    const int dot = absoluteBlobPath.lastIndexOf('.');
    if (dot <= slash) return absoluteBlobPath;   // 无扩展名:理论上不会发生
    QString out = absoluteBlobPath;
    out.insert(dot, QStringLiteral(".thumb%1").arg(nbThumbVersion));
    return out;
}

QString CoreBridge::coreVersion() const {
    if (!m_version) return {};
    // 与 nb_version() 的编码一致:major*10000 + minor*100 + patch
    const unsigned v = m_version();
    return QStringLiteral("%1.%2.%3")
        .arg(v / 10000)
        .arg((v / 100) % 100)
        .arg(v % 100);
}

QImage CoreBridge::captureMonitor(unsigned index) {
    if (!m_captureMonitor || !m_freeBuffer) return {};
    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    if (m_captureMonitor(index, &rgba, &w, &h) != 0 || !rgba || w == 0 || h == 0) {
        if (rgba) m_freeBuffer(rgba, 0);
        return {};
    }
    // 零拷贝:像素直接沿用核心缓冲,不 copy()。QImage 只在最后一份引用析构时
    // 回调 releaseCoreBuffer 交还 nb_free_buffer —— 峰值因此少一份整屏(4K 约 33MB)。
    // 生命周期:遮罩持有冻结底图直到会话结束,期间缓冲一直有效;进程退出时
    // CoreBridge::shutdown() **不 FreeLibrary**(见其注释),故回调指针始终可调用。
    auto* buffer = new CoreBuffer{
        rgba, static_cast<unsigned long long>(w) * h * 4, m_freeBuffer};
    return QImage(rgba, static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888,
                  &releaseCoreBuffer, buffer);
}

QImage CoreBridge::captureMonitorMatched(QScreen* screen) {
    if (!screen) return {};
    return captureMonitorMatched(screen->geometry(), screen->name(), screen->devicePixelRatio());
}

QImage CoreBridge::captureMonitorMatched(const QRect& geo, const QString& name, qreal dpr) {
    if (!m_captureMonitorMatched || !m_freeBuffer) return {};
    const QByteArray nameUtf8 = name.toUtf8();
    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    const int rc = m_captureMonitorMatched(
        nameUtf8.constData(),
        geo.x(), geo.y(),
        static_cast<unsigned>(geo.width()), static_cast<unsigned>(geo.height()),
        static_cast<float>(dpr),
        &rgba, &w, &h);
    if (rc != 0 || !rgba || w == 0 || h == 0) {
        if (rgba) m_freeBuffer(rgba, 0);
        return {};
    }
    // 零拷贝语义与 captureMonitor 完全一致(同一 releaseCoreBuffer 回调)
    auto* buffer = new CoreBuffer{
        rgba, static_cast<unsigned long long>(w) * h * 4, m_freeBuffer};
    return QImage(rgba, static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888,
                  &releaseCoreBuffer, buffer);
}

QJsonArray CoreBridge::listMonitors() {
    const QJsonObject resp = call(QStringLiteral("list_monitors"));
    if (!resp.value("ok").toBool()) return {};
    return resp.value("data").toArray();
}

QImage CoreBridge::captureCrop(const QImage& screen, const QRect& rect) {
    if (!m_captureCrop || !m_freeBuffer || screen.isNull()) return {};
    if (rect.width() <= 0 || rect.height() <= 0) return {};
    // 核心按 RGBA8888 紧凑排布读取:先统一格式再取裸指针
    const QImage src = screen.convertToFormat(QImage::Format_RGBA8888);
    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    const int rc = m_captureCrop(
        src.constBits(),
        static_cast<unsigned long long>(src.sizeInBytes()),
        static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()),
        rect.x(), rect.y(),
        static_cast<unsigned>(rect.width()), static_cast<unsigned>(rect.height()),
        &rgba, &w, &h);
    if (rc != 0 || !rgba || w == 0 || h == 0) {
        if (rgba) m_freeBuffer(rgba, 0);
        return {};
    }
    // 零拷贝:与 captureMonitor 同一套 CoreBuffer + releaseCoreBuffer 托管。
    // 原先这里 `out.copy()` 深拷一份裁剪图再归还核心缓冲 —— 峰值白翻一倍
    // (1500×850 裁剪多 5MB,4K 全屏裁剪多 33MB),而拷贝毫无必要:QImage 自己
    // 就能管住核心缓冲,最后一份引用析构时交还 nb_free_buffer。
    auto* buffer = new CoreBuffer{rgba, static_cast<unsigned long long>(w) * h * 4, m_freeBuffer};
    return QImage(rgba, static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888,
                  &releaseCoreBuffer, buffer);
}

// ---- 滚动长截图(docs/19) ----

QImage CoreBridge::captureMonitorRegion(QScreen* screen, const QRect& regionDevice) {
    if (!screen) return {};
    return captureMonitorRegion(screen->geometry(), screen->name(),
                                screen->devicePixelRatio(), regionDevice);
}

QImage CoreBridge::captureMonitorRegion(const QRect& geo, const QString& name, qreal dpr,
                                        const QRect& regionDevice) {
    if (!m_captureMonitorRegion || !m_freeBuffer) return {};
    if (regionDevice.width() <= 0 || regionDevice.height() <= 0) return {};
    const QByteArray nameUtf8 = name.toUtf8();
    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    const int rc = m_captureMonitorRegion(
        nameUtf8.constData(),
        geo.x(), geo.y(),
        static_cast<unsigned>(geo.width()), static_cast<unsigned>(geo.height()),
        static_cast<float>(dpr),
        static_cast<unsigned>(regionDevice.x()), static_cast<unsigned>(regionDevice.y()),
        static_cast<unsigned>(regionDevice.width()), static_cast<unsigned>(regionDevice.height()),
        &rgba, &w, &h);
    if (rc != 0 || !rgba || w == 0 || h == 0) {
        if (rgba) m_freeBuffer(rgba, 0);
        return {};
    }
    // 零拷贝:与 captureMonitor / captureCrop 同一套 CoreBuffer 托管
    auto* buffer = new CoreBuffer{
        rgba, static_cast<unsigned long long>(w) * h * 4, m_freeBuffer};
    return QImage(rgba, static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888,
                  &releaseCoreBuffer, buffer);
}

bool CoreBridge::stitchBegin(const QString& session, QScreen* screen, const QRect& regionDevice,
                            int maxHeight, qint64 maxBytes, int overlapRatio, QString* error) {
    if (!m_stitchBegin || !m_freeString) {
        if (error) *error = QStringLiteral("当前 nbcore.dll 没有 nb_stitch_begin(核心版本过旧)");
        return false;
    }
    if (session.isEmpty() || regionDevice.width() <= 0 || regionDevice.height() <= 0) {
        if (error) *error = QStringLiteral("长截图会话 id 或选区为空");
        return false;
    }
    // screen 允许为空:仅 --stitch-flow 用它验"拼接契约"(与哪块屏无关)。
    // 生产路径(capture_session)一定带屏,monitor 名会写进核心日志与 finish 元信息。
    const QByteArray sessionUtf8 = session.toUtf8();
    const QByteArray nameUtf8 = screen ? screen->name().toUtf8() : QByteArray();
    char* err = nullptr;
    const int rc = m_stitchBegin(
        sessionUtf8.constData(),
        nameUtf8.constData(),
        regionDevice.x(), regionDevice.y(),
        static_cast<unsigned>(regionDevice.width()), static_cast<unsigned>(regionDevice.height()),
        static_cast<float>(screen ? screen->devicePixelRatio() : 1.0),
        static_cast<long long>(maxHeight), static_cast<long long>(maxBytes),
        static_cast<long long>(overlapRatio),
        &err);
    if (rc != 0) {
        if (error) {
            *error = err ? QString::fromUtf8(err)
                         : QStringLiteral("核心拒绝开始长截图会话(原因见 nbcore.log)");
        }
        if (err) m_freeString(err);
        return false;
    }
    if (err) m_freeString(err);   // 成功时不写出参;防御性释放
    return true;
}

QJsonObject CoreBridge::stitchAppend(const QString& session, const QImage& frame) {
    if (!m_stitchAppend || !m_freeString || frame.isNull()) return {};
    if (session.isEmpty()) return {};
    // 核心按 RGBA8888 紧凑排布读(与 captureCrop / ocrImage 同口径)
    const QImage src = frame.convertToFormat(QImage::Format_RGBA8888);
    const QByteArray sessionUtf8 = session.toUtf8();
    char* out = nullptr;
    const int rc = m_stitchAppend(
        sessionUtf8.constData(),
        src.constBits(),
        static_cast<unsigned long long>(src.sizeInBytes()),
        static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()),
        &out);
    if (rc != 0 || !out) {
        if (out) m_freeString(out);
        return {};
    }
    // 与 nb_call 同款生命周期:立即转 QString,随即归还核心缓冲
    const QByteArray owned(out);
    m_freeString(out);
    const QJsonObject obj = QJsonDocument::fromJson(owned).object();
    return obj;
}

bool CoreBridge::stitchFinish(const QString& session, QImage* outPng, QJsonObject* outMeta) {
    if ((!m_stitchFinish && !m_stitchFinishRgba) || !m_freeString || !m_freeBuffer
        || !outPng || !outMeta) return false;
    if (session.isEmpty()) return false;
    const QByteArray sessionUtf8 = session.toUtf8();
    unsigned char* png = nullptr;
    unsigned long long pngLen = 0;
    char* meta = nullptr;
    // ---- RGBA 直通道(新核心):产物就是拼接 RGBA,零拷贝包成 QImage ----
    // 免掉"拼完 PNG 编码 → 宿主解码"的两次整图编解码(128MB 级长图上秒级
    // CPU、~200MB 峰值);入库那一次 PNG 编码仍在核心侧 ingest 时发生。
    if (m_stitchFinishRgba) {
        unsigned char* rgba = nullptr;
        unsigned long long rgbaLen = 0;
        unsigned w = 0, h = 0;
        const int rcR = m_stitchFinishRgba(sessionUtf8.constData(), &rgba, &rgbaLen, &w, &h, &meta);
        if (rcR != 0 || !rgba || rgbaLen == 0 || w == 0 || h == 0 || !meta) {
            if (rgba) m_freeBuffer(rgba, rgbaLen);
            if (meta) m_freeString(meta);
            return false;
        }
        const QByteArray metaJson(meta);
        m_freeString(meta);
        // 零拷贝:与 captureMonitor 同一套 CoreBuffer 托管,最后一份引用析构
        // 时把缓冲归还核心。会话已取走,不再有第二读者,共享生命周期安全。
        auto* buffer = new CoreBuffer{rgba, rgbaLen, m_freeBuffer};
        *outPng = QImage(rgba, static_cast<int>(w), static_cast<int>(h),
                         QImage::Format_RGBA8888, &releaseCoreBuffer, buffer);
        *outMeta = QJsonDocument::fromJson(metaJson).object();
        return true;
    }

    const int rc = m_stitchFinish(sessionUtf8.constData(), &png, &pngLen, &meta);
    if (rc != 0 || !png || pngLen == 0 || !meta) {
        if (png) m_freeBuffer(png, pngLen);
        if (meta) m_freeString(meta);
        return false;
    }
    const QByteArray metaJson(meta);
    m_freeString(meta);

    // PNG 字节 → QImage:loadFromData 会解码出**自有**的像素数据,
    // 之后即可归还核心缓冲(不跨 FFI 共享生命周期,比零拷贝更简单可靠 ——
    // 长截图只有一次,不是逐帧高频路径,多一次解码换取无所有权纠葛是划算的)。
    const QImage decoded = QImage::fromData(png, static_cast<int>(pngLen), "PNG");
    m_freeBuffer(png, pngLen);
    if (decoded.isNull()) {
        qWarning() << "stitch finish: 核心产物 PNG 解码失败,长度 =" << pngLen;
        return false;
    }
    *outPng = decoded;
    *outMeta = QJsonDocument::fromJson(metaJson).object();
    return true;
}

void CoreBridge::stitchAbort(const QString& session) {
    if (!m_stitchAbort || session.isEmpty()) return;
    const QByteArray sessionUtf8 = session.toUtf8();
    m_stitchAbort(sessionUtf8.constData());
}

// ---- 截图入库(RGBA 直入)----

QJsonObject CoreBridge::ingestCaptureRgba(const QImage& image, const QString& source) {
    return ingestCaptureRgba2(image, source, QString());
}

QJsonObject CoreBridge::ingestCaptureRgba2(const QImage& image, const QString& source,
                                           const QString& owner) {
    if (!m_freeString || image.isNull()) return {};
    // 核心按 RGBA8888 紧凑排布读(与 captureCrop / ocrImage / stitchAppend 同口径)。
    // ⚠️ convertToFormat 在原图已是 RGBA8888 时**返回同一份数据**(隐式共享,不拷贝),
    //    故这里不额外付一份整帧;真正的一次拷贝在核心侧 bytes.to_vec()。
    const QImage src = image.convertToFormat(QImage::Format_RGBA8888);
    const QByteArray srcUtf8 = source.toUtf8();
    const QByteArray ownerUtf8 = owner.toUtf8();
    char* out = nullptr;
    int rc;
    if (m_ingestCaptureRgba2) {
        // v2:带来源应用(docs/25)
        rc = m_ingestCaptureRgba2(
            src.constBits(),
            static_cast<unsigned long long>(src.sizeInBytes()),
            static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()),
            srcUtf8.constData(),
            owner.isEmpty() ? nullptr : ownerUtf8.constData(), &out);
    } else {
        // 旧核心:回退 v1(无来源应用)
        if (!m_ingestCaptureRgba) return {};
        rc = m_ingestCaptureRgba(
            src.constBits(),
            static_cast<unsigned long long>(src.sizeInBytes()),
            static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()),
            srcUtf8.constData(), &out);
    }
    if (rc != 0 || !out) {
        if (out) m_freeString(out);
        return {};
    }
    const QByteArray owned(out);
    m_freeString(out);
    // ⚠️ 返回体是**裸** {"entry": …}(core/nbcore/src/ffi.rs 的 nb_ingest_capture_rgba
    //    直接 report_out_text(json!({"entry": stored}))),**没有** nb_call 那层
    //    {"ok":…,"data":…} 信封 —— 与 nb_ocr_rgba 同一口径(那里曾因照抄信封解析,
    //    导致"核心识别成功、宿主永远取到空串")。
    return QJsonDocument::fromJson(owned).object();
}

bool CoreBridge::copyRgbaToClipboard(const QImage& image) {
    if (!m_copyRgbaToClipboard || image.isNull()) return false;
    const QImage src = image.convertToFormat(QImage::Format_RGBA8888);
    const int rc = m_copyRgbaToClipboard(
        src.constBits(),
        static_cast<unsigned long long>(src.sizeInBytes()),
        static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()));
    return rc == 0;
}

QString CoreBridge::ocrImage(const QImage& image) {
    return ocrImageDetailed(image).text;
}

OcrTextWithLines CoreBridge::ocrImageDetailed(const QImage& image) {
    OcrTextWithLines out;
    if (!m_ocrRgba || !m_freeString || image.isNull()) return out;
    // 核心按 RGBA8888 紧凑排布读取(与 captureCrop 同口径)
    const QImage src = image.convertToFormat(QImage::Format_RGBA8888);
    char* text = nullptr;
    const int rc = m_ocrRgba(
        src.constBits(),
        static_cast<unsigned long long>(src.sizeInBytes()),
        static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()),
        &text);
    if (rc != 0 || !text) {
        if (text) m_freeString(text);
        return out;
    }
    // nb_call 同款生命周期:由此接管并立刻拷出(解析 JSON 本来就要这份内容),
    // 随即归还核心缓冲。
    const QByteArray owned(text);
    m_freeString(text);
    // ⚠️ 返回体是**裸** {"text": …, "lines": […]}(core/nbcore/src/ffi.rs 的
    //    nb_ocr_rgba 直接 report_out_text),**没有** nb_call 那层 {ok,data} 信封。
    //    这里曾照抄 nb_call 的解析写成 resp["data"]["text"] —— data 不存在,
    //    于是**每次 OCR 都"识别成功但取到空串"**:核心日志里明明有
    //    "RapidOCR 完成:…,62 行",用户却什么都拿不到。
    const QJsonObject resp = QJsonDocument::fromJson(owned).object();
    out.text = resp.value("text").toString();
    if (out.text.isEmpty() && resp.contains("data")) {
        // 兼容带信封的形态(核心若统一成 nb_call 风格则无需改宿主)
        out.text = resp.value("data").toObject().value("text").toString();
    }
    if (out.text.isEmpty() && !resp.isEmpty()) {
        // 自诊断:有返回体却取不到文本 = 契约变了,别静默当"没识别到"
        qWarning() << "OCR 返回体没有 text 字段,契约可能已变:" << owned.left(200);
    }
    // 行框:可选字段(旧 nbcore.dll 没有 → 空数组,取字降级为纯文本面板)。
    // 单条无效(空文本/空框)直接丢弃:错位或空框画出来的高亮比没有框更误导。
    const QJsonArray lines = resp.value("lines").toArray();
    for (const QJsonValue& v : lines) {
        const QJsonObject o = v.toObject();
        OcrLineBox line;
        line.text = o.value("text").toString();
        line.rect = QRect(o.value("x").toInt(), o.value("y").toInt(),
                          o.value("width").toInt(), o.value("height").toInt());
        if (!line.text.isEmpty() && !line.rect.isEmpty()) out.lines.push_back(line);
    }
    if (!lines.isEmpty() && out.lines.isEmpty()) {
        qWarning() << "OCR lines 数组存在但全部无效,行框已丢弃:" << owned.left(200);
    }
    return out;
}

QrDecodeResult CoreBridge::qrDecodeRgba(const QImage& image) {
    QrDecodeResult out;
    // 旧 nbcore.dll 没有此导出:与"图中没有码"同一形态返回(功能缺失≠崩溃)
    if (!m_qrDecodeRgba || !m_freeString || image.isNull()) return out;
    const QImage src = image.convertToFormat(QImage::Format_RGBA8888);
    char* body = nullptr;
    const int rc = m_qrDecodeRgba(
        src.constBits(),
        static_cast<unsigned long long>(src.sizeInBytes()),
        static_cast<unsigned>(src.width()), static_cast<unsigned>(src.height()),
        &body);
    if (rc != 0 || !body) {
        if (body) m_freeString(body);
        return out;
    }
    // nb_call 同款生命周期:接管即拷,随即归还核心缓冲
    const QByteArray owned(body);
    m_freeString(body);
    // ⚠️ 返回体是**裸** {"found":…,"text":…}(与 nb_ocr_rgba 同口径,无信封;
    //    那条"照抄信封解析取 data"的历史教训在这里同样适用)。
    const QJsonObject resp = QJsonDocument::fromJson(owned).object();
    out.found = resp.value("found").toBool();
    out.text = resp.value("text").toString();
    if (!resp.contains("found")) {
        qWarning() << "二维码返回体没有 found 字段,契约可能已变:" << owned.left(200);
    }
    return out;
}

bool CoreBridge::copyTextToClipboard(const QString& text) {
    if (text.isEmpty()) return false;
    // 走核心的 copy_text_to_clipboard(而非 QGuiApplication::clipboard()->setText):
    // 核心的 clipboard_win 带**打开重试**(别的进程短暂占用剪贴板是常态,实测日志里
    // 就有 "Retrying to obtain clipboard" → "Unable to obtain clipboard"),
    // 宿主直接写 Qt 剪贴板在这种时刻会静默失败 —— 用户看到"取字了却粘不出来"。
    const QJsonObject resp = call("copy_text_to_clipboard", {{"text", text}});
    if (!resp.value("ok").toBool()) {
        qWarning() << "OCR 文本写剪贴板失败:"
                   << resp.value("error").toObject().value("message").toString();
        return false;
    }
    return true;
}

QImage CoreBridge::loadEntryImage(const QString& blobPath) {
    if (blobPath.isEmpty()) return {};
    // blob 存的是相对 blobs 目录的路径;历史数据可能是绝对路径,两种都兼容
    const QString abs = QFileInfo(blobPath).isRelative() ? blobsDirAbs() + '/' + blobPath
                                                         : blobPath;
    QImage image;
    if (!image.load(abs)) {
        qWarning() << "加载条目图片失败:" << abs;
        return {};
    }
    return image;
}

} // namespace nb
