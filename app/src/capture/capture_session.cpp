#include "capture_session.hpp"

#include "note/note_registry.hpp"
#include "capture/overlay_geometry.hpp"
#include "editor/annotation_toolbar.hpp"
#include "host/core_bridge.hpp"
#include "host/image_save.hpp"
#include "panel/core_facade.hpp"
#include "host/memory_probe.hpp"

#include <QBuffer>
#include <QCursor>
#include <QFont>
#include <QGuiApplication>
#include <QIODevice>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QUndoStack>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QEvent>
#include <QLineEdit>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QDateTime>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>
#include <QUuid>

#include <windows.h>

namespace nb {

namespace {
// 当前会话的全部遮罩(多屏各一块);用 QPointer 防关闭后悬空。
// "会话进行中"直接以它为准(见 pruneOverlays),不再单独维护布尔标记——
// 遮罩若被外部销毁(Alt+F4 / WM 关闭),下一次触发会自动解除忙碌,不会卡死。
QList<QPointer<OverlayWidget>> g_overlays;

// 本次会话的入库后回调(截图翻译/取字接续 OCR 用);会话结束即清空。
CaptureDone g_onCaptured;

/// 剔除已销毁的遮罩;返回是否仍有活跃遮罩(即会话进行中)。
bool pruneOverlays() {
    g_overlays.removeIf([](const QPointer<OverlayWidget>& p) { return p.isNull(); });
    return !g_overlays.isEmpty();
}

/// 会话空闲超时计时器(S6.1)。一次会话只有一组遮罩,共用同一个计时器:
/// 任一遮罩上报 activity 就重置;超时则自动取消会话。
QTimer* g_idleTimer = nullptr;

// ---- 滚动长截图(docs/19)的会话状态 ----
//
// 一次会话只有一个长截图会话 id(选区一旦锁定就只有一块遮罩在采)。放全局是
// 因为动作发生在遮罩的 lambda 里,而 id 必须跨帧存活;会话结束(含超时/取消)
// 统一在 closeAllOverlays 里 abort,避免核心侧缓冲泄漏。
QString g_stitchSession;
/// 该遮罩所属屏的索引(采集时按它回退 captureMonitor)。
QHash<const OverlayWidget*, int> g_stitchScreenOf;

/// 开始(或重置)长截图会话:分配新 id,复位进度。
QString ensureStitchSession() {
    if (g_stitchSession.isEmpty())
        g_stitchSession = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return g_stitchSession;
}

/// 丢弃长截图会话(核心侧缓冲一并释放)。幂等。
void abortStitchSession() {
    if (g_stitchSession.isEmpty()) return;
    CoreBridge::instance().stitchAbort(g_stitchSession);
    g_stitchSession.clear();
    g_stitchScreenOf.clear();
}

/// 在 `obj` 真正析构之后再执行 `fn`。
///
/// 不能用"下一轮事件循环"来等:deleteLater 与零定时器谁先跑并不确定,而窗口占的那份
/// 内存(软件渲染下的后备缓冲 ≈ 窗口尺寸×4)必须等对象真的析构才归还。`destroyed`
/// 是唯一可靠的时点。`fn` 再经一次 singleShot(0) 转出去,避免在半死对象的析构链上
/// 跑编码/写库这类重活。
void runAfterDestroyed(QObject* obj, std::function<void()> fn) {
    QObject::connect(obj, &QObject::destroyed, obj,
                     [fn](QObject*) { QTimer::singleShot(0, qApp, fn); },
                     Qt::DirectConnection);
}

/// 收起本次会话的全部遮罩。
///
/// 先把列表清空再逐个 close():close() 触发 WA_DeleteOnClose → deleteLater(),
/// 因此关闭**当前发信的那块遮罩**也是安全的(延迟删除,不在信号发射栈上析构),
/// 且重入调用会看到空列表而直接返回。
///
/// `after` 非空时在**全部遮罩真正析构之后**执行。要它是因为:close() 返回时三块整屏
/// 底图与各窗口的后备缓冲仍在场(实测会话 +77.5MB),而"确认后入库"原先就挂在同一个
/// 信号栈上同步跑完(PNG 编码 + 入库 + 写剪贴板),于是入库全程都在为已经用不到的遮罩
/// 买内存(实测框选链路工作集峰值 274MB)。
void closeAllOverlays(std::function<void()> after = {}) {
    const QList<QPointer<OverlayWidget>> overlays = g_overlays;
    g_overlays.clear();
    g_onCaptured = {};
    // 长截图会话核在核心侧持有整张结果图,会话结束必须显式丢弃 ——
    // 否则用户中途 Esc / 空闲超时后,那几百 MB 会一直挂在核心的表里。
    abortStitchSession();
    // 会话结束即停表:遮罩已释放,不再需要空闲保护
    if (g_idleTimer) g_idleTimer->stop();
    if (overlays.isEmpty()) {
        // 没有遮罩也要恢复便签(某些取消路径可能已经收掉了遮罩):这是幂等的 ——
        // 没隐藏过就什么都不做。
        NoteRegistry::instance().restoreAfterCapture();
        if (after) QTimer::singleShot(0, qApp, std::move(after));
        return;
    }

    // 内存读数必须等**最后一块遮罩真正销毁**之后再取:close() 只是 deleteLater,
    // 此刻整屏底图还没回收(实测会读成"没释放",与下一次 start 的读数自相矛盾)。
    // 又因为本函数可能正处在某块遮罩的信号栈上(regionConfirmed),不能强制跑
    // deleteLater —— 那会在其事件处理中途析构它。故用 destroyed 信号计数。
    auto* pending = new int(overlays.size());
    const auto countdown = [pending, after] {
        if (--*pending == 0) {
            delete pending;
            // 遮罩全部销毁 = 截图会话真正结束:此刻放回便签(docs/20 §7.4)。
            // 单一恢复点,不逐出口补 show() —— 那一定漏(unparkPinAfterEdit 的教训)。
            NoteRegistry::instance().restoreAfterCapture();
            logMemory("capture-end");
            // 到这一刻遮罩已全部析构、缓冲已归还;转一次事件循环只为离开析构栈。
            if (after) QTimer::singleShot(0, qApp, after);
        }
    };
    for (const QPointer<OverlayWidget>& overlay : overlays) {
        if (!overlay) {
            countdown();   // 已销毁的不会再发 destroyed,直接记账
            continue;
        }
        QObject::connect(overlay, &QObject::destroyed, overlay, countdown);
        overlay->close();
    }
}

QString quickPngBase64(const QImage& image) {
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QString::fromLatin1(buffer.data().toBase64());
}

/// 延时截图的倒计时提示:置顶无边框、**鼠标穿透**(不挡用户去点菜单),屏幕中央显示剩余秒数。
/// 只服务延时截图链路;倒计时结束自行关闭(调用方也会兜底关闭)。
class CountdownOverlay : public QWidget {
public:
    explicit CountdownOverlay(int seconds)
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                              Qt::WindowTransparentForInput) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);   // 不抢焦点:用户还要去展开菜单
        setAttribute(Qt::WA_DeleteOnClose);
        resize(140, 140);
        m_remaining = seconds;
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) move(screen->availableGeometry().center() - rect().center());
        m_timer = new QTimer(this);
        m_timer->setInterval(1000);
        connect(m_timer, &QTimer::timeout, this, [this] {
            if (--m_remaining <= 0) {
                close();
                return;
            }
            update();
        });
        m_timer->start();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        // 后备缓冲/窗口尚未就绪时 begin() 会失败(engine == 0):此时任何 draw*
        // 都是空操作并刷出 "QPainter::begin: Paint device returned engine == 0"
        // + "Painter not active" 一屏错误。Qt 会自行重发 paint,丢掉这一帧即可。
        if (!painter.isActive()) return;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 255, 255, 70), 2));
        painter.setBrush(QColor(20, 22, 28, 205));
        painter.drawEllipse(rect().adjusted(4, 4, -4, -4));
        QFont font = this->font();
        font.setPixelSize(60);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor(240, 244, 250));
        painter.drawText(rect(), Qt::AlignCenter, QString::number(m_remaining));
    }

private:
    int m_remaining = 0;
    QTimer* m_timer = nullptr;
};

} // namespace

