// nbcore.dll 的 C++ 薄封装:加载、JSON 调用、生命周期。
#pragma once

#include <QtGlobal>

#include <windows.h>

#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QImage>
#include <QRect>

class QScreen;

using NbInit = int(__cdecl*)(const char*);
using NbCallFn = int(__cdecl*)(const char*, const char*, char**);
using NbFreeString = void(__cdecl*)(char*);
using NbFreeBuffer = void(__cdecl*)(unsigned char*, unsigned long long);
using NbAbiVersion = unsigned(__cdecl*)();
using NbVersion = unsigned(__cdecl*)();
using NbShutdown = void(__cdecl*)();
using NbCaptureMonitor = int(__cdecl*)(unsigned, unsigned char**, unsigned*, unsigned*);
/// 按宿主屏幕描述配对捕获(多屏遮罩的正确入口,见 capture.rs 的 match_monitor)。
using NbCaptureMonitorMatched = int(__cdecl*)(const char*, int, int, unsigned, unsigned, float,
                                              unsigned char**, unsigned*, unsigned*);
using NbCaptureCrop = int(__cdecl*)(const unsigned char*, unsigned long long,
                                    unsigned, unsigned, int, int, unsigned, unsigned,
                                    unsigned char**, unsigned*, unsigned*);
/// OCR(RGBA 直入):出参为 JSON 字符串,宿主以 nb_free_string 释放。
using NbOcrRgba = int(__cdecl*)(const unsigned char*, unsigned long long,
                                unsigned, unsigned, char**);
/// 长截图开始会话(固化显示器/选区/上限):失败时 out_err 带**可读错误文案**
/// (宿主以 nb_free_string 释放;成功时不写出参)。
using NbStitchBegin = int(__cdecl*)(const char*, const char*, int, int, unsigned, unsigned,
                                    float, long long, long long, long long, char**);
/// 长截图吸收一帧(RGBA 直入,不走 base64):出参为小 JSON(位移/总高/结论/判定数值)。
using NbStitchAppend = int(__cdecl*)(const char*, const unsigned char*, unsigned long long,
                                     unsigned, unsigned, char**);
/// 长截图取回结果:out_png 走 nb_free_buffer,out_meta 走 nb_free_string。
using NbStitchFinish = int(__cdecl*)(const char*, unsigned char**, unsigned long long*, char**);
/// 长截图丢弃会话(幂等)。
using NbStitchAbort = int(__cdecl*)(const char*);
/// 按屏幕描述**直接捕获显示器内的一个矩形**(设备像素,相对该屏左上角)。
/// 长截图逐帧采集走这条:免掉"整屏捕获 + 裁剪 + 格式转换"的三次大拷贝。
using NbCaptureMonitorRegionMatched = int(__cdecl*)(const char*, int, int, unsigned, unsigned,
                                                   float, unsigned, unsigned, unsigned, unsigned,
                                                   unsigned char**, unsigned*, unsigned*);
/// 截图入库(RGBA 直入,不走 base64/PNG):出参为**裸**小 JSON {"entry": …|null}。
using NbIngestCaptureRgba = int(__cdecl*)(const unsigned char*, unsigned long long,
                                          unsigned, unsigned, const char*, char**);
/// 把宿主内存里的 RGBA 直接写进系统剪贴板(CF_DIBV5,不经 PNG/base64)。
using NbCopyRgbaToClipboard = int(__cdecl*)(const unsigned char*, unsigned long long,
                                            unsigned, unsigned);

namespace nb {

// nbcore 通知消息(与 core/nbcore/src/core.rs 对应)
constexpr UINT WM_APP_CLIP_CHANGED = 0x8000 + 1;
constexpr UINT WM_APP_SETTINGS_CHANGED = 0x8000 + 2;
/// 待办到点(remind/notify):只当信号,宿主收到后 `call("todo_due_pending")`
/// 拉取待播报列表(docs/05-todo.md §4.2;⚠️ 常量必须与 core 侧同步)。
/// 取 +4 而非 +3:+3 为 CAPTURE_READY 预留。
constexpr UINT WM_APP_TODO_DUE = 0x8000 + 4;

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

    /// RGBA 通道:捕获显示器整屏(按 xcap 枚举序号)。
    /// ⚠️ 多屏场景**不要**用本入口 —— 序号是 xcap 的顺序,与
    ///    QGuiApplication::screens() 的顺序不同(实测主屏在其末位),
    ///    按序号配对会把别屏的底图画到本屏遮罩上。请用 captureMonitorMatched。
    QImage captureMonitor(unsigned index);

