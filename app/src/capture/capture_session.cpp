#include "capture_session.hpp"

#include "note/note_registry.hpp"
#include "capture/overlay_geometry.hpp"
#include "editor/annotation_toolbar.hpp"
#include "host/core_bridge.hpp"
#include "host/image_save.hpp"
#include "panel/core_facade.hpp"
#include "host/memory_probe.hpp"
#include "host/app_paths.hpp"
#include "ui_theme.hpp"
#include "ui/ocr_result_panel.hpp"

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
#include <QDir>
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

// ---- 滚动长截图(docs/19 v2)的会话状态 ----
//
// 一次截图会话**最多一个**长截图会话 id。v2 不再用全局单例 id 承担"会话归属":
// 归属在核心侧由 begin() 固化(显示器 + 设备像素矩形 + 上限),这里只持有 id。
//
// 旧实现的两个缺陷随重构消失:
// - 全局 QString + 每遮罩各连 handler → 二块屏同时进长截图态会共用会话,
//   帧宽不符只 warn 后静默丢帧。现在第二块屏进入采集会被**显式拒绝**并提示。
// - 旁边那个从未被使用的 g_stitchScreenOf(声明了"按遮罩隔离会话"却没做)。
QString g_stitchSession;
/// 会话级只允许一个采集者:记下当前正在采的遮罩,别的遮罩想采就会被拒。
QPointer<OverlayWidget> g_stitchOwner;

