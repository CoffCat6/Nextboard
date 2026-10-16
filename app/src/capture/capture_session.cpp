#include "capture_session.hpp"
#include "capture/overlay_geometry.hpp"
#include "editor/annotation_toolbar.hpp"
#include "host/core_bridge.hpp"
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
#include <QFileDialog>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>

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
    // 会话结束即停表:遮罩已释放,不再需要空闲保护
    if (g_idleTimer) g_idleTimer->stop();
    if (overlays.isEmpty()) {
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
QString ingestImageAndCopy(const QImage& image) {
    CoreBridge& core = CoreBridge::instance();
    const QString pngBase64 = quickPngBase64(image);
    const QJsonObject resp = core.call("ingest_capture_png", {
        {"png_base64", pngBase64},
        {"source", "capture"},
    });
    if (!resp.value("ok").toBool()) {
        qWarning() << "截图入库失败:"
                   << resp.value("error").toObject().value("message").toString();
        return {};
    }
    // entry 为 null = 超过图片大小上限被忽略
    const QString id =
        resp.value("data").toObject().value("entry").toObject().value("id").toString();
    if (id.isEmpty()) {
        // 未入库,但**仍要写剪贴板**:否则用户按了截图热键却"毫无反应"
        // (第一代同语义:超限时"未入库,仅写剪贴板")。
        qInfo() << "截图超过大小上限,未入库;仅写入剪贴板";
        const QJsonObject direct = core.call("copy_image_png", {{"png_base64", pngBase64}});
        if (!direct.value("ok").toBool())
            qWarning() << "超限截图写剪贴板失败:"
                       << direct.value("error").toObject().value("message").toString();
        return {};
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
    const QString id = ingestImageAndCopy(image);
    if (!id.isEmpty() && done) done(id);
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
        auto* overlay = new OverlayWidget(frozen, i, screen, autoConfirm, rightClickCancel);
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
                             bool rightClickCancel, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
      m_frozen(std::move(frozen)),
      m_screenIndex(screenIndex),
      m_screen(screen),
      m_autoConfirm(autoConfirm),
      m_rightClickCancel(rightClickCancel) {
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
        if (!m_autoConfirm && !m_engine && selectionUsable()) {
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

        // 二级条再浮条(二级条在上层)
        drawToolbar(painter);
        drawSubbar(painter);
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
    // 直通模式:松开即确认;微调模式:保留选区 → 浮出工具栏(docs/14 §16)
    if (m_autoConfirm) {
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
        emit cancelled();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
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
        case 2: return QStringLiteral("贴图");
        case 3: return QStringLiteral("保存");
        case 4: return QStringLiteral("透明");   // 底板不透明度三档循环(docs/17 §17.5)
        case 5: return QStringLiteral("完成");
        default: return QStringLiteral("取消");
    }
}

} // namespace

bool OverlayWidget::toolbarVisible() const {
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
        if (i == 0 && annotating()) fg = accent.isValid() ? accent : QColor(0x1C, 0x7E, 0xD6);
        if (i == 5) fg = accent.isValid() ? accent : QColor(0x40, 0xC0, 0x57);   // 完成:绿
        if (i == 6) fg = accent.isValid() ? accent : QColor(0xE0, 0x31, 0x31);   // 取消:红
        if (i == 4) fg = accent.isValid() ? accent : baseFg;                     // 透明档位
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
        case 4:   // 透明:底板不透明度三档循环(即时生效,不结束会话)
            cycleBarAlpha();
            return;
        case 5:   // 完成
            confirmSelection();
            return;
        case 6:   // 取消
            emit cancelled();
            return;
        case 0:   // 标注:进入/退出标注态(S3)
            if (annotating()) exitAnnotating();
            else enterAnnotating();
            m_toolbarRect = toolbarRect();   // 布局可能因二级条显隐变化
            update();
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
        case 2:    // 贴图
            pinComposedImage(cropped);
            emit cancelled();
            return;
        case 3: {  // 保存
            bakeSaveDialog(cropped);
            emit cancelled();
            return;
        }
        default:
            return;
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
    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath(QStringLiteral("Nextboard-%1.png")
                          .arg(QDateTime::currentDateTime().toString(
                              QStringLiteral("yyyyMMdd-HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("另存为"), suggested, QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) return;
    if (!image.save(path, "PNG"))
        QMessageBox::warning(this, QStringLiteral("另存为"),
                             QStringLiteral("写入失败:%1").arg(path));
}

} // namespace nb