    /// RGBA 通道:**按宿主屏幕描述**配对捕获整屏(多屏遮罩/全屏直通的正确入口)。
    /// 传 QScreen 的 name/几何/DPR,由核心按"设备名 → 物理原点 → 物理尺寸"
    /// 逐级配对(见 core capture.rs 的 match_monitor)。配对失败返回空图。
    QImage captureMonitorMatched(QScreen* screen);

    /// 显示器列表(JSON:name/x/y/width/height/scaleFactor/isPrimary)。
    /// 几何为**物理像素**;仅供诊断与日志,配对由核心内部完成。
    QJsonArray listMonitors();

    /// 核心是否提供"按屏描述配对捕获"(旧 DLL 没有该导出)。
    /// 宿主据此决定走配对路径还是回退按索引。
    bool hasMonitorMatching() const { return m_captureMonitorMatched != nullptr; }

    /// RGBA 通道:从整屏裁剪子区域(框选确认)。`rect` 为**物理像素**且相对该屏左上角
    /// (越界部分由核心钳制)。失败返回空图。
    QImage captureCrop(const QImage& screen, const QRect& rect);

    // ---- 滚动长截图(docs/19) ----
    //
    // ⚠️ 这三个走**原生 RGBA 通道**,不是 `call("stitch_*")`:
    //    逐帧是一整张选区图(1080p ~8MB),走 JSON 门面要 base64(膨胀 1.33 倍)+
    //    核心侧解回来,一次三份峰值拷贝。docs/06 §10 已把"截图 base64 往返"
    //    列为待改项头一条,长截图更是高频路径。

    /// 核心是否提供长截图导出(旧 nbcore.dll 没有)。
    bool hasStitch() const {
        return m_stitchBegin && m_stitchAppend && m_stitchFinish && m_stitchAbort;
    }

    /// 核心是否提供"直抓选区"(旧 DLL 只能整屏 + 裁剪)。
    bool hasRegionCapture() const { return m_captureMonitorRegion != nullptr; }

    /// 开始长截图会话:把「哪块屏、哪个矩形(设备像素)、什么上限」固化在核心侧。
    ///
    /// @param screen        采集所属屏幕(用它做配对,配对失败**硬失败**)。
    /// @param regionDevice  选区在该屏内的**设备像素**矩形(相对屏左上角)。
    /// @param maxHeight     像素高度上限(设置项 stitchMaxHeight)。
    /// @param maxBytes      字节预算(设置项 stitchMaxBytes;0 = 不限制)。
    /// @param overlapRatio  重叠门槛百分比(设置项 stitchOverlapRatio)。
    /// @param error         失败时回填**可读原因**(核心给的原文,可直接提示用户)。
    /// @return 是否成功。
    ///
    /// 为什么把 error 做成出参而不是让宿主去读 nbcore.log:begin 失败意味着
    /// "这个屏根本采不了"(配对不上/选区退化),用户需要当场看到原因
    /// (docs/19 v2:静默降级是这类问题的头号来源)。
    bool stitchBegin(const QString& session, QScreen* screen, const QRect& regionDevice,
                     int maxHeight, qint64 maxBytes, int overlapRatio, QString* error);

    /// 吸收一帧(尺寸必须与 begin 时约定的选区一致,否则核心报错)。
    /// 返回核心给出的小 JSON(含 matched/shiftPx/totalHeight/reason/gaps/bands/debug);
    /// 失败返回空对象。
    QJsonObject stitchAppend(const QString& session, const QImage& frame);

    /// 按屏幕描述**直抓选区**(设备像素,相对屏左上角)。
    /// 配对失败/越界返回空图(原因见 lastError()),**不回退按索引取图**。
    QImage captureMonitorRegion(QScreen* screen, const QRect& regionDevice);

    /// 结束并取回拼接结果。`ok` 为 false 时 outMeta 亦为空。
    /// 成功时 *outPng 为独立 QImage(核心缓冲已在内部转为 PNG 解码后的自有数据,
    /// 无需额外归还),*outMeta 为小 JSON。
    bool stitchFinish(const QString& session, QImage* outPng, QJsonObject* outMeta);

    /// 丢弃会话(幂等)。
    void stitchAbort(const QString& session);