/// 入库 + 写回系统剪贴板,返回条目 id(未入库或失败返回空串)。
///
/// 顺序遵循第一代 docs/phase5.md §3.6:**先入库、后写剪贴板**——剪贴板监听线程
/// 读到同一张图时按 hash 命中去重,只 touch 不新建,不会产生重复条目。
QString ingestImageAndCopy(const QImage& image, bool* outOversize, QString* outNotice) {
    if (outOversize) *outOversize = false;
    if (outNotice) outNotice->clear();
    CoreBridge& core = CoreBridge::instance();
    const bool direct = core.hasRgbaIngest();
    // RGBA 直入(docs/06 §5.4 最后一项):宿主不再 PNG 编码 + base64(1.33× QString)
    // + JSON,核心也不再 base64/PNG 解码 —— 一张 1500×950 截图的瞬时峰值
    // 从实测 299.8MB 降到"一次整帧拷贝"。旧核心(无该导出)走原 PNG 通道回退。
    QString pngBase64;
    QJsonObject resp;
    if (direct) {
        resp = core.ingestCaptureRgba(image, QStringLiteral("capture"));
    } else {
        pngBase64 = quickPngBase64(image);
        resp = core.call("ingest_capture_png", {
            {"png_base64", pngBase64},
            {"source", "capture"},
        });
        if (!resp.value("ok").toBool()) {
            const QString err = resp.value("error").toObject().value("message").toString();
            qWarning() << "截图入库失败:" << err;
            if (outNotice) *outNotice = QStringLiteral("入库失败:%1").arg(err);
            return {};
        }
        resp = resp.value("data").toObject();
    }
    // 原生通道以返回码表达失败(出参为空对象,原因见 nbcore.log);JSON 通道的
    // 失败已在上面按 ok 判过。空响应**必须**在这里拦住:否则会落到下面
    // "id 为空 = 超大小上限"的分支,把一次真实失败说成"超过上限"并写剪贴板。
    if (direct && resp.isEmpty()) {
        qWarning() << "截图入库失败(RGBA 直入):核心返回空";
        if (outNotice) *outNotice = QStringLiteral("入库失败:核心未返回结果");
        return {};
    }
    // entry = null / 缺 id = 超过图片大小上限被忽略
    const QString id = resp.value("entry").toObject().value("id").toString();
    if (id.isEmpty()) {
        // 未入库,但**仍要写剪贴板**:否则用户按了截图热键却"毫无反应"
        // (第一代同语义:超限时"未入库,仅写剪贴板")。
        //
        // ⚠️ 但**必须把"没入库"告诉调用方**:截图(尤其长截图)撞上限时,
        //    用户会以为存下来了,过后在列表里找不到 —— 静默丢数据比报错更糟。
        if (outOversize) *outOversize = true;
        const QJsonObject settings = core.call("get_settings");
        const int limitMb = settings.value("ok").toBool()
            ? settings.value("data").toObject().value("maxImageMb").toInt()
            : 0;
        const double actualMb = image.sizeInBytes() / (1024.0 * 1024.0);
        const QString notice = limitMb > 0
            ? QStringLiteral("图片超过 %1MB 上限,未存入历史(已复制到剪贴板)").arg(limitMb)
            : QStringLiteral("图片超过大小上限,未存入历史(已复制到剪贴板)");
        qInfo().nospace() << "截图超过大小上限(" << limitMb << "MB),未入库;仅写剪贴板。"
                          << " 本次约 " << QString::number(actualMb, 'f', 1) << "MB(RGBA)";
        if (outNotice) *outNotice = notice;
        // 未入库但仍要写剪贴板:直入通道直接用**手上这块像素**(不再 PNG 编码 +
        // base64 往返);旧核心回退 copy_image_png。
        if (direct) {
            if (!core.copyRgbaToClipboard(image))
                qWarning() << "超限截图写剪贴板失败(RGBA 直入)";
        } else {
            const QJsonObject copied = core.call("copy_image_png", {{"png_base64", pngBase64}});
            if (!copied.value("ok").toBool())
                qWarning() << "超限截图写剪贴板失败:"
                           << copied.value("error").toObject().value("message").toString();
        }
        return {};
    }
    // 已入库 → 写剪贴板。直入通道**不读回 blob PNG**:同一块像素直接写 DIB,
    // 省掉"读文件 + PNG 解码 + to_rgba8"(4K 图又是一份整帧)。
    if (direct) {
        if (!core.copyRgbaToClipboard(image))
            qWarning() << "截图写剪贴板失败(RGBA 直入);已入库,id =" << id;
        else
            qInfo() << "截图已入库并写入剪贴板(RGBA 直入):"
                    << image.width() << "x" << image.height();
        return id;
    }
    const QJsonObject copied = core.call("copy_entry", {{"id", id}});
    if (!copied.value("ok").toBool()) {
        // 已入库但写剪贴板失败:仍把 id 交给后续链路(翻译/取字仍可进行)
        qWarning() << "截图写剪贴板失败:"
                   << copied.value("error").toObject().value("message").toString();
        return id;
    }
    qInfo() << "截图已入库并写入剪贴板:" << image.width() << "x" << image.height();
    return id;
}

namespace {

/// 入库 + 写剪贴板,成功后接续回调(截图翻译/取字)。
/// 编辑器是异步窗口,回调必须由调用方显式带过来,不能读会话级的 g_onCaptured。
void finishCapture(const QImage& image, const CaptureDone& done) {
    bool oversize = false;
    QString notice;
    const QString id = ingestImageAndCopy(image, &oversize, &notice);
    if (!id.isEmpty() && done) done(id);
    // 超限必须让用户看见(尤其长截图 —— 尺寸大,撞上限是常态而非意外)。
    // 走 CoreFacade 的 toast:截图会话没有自己的 UI 层,而面板已有统一的提示通道。
    if (oversize && !notice.isEmpty()) {
        CoreFacade::instance().showToast(notice);
    }
}

/// 框选之后去哪儿(§18.4):**单一出口**取代原先两个布尔量
/// (regionSkipEditor × regionAutoConfirm)的四种组合 —— 其中两种自相矛盾
/// (「松开即确认」与「进编辑器」不可能同时成立),且组合语义无法向用户解释。
///
/// ⚠️ 「进编辑器」这条出口**已下线**(§18.5):编辑器只保留唯一入口 = 右键编辑
/// (主面板条目右键 / 贴图右键)。框选链路的编辑能力由**浮条工具栏**承担。
/// 故这里只剩两种:
///   Direct  —— 松手即入库 + 写剪贴板(原 regionSkipEditor=true & autoConfirm=true)
///   Toolbar —— 选区保留、浮出工具栏,就地标注/OCR/贴图/保存/完成/取消
///              (原 regionSkipEditor=true & autoConfirm=false)
enum class RegionExit { Direct, Toolbar };

/// regionAutoConfirm(默认 true):松手即确认(Direct);false 则浮出工具栏(Toolbar)。
/// regionRightClickCancel(默认 true):框选时右键取消本次截图。
} // namespace

