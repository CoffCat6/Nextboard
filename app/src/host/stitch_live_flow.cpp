// 长截图**真实采集**验收(--stitch-live-flow)。
//
// 为什么必须单独有这个流程:--stitch-flow 用构造帧验证算法,`--capture-toolbar-flow`
// 验证状态机与窗口区域,但两者都**没有验证真正的采集链路**:
//
//   遮罩挖洞 → 下层窗口真的可见吗? → 截图抓到的是页面还是遮罩?
//   → 页面滚动后抓到的内容真的变了吗?
//
// 这条链路一旦断了,长截图的表现是"每帧都一样 → 核心判 duplicate → 长图只有
// 一屏高"。用户看到的就是"没有正常拼接",而日志里全是 ok / duplicate —— 完全
// 看不出哪里错了。所以这里用一个**真的会滚的窗口**做端到端取证。
//
// 日志前缀 STITCH-LIVE,失败行以 FAIL 开头。

#include "host/acceptance.hpp"

#include "capture/capture_session.hpp"
#include "capture/overlay_geometry.hpp"
#include "host/core_bridge.hpp"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QTimer>

#include <memory>

namespace nb {
namespace {

int g_liveFailures = 0;

void liveCheck(bool ok, const QString& what) {
    if (ok) {
        qInfo() << "STITCH-LIVE   PASS" << what;
    } else {
        ++g_liveFailures;
        qWarning() << "STITCH-LIVE   FAIL" << what;
    }
}

/// 一个**真的会滚动**的页面替身。
///
/// 内容按"内容坐标"着色,滚动只改偏移 —— 于是"屏幕上某点的颜色"唯一确定了
/// "页面滚到了哪里"。测试因此可以判断"截到的是滚动后的内容"而不是"冻结画面"。
class LivePage : public QWidget {
public:
    explicit LivePage(QWidget* parent = nullptr)
        // ⚠️ 必须是**普通顶层窗口**且 show + raise + activate:
        //    早先写成 Qt::Tool 且未激活,结果它根本没被合成到屏幕上 ——
        //    整屏抓到的是一片纯色,测试因此把"页面没显示"误报成"挖洞失效"。
        //    这类假失败比漏测更糟:会把排查引向错误的代码位置。
        // ⚠️ 必须置顶:否则桌面上的其它应用(开发时实测是宿主 GUI 自己)
        //    会盖住这块区域,采到的是别人的像素 —— 测试把"环境遮挡"误报成
        //    "挖洞失效",排查会跑偏。置顶后,遮罩(同样置顶且后 raise)仍在它上面,
        //    层级关系与真实使用一致。
        : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_OpaquePaintEvent);   // 不透明:便于判定"抓到的是页面"
        // ⚠️ 必须关掉 Qt 的背景自动填充:QWidget 默认会用调色板 base 色刷一遍,
        //    覆盖 paintEvent 之前的内容 —— 实测抓到一片 #ffffff。
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setWindowTitle(QStringLiteral("STITCH-LIVE-PAGE"));
    }

    /// 内容坐标 -> 颜色。刻意让相邻两行差异明显(红通道随 y 变化),
    /// 这样"滚动 60px"与"没滚动"在采样点上一定不同。
    static QColor colorAt(int contentY) {
        const int r = (contentY * 3) % 256;
        const int g = (contentY / 7) % 256;
        return QColor(r, g, 128);
    }

    /// 页面内容总高(足够长,测试只滚几百像素)
    static constexpr int kContentHeight = 4000;

    void scrollBy(int dy) {
        m_offset = qBound(0, m_offset + dy, kContentHeight - height());
        update();
    }
    int offset() const { return m_offset; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        // 逐行**填充**(不是画线):drawLine 在部分后端上会有取整/丢行问题,
        // 而且 QWidget 默认先刷背景 —— 必须自己把每一行涂满,否则抓到的是空白。
        for (int y = 0; y < height(); ++y) {
            const int contentY = y + m_offset;
            p.fillRect(0, y, width(), 1, colorAt(contentY));
        }
    }

private:
    int m_offset = 0;
};

