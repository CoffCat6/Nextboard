#include "capture_session.hpp"
#include "editor/editor_window.hpp"
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
#include <QPointer>
#include <QToolTip>
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

/// regionSkipEditor(默认 true):框选确认后跳过编辑器,直接入库 + 写剪贴板。
bool skipEditor() {
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return true;   // 读不到设置时按默认(直通)
    return resp.value("data").toObject().value("regionSkipEditor").toBool(true);
}

/// regionAutoConfirm(默认 true):框选松开鼠标即确认;false 则选区保留、可用 8 向
/// 手柄微调,需 Enter 或双击确认(第一代 F10 的两种确认方式)。
/// regionRightClickCancel(默认 true):框选时右键取消本次截图。
} // namespace

void startCaptureSession(CaptureIntent intent, CaptureDone onCaptured, bool allowEditor) {
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
                [frozen, allowEditor](const QRect& physicalRect) {
            const CaptureDone done = g_onCaptured;   // closeAllOverlays 会清空,先取出
            // 任一屏确认即收起全部遮罩,并**等它们真正析构后**再裁剪/入库/建编辑器
            // (原因见 closeAllOverlays 的注释:否则整屏底图 + 后备缓冲会陪跑整条链)。
            closeAllOverlays([frozen, physicalRect, done, allowEditor] {
                const QImage cropped =
                    CoreBridge::instance().captureCrop(frozen, physicalRect);
                if (cropped.isNull()) {
                    qWarning() << "选区裁剪失败(空选区或越界)";
                    return;
                }
                if (!allowEditor || skipEditor()) {
                    finishCapture(cropped, done);
                    return;
                }
                // regionSkipEditor=false:先进编辑器标注,「完成」时才入库 + 写剪贴板
                auto* editor = new EditorWindow(cropped);
                QObject::connect(editor, &EditorWindow::committed, editor,
                        [editor, done](const QImage& composed) {
                    // finish() 里 close() 只是 deleteLater:此刻编辑器仍持底图 +
                    // 合成输出 + 窗口后备缓冲。等它真析构完再入库,峰值里就不含这一份。
                    runAfterDestroyed(editor, [done, composed] {
                        finishCapture(composed, done);
                    });
                });
                QObject::connect(editor, &EditorWindow::cancelled, editor,
                                 [] { qInfo() << "截图编辑已取消"; });
                // 「贴图」:编辑器只发合成图;贴图窗口归宿主(软上限/淘汰/设置读取
                // 都在 pinComposedImage 一条通道,不在编辑器里绕过)。
                // 决策(docs/14 §15.6-4):贴图后编辑器自己关闭 —— 信号在 close()
                // 之前发出,这里等析构完再贴,避免编辑器底图陪跑。
                QObject::connect(editor, &EditorWindow::pinRequested, editor,
                        [editor](const QImage& composed) {
                    runAfterDestroyed(editor, [composed] { pinComposedImage(composed); });
                });
                // 「OCR 取字」:识别与弹层都在编辑器内;宿主只记日志。
                // 不入库、不写剪贴板(复制能力预留,docs/14 §15.6-2)。
                QObject::connect(editor, &EditorWindow::ocrFinished, editor,
                                 [](const QString&, const QString&) {
                    qInfo() << "截图 OCR 取字完成";
                });
                editor->show();
                editor->raise();
                editor->activateWindow();
            });
        });
        QObject::connect(overlay, &OverlayWidget::cancelled, overlay, [] {
            closeAllOverlays();
            qInfo() << "截图已取消";
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

void startDelayedCapture(int seconds, CaptureIntent intent, CaptureDone onCaptured,
                         bool allowEditor) {
    if (seconds <= 0) {
        startCaptureSession(intent, std::move(onCaptured), allowEditor);
        return;
    }
    auto* overlay = new CountdownOverlay(seconds);
    QPointer<CountdownOverlay> guard(overlay);
    overlay->show();
    QTimer::singleShot(seconds * 1000, [guard, intent, onCaptured, allowEditor] {
        // 先收起倒计时(否则数字会被冻进底图),再等窗口真正消失后采屏
        if (guard) guard->close();
        QTimer::singleShot(80, [intent, onCaptured, allowEditor] {
            startCaptureSession(intent, onCaptured, allowEditor);
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

        // 微调模式:画 8 个手柄(提示可拖动调整)
        if (!m_autoConfirm && selectionUsable()) {
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

        // 浮条最后画(压在手柄/选区之上)
        drawToolbar(painter);
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

    // 浮条命中:**先于一切** —— 点按钮走动作,点浮条空白不当作重新框选
    if (toolbarContains(e->pos())) {
        m_toolbarAction = true;
        handleToolbarClick(e->pos());
        return;
    }
    // 点到浮条外:收起浮条(允许直接重新框选),见下方起新选区逻辑
    if (toolbarVisible()) hideToolbar();

    // 微调模式:已有选区时,按在手柄/内部上就进入调整,而不是重新框选
    if (!m_autoConfirm && !m_selection.isNull()) {
        const Handle handle = handleAt(e->pos());
        if (handle != Handle::None) {
            m_adjusting = true;
            m_activeHandle = handle;
            m_adjustStartRect = m_selection.normalized();
            m_adjustStartPos = e->pos();
            return;
        }
    }

    // 起新选区
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
    if (m_adjusting) {
        applyAdjustment(e->pos());
        update();
        return;
    }
    if (toolbarVisible() && m_selection.isNull()) hideToolbar();
    // 悬停时给出光标反馈(仅微调模式有意义)
    if (!m_autoConfirm && !m_selection.isNull()) {
        setCursor(cursorForHandle(handleAt(e->pos())));
    }
}

void OverlayWidget::mouseReleaseEvent(QMouseEvent* e) {
    emit activity();
    if (e->button() != Qt::LeftButton) return;

    if (m_adjusting) {
        m_adjusting = false;
        m_activeHandle = Handle::None;
        if (toolbarVisible()) showToolbar();   // 选区变了,浮条跟随
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
    emit regionConfirmed(selectionPhysical());
}

void OverlayWidget::keyPressEvent(QKeyEvent* e) {
    emit activity();
    if (e->key() == Qt::Key_Escape) {
        emit cancelled();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (!m_autoConfirm && selectionUsable()) emit regionConfirmed(selectionPhysical());
    }
}

// ---- 框选后浮出工具栏(docs/14 §16;仅 autoConfirm=false 生效)----

namespace {

constexpr int kTbHeight = 44;          // 浮条高(逻辑像素)
constexpr int kTbMargin = 8;           // 离选区底边的间距
constexpr int kBtnCount = 6;           // 标注/OCR/贴图/保存/完成/取消

/// 按钮文案与索引一一对应;宽度按文字自适应由 draw 时计算,这里给最小宽。
QString toolbarButtonText(int index) {
    switch (index) {
        case 0: return QStringLiteral("标注");
        case 1: return QStringLiteral("OCR取字");
        case 2: return QStringLiteral("贴图");
        case 3: return QStringLiteral("保存");
        case 4: return QStringLiteral("完成");
        default: return QStringLiteral("取消");
    }
}

} // namespace

bool OverlayWidget::toolbarVisible() const {
    return !m_autoConfirm && selectionUsable();
}

QRect OverlayWidget::toolbarRect() const {
    const QRect sel = m_selection.normalized();
    const int w = 92 * kBtnCount;      // 均分按钮宽(文字短,固定宽最省事且不跳动)
    const int h = kTbHeight;
    int x = sel.center().x() - w / 2;
    int y = sel.bottom() + kTbMargin;
    // 越出屏幕底部 → 翻到选区上方
    if (y + h > height() - 4) y = sel.top() - kTbMargin - h;
    // 左右夹在屏内
    x = qBound(4, x, width() - w - 4);
    return QRect(x, y, w, h);
}

void OverlayWidget::showToolbar() {
    m_toolbarRect = toolbarRect();
    update();
}

void OverlayWidget::hideToolbar() {
    m_toolbarRect = QRect();
    update();
}

bool OverlayWidget::toolbarContains(const QPoint& pos) const {
    return toolbarVisible() && m_toolbarRect.contains(pos);
}

void OverlayWidget::drawToolbar(QPainter& painter) const {
    if (!toolbarVisible()) return;
    const QRect rc = m_toolbarRect;
    // 底板
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x26, 0x27, 0x2E, 242));
    painter.drawRoundedRect(rc, 8, 8);
    // 按钮均分;完成/取消给强调/弱化色,其余普通
    const int bw = rc.width() / kBtnCount;
    for (int i = 0; i < kBtnCount; ++i) {
        const QRect btn(rc.x() + i * bw, rc.y(), bw, rc.height());
        QColor fg = QColor(0xDF, 0xE1, 0xE8);
        if (i == 4) fg = QColor(0x40, 0xC0, 0x57);   // 完成:绿
        if (i == 5) fg = QColor(0xE0, 0x31, 0x31);   // 取消:红
        painter.setPen(fg);
        painter.drawText(btn, Qt::AlignCenter, toolbarButtonText(i));
        if (i < kBtnCount - 1) {
            painter.setPen(QColor(255, 255, 255, 28));
            painter.drawLine(btn.topRight() + QPoint(0, 8),
                             btn.bottomRight() - QPoint(0, 8));
        }
    }
}

void OverlayWidget::handleToolbarClick(const QPoint& pos) {
    if (!toolbarContains(pos)) return;
    const QRect rc = m_toolbarRect;
    const int bw = rc.width() / kBtnCount;
    const int index = qBound(0, (pos.x() - rc.x()) / bw, kBtnCount - 1);
    m_toolbarAction = false;
    switch (index) {
        case 4:   // 完成:走既有确认通道(入库+剪贴板)
            emit regionConfirmed(selectionPhysical());
            return;
        case 5:   // 取消
            emit cancelled();
            return;
        case 0:   // 标注:S3 接 AnnotationEngine;本期按下不动作
            return;
        default:
            break;
    }
    // OCR/贴图/保存:输入 = 选区图(未标注时零 compose 开销,docs/14 §16.3-6)
    const QImage cropped = m_frozen.copy(selectionPhysical());
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
                // 弹层展示(v1 只展示;复制预留)。结束后关闭会话。
                const QPoint gp = mapToGlobal(QPoint(m_toolbarRect.x(), m_toolbarRect.y() - 8));
                QToolTip::showText(gp, text.isEmpty()
                    ? QStringLiteral("未识别到文字(图片可能不含可识别文本)")
                    : text, this, QRect(), 8000);
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