void startCaptureSession(CaptureIntent intent, CaptureDone onCaptured) {
    // 会话进行中 = 屏幕上还有遮罩(第一代 CAPTURE_BUSY 语义)
    if (pruneOverlays()) {
        qInfo() << "截图进行中,忽略本次触发";
        return;
    }
    g_onCaptured = onCaptured;
    // 桌面便签是置顶窗口,截图时不该被拍进画面(docs/20 §7.4)。隐藏是**产品约定**
    // 而非"当前后端恰好没暴露"的补偿:xcap 现在的 GDI BitBlt 不带 CAPTUREBLT,
    // 大概率本来就拍不到分层窗口,但换 DXGI/WGC 后端就会。代价只是两次调用。
    //
    // ⚠️ 位置必须在 pruneOverlays 的忙碌早退**之后**:放在前面的话,"截图进行中被
    //    忽略"的那次触发会顺手隐藏便签,却没人负责恢复。
    NoteRegistry::instance().hideAllForCapture();
    logMemory("capture-start");

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) return;

    // 找到光标所在屏(决策 D5)
    int cursorScreen = 0;
    const QPoint cursor = QCursor::pos();
    for (int i = 0; i < screens.size(); ++i) {
        if (screens[i]->geometry().contains(cursor)) {
            cursorScreen = i;
            break;
        }
    }

    CoreBridge& core = CoreBridge::instance();
    // ⚠️ 多屏配对必须由**核心按屏描述**完成,不能用索引 —— 这是
    //    "多屏位置/分辨率混乱"的根因:QGuiApplication::screens() 的顺序与
    //    核心侧(xcap EnumDisplayMonitors)的**顺序不同**(实测:Qt 把主屏排在
    //    最前,xcap 把它排在最后),按索引取图会把 A 屏的底图画到 B 屏遮罩上
    //    —— 表现为位置错位、分辨率也对不上。
    //    也不能用 QScreen::name():它返回的是 EDID 友好名("S24 PRO"),
    //    与核心的 `\\.\DISPLAYn` **不同源**(实测三屏全配不上)。
    //    可靠的键是**物理桌面原点**(两端一致)+ 物理尺寸兜底,逻辑在
    //    core 的 match_monitor(有单测覆盖实测机器布局)。
    //    回退策略:核心未导出配对接口(旧 DLL)时才退回按索引。
    const bool matched = core.hasMonitorMatching();
    auto frozenFor = [&core, matched](QScreen* screen, int index) -> QImage {
        if (matched) {
            const QImage img = core.captureMonitorMatched(screen);
            if (!img.isNull()) return img;
            qWarning() << "屏" << screen->name() << "(索引" << index
                       << ")配对捕获失败,回退按索引取图";
        }
        return core.captureMonitor(static_cast<unsigned>(index));
    };

    // 长截图意图:必须**有选区**才有采集窗口,故走遮罩框选(不能像全屏那样直通)。
    // 遮罩上"松手即进长截图就绪态",由 autoStartStitching 承担(见 mouseReleaseEvent)。
    const bool scrollingIntent = (intent == CaptureIntent::Scrolling);

    if (intent == CaptureIntent::Fullscreen) {
        // 直通:捕获 → 入库(source=capture)→ 写剪贴板,同步完成
        // (同样按名配对:光标屏的索引在两端含义不同)
        const QImage shot = frozenFor(screens[cursorScreen], cursorScreen);
        if (shot.isNull()) {
            qWarning() << "全屏捕获失败";
        } else {
            const CaptureDone done = g_onCaptured;
            g_onCaptured = {};
            finishCapture(shot, done);
            // 全屏直通**不经过** closeAllOverlays(:101),便签的恢复必须在这里补 ——
            // 漏了的表现是"按一次全屏截图热键,便签全部消失且再也回不来"。
            NoteRegistry::instance().restoreAfterCapture();
            logMemory("capture-end");   // 全屏直通没有遮罩,closeAllOverlays 不会走
        }
        return;
    }

    // 框选:每屏一块遮罩,各自拿本屏冻结底图。
    // 确认方式与右键取消都由设置决定,会话内只读一次。
    const QJsonObject settings = CoreBridge::instance().call("get_settings");
    const QJsonObject data =
        settings.value("ok").toBool() ? settings.value("data").toObject() : QJsonObject{};
    const bool autoConfirm = data.value("regionAutoConfirm").toBool(true);
    const bool rightClickCancel = data.value("regionRightClickCancel").toBool(true);
    // 空闲超时(S6.1):遮罩持有整屏冻结底图(4K 约 33MB),用户触发后离开会无限期占着。
    // 0 = 不自动取消;会话内任何鼠标/键盘动作都会重置计时。
    // 兜底值必须与 core 默认值一致(settings.rs 的 overlay_idle_secs = 60):
    // 这里原先写 600,一旦 get_settings 失败(核心未就绪/新旧 dll 错配)就会让遮罩
    // 白占整屏冻结底图十分钟 —— 实测遮罩会话常驻 +77.5MB。
    // 浮条底板不透明度(docs/17 §17.5):三档,兜底 217 必须与 core 默认
    // (settings.rs 的 capture_bar_alpha = 217)一致 —— 读不到设置时退化成
    // "半透",而不是硬编码的不透明。
    const int barAlpha = data.value("captureBarAlpha").toInt(OverlayWidget::kBarAlphaHalf);
    const int idleSecs = qBound(0, data.value("overlayIdleSecs").toInt(60), 3600);
    if (idleSecs > 0 && !g_idleTimer) {
        g_idleTimer = new QTimer(qApp);
        g_idleTimer->setSingleShot(true);
        QObject::connect(g_idleTimer, &QTimer::timeout, [] {
            qInfo() << "截图会话空闲超时,自动取消并释放遮罩";
            closeAllOverlays();
        });
    }
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* screen = screens[i];
        // 按**设备名**取本屏底图(见上方 byName 注释:索引在两端口径不同)
        const QImage frozen = frozenFor(screen, i);
        if (frozen.isNull()) {
            qWarning() << "屏" << screen->name() << "(索引" << i
                       << ")捕获失败,跳过该屏遮罩";
            continue;
        }
        // 长截图意图下 autoConfirm 只剩"点空是否取消"的作用:松手不再直接入库,
        // 否则设了 regionAutoConfirm=true 的用户永远进不了长截图(见构造函数注释)。
        auto* overlay = new OverlayWidget(frozen, i, screen, autoConfirm, rightClickCancel,
                                          scrollingIntent);
        overlay->setBarAlpha(barAlpha);   // 会话内三档可调,初始取设置
        overlay->setAttribute(Qt::WA_DeleteOnClose);
        g_overlays.append(overlay);

        // 任一屏上的动作都重置空闲计时(start() 无参 = 沿用当前 interval)
        if (idleSecs > 0) {
            QObject::connect(overlay, &OverlayWidget::activity, overlay, [] {
                if (g_idleTimer && g_idleTimer->isActive()) g_idleTimer->start();
            });
        }

        // 冻结底图已在宿主侧(QImage 隐式共享,lambda 捕获代价为 0);
        // 裁剪仍走核心 RGBA 通道 nb_capture_crop,保持"有逻辑的归 Rust"。
        QObject::connect(overlay, &OverlayWidget::regionConfirmed, overlay,
                [frozen](const QRect& physicalRect) {
            const CaptureDone done = g_onCaptured;   // closeAllOverlays 会清空,先取出
            // 任一屏确认即收起全部遮罩,并**等它们真正析构后**再裁剪/入库
            // (原因见 closeAllOverlays 的注释:否则整屏底图 + 后备缓冲会陪跑整条链)。
            closeAllOverlays([frozen, physicalRect, done] {
                const QImage cropped =
                    CoreBridge::instance().captureCrop(frozen, physicalRect);
                if (cropped.isNull()) {
                    qWarning() << "选区裁剪失败(空选区或越界)";
                    return;
                }
                // ⚠️ 框选确认后**一律直通**(入库 + 写剪贴板):「进编辑器」这条出口
                //    已下线(§18.5)—— 编辑器只保留唯一入口 = 右键编辑
                //    (主面板条目右键 / 贴图右键);框选链路要标注就走浮条工具栏的
                //    「标注」,要事后精修走右键「编辑」。
                finishCapture(cropped, done);
            });
        });
        QObject::connect(overlay, &OverlayWidget::cancelled, overlay, [] {
            closeAllOverlays();
            qInfo() << "截图已取消";
        });

                // 带标注的确认(浮条标注态):合成图直接入库+剪贴板,跳过裁剪。
                // closeAllOverlays 会清空 g_onCaptured,须先取出(与 regionConfirmed 同套路)。
                // ⚠️ composed 必须**按值**捕获:closeAllOverlays 的回调是延迟执行的
                //    (等遮罩真正析构后才跑),按引用捕获拿到的是已出栈的信号实参。
                QObject::connect(overlay, &OverlayWidget::annotatedConfirmed, overlay,
                        [](const QImage& composed) {
                    const CaptureDone done = g_onCaptured;
                    g_onCaptured = {};
                    closeAllOverlays([composed, done] {
                        finishCapture(composed, done);
                    });
                });

        // ---- 滚动长截图(docs/19)的会话级接线 ----
        //
        // ⚠️ 采集**必须重新捕获该屏再裁选区**,不能用上面捕获的 frozen:
        //    frozen 是框选那一刻的静止画面,而长截图要的正是"滚动后的新内容"。
        //    也不能 grab() 遮罩窗口 —— 遮罩是全屏半透明窗口,会把自己也采进去,
        //    长图上就会出现半透明遮罩与浮条残影(docs/19 §7 R1,本功能最致命的坑)。
        {
            QScreen* stitchScreen = screen;
            const int screenIdx = i;
            const bool useMatched = matched;
            // ⚠️ 遮罩指针按值捕获(它不是 lambda 的所有者;上面已 WA_DeleteOnClose,
            //    但 connect 挂在 overlay 自身,信号发出时它必然还活着)。
            OverlayWidget* stitchOverlay = overlay;
            QObject::connect(overlay, &OverlayWidget::stitchFrameRequested, overlay,
                    [stitchScreen, screenIdx, useMatched, stitchOverlay](
                        const QRect& physicalRect) {
                // ⚠️ 这里开始就必须把 m_stitchSampling 复位路径铺全:任何提前 return
                //    都会让节拍器以为"上一拍还没回来"而**永久停摆**(页面还在滚,
                //    长图却再也不更新)。所以用一个 RAII 守卫统一复位。
                struct SampleGuard {
                    OverlayWidget* w;
                    ~SampleGuard() { if (w) w->markSampleReturned(); }
                } guard{stitchOverlay};

                if (physicalRect.isEmpty()) { qWarning() << "长截图:矩形为空,跳过"; return; }
                // 重新捕获整屏(xcap):配对优先,失败回退按枚举序号
                QImage fresh;
                if (useMatched) fresh = CoreBridge::instance().captureMonitorMatched(stitchScreen);
                if (fresh.isNull())
                    fresh = CoreBridge::instance().captureMonitor(
                        static_cast<unsigned>(screenIdx));
                if (fresh.isNull()) {
                    qWarning() << "长截图:本帧整屏捕获失败";
                    return;
                }
                const QImage frame = CoreBridge::instance().captureCrop(fresh, physicalRect);
                if (frame.isNull()) {
                    qWarning() << "长截图:本帧裁剪失败" << physicalRect;
                    return;
                }
                // 首帧重置(选区已锁定,reset 只在会话内用一次)
                const bool first = g_stitchSession.isEmpty();
                const QString session = ensureStitchSession();
                const QJsonObject r = CoreBridge::instance().stitchAppend(session, frame, first);
                if (r.isEmpty()) {
                    qWarning() << "长截图:核心拒绝本帧(会话" << session << ")";
                    return;
                }
                const QString reason = r.value("reason").toString();
                qInfo().nospace() << "长截图: shift=" << r.value("shiftPx").toInt()
                                  << " total=" << r.value("totalHeight").toInt()
                                  << " gaps=" << r.value("gaps").toInt()
                                  << " reason=" << reason;
                if (stitchOverlay) {
                    stitchOverlay->updateStitchProgress(r.value("totalHeight").toInt(),
                                                        r.value("gaps").toInt(), reason);
                }
            });

            // 「结束」:取回长图 → 走与 annotatedConfirmed 相同的入库通道
            //
            // ⚠️ **必须转出当前信号栈**:本信号是在 OverlayWidget 的点击处理里发出的,
            //    而 closeAllOverlays() 会 close() 掉**正在发信号的这块遮罩**。
            //    虽然 close() 只是 deleteLater,但随后 closeAllOverlays 里的
            //    destroyed 计数、以及 finishCapture 的整段重活(PNG 编码 + 写库 +
            //    写剪贴板)都跑在同一个栈上 —— 实测会访问违例
            //    (0xc0000005,点「结束」后立即崩溃)。
            //    转一次事件循环既退出该栈,也让"合成中"的画面有机会刷出来。
            QObject::connect(overlay, &OverlayWidget::stitchFinishRequested, overlay,
                    [] {
                QTimer::singleShot(0, qApp, [] {
                    const QString session = g_stitchSession;
                    if (session.isEmpty()) {
                        qWarning() << "长截图:没有可结束的会话";
                        return;
                    }
                    QImage composed;
                    QJsonObject meta;
                    const bool ok = CoreBridge::instance().stitchFinish(session, &composed, &meta);
                    // 无论成败,会话都已在核心侧被取走/销毁 —— 清掉 id 以免二次 finish
                    g_stitchSession.clear();
                    if (!ok || composed.isNull()) {
                        qWarning() << "长截图:合成失败(会话" << session << ")";
                        return;
                    }
                    const int gaps = meta.value("gaps").toInt();
                    if (gaps > 0) {
                        qWarning().nospace() << "长截图:有 " << gaps << " 处缺口(滚动过快漏内容)";
                    }
                    qInfo().nospace() << "长截图:完成 " << composed.width() << "x"
                                      << composed.height() << " gaps=" << gaps;
                    const CaptureDone done = g_onCaptured;
                    g_onCaptured = {};
                    closeAllOverlays([composed, done] {
                        finishCapture(composed, done);
                    });
                });
            });

            // 「取消」:只丢长截图会话,不结束整次截图(选区仍在)
            QObject::connect(overlay, &OverlayWidget::stitchAborted, overlay, [] {
                abortStitchSession();
                qInfo() << "长截图:已取消(选区保留)";
            });
        }

        // ⚠️ showFullScreen() 只把窗口放到"它当前所在的屏",而新建窗口默认落在主屏:
        // 多屏时若不先定位,所有遮罩会**叠在主屏上**,看到的其实是别屏的底图被拉伸
        // (既糊又错位),点一次只关掉最上面那块 → 表现为"逐屏依次确认"。
        // 先指定目标屏 + 设好几何,再全屏,才能做到每屏一块。
        overlay->setScreen(screen);
        overlay->setGeometry(screen->geometry());
        overlay->showFullScreen();
    }

    if (g_overlays.isEmpty()) return;
    // 计时从会话真正建立(至少有一块遮罩)开始;此前不计,避免多屏采集耗时被算进去
    if (idleSecs > 0 && g_idleTimer) g_idleTimer->start(idleSecs * 1000);
    // 让最后一块遮罩拿到键盘焦点,Esc 才有接收者;点击其它屏时 Windows 会自动
    // 激活被点的窗口,所以任意屏上的 Esc 都能取消整个会话。
    if (OverlayWidget* last = g_overlays.last().data()) {
        last->raise();
        last->activateWindow();
        last->setFocus();
    }
}

// ---- OverlayWidget ----

void startDelayedCapture(int seconds, CaptureIntent intent, CaptureDone onCaptured) {
    if (seconds <= 0) {
        startCaptureSession(intent, std::move(onCaptured));
        return;
    }
    auto* overlay = new CountdownOverlay(seconds);
    QPointer<CountdownOverlay> guard(overlay);
    overlay->show();
    QTimer::singleShot(seconds * 1000, [guard, intent, onCaptured] {
        // 先收起倒计时(否则数字会被冻进底图),再等窗口真正消失后采屏
        if (guard) guard->close();
        QTimer::singleShot(80, [intent, onCaptured] {
            startCaptureSession(intent, onCaptured);
        });
    });
}