/// 生成新会话 id(每次进入采集都重新分配,不复用旧 id)。
QString newStitchSession() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/// 丢弃长截图会话(核心侧缓冲一并释放)。幂等。
void abortStitchSession() {
    if (g_stitchSession.isEmpty()) {
        g_stitchOwner = nullptr;
        return;
    }
    CoreBridge::instance().stitchAbort(g_stitchSession);
    g_stitchSession.clear();
    g_stitchOwner = nullptr;
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
    // 标注偏好(颜色/粗细/字号):跨会话记忆。用户反馈"每次都退回红笔 4px,
    // 而底板不透明度却记得住" —— 口吻不一致,这里补齐。
    // ⚠️ data 在这段之后还可能被别的分支用,这里只读不消费。
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
        overlay->applyAnnotationPrefs(data);   // 标注偏好(颜色/粗细/字号)跨会话记忆
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

        // ---- 滚动长截图(docs/19 v2)的会话级接线 ----
        //
        // v2 的形态(docs/19 v2 §3.1):采集期**整块隐藏遮罩**,页面完全可见可交互;
        // UI 由 StitchHud(选区描边 + 「结束/取消」面板)承担。
        //
        // 为什么不沿用 v1 的"挖洞":窗口区域吃设备像素、选区是逻辑像素,150% 屏上
        // 洞会按 1:1 挖小 → 选区被遮罩盖住一部分 → 每帧都是静止画面 → 核心判
        // duplicate → 长图只有一屏高;而"挖洞"与"在选区上画 UI"本身互斥
        // (洞里画不出、也点不到),整屏选区时更会退化成空区域。
        {
            QScreen* stitchScreen = screen;
            OverlayWidget* stitchOverlay = overlay;
            QObject::connect(overlay, &OverlayWidget::stitchFrameRequested, overlay,
                    [stitchScreen, stitchOverlay] {
                // ⚠️ 任何提前 return 都必须复位 m_stitchSampling:漏一次节拍器就
                //    永久停摆(页面还在滚,长图却再也不更新)。用 RAII 守卫统一复位。
                struct SampleGuard {
                    OverlayWidget* w;
                    ~SampleGuard() { if (w) w->markSampleReturned(); }
                } guard{stitchOverlay};

                if (g_stitchSession.isEmpty()) {
                    qWarning() << "长截图:采帧请求到来但会话 id 为空(状态错乱?)";
                    return;
                }
                const QRect region = stitchOverlay->stitchRegionDevice();
                if (region.isEmpty()) {
                    qWarning() << "长截图:选区为空,跳过本帧";
                    return;
                }
                CoreBridge& core = CoreBridge::instance();

                // ---- 采集:直抓选区(不再整屏 + 裁剪) ----
                QImage frame;
                if (core.hasRegionCapture()) {
                    frame = core.captureMonitorRegion(stitchScreen, region);
                    if (frame.isNull()) {
                        // 配对失败/越界:硬失败要**说出来**,不能静默跳过 ——
                        // 静默跳过的表现是"长图不长了",用户无从判断原因。
                        qWarning().nospace() << "长截图:直抓选区失败 屏="
                                             << (stitchScreen ? stitchScreen->name() : "?")
                                             << " region=" << region
                                             << "(配对失败或越界,详见 nbcore.log)";
                        return;
                    }
                } else {
                    // 旧核心没有直抓导出:回退"整屏 + 裁剪"。配对失败**不再回退按索引**
                    // (索引在 Qt 与 xcap 两端口径不同,回退等于把 A 屏内容拼进 B 屏)。
                    const QImage full = core.captureMonitorMatched(stitchScreen);
                    if (full.isNull()) {
                        qWarning() << "长截图:整屏捕获失败(配对不上该屏),本帧跳过";
                        return;
                    }
                    frame = core.captureCrop(full, region);
                    if (frame.isNull()) {
                        qWarning() << "长截图:本帧裁剪失败" << region;
                        return;
                    }
                }

                // ---- 采集时序(始终记录)----
                //
                // 为什么必须记时序:真机日志里只有两种结果 ——
                //   ① run=955 k=0   (整帧逐行全等 = 两次采样之间画面没变)
                //   ② run=0 no-overlap(下一帧首行在上一帧里完全找不到 = 跨度超过整屏)
                // 中间没有任何"部分重叠"的位移。这只有两种成因:
                //   A 采样停摆 —— 两次采样间隔远大于 150ms,滚动在这一大段里走完了一屏;
                //   B 逐屏跳滚 —— 间隔正常,但应用一次滚动就跨了整屏。
                // 两者的处置完全不同(A 要修采集节拍,B 只能改提示语/选区高度),
                // 而它们靠**帧间隔**就能区分,故这一行必须无条件打印。
                static qint64 lastFrameNs = 0;
                static int frameSeq = 0;
                const qint64 nowNs = QDateTime::currentMSecsSinceEpoch() * 1000 * 1000;
                const qint64 dtMs = lastFrameNs == 0 ? -1 : (nowNs - lastFrameNs) / 1000000;
                lastFrameNs = nowNs;

                // 轻量变化度:与上一帧相比有多少像素不同(抽样,~1/64 像素)。
                // 用于区分"画面真的没变"与"变了但指纹判不出来"。
                static QImage lastThumb;
                int changedPctX100 = -1;
                if (!lastThumb.isNull() && lastThumb.size() == frame.size()) {
                    qint64 changed = 0, total = 0;
                    for (int y = 0; y < frame.height(); y += 8) {
                        const uchar* a = frame.constScanLine(y);
                        const uchar* b = lastThumb.constScanLine(y);
                        for (int x = 0; x < frame.width(); x += 8) {
                            ++total;
                            if (a[x * 4] != b[x * 4] || a[x * 4 + 1] != b[x * 4 + 1]) ++changed;
                        }
                    }
                    changedPctX100 = total > 0 ? static_cast<int>(changed * 10000 / total) : 0;
                }
                lastThumb = frame;

                qInfo().nospace() << "长截图[时序] #" << frameSeq++ << " dt=" << dtMs << "ms"
                                  << " 变化=" << (changedPctX100 < 0
                                                      ? QStringLiteral("首帧")
                                                      : QStringLiteral("%1.%2%")
                                                            .arg(changedPctX100 / 100)
                                                            .arg(changedPctX100 % 100, 2, 10,
                                                                 QLatin1Char('0')));
                // 交给遮罩:no-overlap 的提示要按"跨度超屏 / 采集卡顿 / 真的滚太快"分流
                if (stitchOverlay) stitchOverlay->setStitchFrameStats(changedPctX100, dtMs);

                // 可选:像素级体检(NB_STITCH_DIAG=1)。默认关闭 —— 它会额外跑一次整屏捕获,
                // 本身就会拖慢采样,开着的时候时序数据不具代表性。
                static int diagFrame = 0;
                if (qEnvironmentVariableIsSet("NB_STITCH_DIAG") && diagFrame < 12) {
                    const QString dir = nb::appDataDir() + QStringLiteral("/stitch-diag");
                    QDir().mkpath(dir);
                    const QString tag =
                        QStringLiteral("%1").arg(diagFrame, 2, 10, QLatin1Char('0'));
                    frame.save(dir + QStringLiteral("/direct-") + tag + QStringLiteral(".png"));
                    qInfo().nospace() << "长截图[体检] 落盘 direct-" << tag << ".png ->" << dir;
                    ++diagFrame;
                }

                // ---- 拼接 ----
                const QJsonObject r = core.stitchAppend(g_stitchSession, frame);
                if (r.isEmpty()) {
                    qWarning().nospace() << "长截图:核心拒绝本帧(会话" << g_stitchSession
                                         << " 帧=" << frame.width() << "x" << frame.height()
                                         << ")(原因见 nbcore.log)";
                    return;
                }
                const QString reason = r.value("reason").toString();
                const QJsonObject dbg = r.value("debug").toObject();
                qInfo().nospace() << "长截图: shift=" << r.value("shiftPx").toInt()
                                  << " total=" << r.value("totalHeight").toInt()
                                  << " gaps=" << r.value("gaps").toInt()
                                  << " bands=" << r.value("bands").toInt()
                                  << " reason=" << reason
                                  << " [k=" << dbg.value("k").toInt()
                                  << " run=" << dbg.value("run").toInt()
                                  << " evidence=" << dbg.value("evidence").toInt()
                                  << " overlap=" << dbg.value("overlap").toInt()
                                  << " thr=" << dbg.value("threshold").toInt()
                                  << " age=" << dbg.value("baselineAge").toInt() << "]";
                if (stitchOverlay) {
                    stitchOverlay->updateStitchProgress(r.value("totalHeight").toInt(),
                                                        r.value("gaps").toInt(),
                                                        r.value("bands").toInt(), reason);
                }
            });

            // 「结束」:取回长图 → 走与 annotatedConfirmed 相同的入库通道
            //
            // ⚠️ **必须转出当前信号栈**:本信号可能在面板点击处理里发出,而
            //    closeAllOverlays() 会 close 掉遮罩、HUD 也会跟着销毁;
            //    随后 PNG 编码 + 写库 + 写剪贴板全跑在同一栈上 —— 实测访问违例
            //    (0xc0000005,点「结束」后立即崩溃)。转一次事件循环既退出该栈,
            //    也让「合成中」的画面有机会刷出来。
            QObject::connect(overlay, &OverlayWidget::stitchFinishRequested, overlay,
                    [stitchOverlay = QPointer<OverlayWidget>(overlay)] {
                QTimer::singleShot(0, qApp, [stitchOverlay] {
                    const QString session = g_stitchSession;
                    if (session.isEmpty()) {
                        qWarning() << "长截图:没有可结束的会话";
                        return;
                    }
                    QImage composed;
                    QJsonObject meta;
                    const bool ok =
                        CoreBridge::instance().stitchFinish(session, &composed, &meta);
                    // 无论成败,会话都已在核心侧被取走/销毁 —— 清掉 id 以免二次 finish
                    g_stitchSession.clear();
                    g_stitchOwner = nullptr;
                    if (!ok || composed.isNull()) {
                        qWarning() << "长截图:合成失败(会话" << session << ")";
                        return;
                    }
                    const int gaps = meta.value("gaps").toInt();
                    const int bands = meta.value("bands").toInt();
                    if (gaps > 0) {
                        qWarning().nospace() << "长截图:有 " << gaps << " 处缺口(滚动过快漏内容),"
                                             << " 已插入 " << bands << " 条警示条";
                    }
                    qInfo().nospace() << "长截图:完成 " << composed.width() << "x"
                                      << composed.height() << " gaps=" << gaps
                                      << " bands=" << bands
                                      << " monitor=" << meta.value("monitor").toString();
                    // 把成品转发给遮罩的 stitchFinished:验收据此断言最终尺寸,
                    // 产品侧暂无订阅者(入库走下面的 finishCapture)。
                    if (stitchOverlay) stitchOverlay->reportStitchFinished(composed);
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

        // 选区边框 = theme.md §21「2px Accent」。取主题 accent(而非写死 #50A0FF):
        // 深/浅两套主题的 accent 对比度不同,写死一个值总有一套显得发灰。
        painter.setPen(QPen(ui::themeTokens().accent, 2));
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
            painter.setPen(QPen(ui::themeTokens().accent, 1));
            painter.setBrush(QColor(255, 255, 255));
            for (const QPoint& point : handlePoints(sel)) {
                painter.drawRect(QRect(point.x() - kHandleSize / 2,
                                       point.y() - kHandleSize / 2, kHandleSize, kHandleSize));
            }
        }

        // 尺寸提示按**物理像素**显示,即最终截出来的分辨率。
        // ⚠️ 这一行文字压在**冻结底图**上(不是压在主题面板上),底图颜色由用户桌面
        //    决定 —— 因此**刻意不跟主题**:白字 + 深色底纹才是任意底色上都读得清的组合。
        //    改成主题前景色,在浅色页面上会直接消失。
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
        // 同上:空态提示压在底图上,固定白字(理由见尺寸提示那条)。
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
    hideHoverTip();   // 浮条马上要消失,别留一条孤立的提示
    m_origin = e->pos();
    m_selection = QRect(m_origin, QSize(0, 0));
    m_selecting = true;
    update();
}

void OverlayWidget::mouseMoveEvent(QMouseEvent* e) {
    emit activity();
    // 拖拽/绘制过程中的移动一律不清高亮之外的状态;高亮本身只在"悬停浮条"分支里
    // 设置,下面每个提前 return 的分支都会把光标移出浮条 —— 统一在这里清一次即可。
    if (m_selecting || m_adjusting || m_draggingStroke) setHoverButton(-1);
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
    if (toolbarContains(e->pos())) {
        // 悬停高亮:按**具体按钮**高亮,整条一起亮等于没告诉用户"点哪儿"。
        setHoverButton(toolbarButtonAt(e->pos()));
        setCursor(Qt::PointingHandCursor);
        return;
    }
    // 离开浮条 → 清掉高亮(否则光标移到选区里,浮条上还留着一块亮色)
    setHoverButton(-1);
    if (subbarWidgetContains(e->pos())) {
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
        return;
    }

    // ---- 标注态撤销/重做(docs/17 §17.3 A 漏项)----
    //
    // 二级条上一直有撤销/重做按钮,但**键盘没有任何入口**:而标注是"一手鼠标一手
    // 快捷键"的操作,画错一笔还得移回二级条点按钮,手感很差。编辑器侧(Ctrl+Z /
    // Ctrl+Shift+Z / Ctrl+Y)早已支持,这里补齐同一套,免得两边不一致。
    //
    // ⚠️ 必须在**标注态**才处理:非标注态的 Ctrl+Z 属于别的语义(将来若有),
    //    这里不该吞掉按键。
    if (m_engine && (e->modifiers() & Qt::ControlModifier)) {
        const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);
        if (e->key() == Qt::Key_Z) {
            if (shift) m_engine->undoStack()->redo();
            else m_engine->undoStack()->undo();
            update(m_selection.normalized().adjusted(-2, -2, 2, 2));
            return;
        }
        if (e->key() == Qt::Key_Y) {
            m_engine->undoStack()->redo();
            update(m_selection.normalized().adjusted(-2, -2, 2, 2));
            return;
        }
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

/// 按钮说明(悬停停留后显示)。与 toolbarButtonText 的索引一一对应。
///
/// 为什么需要:「透明」「贴图」这类两字标签**不自明** —— 用户不知道"透明"是
/// 调底板档位、"贴图"是钉到屏幕。二级条是真控件所以有 tooltip,主浮条是自绘的,
/// 一直没有(barAlphaTip() 写了却从没人调用,是典型的"想做但没接上")。
QString toolbarButtonTipText(int index) {
    switch (index) {
        case 0: return QStringLiteral("标注:在选区内就地画笔/箭头/文字");
        case 1: return QStringLiteral("OCR 取字:识别选区文字并复制到剪贴板");
        case 2: return QStringLiteral("长截图:滚动采集(选区转为采集窗口)");
        case 3: return QStringLiteral("贴图:把选区钉在屏幕上");
        case 4: return QStringLiteral("保存:选区另存为图片文件");
        case 5: return QStringLiteral("底板不透明度:不透明 / 半透 / 极简 循环");
        case 6: return QStringLiteral("完成:入库并写剪贴板 (Enter)");
        default: return QStringLiteral("取消本次截图 (Esc)");
    }
}

/// 悬停停留多久才弹提示(毫秒)。
///
/// 取 700:浮条是一次性扫过的目标,鼠标划过每个按钮都弹提示会把屏幕刷满;
/// 而 200~300ms 又太容易"手抖触发"。700ms 的含义是"主动停下想看清这是什么"。
constexpr int kHoverTipDelayMs = 700;

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

QRect OverlayWidget::toolbarButtonRect(int index) const {
    const int count = overlay::kToolbarButtons;
    if (index < 0 || index >= count) return QRect();
    // 均分。**余数给最后一个按钮**:rc.width() 未必被 count 整除(见 overlay_geometry
    // 的 kToolbarWidth 说明),按整数除法算宽度时最右侧会缺几条像素 —— 那几条属于
    // 最后一个按钮,绘图与命中都按同一个算式取,点它才不会有"差 1px 点不到"的死角。
    const int base = m_toolbarRect.width() / count;
    const int x = m_toolbarRect.x() + index * base;
    const int w = (index == count - 1) ? (m_toolbarRect.right() - x + 1) : base;
    return QRect(x, m_toolbarRect.y(), w, m_toolbarRect.height());
}

int OverlayWidget::toolbarButtonAt(const QPoint& pos) const {
    if (!toolbarVisible() || !m_toolbarRect.contains(pos)) return -1;
    const int count = overlay::kToolbarButtons;
    for (int i = 0; i < count; ++i) {
        if (toolbarButtonRect(i).contains(pos)) return i;
    }
    return -1;
}

QString OverlayWidget::toolbarButtonTip(int index) const {
    if (index < 0 || index >= overlay::kToolbarButtons) return QString();
    // 「透明」档位的说明跟着当前档位走(barAlphaTip 终于有了调用点):
    // 用户点一下想知道"现在是哪一档",光说"循环切换"没用。
    if (index == 5) return barAlphaTip();
    // 「OCR取字」进行中:说明还要告诉用户"为什么点不动"
    if (index == 1 && m_ocrPending) return QStringLiteral("正在识别中…(完成前不能重复触发)");
    return toolbarButtonTipText(index);
}

void OverlayWidget::scheduleHoverTip(int index) {
    if (index < 0) {
        hideHoverTip();
        return;
    }
    if (m_hoverTipShownFor == index) return;   // 同一个按钮已经弹过,别重复弹
    if (!m_hoverTipTimer) {
        m_hoverTipTimer = new QTimer(this);
        m_hoverTipTimer->setSingleShot(true);
        m_hoverTipTimer->setInterval(kHoverTipDelayMs);
        connect(m_hoverTipTimer, &QTimer::timeout, this, [this] {
            if (m_hoverButton < 0) return;
            const QString tip = toolbarButtonTip(m_hoverButton);
            if (tip.isEmpty()) return;
            m_hoverTipShownFor = m_hoverButton;
            // 锚在按钮正上方;QToolTip 会自动夹进屏幕
            const QRect btn = toolbarButtonRect(m_hoverButton);
            QToolTip::showText(mapToGlobal(QPoint(btn.center().x(), btn.top() - 6)), tip,
                               this, QRect(), 6000);
        });
    }
    m_hoverTipTimer->start();
}

void OverlayWidget::hideHoverTip() {
    if (m_hoverTipTimer) m_hoverTipTimer->stop();
    m_hoverTipShownFor = -1;
}

void OverlayWidget::setHoverButton(int index) {
    if (m_hoverButton == index) return;
    m_hoverButton = index;
    // 换按钮 = 重新开始计时;移出浮条 = 立刻收起(提示不该跟着鼠标乱飘)
    scheduleHoverTip(index);
    if (m_toolbarRect.isNull()) {
        update();
        return;
    }
    // 只重绘浮条(含一点余量给悬停圆角),不整屏刷 —— 遮罩是整屏窗口,4K 上整屏
    // 重绘约 10ms,鼠标每动一格都刷会明显掉帧。
    update(m_toolbarRect.adjusted(-2, -2, 2, 2));
}

void OverlayWidget::drawToolbar(QPainter& painter) const {
    if (!toolbarVisible()) return;
    const ui::ThemeTokens& t = ui::themeTokens();
    const QRect rc = m_toolbarRect;
    const int count = overlay::kToolbarButtons;

    // ---- 底板:主题 surface + 用户可调不透明度(docs/17 §17.5)----
    // ⚠️ 底色**不再写死 #26272E**:那是"无论什么主题工具栏都是深色"的根因。
    //    底板取主题 surface,描边取主题 border —— 浅色主题下才是"白底 + 浅灰边"。
    const int radius = 8;   // theme.md §11:Panel 圆角 8
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(ui::panelBorder(), 1));
    painter.setBrush(ui::panelPlate(barAlpha()));
    painter.drawRoundedRect(QRectF(rc).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // 极简档(70% 不透明)下普通文字会"透到看不清" —— 提高对比度兜底。
    // 只用**纯黑/纯白**做兜底色:主题前景色本身就是最高对比档,再"提亮"只会
    // 变成同色。语义色(完成/取消/激活)不受影响,见下。
    const QColor baseFg = barNeedsContrastBoost()
                              ? (ui::isDarkTheme() ? QColor(0xFF, 0xFF, 0xFF)
                                                   : QColor(0x00, 0x00, 0x00))
                              : t.textPrimary;

    for (int i = 0; i < count; ++i) {
        const QRect btn = toolbarButtonRect(i);
        const QRect fill = btn.adjusted(2, 3, -2, -3);   // 高亮/激活底衬内缩,不顶到描边
        // 索引顺序(与 toolbarButtonText 一一对应):
        // 0 标注 / 1 OCR取字 / 2 长截图 / 3 贴图 / 4 保存 / 5 透明 / 6 完成 / 7 取消
        const bool active = (i == 0 && annotating()) || (i == 2 && stitchActive());

        if (active) {
            // 激活态 = theme.md §15 的 Selected:AccentSoft 底 + Accent 字。
            // 底板是半透的,故用 accent 低 alpha 衬底(等效 AccentSoft 叠在 surface 上)。
            painter.setPen(Qt::NoPen);
            painter.setBrush(ui::activeFill());
            painter.drawRoundedRect(fill, 6, 6);
        } else if (i == m_hoverButton) {
            // 悬停 = theme.md §15 的 Hover:SurfaceHover。同样用前景低 alpha 表达,
            // 深色下变"提亮"、浅色下变"压暗",一套算式两套主题都成立。
            painter.setPen(Qt::NoPen);
            painter.setBrush(ui::hoverFill(barNeedsContrastBoost() ? 40 : 28));
            painter.drawRoundedRect(fill, 6, 6);
        }

        QColor fg = baseFg;
        if (active) fg = t.accent;
        if (i == 6) fg = t.success;   // 完成:绿(语义色,主题内两套都保证对比度)
        if (i == 7) fg = t.error;     // 取消:红
        if (i == 5) fg = t.textSecondary;   // 透明档位:次要动作,弱化
        // 识别进行中:「OCR取字」置灰(它自己也会拒绝重入,这里是**看得见**的那一半)。
        // 只靠"点了没反应"用户会以为按钮坏了 —— 状态必须同时可见。
        if (i == 1 && m_ocrPending) fg = t.textDisabled;
        painter.setPen(fg);
        painter.drawText(btn, Qt::AlignCenter, toolbarButtonText(i));

        if (i < count - 1) {
            // 分隔线:主题边框色低 alpha。原来是白色 28α —— 浅色主题下等于看不见。
            painter.setPen(ui::withAlpha(t.borderStrong, 170));
            painter.drawLine(btn.topRight() + QPoint(0, 9),
                             btn.bottomRight() - QPoint(0, 9));
        }
    }
}

void OverlayWidget::handleToolbarClick(const QPoint& pos) {
    // 命中走 toolbarButtonAt —— 与 drawToolbar 共用同一份几何(见 toolbarButtonRect)。
    const int index = toolbarButtonAt(pos);
    if (index < 0) return;
    hideHoverTip();   // 已经点下去了,说明用户看懂了;提示别继续压在上面
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
        case 1: {  // OCR取字:异步(0.4~5s)
            // ---- 防重入 ----
            // 识别是 0.4~5s 的异步任务。此前没有任何进行中状态:连点 N 次就并发起
            // N 个识别,而每个 finished 回调都会 emit cancelled() —— 用户看到的是
            // "点一次取字,截图会话被取消好几次",还白烧 N 次推理(全屏 OCR 很贵)。
            if (m_ocrPending) return;
            m_ocrPending = true;
            // 面板定位基准在**此刻**固化(理由见 showOcrResultPanel 的注释)
            const QRect ocrAnchor = QRect(mapToGlobal(m_selection.normalized().topLeft()),
                                          m_selection.normalized().size());
            emit activity();
            update(m_toolbarRect.adjusted(-2, -2, 2, 2));   // 「OCR取字」按钮置灰
            auto* watcher = new QFutureWatcher<OcrTextWithLines>(this);
            // cropped 按值捕获:识别结果面板要保存它(图中取字的高亮底图),
            // 而本函数返回后就地销毁了选区状态。
            connect(watcher, &QFutureWatcher<OcrTextWithLines>::finished, this,
                    [this, watcher, cropped, ocrAnchor] {
                watcher->deleteLater();
                const OcrTextWithLines ocr = watcher->result();
                const QString& text = ocr.text;
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
                m_ocrPending = false;
                // ---- 结果**驻留**(与编辑器 showOcrResult 同一形态)----
                // 此前只用 QToolTip 提示 4 秒并立刻结束会话:用户既看不清整段文本,
                // 也没法只取其中几行 —— 同一个 OCR 能力在两处体验完全不同。
                // 现在弹一个可驻留、可鼠标选取的面板,用户读完自己关。
                showOcrResultPanel(text, notice, cropped, ocrAnchor, ocr.lines);
                update();
                emit cancelled();
            });
            connect(watcher, &QFutureWatcher<OcrTextWithLines>::canceled, watcher,
                    &QObject::deleteLater);
            watcher->setFuture(QtConcurrent::run([cropped]() -> OcrTextWithLines {
                return CoreBridge::instance().ocrImageDetailed(cropped);
            }));
            // 进行中提示仍走轻量 tooltip(此刻没有可操作的文本,弹面板只是噪音)
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


// ---- OCR 结果驻留面板 ----

void OverlayWidget::showOcrResultPanel(const QString& text, const QString& reason,
                                       const QImage& source, const QRect& anchorGlobal,
                                       const QVector<OcrLineBox>& lines) {
    // ⚠️ 调用点紧跟着 emit cancelled():遮罩会被 close() 销毁。因此这里
    //    **必须**用顶层窗口(内部 new 的 parent 是 nullptr),不能挂 this 当子控件
    //    —— 否则面板与遮罩同生共死,用户根本看不到结果。
    //
    // popup=false:工具窗口,点别处不会关。用户常常要对照着原图一段段挑文本,
    // 点一下就被关掉会很难用;关闭由用户自己点「关闭」。
    // anchor=this:定位时仍以遮罩(此刻还在)为基准,夹进屏幕可用区。
    // ⚠️ 基准是**选区**的全局矩形,不是 this。
    //    this 是整屏遮罩,frameGeometry() 就是整块屏 —— 传它会把面板摆到屏幕边角,
    //    而不是用户刚框的那块内容旁边(用户报的缺陷)。
    OcrResultPanel::showResult(text, reason, source, anchorGlobal, /*popup=*/false, lines);
}

// ---- 滚动长截图态(docs/19 §3.2)----

void OverlayWidget::enterStitching() {
    if (m_stitch != StitchState::Off) return;
    if (!selectionUsable()) return;   // 没有选区就没有采集窗口
    // 标注与长截图**互斥**:标注锚定在选区上,而长截图的选区是采集窗口。
    // 两者同时生效会让"画在选区里的笔画"跟着长图一起被滚走,语义混乱。
    //
    // ⚠️ 但**不能静默丢弃**:此前这里直接 exitAnnotating(),用户画完标注再点
    //    「长截图」,画的东西一声不响就没了。有标注时必须先问一句 —— 只在
    //    "真的有标注"时问,空引擎(进了标注态但没画)照旧静默退出。
    if (m_engine) {
        if (hasAnnotations()) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("长截图"),
                QStringLiteral("长截图会重新采集滚动内容,当前选区上的 %1 条标注将被丢弃。\n"
                               "要继续吗?")
                    .arg(m_engine->undoStack()->count()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) return;   // 用户反悔:留在标注态,什么也不变
        }
        exitAnnotating();
    }
    m_stitch = StitchState::Ready;
    m_stitchTotalHeight = 0;
    m_stitchGaps = 0;
    m_stitchBands = 0;
    m_stitchNotice = QStringLiteral("点「开始」后滚动页面,内容变化即自动采集");
    m_stitchPanelRect = stitchPanelRect();
    update();
    qInfo() << "长截图:进入就绪态,选区 =" << m_selection.normalized();
}

void OverlayWidget::exitStitching() {
    if (m_stitch == StitchState::Off) return;
    stopStitchSampling();
    setStitchOverlayHidden(false);
    m_stitch = StitchState::Off;
    m_stitchNotice.clear();
    m_stitchTotalHeight = 0;
    m_stitchGaps = 0;
    m_stitchBands = 0;
    m_stitchRegionDevice = QRect();
    update();
}

bool OverlayWidget::toggleStitchCapture() {
    if (m_stitch == StitchState::Off) return false;
    CoreBridge& core = CoreBridge::instance();

    if (m_stitch == StitchState::Ready) {
        // ---- 进入采集:先把"截哪块、什么上限"固化到核心 ----
        //
        // 顺序不能颠倒:begin 成功才隐藏遮罩。begin 失败时遮罩原样留着,
        // 用户还能改选区/取消 —— 而不是面对一个"界面不见了但什么都没发生"的局面。
        if (!core.hasStitch()) {
            m_stitchNotice = QStringLiteral("核心版本过旧,缺少长截图导出(nb_stitch_begin)");
            update();
            return false;
        }
        if (!g_stitchSession.isEmpty()) {
            // 会话级只允许一个采集者。第二块屏想采时必须**明确拒绝**:
            // 旧实现是共用全局 id,后一块屏的帧因宽度不符被核心拒绝后只 warn。
            m_stitchNotice = QStringLiteral("另一块屏正在采集,请先在那块屏点「结束」");
            update();
            qWarning() << "长截图:拒绝第二个采集者(已有会话" << g_stitchSession << ")";
            return false;
        }

        // 坐标真值:**设备像素、相对本屏左上角**。只用一次换算,采集期不再重算。
        m_stitchRegionDevice = selectionPhysical();
        if (m_stitchRegionDevice.isEmpty()) {
            m_stitchNotice = QStringLiteral("选区为空,无法开始");
            update();
            return false;
        }
        const QJsonObject settings = core.call(QStringLiteral("get_settings"));
        const QJsonObject data =
            settings.value("ok").toBool() ? settings.value("data").toObject() : QJsonObject{};
        const int maxHeight = data.value("stitchMaxHeight").toInt(30000);
        const qint64 maxBytes =
            static_cast<qint64>(data.value("stitchMaxBytes").toDouble(128.0 * 1024 * 1024));
        const int overlapRatio = data.value("stitchOverlapRatio").toInt(3);
        const int sampleMs = qBound(overlay::kStitchSampleMinMs,
                                    data.value("stitchSampleIntervalMs").toInt(
                                        overlay::kStitchSampleIntervalMs),
                                    overlay::kStitchSampleMaxMs);

        // 会话 id 先接住:begin 成功后它必须跨帧存活(采集 handler 用它 append)
        const QString session = newStitchSession();
        QString err;
        if (!core.stitchBegin(session, m_screen, m_stitchRegionDevice, maxHeight, maxBytes,
                              overlapRatio, &err)) {
            m_stitchNotice = QStringLiteral("无法开始长截图:%1").arg(err);
            update();
            qWarning() << "长截图:begin 失败," << err;
            return false;
        }
        g_stitchSession = session;
        g_stitchOwner = this;
        qInfo().nospace() << "长截图:会话" << session << " 选区(设备像素)="
                          << m_stitchRegionDevice
                          << " 屏=" << (m_screen ? m_screen->name() : QStringLiteral("?"))
                          << " dpr=" << dpr() << " 采样=" << sampleMs << "ms";

        m_stitch = StitchState::Capturing;
        m_stitchTotalHeight = 0;
        m_stitchGaps = 0;
        m_stitchBands = 0;
        m_stitchNotice = QStringLiteral("采集中…滚动页面;点「结束」或按 Esc 完成");

        // ---- 隐藏遮罩 + 立起 HUD,然后**等一轮事件循环**再采首帧 ----
        //
        // 顺序是硬要求:遮罩必须先真正消失,首帧才可能是"纯页面"。
        // 旧实现在同一个调用栈里先挖洞再同步采首帧,挖洞后的重绘是异步的,
        // 首帧常拿到遮罩残影,与后续帧无法匹配 → 后续全 no-overlap。
        setStitchOverlayHidden(true);
        if (!m_stitchHud) {
            m_stitchHud = new StitchHud(m_screen, m_stitchRegionDevice, dpr(), this);
            connect(m_stitchHud, &StitchHud::finishRequested, this,
                    [this] { toggleStitchCapture(); });
            connect(m_stitchHud, &StitchHud::abortRequested, this,
                    [this] { abortStitchFromPanel(); });
        }
        m_stitchHud->setStatus(QStringLiteral("准备采集…"));
        m_stitchHud->showHud();
        if (m_stitchHud->escUnavailable()) {
            m_stitchNotice = QStringLiteral("Esc 被占用,请用面板按钮收尾");
        }
        update();

        auto* timer = m_stitchTimer;   // 启动节拍(见 startStitchSampling)
        QTimer::singleShot(0, this, [this, timer, sampleMs] {
            if (!stitchCapturing()) return;   // 期间已被取消
            if (timer) timer->setInterval(sampleMs);
            startStitchSampling();
            // 首帧:遮罩已隐藏且至少经过一轮事件循环,此时抓到的是纯页面
            m_stitchSampling = true;
            emit stitchFrameRequested();
        });
        return true;
    }

    // ---- 采集中 → 结束:交给会话层收尾(它调核心 finish 并入库) ----
    m_stitch = StitchState::Ready;
    stopStitchSampling();
    if (m_stitchHud) {
        m_stitchHud->setStatus(QStringLiteral("正在合成…"));
        m_stitchHud->hideHud();
    }
    setStitchOverlayHidden(false);
    m_stitchNotice = QStringLiteral("正在合成…");
    update();
    emit stitchFinishRequested();
    return false;
}

/// 面板「取消」:丢弃已采集内容、退出长截图态(选区仍在,可重新点开始)。
void OverlayWidget::abortStitchFromPanel() {
    stopStitchSampling();
    if (m_stitchHud) {
        m_stitchHud->hideHud();
        m_stitchHud->deleteLater();
        m_stitchHud = nullptr;
    }
    setStitchOverlayHidden(false);
    m_stitch = StitchState::Off;
    m_stitchNotice.clear();
    m_stitchTotalHeight = 0;
    m_stitchGaps = 0;
    m_stitchBands = 0;
    m_stitchRegionDevice = QRect();
    update();
    emit stitchAborted();
}

void OverlayWidget::setStitchOverlayHidden(bool hidden) {
    m_stitchOverlayHidden = hidden;
    if (hidden) {
        // 隐藏前把焦点还给页面:遮罩持有焦点时,滚轮会被它吞掉(它已隐藏,但
        // 焦点残留会让下一轮键盘输入落到一个不可见窗口上)。
        clearFocus();
        hide();
        qInfo() << "长截图:遮罩已隐藏(页面可交互,UI 由 HUD 承担)";
    } else {
        show();
        raise();
        qInfo() << "长截图:遮罩已恢复";
    }
}

QRect OverlayWidget::stitchCaptureRect() const {
    return m_stitchRegionDevice.isEmpty() ? selectionPhysical() : m_stitchRegionDevice;
}

QRect OverlayWidget::stitchPanelRect() const {
    return overlay::stitchPanelRectFor(
        m_selection.normalized(), size(),
        QSize(overlay::kStitchPanelWidth, overlay::kStitchPanelHeight),
        overlay::kBarMargin);
}

bool OverlayWidget::stitchPanelContains(const QPoint& pos) const {
    return stitchActive() && !m_stitchOverlayHidden && m_stitchPanelRect.contains(pos);
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
    abortStitchFromPanel();
}

void OverlayWidget::leaveEvent(QEvent* e) {
    QWidget::leaveEvent(e);
    // 光标离开遮罩(切到别的屏 / 退到任务栏):浮条上不该留着高亮。
    setHoverButton(-1);
}

void OverlayWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    // 就绪态面板是算出来的几何,窗口尺寸变了要重算(采集期以 HUD 为准,与遮罩无关)
    if (stitchActive()) {
        m_stitchPanelRect = stitchPanelRect();
        update();
    }
}

void OverlayWidget::updateStitchProgress(int totalHeight, int gaps, int bands,
                                         const QString& reason) {
    m_stitchSampling = false;   // 本拍已回来,允许下一拍投递
    m_stitchTotalHeight = totalHeight;
    m_stitchGaps = gaps;
    m_stitchBands = bands;
    const qreal k = dpr();
    const QString heightText = QStringLiteral("已拼接 %1 px")
        .arg(qRound(totalHeight / (k > 0.01 ? k : 1.0)));
    if (reason == QLatin1String("no-overlap")) {
        // ⚠️ 这里以前一律写「滚动过快」,而真机上用户明明滚得很慢 —— 错的归因比不提示
        //    更糟:用户会照着提示放慢,然后仍然失败,最后认定"这功能坏了"。
        //
        // 真正的原因有三种,提示必须按**可观测的数值**分流:
        //  ① 相邻帧几乎全等(changedPct 很小)→ 跨度直接超过整屏,没有任何重叠可用;
        //  ② 帧间隔异常(远大于采样间隔)→ 采集卡顿,滚动在这一大段里走过去了;
        //  ③ 确实滚得快 → 原本的提示才成立。
        // 判据来自会话层刚记录的那一帧(MarkerFrame),见 capture_session.cpp。
        if (m_stitchLastChangedPct >= 0 && m_stitchLastChangedPct < 2) {
            m_stitchNotice = QStringLiteral(
                "这一屏换了整屏内容(无重叠):单次滚动跨度超过选区高度。"
                "请把选区拉高一些,或改用滚轮小幅滚动");
        } else if (m_stitchLastDtdMs > 1200) {
            m_stitchNotice = QStringLiteral(
                "采集出现卡顿(两帧间隔 %1 ms)。请重试;若反复出现请反馈日志")
                .arg(m_stitchLastDtdMs);
        } else {
            m_stitchNotice = QStringLiteral("滚动过快,有内容漏掉(缺口 %1)").arg(gaps);
        }
    } else if (reason == QLatin1String("backward")) {
        m_stitchNotice = QStringLiteral("已到顶部,无法继续向上拼接");
    } else if (reason == QLatin1String("limit")) {
        m_stitchNotice = QStringLiteral("已达高度上限,点「结束」生成");
    } else if (reason == QLatin1String("duplicate")) {
        // 未滚动:不打扰,保留上一次文案
    } else {
        m_stitchNotice = heightText;
    }
    // 采集期遮罩是隐藏的,文案必须送到 HUD —— 否则用户全程看不到任何进度
    if (m_stitchHud) m_stitchHud->setStatus(m_stitchNotice);
    if (!m_stitchOverlayHidden) update();
}

void OverlayWidget::startStitchSampling() {
    if (!m_stitchTimer) {
        m_stitchTimer = new QTimer(this);
        // 间隔默认取设置项(stitchSampleIntervalMs,默认 150ms);
        // 太快(低于 80ms)在 4K 上会把 CPU 吃满,太慢(高于 1000ms)快速滚动时
        // 相邻帧重叠不足 → 核心判 no-overlap → 长图缺行。
        m_stitchTimer->setInterval(overlay::kStitchSampleIntervalMs);
        // 定时拍:每拍请求一帧。核心按行指纹去重(duplicate 不占预算),
        // 所以"手停了还在拍"不会让长图膨胀,只是几行整数比对。
        connect(m_stitchTimer, &QTimer::timeout, this, [this] {
            if (!stitchCapturing()) return;   // 已结束/已取消:静默停拍
            // 上一拍投出去的请求若还没回来(核心慢),跳过本拍 —— 不要堆请求
            if (m_stitchSampling) return;
            m_stitchSampling = true;
            emit stitchFrameRequested();
        });
    }
    m_stitchSampling = false;
    m_stitchTimer->start();
    qInfo() << "长截图:开始采集,间隔" << m_stitchTimer->interval() << "ms";
}

void OverlayWidget::stopStitchSampling() {
    if (m_stitchTimer) m_stitchTimer->stop();
    m_stitchSampling = false;
}

void OverlayWidget::drawStitchPanel(QPainter& painter) const {
    // 采集期遮罩已隐藏,面板由 HUD 画;这里只负责**就绪态**(「开始」按钮)。
    if (!stitchActive() || m_stitchOverlayHidden) return;
    const ui::ThemeTokens& t = ui::themeTokens();
    const QRect rc = m_stitchPanelRect;

    // 底板与主浮条**同一套**(主题 surface + 用户档位 alpha + 1px border):
    // 长截图就绪态面板与浮条在同一次交互里先后出现,底色/圆角不一致会显得像两个控件。
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(ui::panelBorder(), 1));
    painter.setBrush(ui::panelPlate(barAlpha()));
    painter.drawRoundedRect(QRectF(rc).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int count = overlay::kStitchButtons;
    const int bw = rc.width() / count;
    for (int i = 0; i < count; ++i) {
        const QRect btn(rc.x() + i * bw, rc.y(), bw, rc.height());
        // 「开始」= success(推进动作),「取消」= error(丢弃)—— 语义色两套主题都成立。
        painter.setPen((i == 0) ? t.success : t.error);
        const QString label =
            (i == 0) ? QStringLiteral("开始") : QStringLiteral("取消");
        painter.drawText(btn, Qt::AlignCenter, label);
        if (i < count - 1) {
            painter.setPen(ui::withAlpha(t.borderStrong, 170));
            painter.drawLine(btn.topRight() + QPoint(0, 9),
                             btn.bottomRight() - QPoint(0, 9));
        }
    }

    if (!m_stitchNotice.isEmpty()) {
        // 就绪态:文案挂在面板下方。
        // ⚠️ 这段文字**不在面板底板上**,而是直接压在冻结底图上 —— 浅色页面上用
        //    主题前景色(浅色主题=深字)是对的,但深色页面上深字会消失。故按
        //    "是否已有面板底板"区分:有底板用主题正文色,没底板(浮在图上)用白字+描边。
        //    这里面板**总是**画了底板(见上),但文案在底板**外面** → 仍需自保证对比度。
        //    做法:先画深色描边再画白字,任意底图上都读得清(与尺寸提示同一手法)。
        const QRect textRect(rc.x() - 60, rc.bottom() + 6, rc.width() + 120, 20);
        painter.setPen(QColor(0, 0, 0, 160));
        for (const QPoint& d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)})
            painter.drawText(textRect.translated(d), Qt::AlignCenter, m_stitchNotice);
        painter.setPen(QColor(0xFF, 0xFF, 0xFF, 235));
        painter.drawText(textRect, Qt::AlignCenter, m_stitchNotice);
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
            saveAnnotationPrefs();   // 改了即记(与 cycleBarAlpha 同一口吻)
            if (m_engine) m_engine->setColor(c);
            update();
        });
        connect(m_subbar, &AnnotationToolbar::strokeWidthChanged, this, [this](int w) {
            m_penSize = w;
            saveAnnotationPrefs();   // 改了即记
            if (m_engine) m_engine->setStrokeWidth(w);
            update();
        });
        connect(m_subbar, &AnnotationToolbar::fontSizeChanged, this, [this](int px) {
            m_fontSize = px;
            saveAnnotationPrefs();   // 改了即记
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
    // 输入框跟随主题:底板 = 主题 surface(与浮条同一块面),描边 = accent(聚焦态),
    // 文字色 = 当前标注色(用户正在选的颜色就是输入内容的颜色,所见即所得)。
    // ⚠️ 原来是写死的深色 rgba(38,39,46,235):浅色主题下突然出现一块深色输入框。
    const ui::ThemeTokens& t = ui::themeTokens();
    edit->setStyleSheet(QStringLiteral("QLineEdit{background:rgba(%1,%2,%3,235);"
                                       "color:%4;border:1px solid %5;"
                                       "border-radius:3px;padding:2px 4px;"
                                       "selection-background-color:%5;selection-color:%6;}")
                            .arg(t.surface.red()).arg(t.surface.green()).arg(t.surface.blue())
                            .arg(m_annoColor.name(), t.accent.name(), t.accentText.name()));
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

void OverlayWidget::applyAnnotationPrefs(const QJsonObject& data) {
    // 容错优先:设置里可能是旧版本写的值、或手工改坏的值。这里**只取值、不抛错** ——
    // 一个坏值不该让用户无法截图。档位不合法就退回当前值(而 core 的 validate
    // 会把写入口挡住,所以真出现非法值说明是配置文件被手改过)。
    const QString color = data.value("annotationColor").toString();
    const QColor parsed(color);
    if (parsed.isValid()) m_annoColor = parsed;

    const int pen = data.value("annotationPenSize").toInt(m_penSize);
    if (strokeSizes().contains(pen)) m_penSize = pen;

    const int font = data.value("annotationFontSize").toInt(m_fontSize);
    if (textFontSizes().contains(font)) m_fontSize = font;
}

void OverlayWidget::saveAnnotationPrefs() {
    // 与 cycleBarAlpha 同一手法:单字段 save_settings(核心是深合并,
    // 只传要改的键,不会把其余设置清零 —— 见 core 的 merge_settings)。
    // ⚠️ 必须一次传**三个键**:颜色/粗细/字号的 UI 是三个互斥组,但语义上
    //    总是一起变化(用户换笔色时粗细往往也刚调过),分开传会多写两次盘。
    CoreBridge::instance().call("save_settings",
                                {{"annotationColor", m_annoColor.name()},
                                 {"annotationPenSize", m_penSize},
                                 {"annotationFontSize", m_fontSize}});
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
    //    视觉随主浮条(barAlpha 三档 + 主题 surface)。
    //
    // ⚠️ 底板颜色**必须与主浮条同源**:控件里的图标描边取的是 QPalette 的
    //    ButtonText(见 tool_icons.cpp iconInkFor),即**跟随主题**;底板若还写死
    //    深色,浅色主题下就是"浅底 + 深字"以外的错配 —— 深底配深图标,整条看不清。
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(ui::panelBorder(), 1));
    painter.setBrush(ui::panelPlate(barAlpha()));
    painter.drawRoundedRect(QRectF(m_subbarRect).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.setRenderHint(QPainter::Antialiasing, false);
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