    // ---- 截图入库(RGBA 直入;docs/06 §5.4 的"最后一项")----
    //
    // ⚠️ 这两条走**原生 RGBA 通道**,不是 call("ingest_capture_png") /
    //    call("copy_image_png"):原路径是 PNG 编码 → base64(1.33× QString) →
    //    JSON → 核心 base64 解码 → PNG 解码 → to_rgba8 → Vec,一张 1500×950 截图
    //    实测瞬时峰值 299.8MB。改后只剩一次整帧拷贝。

    /// 核心是否提供 RGBA 直入入库(旧 nbcore.dll 没有)。
    bool hasRgbaIngest() const { return m_ingestCaptureRgba != nullptr; }

    /// 截图入库(RGBA 直入)。返回**裸**响应对象:{"entry": {...}} 或 {"entry": null}
    /// (后者 = 超过 maxImageMb 未入库,调用方仍应写剪贴板)。
    ///
    /// 失败返回空对象(原因见 lastError()/nbcore.log,与既有 captureCrop 同口径)。
    /// ⚠️ 阻塞调用(PNG 编码 + 缩略图 + 落盘):大图几十毫秒到几百毫秒,
    ///    **别在 UI 线程高频调用**(截图链路本就是用户动作触发的低频路径)。
    QJsonObject ingestCaptureRgba(const QImage& image, const QString& source);

    /// 把该图直接写进系统剪贴板(不经 PNG/base64)。返回是否成功。
    bool copyRgbaToClipboard(const QImage& image);

    /// 对任意 QImage 直接 OCR(编辑器「取字」):不走落库通道,设置项全生效。
    /// ⚠️ 阻塞调用 —— det 推理 0.4~5s,**必须在非 UI 线程调用**。
    /// 失败/不可用时返回空串(错误文本见 lastError())。
    QString ocrImage(const QImage& image);

    /// 把文本写进系统剪贴板(经核心,与其它复制路径同一通道 —— 核心的
    /// clipboard_win 带打开重试,宿主直接用 Qt 剪贴板在别的进程占用时会静默失败)。
    /// 返回是否写入成功。**「OCR 取字」的落点就是这里**:识别结果必须落到剪贴板,
    /// 否则用户"取了字"却粘不出任何东西。
    bool copyTextToClipboard(const QString& text);

    /// 按条目的 blob 路径加载图片(相对 blobs 目录,兼容历史绝对路径)。失败返回空图。
    static QImage loadEntryImage(const QString& blobPath);

    /// blobs 目录绝对路径(图片条目磁盘位置;主面板经 file:// 加载)。
    static QString blobsDirAbs();

    /// 由原图**绝对路径**推导列表缩略图路径(带尺寸版本号,同目录)。
    ///
    /// ⚠️ 命名规则必须与 core 的 `clip::thumbnail_path()` 完全一致:
    /// 版本号(`nbThumbVersion`)参与文件名 —— 调大缩略图边长后,旧缓存自动失效、
    /// 后台回填按新版本重建,**无需手动清缓存**(见 docs/progress.md 缩略图条目)。
    /// 例:`.../2026-10/abc.png` → `.../2026-10/abc.thumb640.png`。
    static QString thumbnailPathFor(const QString& absoluteBlobPath);

    /// 缩略图文件名版本号(与 core 的 `clip::THUMB_MAX_EDGE` 对齐)。改它 = 弃用旧缓存。
    static constexpr int nbThumbVersion = 640;

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
    NbCaptureMonitorMatched m_captureMonitorMatched = nullptr;
    NbCaptureCrop m_captureCrop = nullptr;
    NbCaptureMonitorRegionMatched m_captureMonitorRegion = nullptr;
    NbOcrRgba m_ocrRgba = nullptr;
    NbStitchBegin m_stitchBegin = nullptr;
    NbStitchAppend m_stitchAppend = nullptr;
    NbStitchFinish m_stitchFinish = nullptr;
    NbStitchAbort m_stitchAbort = nullptr;
    NbIngestCaptureRgba m_ingestCaptureRgba = nullptr;
    NbCopyRgbaToClipboard m_copyRgbaToClipboard = nullptr;
    bool m_initialized = false;
    QString m_dataDir;
    /// 生效数据目录(设置里启用自定义目录时与 m_dataDir 不同;blobs 定位用)。
    QString m_effectiveDataDir;
};

} // namespace nb