int captureDelaySeconds() {
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    const int secs = resp.value("data").toObject().value("captureDelaySecs").toInt(3);
    return qBound(1, secs, 10);
}

namespace {
/// 手柄命中半径与绘制边长(逻辑像素)
constexpr int kHandleGrab = 6;
constexpr int kHandleSize = 6;
/// 判定"选区可用"的最小边长(逻辑像素):太小多半是误点
constexpr int kMinSelection = 4;
} // namespace

OverlayWidget::OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, bool autoConfirm,
                             bool rightClickCancel, bool autoStartStitching, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      m_frozen(std::move(frozen)),
      m_screenIndex(screenIndex),
      m_screen(screen),
      m_autoConfirm(autoConfirm),
      m_rightClickCancel(rightClickCancel),
      m_autoStartStitching(autoStartStitching) {
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
}

qreal OverlayWidget::dpr() const {
    // ⚠️ 缩放比**从底图实际尺寸推导**,而不是直接信 `screen->devicePixelRatio()`:
    //    底图是核心按物理像素采的,而 geometry() 是 Qt 的逻辑像素,两者的比值
    //    就是本屏真实的"物理/逻辑"比例 —— 与底图天然自洽。
    //    直接用 devicePixelRatio() 在混合 DPI 下会与实际取到的底图脱节:
    //    一旦某屏走了回退取图(尺寸与预期不符),预览会被拉伸、框选换算也偏,
    //    表现就是"分辨率乱了"。取不到底图时才退回屏幕 DPR。
    if (m_screen) {
        const QSize logical = m_screen->geometry().size();
        if (!m_frozen.isNull() && logical.width() > 0 && logical.height() > 0) {
            const qreal sx = static_cast<qreal>(m_frozen.width()) / logical.width();
            const qreal sy = static_cast<qreal>(m_frozen.height()) / logical.height();
            // 横竖比应一致(等比缩放);不一致说明底图异常,退回屏幕 DPR 更安全
            if (qAbs(sx - sy) < 0.01 && sx > 0.1 && sx < 8.0) return sx;
        }
        return m_screen->devicePixelRatio();
    }
    return devicePixelRatioF();
}

QRect OverlayWidget::selectionPhysical() const {
    const qreal scale = dpr();
    const QRect sel = m_selection.normalized();
    return QRectF(QPointF(sel.topLeft()) * scale, QSizeF(sel.size()) * scale).toAlignedRect();
}

bool OverlayWidget::selectionUsable() const {
    const QRect sel = m_selection.normalized();
    return !sel.isNull() && sel.width() >= kMinSelection && sel.height() >= kMinSelection;
}

QList<QPoint> OverlayWidget::handlePoints(const QRect& rect) const {
    const int cx = rect.center().x();
    const int cy = rect.center().y();
    return {
        rect.topLeft(),     QPoint(cx, rect.top()),    rect.topRight(),
        QPoint(rect.right(), cy),                      rect.bottomRight(),
        QPoint(cx, rect.bottom()),                     rect.bottomLeft(),
        QPoint(rect.left(), cy),
    };
}

OverlayWidget::Handle OverlayWidget::handleAt(const QPoint& pos) const {
    if (m_selection.isNull()) return Handle::None;
    const QRect r = m_selection.normalized();
    // 先看是否贴近某条边/某个角(留 kHandleGrab 的容差)
    const bool nearLeft = qAbs(pos.x() - r.left()) <= kHandleGrab;
    const bool nearRight = qAbs(pos.x() - r.right()) <= kHandleGrab;
    const bool nearTop = qAbs(pos.y() - r.top()) <= kHandleGrab;
    const bool nearBottom = qAbs(pos.y() - r.bottom()) <= kHandleGrab;
    const bool withinX = pos.x() >= r.left() - kHandleGrab && pos.x() <= r.right() + kHandleGrab;
    const bool withinY = pos.y() >= r.top() - kHandleGrab && pos.y() <= r.bottom() + kHandleGrab;
    if (!withinX || !withinY) return Handle::None;

    if (nearLeft && nearTop) return Handle::TopLeft;
    if (nearRight && nearTop) return Handle::TopRight;
    if (nearLeft && nearBottom) return Handle::BottomLeft;
    if (nearRight && nearBottom) return Handle::BottomRight;
    if (nearTop) return Handle::Top;
    if (nearBottom) return Handle::Bottom;
    if (nearLeft) return Handle::Left;
    if (nearRight) return Handle::Right;
    if (r.contains(pos)) return Handle::Inside;   // 内部 = 整体拖动
    return Handle::None;
}

Qt::CursorShape OverlayWidget::cursorForHandle(Handle handle) {
    switch (handle) {
        case Handle::TopLeft:
        case Handle::BottomRight: return Qt::SizeFDiagCursor;
        case Handle::TopRight:
        case Handle::BottomLeft: return Qt::SizeBDiagCursor;
        case Handle::Top:
        case Handle::Bottom: return Qt::SizeVerCursor;
        case Handle::Left:
        case Handle::Right: return Qt::SizeHorCursor;
        case Handle::Inside: return Qt::SizeAllCursor;
        case Handle::None: break;
    }
    return Qt::CrossCursor;
}

void OverlayWidget::applyAdjustment(const QPoint& pos) {
    const QPoint delta = pos - m_adjustStartPos;
    QRect r = m_adjustStartRect;
    if (m_activeHandle == Handle::Inside) {
        r.translate(delta);
    } else {
        if (m_activeHandle == Handle::TopLeft || m_activeHandle == Handle::Left ||
            m_activeHandle == Handle::BottomLeft)
            r.setLeft(r.left() + delta.x());
        if (m_activeHandle == Handle::TopRight || m_activeHandle == Handle::Right ||
            m_activeHandle == Handle::BottomRight)
            r.setRight(r.right() + delta.x());
        if (m_activeHandle == Handle::TopLeft || m_activeHandle == Handle::Top ||
            m_activeHandle == Handle::TopRight)
            r.setTop(r.top() + delta.y());
        if (m_activeHandle == Handle::BottomLeft || m_activeHandle == Handle::Bottom ||
            m_activeHandle == Handle::BottomRight)
            r.setBottom(r.bottom() + delta.y());
        r = r.normalized();
    }
    // 夹在窗口内:遮罩就是整屏,拖到屏外没有意义
    m_selection = r.intersected(rect());
}

void OverlayWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // ⚠️ 守卫必须有:showFullScreen() 之后的第一帧里,窗口/后备缓冲可能尚未就绪,
    // begin() 返回 false(engine == 0),后续 draw* 全部空转并刷出
    // "QPainter::begin: Paint device returned engine == 0" + "Painter not active"。
    // docs/06 §10 记录的现象(疑似首帧黑屏)即由此而来 —— 实测缺的正是这一帧,
    // 丢掉它由 Qt 重发即可,冻结底图不会因此丢失。
    if (!painter.isActive()) return;
    const qreal scale = dpr();

    // 冻结底图是**物理像素**,窗口坐标是**逻辑像素**:
    // 按 DPR 反向缩放绘制,使 1 图像像素 = 1 设备像素。若直接
    // drawImage(rect(), frozen),Qt 会把图重采样到逻辑矩形,高分屏上预览发虚。
    if (!m_frozen.isNull()) {
        painter.save();
        painter.scale(1.0 / scale, 1.0 / scale);
        painter.drawImage(QPoint(0, 0), m_frozen);
        painter.restore();
    } else {
        painter.fillRect(rect(), QColor(30, 30, 34));
    }

    // 半透明黑罩(逻辑坐标,覆盖整窗)
    painter.fillRect(rect(), QColor(0, 0, 0, 90));

    const QRect sel = m_selection.normalized();
    if (!sel.isNull()) {
        // 选区内恢复原图:目标矩形是逻辑坐标,但**源矩形必须是物理像素**,
        // 否则会从底图上取到错位的区域。
        const QRect src = selectionPhysical();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(sel, m_frozen, src);

        painter.setPen(QPen(QColor(80, 160, 255), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(sel);

        // 标注态:引擎场景**直接渲染**进遮罩画布(不经中间 QImage)。
        //
        // 坐标换算(与 overlay_geometry.hpp 同一口径):
        //   scene 坐标 = 选区**局部**物理像素;painter 平移到选区左上并 scale(1/dpr) 后,
        //   1 场景单位 = 1 物理像素。
        //   ⚠️ render() 的 target 与 source 都必须是**场景单位**:target 写成逻辑尺寸
        //      (sel.width()) 会再被 painter 缩一次,净效果 1/dpr² —— 标注"跟着鼠标跑偏
        //      且变小",这正是 S3 标注看起来画不准的正脸。
        if (m_engine && selectionUsable()) {
            const qreal k = dpr();
            const QRectF sceneRect(annotationSceneRect());
            painter.save();
            painter.translate(sel.topLeft());
            painter.scale(1.0 / k, 1.0 / k);
            painter.setClipRect(sceneRect);          // 标注不出选区
            // 底图已由上面画过(同一张冻结图),临时藏掉场景底图项省一次整图合成
            QGraphicsItem* bg = m_engine->backgroundItem();
            if (bg) bg->setVisible(false);
            m_engine->scene()->render(&painter, sceneRect, sceneRect);
            if (bg) bg->setVisible(true);
            painter.restore();
        }
        // 微调模式:画 8 个手柄(提示可拖动调整);有标注时不画(互斥)
        // ⚠️ 长截图态也**不画**手柄:选区已锁定为采集窗口,让用户以为还能拖
        //    就会拖出新尺寸,而各帧宽度不一致时拼接必然错位(docs/19 §3.3 I1)。
        if (!m_autoConfirm && !m_engine && !stitchActive() && selectionUsable()) {
            painter.setPen(QPen(QColor(80, 160, 255), 1));
            painter.setBrush(QColor(255, 255, 255));
            for (const QPoint& point : handlePoints(sel)) {
                painter.drawRect(QRect(point.x() - kHandleSize / 2,
                                       point.y() - kHandleSize / 2, kHandleSize, kHandleSize));
            }
        }

        // 尺寸提示按**物理像素**显示,即最终截出来的分辨率
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(QPoint(sel.x(), sel.y() - 6),
                         QStringLiteral("%1 × %2").arg(src.width()).arg(src.height()));

        // 二级条再浮条(二级条在上层);长截图态下浮条让位给长截图面板
        if (!stitchActive()) {
            drawToolbar(painter);
            drawSubbar(painter);
        } else {
            drawStitchPanel(painter);
        }
    } else if (!m_selecting) {
        painter.setPen(QColor(255, 255, 255, 200));
        painter.drawText(rect(), Qt::AlignHCenter | Qt::AlignTop,
                         m_autoConfirm
                             ? QStringLiteral("拖拽框选,Esc 取消(屏 %1)").arg(m_screenIndex)
                             : QStringLiteral("拖拽框选;可拖手柄微调,Enter/双击/完成按钮确认,Esc 取消(屏 %1)")
                                   .arg(m_screenIndex));
    }
}

void OverlayWidget::mousePressEvent(QMouseEvent* e) {
    emit activity();
    // 右键取消(可在设置里关掉)
    if (e->button() == Qt::RightButton) {
        if (m_rightClickCancel) emit cancelled();
        return;
    }
    if (e->button() != Qt::LeftButton) return;

    // 文字输入中:先落字(点别处 = 确认,不再需要第二次点击)
    if (m_textEdit) commitInlineText();

    // 长截图面板命中:**先于一切**(含浮条)。面板激活时浮条已不显示,
    // 但两者几何可能重叠,顺序写死避免"点了面板却触发了浮条"。
    if (stitchPanelContains(e->pos())) {
        handleStitchPanelClick(e->pos());
        return;
    }
    // 长截图态下**禁止重新框选**:选区是采集窗口,拖出新选区会让已采帧与新帧
    // 宽度不一致,拼接必然错位(docs/19 §3.3 I1)。面板之外的点击一律忽略。
    if (stitchActive()) return;

    // 二级条/浮条命中:**先于一切** —— 点按钮走动作,点条上不当作重新框选。
    // 二级条已是**共享控件**(§17.3 A):按钮是真实子控件,点击根本不会落到这里;
    // 这条兜底拦的是控件的空白内边距区(落在这里 = 点在条上,不该当重新框选)。
    if (subbarWidgetContains(e->pos())) {
        return;   // 控件自己处理;遮罩什么都不做(不重框、不取消)
    }
    if (toolbarContains(e->pos())) {
        handleToolbarClick(e->pos());
        return;
    }
    // ⚠️ 这里**不能**收浮条:只要还有选区,浮条就该在(它是「完成/保存/取消」的唯一
    //    出口)。原先在"点到浮条外"时无条件 hideToolbar(),而画布上任何一次按下都算
    //    "浮条外" —— 于是**画完第一笔浮条就整条消失**,用户再也点不到完成/保存,
    //    表现就是"标注画不准、还用不了也存不了"。重新框选时选区被清空,
    //    toolbarVisible() 自然为假,不需要额外收。

    // 微调模式:已有选区时,按在手柄/内部上就进入调整,而不是重新框选。
    // ⚠️ 标注态**跳过**本段:Inside 命中会把"按下开始画画"吞成"整体拖动选区"
    // (实测症状:笔画不出现、选区跟着鼠标跑 = 整体偏移);手柄此时本就隐藏。
    if (!m_autoConfirm && !m_engine && !m_selection.isNull()) {
        const Handle handle = handleAt(e->pos());
        if (handle != Handle::None) {
            m_adjusting = true;
            m_activeHandle = handle;
            m_adjustStartRect = m_selection.normalized();
            m_adjustStartPos = e->pos();
            return;
        }
    }

    // 标注态:选区内的按下交给引擎(坐标换算:逻辑 → 选区局部物理,见 overlay_geometry)
    if (m_engine && m_selection.normalized().contains(e->pos())) {
        const QRect sel = m_selection.normalized();
        // 文字:浮条没有 QGraphicsView,引擎的文字图元收不到键盘 —— 用宿主 QLineEdit 收
        if (m_engine->tool() == EditorTool::Text) {
            beginInlineText(e->pos());
            return;
        }
        const QPointF scenePos = overlay::toScene(e->pos(), sel, dpr());
        m_engine->onCanvasPressed(scenePos);
        // 取色器:引擎采样后会自动切回上一个绘图工具并把颜色改掉,宿主同步选中态与光标
        m_annoTool = m_engine->tool();
        m_annoColor = m_engine->color();
        setCursor(toolCursor(m_annoTool));
        m_draggingStroke = true;
        update(sel.adjusted(-2, -2, 2, 2));   // 只重画选区(整屏重绘在 4K 上很吃帧)
        return;
    }

    // 起新选区:旧标注锚定在旧选区上,选区一变全部失效 → 退出标注态(重新来)
    if (m_engine) exitAnnotating();
    m_origin = e->pos();
    m_selection = QRect(m_origin, QSize(0, 0));
    m_selecting = true;
    update();
}

void OverlayWidget::mouseMoveEvent(QMouseEvent* e) {
    emit activity();
    if (m_selecting) {
        m_selection = QRect(m_origin, e->pos()).normalized();
        update();
        return;
    }
    if (m_engine && m_draggingStroke) {
        const QRect sel = m_selection.normalized();
        m_engine->onCanvasMoved(overlay::toScene(e->pos(), sel, dpr()));
        update(sel.adjusted(-2, -2, 2, 2));
        return;
    }
    if (m_adjusting) {
        applyAdjustment(e->pos());
        update();
        return;
    }
    // 悬停在浮条/二级条的按钮上:给"可点"的手型(否则标注态会顶着一个画笔光标,
    // 用户不知道这里点得动)
    if (toolbarContains(e->pos()) || subbarWidgetContains(e->pos())) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    // 标注态:光标按**工具**给(文字=竖线/取色=手型/其余=十字)。
    // ⚠️ 不能落到下面的手柄逻辑:选区内会命中 Handle::Inside → SizeAllCursor,
    //    而标注态在这里是"画画",显示"可整体拖动"是误导。
    if (m_engine) {
        setCursor(toolCursor(m_engine->tool()));
        return;
    }
    // 悬停时给出光标反馈(仅微调模式有意义)
    if (!m_autoConfirm && !m_selection.isNull()) {
        setCursor(cursorForHandle(handleAt(e->pos())));
    }
}

void OverlayWidget::mouseReleaseEvent(QMouseEvent* e) {
    emit activity();
    if (e->button() != Qt::LeftButton) return;

    if (m_engine && m_draggingStroke) {
        const QRect sel = m_selection.normalized();
        m_engine->onCanvasReleased(overlay::toScene(e->pos(), sel, dpr()));
        m_draggingStroke = false;
        update(sel.adjusted(-2, -2, 2, 2));
        return;
    }
    if (m_adjusting) {
        m_adjusting = false;
        m_activeHandle = Handle::None;
        if (toolbarVisible()) {
            showToolbar();                    // 选区变了,浮条跟随
            if (m_subbarVisible) m_subbarRect = subbarRect();
        }
        return;   // 微调结束不触发确认
    }
    if (!m_selecting) return;

    m_selecting = false;
    m_selection = QRect(m_origin, e->pos()).normalized();
    if (!selectionUsable()) {
        // 误点(没拖出有效范围):清掉选区,等用户重新框
        m_selection = QRect();
        update();
        if (m_autoConfirm) emit cancelled();   // 直通模式保持"点空即取消"的原有手感
        return;
    }
    // 三种出口:
    //   1) 长截图意图(热键 Ctrl+Shift+L)→ 直接进长截图就绪态;
    //      与 regionAutoConfirm **无关** —— 用户已经明确要长截图了。
    //   2) 直通模式 → 松手即确认入库;
    //   3) 微调模式 → 保留选区 + 浮出工具栏(docs/14 §16)。
    if (m_autoStartStitching) {
        // ⚠️ 先 showToolbar() 再进长截图态?不要 —— 长截图态下 toolbarVisible()
        //    恒为 false,浮条根本不会显示;直接进即可。
        enterStitching();
    } else if (m_autoConfirm) {
        emit regionConfirmed(selectionPhysical());
    } else {
        showToolbar();
    }
    update();
}

void OverlayWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (m_autoConfirm || e->button() != Qt::LeftButton) return;
    if (!selectionUsable()) return;
    // 标注态双击 = 在画布上点两下,不该当成"确认"把标注丢掉
    if (m_engine) return;
    confirmSelection();
}

void OverlayWidget::keyPressEvent(QKeyEvent* e) {
    emit activity();
    if (e->key() == Qt::Key_Escape) {
        // 长截图态的 Esc 是**分层**的(docs/19 §3.4):
        //   采集中 → 结束并合成(不丢已采内容;用户常按 Esc 表示"拍到这儿")
        //   就绪态 → 退出长截图态,回到普通框选(选区仍在)
        // 只有在普通框选态才取消整次截图。
        if (stitchCapturing()) {
            toggleStitchCapture();
            return;
        }
        if (stitchActive()) {
            exitStitching();
            return;
        }
        emit cancelled();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (stitchActive()) return;   // 长截图态用面板按钮,Enter 不做确认
        if (!m_autoConfirm && selectionUsable()) confirmSelection();
    }
}

// ---- 框选后浮出工具栏(docs/14 §16;仅 autoConfirm=false 生效)----

namespace {

/// 按钮文案与索引一一对应(宽度均分,见 overlay::kToolbarWidth)。
QString toolbarButtonText(int index) {
    switch (index) {
        case 0: return QStringLiteral("标注");
        case 1: return QStringLiteral("OCR取字");
        case 2: return QStringLiteral("长截图");
        case 3: return QStringLiteral("贴图");
        case 4: return QStringLiteral("保存");
        case 5: return QStringLiteral("透明");   // 底板不透明度三档循环(docs/17 §17.5)
        case 6: return QStringLiteral("完成");
        default: return QStringLiteral("取消");
    }
}

} // namespace

bool OverlayWidget::toolbarVisible() const {
    // 长截图态下浮条让位:两套面板叠在一起既挡内容又抢命中
    if (stitchActive()) return false;
    return !m_autoConfirm && selectionUsable();
}

QRect OverlayWidget::toolbarRect() const {
    // 摆位(下/上翻转 + 夹屏)统一在 overlay::toolbarRectFor —— 与验收共用同一份算式
    return overlay::toolbarRectFor(m_selection.normalized(), size(),
                                   QSize(overlay::kToolbarWidth, overlay::kButtonHeight),
                                   overlay::kBarMargin);
}

void OverlayWidget::showToolbar() {
    m_toolbarRect = toolbarRect();
    if (m_subbarVisible) {
        m_subbarRect = subbarRect();   // 二级条跟着浮条走
        if (m_subbar) { m_subbar->setGeometry(m_subbarRect); m_subbar->raise(); }
    }
    update();
}

