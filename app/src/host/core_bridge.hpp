// nbcore.dll 的 C++ 薄封装:加载、JSON 调用、生命周期。
#pragma once

#include <QtGlobal>

#include <windows.h>

#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QImage>
#include <QRect>

using NbInit = int(__cdecl*)(const char*);
using NbCallFn = int(__cdecl*)(const char*, const char*, char**);
using NbFreeString = void(__cdecl*)(char*);
using NbFreeBuffer = void(__cdecl*)(unsigned char*, unsigned long long);
using NbAbiVersion = unsigned(__cdecl*)();
using NbVersion = unsigned(__cdecl*)();
using NbShutdown = void(__cdecl*)();
using NbCaptureMonitor = int(__cdecl*)(unsigned, unsigned char**, unsigned*, unsigned*);
using NbCaptureCrop = int(__cdecl*)(const unsigned char*, unsigned long long,
                                    unsigned, unsigned, int, int, unsigned, unsigned,
                                    unsigned char**, unsigned*, unsigned*);

namespace nb {

// nbcore 通知消息(与 core/nbcore/src/core.rs 对应)
constexpr UINT WM_APP_CLIP_CHANGED = 0x8000 + 1;
constexpr UINT WM_APP_SETTINGS_CHANGED = 0x8000 + 2;

/// nbcore.dll 加载与调用。进程内单份;失败时 ok()=false 且 lastError 有原因。
class CoreBridge {
public:
    CoreBridge() = default;
    ~CoreBridge() { shutdown(); }
    CoreBridge(const CoreBridge&) = delete;
    CoreBridge& operator=(const CoreBridge&) = delete;

    bool init(bool startHidden, QString* error);
    void shutdown();

    /// 更新 nbcore 的通知 HWND(主面板窗口创建后调用;此前通知被丢弃)。
    void setNotifyHwnd(HWND hwnd);

    bool ok() const { return m_callFn != nullptr; }

    /// JSON 门面调用。返回响应对象 { ok, data|error };transport 失败时返回含 error 的对象。
    QJsonObject call(const QString& method, const QJsonObject& req = {});

    /// RGBA 通道:捕获显示器整屏。失败返回空图。
    QImage captureMonitor(unsigned index);

    /// RGBA 通道:从整屏裁剪子区域(框选确认)。`rect` 为**物理像素**且相对该屏左上角
    /// (越界部分由核心钳制)。失败返回空图。
    QImage captureCrop(const QImage& screen, const QRect& rect);

    /// 按条目的 blob 路径加载图片(相对 blobs 目录,兼容历史绝对路径)。失败返回空图。
    static QImage loadEntryImage(const QString& blobPath);

    /// blobs 目录绝对路径(图片条目磁盘位置;主面板经 file:// 加载)。
    static QString blobsDirAbs();

    /// nbcore 版本(形如 "0.1.0";未加载时返回空串)。设置页「关于」显示用。
    QString coreVersion() const;

    static CoreBridge& instance();

private:
    HMODULE m_dll = nullptr;
    NbInit m_init = nullptr;
    NbCallFn m_callFn = nullptr;
    NbFreeString m_freeString = nullptr;
    NbFreeBuffer m_freeBuffer = nullptr;
    NbAbiVersion m_abi = nullptr;
    NbVersion m_version = nullptr;
    NbShutdown m_shutdown = nullptr;
    NbCaptureMonitor m_captureMonitor = nullptr;
    NbCaptureCrop m_captureCrop = nullptr;
    bool m_initialized = false;
    QString m_dataDir;
    /// 生效数据目录(设置里启用自定义目录时与 m_dataDir 不同;blobs 定位用)。
    QString m_effectiveDataDir;
};

} // namespace nb