void sendPress(QWidget* w, const QPoint& p) {
    QMouseEvent e(QEvent::MouseButtonPress, p, w->mapToGlobal(p),
                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}
void sendRelease(QWidget* w, const QPoint& p) {
    QMouseEvent e(QEvent::MouseButtonRelease, p, w->mapToGlobal(p),
                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}
void sendMove(QWidget* w, const QPoint& p, Qt::MouseButtons btns) {
    QMouseEvent e(QEvent::MouseMove, p, w->mapToGlobal(p),
                  Qt::NoButton, btns, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}

} // namespace

void runStitchLiveFlow() {
    QTimer::singleShot(1200, qApp, [] {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qWarning() << "STITCH-LIVE   FAIL 无可用屏幕";
            QApplication::quit();
            return;
        }
        const QRect geo = screen->geometry();
        CoreBridge& core = CoreBridge::instance();
        if (!core.hasStitch()) {
            qWarning() << "STITCH-LIVE   FAIL nbcore 没有长截图导出";
            QApplication::quit();
            return;
        }

        // ---- 下层页面:真的会滚 ----
        auto* page = new LivePage;
        const QRect pageRect(geo.left() + 60, geo.top() + 60,
                             qMin(900, geo.width() - 120), qMin(700, geo.height() - 120));
        page->setGeometry(pageRect);
        page->show();
        page->raise();
        page->activateWindow();
        // ⚠️ 不要用 QThread::msleep 等"合成上去":它阻塞主线程,窗口恰恰因此
        //    得不到重绘 —— 实测导致偶发访问违例(0xc0000005)与采到桌面背景。
        //    正确做法是交给事件循环:processEvents 若干轮 + 让窗口自己 commit。
        for (int i = 0; i < 8; ++i) {
            QApplication::processEvents();
            QThread::msleep(20);   // 短睡让合成线程有机会出帧;40ms 总时长远小于一次超时
            QApplication::processEvents();
        }
        qInfo() << "STITCH-LIVE  page" << pageRect << "offset" << page->offset();

        // ---- 走**真实入口**起会话(CaptureIntent::Scrolling)----
        //
        // ⚠️ 早先这里自己 new OverlayWidget,结果**绕过了 startCaptureSession 里的
        //    信号连接** —— 采集槽根本没接上,日志里连"槽进入"都没有,于是
        //    "没采到帧"被误报成产品缺陷,排查一路跑偏。
        //    验收必须走产品同一条入口,否则验的是测试自己搭的假链路。
        startCaptureSession(CaptureIntent::Scrolling);
        QApplication::processEvents();

        // startCaptureSession 内部每屏建一块遮罩并 showFullScreen。
        // 取出本屏那块(测试只关心光标所在屏,即 primaryScreen 那块)。
        // ⚠️ 多屏时会为**每屏**各建一块遮罩。必须挑"本屏那块"(screen 相同),
        //    否则后续在该遮罩上框选、采帧都发生在别的屏上 —— 采样点对不上,
        //    日志里出现 1080x1920 之类的怪尺寸(实测踩过,排查跑偏很久)。
        OverlayWidget* overlay = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* o = qobject_cast<OverlayWidget*>(w);
            if (o && o->screen() == screen) { overlay = o; break; }
        }
        if (!overlay) {
            qWarning() << "STITCH-LIVE   FAIL 会话未建立遮罩";
            QApplication::quit();
            return;
        }
        overlay->raise();
        QApplication::processEvents();

        // ---- 框选一块区域(全在页面内)----
        const QRect sel(pageRect.left() + 80, pageRect.top() + 80, 400, 300);
        sendPress(overlay, sel.topLeft());
        sendMove(overlay, sel.bottomRight(), Qt::LeftButton);
        sendRelease(overlay, sel.bottomRight());
        QApplication::processEvents();

        liveCheck(overlay->stitching(), QStringLiteral("框完进入长截图态(热键意图)"));

        // ---- 点「开始」:进入采集(v2:遮罩隐藏 + HUD 立起 + 直抓选区)----
        const QRect panel = overlay::stitchPanelRectFor(
            sel, geo.size(),
            QSize(overlay::kStitchPanelWidth, overlay::kStitchPanelHeight),
            overlay::kBarMargin);
        const int bw = panel.width() / overlay::kStitchButtons;
        const QPoint startBtn(panel.x() + bw / 2, panel.y() + panel.height() / 2);
        sendPress(overlay, startBtn);
        sendRelease(overlay, startBtn);
        QApplication::processEvents();
        liveCheck(overlay->stitchCapturing(), QStringLiteral("点「开始」进入采集中"));

        // 等 HUD 起来、遮罩真正消失、节拍器采到首帧
        QTimer::singleShot(900, qApp, [overlay, page, screen, sel, geo, &core] {
            // ================= v2 的核心断言 =================
            //
            // ① 遮罩必须**整块隐藏**:只要它还可见,选区上的滚轮就被它吞掉,
            //    页面不滚、每帧同一张图 → 长图只有一屏高。v1 靠"挖洞"达到同一
            //    目的,但窗口区域吃设备像素、选区是逻辑像素,150% 屏上洞会挖小
            //    → 选区仍被盖住一部分 → 正是真机那个现象。
            liveCheck(!overlay->isVisible(),
                      QStringLiteral("采集期遮罩整体隐藏(页面完全可交互)"));

            // ② 采集选区 = 选区设备像素矩形(固化后不再每帧重算)
            const QRect region = overlay->stitchRegionDevice();
            const QSize wantRegion(qRound(sel.width() * screen->devicePixelRatio()),
                                   qRound(sel.height() * screen->devicePixelRatio()));
            qInfo().nospace() << "STITCH-LIVE  regionDevice =" << region
                              << " expected ≈" << wantRegion
                              << " dpr =" << screen->devicePixelRatio();
            liveCheck(qAbs(region.width() - wantRegion.width()) <= 2
                          && qAbs(region.height() - wantRegion.height()) <= 2,
                      QStringLiteral("采集选区 = 选区设备像素(150% 屏上不再是逻辑像素)"));

            // ③ 直抓选区:尺寸必须等于选区设备矩形(1:1,不缩放不裁剪)
            {
                const QImage direct = core.captureMonitorRegion(screen, region);
                qInfo().nospace() << "STITCH-LIVE  region capture =" << direct.size()
                                  << " requested =" << region.size();
                liveCheck(!direct.isNull()
                              && direct.width() == region.width()
                              && direct.height() == region.height(),
                          QStringLiteral("直抓选区返回尺寸与请求**完全一致**"
                                         "(150% 屏上也不缩放)"));
            }

            // ④ 跳过"窗口区域"这套机制:v2 不再设 SetWindowRgn
            {
                const HWND hwnd = reinterpret_cast<HWND>(overlay->winId());
                const HRGN rgn = ::CreateRectRgn(0, 0, 0, 0);
                const int kind = ::GetWindowRgn(hwnd, rgn);
                ::DeleteObject(rgn);
                qInfo() << "STITCH-LIVE  overlay window region kind =" << kind
                        << "(v1 曾是 COMPLEXREGION 的挖洞区域)";
                // 遮罩隐藏时无关紧要;这条只作信息,不进断言 —— 真正的判据是①。
            }

            // ⑤ 选区里看到的是**真实页面**:直接读屏幕像素与页面自报的颜色比对
            //
            // ⚠️ 全部按值捕获:内层 lambda 会延后执行,引用会悬空(实测 0xc0000005)
            const auto sampleCenter = [screen, geo, sel, page, &core](const char* tag) {
                const QImage shot = core.captureMonitorMatched(screen);
                if (shot.isNull()) {
                    qWarning() << "STITCH-LIVE   FAIL 整屏捕获失败";
                    ++g_liveFailures;
                    return QColor();
                }
                const QPoint c = sel.center();
                const qreal sx = static_cast<qreal>(shot.width()) / geo.width();
                const qreal sy = static_cast<qreal>(shot.height()) / geo.height();
                const int px = qBound(0, qRound(c.x() * sx), shot.width() - 1);
                const int py = qBound(0, qRound(c.y() * sy), shot.height() - 1);
                const QColor got = shot.pixelColor(px, py);
                const int contentY = (c.y() - page->y()) + page->offset();
                const QColor want = LivePage::colorAt(contentY);
                qInfo().nospace() << "STITCH-LIVE  sample[" << tag << "] at(" << px << ","
                                  << py << ") got=" << got.name() << " want(page)="
                                  << want.name() << " offset=" << page->offset();
                return got;
            };

            const QColor before = sampleCenter("start");
            const QColor wantBefore =
                LivePage::colorAt((sel.center().y() - page->y()) + page->offset());
            const bool pageVisible = qAbs(before.red() - wantBefore.red()) <= 12
                && qAbs(before.green() - wantBefore.green()) <= 12
                && qAbs(before.blue() - wantBefore.blue()) <= 12;
            liveCheck(pageVisible,
                      QStringLiteral("选区里看到的是**真实页面**(不是遮罩残影) "
                                     "got=%1 want=%2").arg(before.name(), wantBefore.name()));

            // ⑥ 收尾:等几拍,再让页面滚动,确认**拼接真的在长高**
            //
            // 这是"长图只有一屏高"最直接的回归判据:滚动之后产物高度必须
            // 大于选区高度。v1 若每帧都判 duplicate,这里必然失败。
            // ⚠️ 必须堆分配:emit 发生在**另一个** QTimer 回调里,而此时本 lambda
            //    早已返回 —— 按引用捕获栈上的 QImage 就是写已销毁的内存,
            //    实测正是 0xc0000005(docs/progress.md:3927 记过同类坑)。
            auto finishedImage = std::make_shared<QImage>();
            QObject::connect(overlay, &OverlayWidget::stitchFinished, overlay,
                             [finishedImage](const QImage& img) { *finishedImage = img; });

            QTimer::singleShot(400, qApp,
                               [overlay, page, sel, screen, sampleCenter, finishedImage] {
                page->scrollBy(80);
                QApplication::processEvents();
                QTimer::singleShot(500, qApp,
                                   [overlay, page, sel, screen, sampleCenter, finishedImage] {
                    const QColor after = sampleCenter("after-scroll");
                    const QColor wantAfter =
                        LivePage::colorAt((sel.center().y() - page->y()) + page->offset());
                    const bool scrolled = qAbs(after.red() - wantAfter.red()) <= 12
                        && qAbs(after.green() - wantAfter.green()) <= 12
                        && qAbs(after.blue() - wantAfter.blue()) <= 12;
                    liveCheck(scrolled,
                              QStringLiteral("页面滚动后抓到的内容**跟着变**"
                                             "(offset=%1 got=%2 want=%3)")
                                  .arg(page->offset())
                                  .arg(after.name(), wantAfter.name()));

                    // ---- 点「结束」:产物高度必须 > 选区高度 ----
                    //
                    // v2 遮罩已隐藏,「结束」在 HUD 面板上;这里直接走状态机
                    // (HUD 的几何/可点性已由 --capture-toolbar-flow 的纯几何断言
                    // 与 panelOutsideSelection 覆盖)。
                    overlay->requestStitchFinishForTest();
                    QTimer::singleShot(700, qApp,
                                       [overlay, sel, screen, finishedImage] {
                        qInfo() << "STITCH-LIVE  finished image ="
                                << finishedImage->width() << "x" << finishedImage->height()
                                << " (selection device height ≈"
                                << qRound(sel.height() * screen->devicePixelRatio()) << ")";
                        const int selDeviceH =
                            qRound(sel.height() * screen->devicePixelRatio());
                        liveCheck(!finishedImage->isNull(),
                                  QStringLiteral("点「结束」产出了长图"));
                        liveCheck(finishedImage->height() > selDeviceH,
                                  QStringLiteral("长图高度**大于**选区高度"
                                                 "(滚动内容真的拼进去了;实得 %1 vs %2)")
                                      .arg(finishedImage->height())
                                      .arg(selDeviceH));
                        liveCheck(finishedImage->width() ==
                                      qRound(sel.width() * screen->devicePixelRatio()),
                                  QStringLiteral("长图宽度 = 选区设备像素宽"));

                        qInfo() << "STITCH-LIVE  RESULT failures =" << g_liveFailures;
                        QApplication::quit();
                    });
                });
            });
        });
    });
}

} // namespace nb