void OverlayWidget::confirmSelection() {
    // 标注态:提交**含标注**的合成图。annotatedConfirmed 此前只连接、从不发射 ——
    // 于是"标注后 Enter/完成"入库的都是没标注的底图,用户看到的就是"标注保存不下来"。
    if (m_engine) {
        m_engine->bakeTextItem();
        const QImage composed = m_engine->compose(annotationSceneRect());
        if (!composed.isNull()) {
            emit annotatedConfirmed(composed);
            return;
        }
    }
    emit regionConfirmed(selectionPhysical());
}

bool OverlayWidget::toolbarContains(const QPoint& pos) const {
    return toolbarVisible() && m_toolbarRect.contains(pos);
}

void OverlayWidget::drawToolbar(QPainter& painter) const {
    if (!toolbarVisible()) return;
    const QRect rc = m_toolbarRect;
    // 底板
    painter.setPen(Qt::NoPen);
    // 底板不透明度走**用户可调的三档**(docs/17 §17.5):原先硬编码 242(≈95% 不透明),
    // 压在选区底部会把待截内容整片盖住 —— 这正是"浮条挡原图"的成因。
    painter.setBrush(QColor(0x26, 0x27, 0x2E, barAlpha()));
    painter.drawRoundedRect(rc, 8, 8);
    // 按钮均分;完成/取消给强调/弱化色,其余普通
    const int count = overlay::kToolbarButtons;
    const int bw = rc.width() / count;
    // 极简档下底板仅 70% 不透明,默认灰字会"透到看不清" —— 提高对比度兜底
    const QColor baseFg = barNeedsContrastBoost() ? QColor(0xFF, 0xFF, 0xFF)
                                                  : QColor(0xDF, 0xE1, 0xE8);
    for (int i = 0; i < count; ++i) {
        const QRect btn(rc.x() + i * bw, rc.y(), bw, rc.height());
        QColor fg = baseFg;
        const QColor accent = barNeedsContrastBoost() ? QColor(255, 255, 255) : QColor();
        // 索引顺序(与 toolbarButtonText 一一对应):
        // 0 标注 / 1 OCR取字 / 2 长截图 / 3 贴图 / 4 保存 / 5 透明 / 6 完成 / 7 取消
        if (i == 0 && annotating()) fg = accent.isValid() ? accent : QColor(0x1C, 0x7E, 0xD6);
        if (i == 2 && stitchActive()) fg = accent.isValid() ? accent : QColor(0x1C, 0x7E, 0xD6);
        if (i == 6) fg = accent.isValid() ? accent : QColor(0x40, 0xC0, 0x57);   // 完成:绿
        if (i == 7) fg = accent.isValid() ? accent : QColor(0xE0, 0x31, 0x31);   // 取消:红
        painter.setPen(fg);
        painter.drawText(btn, Qt::AlignCenter, toolbarButtonText(i));
        if (i < count - 1) {
            painter.setPen(QColor(255, 255, 255, 28));
            painter.drawLine(btn.topRight() + QPoint(0, 8),
                             btn.bottomRight() - QPoint(0, 8));
        }
    }
}

void OverlayWidget::handleToolbarClick(const QPoint& pos) {
    if (!toolbarContains(pos)) return;
    const QRect rc = m_toolbarRect;
    const int count = overlay::kToolbarButtons;
    const int bw = rc.width() / count;
    const int index = qBound(0, (pos.x() - rc.x()) / bw, count - 1);
    switch (index) {
        case 5:   // 透明:底板不透明度三档循环(即时生效,不结束会话)
            cycleBarAlpha();
            return;
        case 6:   // 完成
            confirmSelection();
            return;
        case 7:   // 取消
            emit cancelled();
            return;
        case 0:   // 标注:进入/退出标注态(S3)
            if (annotating()) exitAnnotating();
            else enterAnnotating();
            m_toolbarRect = toolbarRect();   // 布局可能因二级条显隐变化
            update();
            return;
        case 2:   // 长截图:进入长截图态(不结束会话 —— 选区转为采集窗口)
            enterStitching();
            return;
        default:
            break;
    }
    // OCR/贴图/保存:输入 = 选区图(未标注时零 compose 开销,docs/14 §16.3-6);
    // 有标注则用引擎合成(含标注)。
    // ⚠️ 引擎场景是**选区局部**物理像素(原点在选区左上),必须传 annotationSceneRect();
    //    传全局 selectionPhysical() 等于取"场景的右下角一块",实测存出来是错位 + 右下空白。
    const QImage cropped = m_engine ? m_engine->compose(annotationSceneRect())
                                    : m_frozen.copy(selectionPhysical());
    if (cropped.isNull()) return;
    // 决策:三者在弹层/贴图/另存完成后**结束会话** —— 与形态 B 一致。
    // 索引:1 OCR取字 / 3 贴图 / 4 保存(长截图插在 2,见上)
    switch (index) {
        case 1: {  // OCR取字:异步(0.4~5s),弹层先占位
            emit activity();
            auto* watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcher<QString>::finished, this,
                    [this, watcher] {
                watcher->deleteLater();
                const QString text = watcher->result();
                // 「取字」的落点是**系统剪贴板**:识别到就写进去,用户 Ctrl+V 直接可用。
                // ⚠️ 此前这里只把文本塞进 QToolTip 展示、**从不写剪贴板** ——
                //    用户点「OCR取字」后剪贴板里当然什么都没有(docs/14 §16 只写了
                //    "识别 → 弹层",漏了写剪贴板这一步;参照实现是 CoreFacade::
                //    processCapture 的 copy_text_to_clipboard)。
                //    经核心写入而非 Qt:核心的 clipboard_win 带打开重试,别的进程
                //    短暂占用时 Qt 会静默失败(日志里 "Unable to obtain clipboard" 即此类)。
                QString notice;
                if (text.isEmpty()) {
                    notice = QStringLiteral("未识别到文字(图片可能不含可识别文本)");
                } else if (CoreBridge::instance().copyTextToClipboard(text)) {
                    notice = QStringLiteral("已识别 %1 个字符并复制到剪贴板").arg(text.size());
                } else {
                    notice = QStringLiteral("识别成功,但写入剪贴板失败");
                }
                const QPoint gp = mapToGlobal(QPoint(m_toolbarRect.x(), m_toolbarRect.y() - 8));
                QToolTip::showText(gp, notice, this, QRect(), 4000);
                emit cancelled();
            });
            connect(watcher, &QFutureWatcher<QString>::canceled, watcher,
                    &QObject::deleteLater);
            watcher->setFuture(QtConcurrent::run([cropped]() -> QString {
                return CoreBridge::instance().ocrImage(cropped);
            }));
            QToolTip::showText(mapToGlobal(m_toolbarRect.topLeft()),
                               QStringLiteral("识别中…"), this, QRect(), 2500);
            return;
        }
        case 3:    // 贴图
            pinComposedImage(cropped);
            emit cancelled();
            return;
        case 4: {  // 保存
            bakeSaveDialog(cropped);
            emit cancelled();
            return;
        }
        default:
            return;
    }
}


// ---- 滚动长截图态(docs/19 §3.2)----

void OverlayWidget::enterStitching() {
    if (m_stitch != StitchState::Off) return;
    if (!selectionUsable()) return;   // 没有选区就没有采集窗口
    // 标注与长截图**互斥**:标注锚定在选区上,而长截图的选区是采集窗口。
    // 两者同时生效会让"画在选区里的笔画"跟着长图一起被滚走,语义混乱。
    if (m_engine) exitAnnotating();
    m_stitch = StitchState::Ready;
    m_stitchTotalHeight = 0;
    m_stitchGaps = 0;
    m_stitchNotice = QStringLiteral("点「开始」后滚动页面,内容变化即自动采集");
    m_stitchPanelRect = stitchPanelRect();
    update();
    qInfo() << "长截图:进入就绪态,选区 =" << m_selection.normalized();
}

void OverlayWidget::exitStitching() {
    if (m_stitch == StitchState::Off) return;
    stopStitchSampling();
    applyStitchHole(false);   // 恢复正常窗口区域
    m_stitch = StitchState::Off;
    m_stitchNotice.clear();
    m_stitchTotalHeight = 0;
    m_stitchGaps = 0;
    update();
}

bool OverlayWidget::toggleStitchCapture() {
    if (m_stitch == StitchState::Off) return false;
    if (m_stitch == StitchState::Ready) {
        m_stitch = StitchState::Capturing;
        m_stitchNotice = QStringLiteral("采集中…滚动页面;点「结束」或按 Esc 完成");
        m_stitchTotalHeight = 0;
        m_stitchGaps = 0;
        m_stitchPanelRect = stitchPanelRect();
        // 先挖洞再采首帧:挖洞后页面才可交互,而首帧应当拍"开始之前"的样子
        // (用户已停在想开始的位置)。顺序反了会把"刚开始那一刻"漏掉。
        applyStitchHole(true);
        update();
        // 首帧立刻采一次(不等第一个 500ms 拍:用户已停在想开始的位置,
        // 那一屏必须收进去),随后交给节拍器持续采样。
        m_stitchSampling = true;
        emit stitchFrameRequested(selectionPhysical());
        startStitchSampling();
        return true;
    }
    // 采集中 → 结束:交给会话层收尾(它调核心 finish 并入库)
    // 先把洞补上 —— 否则窗口区域缺口会在"合成中"这段时间里一直露着下层窗口,
    // 视觉上像遮罩破了一块。
    m_stitch = StitchState::Ready;
    stopStitchSampling();
    applyStitchHole(false);
    m_stitchNotice = QStringLiteral("正在合成…");
    update();
    emit stitchFinishRequested();
    return false;
}

QRect OverlayWidget::stitchCaptureRect() const {
    return selectionPhysical();
}

QRect OverlayWidget::stitchPanelRect() const {
    return overlay::stitchPanelRectFor(
        m_selection.normalized(), size(),
        QSize(overlay::kStitchPanelWidth, overlay::kStitchPanelHeight),
        overlay::kBarMargin);
}

bool OverlayWidget::stitchPanelContains(const QPoint& pos) const {
    return stitchActive() && m_stitchPanelRect.contains(pos);
}

void OverlayWidget::handleStitchPanelClick(const QPoint& pos) {
    if (!stitchPanelContains(pos)) return;
    const QRect rc = m_stitchPanelRect;
    const int bw = rc.width() / overlay::kStitchButtons;
    const int index = qBound(0, (pos.x() - rc.x()) / bw, overlay::kStitchButtons - 1);
    if (index == 0) {
        toggleStitchCapture();
        return;
    }
    // 「取消」:丢弃已采集内容并退出长截图态(不结束整次截图,选区仍在)
    stopStitchSampling();
    applyStitchHole(false);
    m_stitch = StitchState::Off;
    m_stitchNotice.clear();
    m_stitchTotalHeight = 0;
    m_stitchGaps = 0;
    update();
    emit stitchAborted();
}

void OverlayWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    // 窗口尺寸变了(多屏 DPI 切换 / 全屏还原),挖洞区域必须按新尺寸重算,
    // 否则洞会留在旧坐标上 —— 表现为"页面上有一块地方不能滚"
    if (m_stitchHoleApplied) refreshStitchHole();
}

void OverlayWidget::updateStitchProgress(int totalHeight, int gaps, const QString& reason) {
    m_stitchSampling = false;   // 本拍已回来,允许下一拍投递
    m_stitchTotalHeight = totalHeight;
    m_stitchGaps = gaps;
    const qreal k = dpr();
    const QString heightText = QStringLiteral("已拼接 %1 px")
        .arg(qRound(totalHeight / (k > 0.01 ? k : 1.0)));
    if (reason == QLatin1String("no-overlap")) {
        m_stitchNotice = QStringLiteral("滚动过快,有内容漏掉(缺口 %1)").arg(gaps);
    } else if (reason == QLatin1String("backward")) {
        m_stitchNotice = QStringLiteral("已到顶部,无法继续向上拼接");
    } else if (reason == QLatin1String("limit")) {
        m_stitchNotice = QStringLiteral("已达高度上限,点「结束」生成");
    } else if (reason == QLatin1String("duplicate")) {
        // 未滚动:不打扰,保留上一次文案
    } else {
        m_stitchNotice = heightText;
    }
    update();
}


// ---- 长截图采集的"挖洞"(docs/19 §7 R2)----
//
// 遮罩铺满整屏且置顶,会吞掉所有鼠标事件 —— 用户在选区上滚动时,滚轮被遮罩
// 接收,底下的浏览器/记事本根本不动,采到的每一帧都是同一张图。
//
// 解法:**采集期间把选区从窗口区域里挖掉**(SetWindowRgn)。挖掉的那块没有窗口,
// 鼠标事件直达下层真实窗口 → 页面真的会滚、且用户**看得见**滚动结果。
// 遮罩其余部分(黑罩 + 面板 + 进度条)照旧在,所以「结束」「取消」仍点得到。
//
// 为什么不用"转发滚轮":那样用户看到的一直是冻结画面,等于盲滚;
// 也不能整个窗口透明:连按钮都点不到。

void OverlayWidget::applyStitchHole(bool enable) {
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    if (!enable) {
        // ⚠️ 补洞**不能**写 SetWindowRgn(hwnd, nullptr, TRUE)。
        //    实测:那样之后 GetWindowRgn 返回 NULLREGION(0),即"窗口没有任何可见
        //    区域"—— 遮罩在合成期间整块消失(用户看到桌面闪一下)。
        //    正确做法是显式设一个覆盖整窗的矩形区域。
        const QRect win = rect();
        HRGN full = ::CreateRectRgn(win.left(), win.top(),
                                    win.right() + 1, win.bottom() + 1);
        if (full) {
            if (::SetWindowRgn(hwnd, full, TRUE) == 0) {
                ::DeleteObject(full);
                qWarning() << "长截图:恢复整窗区域失败";
            }
        }
        m_stitchHoleApplied = false;
        return;
    }
    refreshStitchHole();
#else
    Q_UNUSED(enable);
#endif
}

void OverlayWidget::refreshStitchHole() {
#ifdef Q_OS_WIN
    if (!stitchCapturing() || !selectionUsable()) {
        applyStitchHole(false);
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    const QRect win = rect();
    const QRect hole = m_selection.normalized();
    if (hole.isEmpty() || !win.contains(hole)) return;

    // 造一个"整屏减去选区"的区域:四个矩形拼成(上/下/左/右)。
    // 比 Region 差集更可控 —— 差集在 Windows 上要 HRGN 组合,失败时没有可读错误。
    HRGN rgn = ::CreateRectRgn(0, 0, 0, 0);
    if (!rgn) return;
    const auto add = [&rgn](const QRect& r) {
        if (r.width() <= 0 || r.height() <= 0) return;
        HRGN piece = ::CreateRectRgn(r.left(), r.top(), r.right() + 1, r.bottom() + 1);
        if (piece) {
            ::CombineRgn(rgn, rgn, piece, RGN_OR);
            ::DeleteObject(piece);
        }
    };
    // 上条
    add(QRect(win.left(), win.top(), win.width(), hole.top() - win.top()));
    // 下条
    add(QRect(win.left(), hole.bottom() + 1, win.width(), win.bottom() - hole.bottom()));
    // 左条(限于洞的高度)
    add(QRect(win.left(), hole.top(), hole.left() - win.left(), hole.height()));
    // 右条(限于洞的高度)
    add(QRect(hole.right() + 1, hole.top(), win.right() - hole.right(), hole.height()));

    // SetWindowRgn 接管 rgn 的所有权(成功后不得再 DeleteObject)
    if (::SetWindowRgn(hwnd, rgn, TRUE) == 0) {
        ::DeleteObject(rgn);
        qWarning() << "长截图:设置窗口区域失败,页面将无法滚动";
        return;
    }
    m_stitchHoleApplied = true;
    // ⚠️ 关键:SetWindowRgn **不会**让那块屏幕区域自动重绘。
    //
    // Windows 只是把这块从"本窗口的区域"里去掉,屏幕上残留的仍是**上一次绘制
    // 留下的像素**(半透明黑罩)。于是截图抓到的是遮罩残影,不是下层页面 ——
    // 表现:每帧都一样 → 核心判 duplicate → 长图只有一屏高,而日志全是 ok。
    //
    // 修复:只让**洞那一块**重绘。
    // ⚠️ 不要用 InvalidateRect(nullptr, nullptr, ...) —— 那是无效化整个桌面,
    //    在多屏/多窗口下会引发整屏重绘(实测有可见闪烁),而我们需要刷新的
    //    仅是被挖掉的那块区域。
    // 洞在窗口坐标里就是 hole;转成屏幕坐标后无效化该矩形,系统会让覆盖它的
    // 所有窗口(含下层页面)重画该区域。
    RECT holeScreen{hole.left(), hole.top(), hole.right() + 1, hole.bottom() + 1};
    // 窗口是屏坐标布局(遮罩铺满整屏,窗口原点 = 屏原点),ClientToScreen 兜底
    ::ClientToScreen(hwnd, reinterpret_cast<POINT*>(&holeScreen.left));
    ::ClientToScreen(hwnd, reinterpret_cast<POINT*>(&holeScreen.right));
    ::InvalidateRect(nullptr, &holeScreen, TRUE);
    // 自己的可见部分(洞之外)同样要重画:区域变了
    ::RedrawWindow(hwnd, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    qInfo().nospace() << "长截图:已在选区挖洞 " << hole << "(滚轮直达下层窗口)";
#else
#endif
}


void OverlayWidget::startStitchSampling() {
    if (!m_stitchTimer) {
        m_stitchTimer = new QTimer(this);
        m_stitchTimer->setInterval(overlay::kStitchSampleIntervalMs);
        // 定时拍:每拍请求一帧。核心按行指纹去重(duplicate 不占预算),
        // 所以"手停了还在拍"不会让长图膨胀,只是几行整数比对。
        connect(m_stitchTimer, &QTimer::timeout, this, [this] {
            if (!stitchCapturing()) return;   // 已结束/已取消:静默停拍
            // 上一拍投出去的请求若还没回来(核心慢),跳过本拍 —— 不要堆请求
            if (m_stitchSampling) return;
            m_stitchSampling = true;
            emit stitchFrameRequested(selectionPhysical());
        });
    }
    m_stitchSampling = false;
    m_stitchTimer->start();
    qInfo() << "长截图:开始采集,间隔" << overlay::kStitchSampleIntervalMs << "ms";
}

void OverlayWidget::stopStitchSampling() {
    if (m_stitchTimer) m_stitchTimer->stop();
    m_stitchSampling = false;
}

void OverlayWidget::drawStitchPanel(QPainter& painter) const {
    if (!stitchActive()) return;
    const QRect rc = m_stitchPanelRect;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x26, 0x27, 0x2E, barAlpha()));
    painter.drawRoundedRect(rc, 8, 8);

    const int count = overlay::kStitchButtons;
    const int bw = rc.width() / count;
    const QColor baseFg = barNeedsContrastBoost() ? QColor(0xFF, 0xFF, 0xFF)
                                                  : QColor(0xDF, 0xE1, 0xE8);
    for (int i = 0; i < count; ++i) {
        const QRect btn(rc.x() + i * bw, rc.y(), bw, rc.height());
        QColor fg = baseFg;
        if (i == 0) {
            // 开始=绿,结束=橙:同一个按钮的两种状态必须一眼可辨
            fg = stitchCapturing() ? QColor(0xE8, 0x9B, 0x2B) : QColor(0x40, 0xC0, 0x57);
        } else {
            fg = QColor(0xE0, 0x31, 0x31);
        }
        painter.setPen(fg);
        const QString label = (i == 0) ? (stitchCapturing() ? QStringLiteral("结束")
                                                            : QStringLiteral("开始"))
                                       : QStringLiteral("取消");
        painter.drawText(btn, Qt::AlignCenter, label);
        if (i < count - 1) {
            painter.setPen(QColor(255, 255, 255, 28));
            painter.drawLine(btn.topRight() + QPoint(0, 8),
                             btn.bottomRight() - QPoint(0, 8));
        }
    }

    // 采集中:选区底部进度条 + 状态文案
    if (stitchCapturing()) {
        const QRect bar = overlay::stitchProgressRectFor(
            m_selection.normalized(), size(), overlay::kStitchProgressHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x26, 0x27, 0x2E, qMin(255, barAlpha() + 20)));
        painter.drawRoundedRect(bar, 6, 6);
        painter.setPen(barNeedsContrastBoost() ? QColor(255, 255, 255) : baseFg);
        painter.drawText(bar.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         m_stitchNotice);
    } else if (!m_stitchNotice.isEmpty()) {
        // 就绪态:文案挂在面板下方
        painter.setPen(QColor(255, 255, 255, 210));
        painter.drawText(QRect(rc.x() - 60, rc.bottom() + 6, rc.width() + 120, 20),
                         Qt::AlignCenter, m_stitchNotice);
    }
}

// ---- 标注态(S3)----

bool OverlayWidget::hasAnnotations() const {
    // 引擎存在且已有至少一条标注(撤销栈非空)
    return m_engine && m_engine->undoStack()->index() > 0;
}

void OverlayWidget::enterAnnotating() {
    if (m_engine) return;
    // 引擎吃**选区图**(物理像素):场景 = 选区**局部**物理像素(原点在选区左上),
    // 故鼠标换算一律走 overlay::toScene —— 别再拿全局 selectionPhysical() 去索引场景。
    const QImage crop = m_frozen.copy(selectionPhysical());
    m_engine = new AnnotationEngine(crop, this);
    m_engine->setTool(m_annoTool);
    m_engine->setColor(m_annoColor);
    m_engine->setStrokeWidth(m_penSize);
    m_engine->setFontSize(m_fontSize);
    m_subbarVisible = true;
    m_toolbarRect = toolbarRect();   // 先定浮条位置
    m_subbarRect = subbarRect();     // 二级条几何依赖浮条(文档 §16.1:叠在其上方)
    // 二级条换成**共享控件**(§17.3 A):与编辑器同一个 AnnotationToolbar,
    // 工具/颜色/粗细/字号的图标与选中态逻辑只此一份。遮罩上 QLineEdit 内联输入
    // 已是先例,子控件在遮罩窗口上工作正常。透明背景 → 底板仍由 paintEvent 画,
    // 视觉与主浮条一致(同一 barAlpha 三档)。
    if (!m_subbar) {
        m_subbar = new AnnotationToolbar(this);
        m_subbar->setAttribute(Qt::WA_TranslucentBackground);
        m_subbar->setAttribute(Qt::WA_NoMousePropagation);   // 事件别漏给遮罩
        m_subbar->setOverlayMode(true);                       // 无手型(遮罩无画布可拖)
        m_subbar->setGeometry(m_subbarRect);
        m_subbar->show();
        // 控件点按钮 → 引擎与浮条成员同步(取色器回退等仍走 setAnnoTool)
        connect(m_subbar, &AnnotationToolbar::toolChanged, this,
                [this](EditorTool tool) { setAnnoTool(tool); });
        connect(m_subbar, &AnnotationToolbar::colorChanged, this, [this](const QColor& c) {
            m_annoColor = c;
            if (m_engine) m_engine->setColor(c);
            update();
        });
        connect(m_subbar, &AnnotationToolbar::strokeWidthChanged, this, [this](int w) {
            m_penSize = w;
            if (m_engine) m_engine->setStrokeWidth(w);
            update();
        });
        connect(m_subbar, &AnnotationToolbar::fontSizeChanged, this, [this](int px) {
            m_fontSize = px;
            if (m_engine) m_engine->setFontSize(px);
            update();
        });
        // 控件初值 = 浮条成员(用户可能先点过主浮条的颜色再进标注态)
        m_subbar->setEngine(m_engine);
    }
    setCursor(crosshairCursor(palette().color(QPalette::Window).lightness() < 128, dpr()));
    update();
}

void OverlayWidget::exitAnnotating() {
    if (!m_engine) return;
    cancelInlineText();              // 退出标注态,未确认的文字一并丢弃
    m_engine->deleteLater();
    m_engine = nullptr;
    if (m_subbar) { m_subbar->hide(); m_subbar->deleteLater(); }   // 共享控件随标注态销毁
    m_subbarVisible = false;
    m_subbarRect = QRect();
    setCursor(Qt::CrossCursor);          // 回到框选语义的十字
    update();
}

// ---- 文字工具:宿主内联输入(浮条没有 QGraphicsView,引擎的文字图元收不到键盘)----

void OverlayWidget::beginInlineText(const QPoint& widgetPos) {
    if (!m_engine) return;
    cancelInlineText();   // 上一处未确认:丢弃(等价于换位置重开)
    m_textAnchor = widgetPos;
    auto* edit = new QLineEdit(this);
    m_textEdit = edit;
    edit->setFrame(false);
    edit->setPlaceholderText(QStringLiteral("输入文字,Enter 确认"));
    QFont f = edit->font();
    // 字号档位是**物理像素**(场景单位),控件上按逻辑像素显示才对得上落字效果
    f.setPixelSize(qBound(10, qRound(m_fontSize / dpr()), 96));
    edit->setFont(f);
    edit->setStyleSheet(QStringLiteral("QLineEdit{background:rgba(38,39,46,235);"
                                       "color:%1;border:1px solid #1C7ED6;"
                                       "border-radius:3px;padding:2px 4px;}")
                            .arg(m_annoColor.name()));
    const int w = qBound(160, 12 * f.pixelSize(), 420);
    edit->setFixedWidth(w);
    edit->move(qBound(0, widgetPos.x(), qMax(0, width() - w)),
               qBound(0, widgetPos.y(), qMax(0, height() - 34)));
    edit->installEventFilter(this);
    setCursor(Qt::IBeamCursor);   // 输入期间整体保持竖线(含框外),与文本框连成一体
    connect(edit, &QLineEdit::returnPressed, this, [this] { commitInlineText(); });
    edit->show();
    edit->setFocus();
    update();
}

void OverlayWidget::commitInlineText() {
    if (!m_textEdit) return;
    QLineEdit* edit = m_textEdit;
    m_textEdit = nullptr;            // 先摘指针:防 FocusOut/returnPressed 重入
    const QString text = edit->text();
    edit->removeEventFilter(this);
    edit->hide();
    edit->deleteLater();
    if (m_engine && !text.trimmed().isEmpty()) {
        m_engine->addText(text,
                          overlay::toScene(m_textAnchor, m_selection.normalized(), dpr()),
                          m_annoColor, m_fontSize);
    }
    if (m_engine) setCursor(toolCursor(m_engine->tool()));   // 回到工具光标(文字工具=竖线)
    update();
}

void OverlayWidget::cancelInlineText() {
    if (!m_textEdit) return;
    QLineEdit* edit = m_textEdit;
    m_textEdit = nullptr;
    edit->removeEventFilter(this);
    edit->hide();
    edit->deleteLater();
    if (m_engine) setCursor(toolCursor(m_engine->tool()));
    update();
}

bool OverlayWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_textEdit) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                cancelInlineText();
                return true;   // Esc 只收掉这段文字,不结束整个截图会话
            }
        } else if (event->type() == QEvent::FocusOut) {
            commitInlineText();
        }
    }
    return QWidget::eventFilter(watched, event);
}

QRect OverlayWidget::subbarRect() const {
    // 叠在浮条上方;顶部放不下改到下方,并夹在窗口内(见 overlay::subbarRectFor)
    return overlay::subbarRectFor(m_toolbarRect,
                                  QSize(overlay::kSubbarWidth, overlay::kSubbarHeight),
                                  overlay::kBarGap, size());
}

bool OverlayWidget::subbarContains(const QPoint& pos) const {
    return m_subbarVisible && m_subbarRect.contains(pos);
}

bool OverlayWidget::subbarWidgetContains(const QPoint& pos) const {
    // 控件的真实边界(它有自己的内边距/按钮),比 m_subbarRect 更精确;
    // 控件不在(未进标注态)时退回几何判断,选区微调的拖拽不受影响。
    return m_subbar ? m_subbar->geometry().contains(pos) : subbarContains(pos);
}

QRect OverlayWidget::annotationSceneRect() const {
    return overlay::annotationSceneRect(m_selection.normalized(), dpr());
}

void OverlayWidget::setBarAlpha(int alpha) {
    // 只接受三档:设置校验已限定,这里再夹一次防越界值直接进来
    m_barAlpha = (alpha == kBarAlphaSolid || alpha == kBarAlphaMinimal) ? alpha : kBarAlphaHalf;
    update();
}

void OverlayWidget::cycleBarAlpha() {
    // 半透 → 极简 → 不透明 → 半透(默认档居中,两个方向都只需一次点击)
    const int next = m_barAlpha == kBarAlphaHalf      ? kBarAlphaMinimal
                     : m_barAlpha == kBarAlphaMinimal ? kBarAlphaSolid
                                                      : kBarAlphaHalf;
    setBarAlpha(next);
    // 持久化:同类工具的"记住上次选择"语义(贴图不透明度也是设置项)
    CoreBridge::instance().call("save_settings",
                                {{"captureBarAlpha", static_cast<int>(next)}});
}

QString OverlayWidget::barAlphaTip() const {
    if (m_barAlpha >= kBarAlphaSolid) return QStringLiteral("底板不透明(点按切换)");
    if (m_barAlpha <= kBarAlphaMinimal) return QStringLiteral("底板极简(尽量不挡原图)");
    return QStringLiteral("底板半透(点按切换)");
}
void OverlayWidget::setAnnoTool(EditorTool tool) {
    if (!m_engine) return;
    m_annoTool = tool;
    m_engine->setTool(tool);          // 内部会先把进行中的文字图元烘焙掉
    m_engine->setColor(m_annoColor);
    m_engine->setStrokeWidth(m_penSize);
    m_engine->setFontSize(m_fontSize);
    setCursor(toolCursor(tool));      // 切工具立刻换光标
    update();
}

void OverlayWidget::drawSubbar(QPainter& painter) const {
    if (!m_subbarVisible || m_subbarRect.isNull()) return;
    // ⚠️ 只画**底板**。条目(工具/颜色/粗细/字号/撤销重做)已换成共享控件
    //    AnnotationToolbar(§17.3 A),不再自绘 —— 自绘版的"绘制与命中测试各写
    //    一份"是反复错位的来源。控件本体带透明背景,底板由这里统一画,
    //    视觉随主浮条(barAlpha 三档)。
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x26, 0x27, 0x2E, barAlpha()));
    painter.drawRoundedRect(m_subbarRect, 8, 8);
}

void OverlayWidget::bakeSaveDialog(const QImage& image) {
    // 格式(PNG/JPEG/WebP)与白底、质量等细节统一交给 image_save,
    // 详见该头文件开头的三条规则。
    const QString base = QStringLiteral("Nextboard-%1").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString error;
    const QString saved = image_save::saveImageWithDialog(this, image, base, &error);
    if (saved.isEmpty() && !error.isEmpty())
        QMessageBox::warning(this, QStringLiteral("另存为"), error);
}

} // namespace nb