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
        const QPoint from = sel.topLeft();
        const QPoint to = sel.bottomRight();
        sendPress(overlay, from);
        sendMove(overlay, to, Qt::LeftButton);
        sendRelease(overlay, to);
        QApplication::processEvents();

        liveCheck(overlay->stitching(), QStringLiteral("框完进入长截图态(热键意图)"));

        // ---- 点「开始」:挖洞 + 起采集 ----
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

        // 给挖洞后的重绘一点时间,并让节拍器采到首帧
        QTimer::singleShot(900, qApp, [overlay, page, screen, sel, geo, &core] {
            // ---- 关键取证:选区中心此刻抓到的是**页面**还是**遮罩**? ----
            //
            // 这是"没正常拼接"最可能的根因:挖洞后下层窗口没有重绘,屏幕上仍
            // 残留遮罩的暗化像素 → 每帧都抓到同一张冻结画面 → 核心全判 duplicate
            // → 长图只有一屏高。
            // 必须按值捕获:sampleCenter 会被内层 lambda 复制进去并在 900ms 后执行,
            // 那时外层 lambda 已返回,它引用的 screen/geo/sel/page 全部悬空。
            // 实测表现为访问违例 0xc0000005,且**首次同步调用正常、第二次才崩** ——
            // 极易误判成产品缺陷。
            const auto sampleCenter = [screen, geo, sel, page, &core](const char* tag) {
                const QImage shot = core.captureMonitorMatched(screen);
                if (shot.isNull()) {
                    qWarning() << "STITCH-LIVE   FAIL 整屏捕获失败";
                    ++g_liveFailures;
                    return QColor();
                }
                // 选区中心:逻辑坐标 -> 物理像素
                const QPoint c = sel.center();
                const qreal sx = static_cast<qreal>(shot.width()) / geo.width();
                const qreal sy = static_cast<qreal>(shot.height()) / geo.height();
                const int px = qBound(0, qRound(c.x() * sx), shot.width() - 1);
                const int py = qBound(0, qRound(c.y() * sy), shot.height() - 1);
                const QColor got = shot.pixelColor(px, py);
                // 期望:页面在该点的颜色。屏幕逻辑 y - 页面顶 + 页面偏移
                const int contentY = (c.y() - page->y()) + page->offset();
                const QColor want = LivePage::colorAt(contentY);
                qInfo().nospace() << "STITCH-LIVE  sample[" << tag << "] at(" << px << ","
                                  << py << ") got=" << got.name() << " want(page)="
                                  << want.name() << " offset=" << page->offset();
                return got;
            };

            // ---- 取证:窗口区域的真实形状(是否真的挖了洞)----
            {
                const HWND hwnd = reinterpret_cast<HWND>(overlay->winId());
                const HRGN rgn = ::CreateRectRgn(0, 0, 0, 0);
                const int kind = ::GetWindowRgn(hwnd, rgn);
                const QRect win = overlay->rect();
                const int probeL = sel.left(), probeT = sel.top();
                const int probeR = sel.right(), probeB = sel.bottom();
                const bool inCenter = ::PtInRegion(rgn, sel.center().x(), sel.center().y()) != 0;
                const bool inEdge = ::PtInRegion(rgn, win.left() + 2, win.top() + 2) != 0;
                // 洞的四条边:应都不在区域内
                const bool inHoleTL = ::PtInRegion(rgn, probeL + 2, probeT + 2) != 0;
                const bool inHoleBR = ::PtInRegion(rgn, probeR - 2, probeB - 2) != 0;
                ::DeleteObject(rgn);
                qInfo().nospace() << "STITCH-LIVE  region kind=" << kind
                                  << " win=" << win << " sel=" << sel
                                  << " | centerInRegion=" << inCenter
                                  << " edgeInRegion=" << inEdge
                                  << " holeTLInRegion=" << inHoleTL
                                  << " holeBRInRegion=" << inHoleBR;
                liveCheck(!inCenter && !inHoleTL && !inHoleBR,
                          QStringLiteral("选区三处采样点都在窗口区域**之外**(洞真的挖掉了)"));
                liveCheck(inEdge,
                          QStringLiteral("窗口边缘仍在区域**之内**(遮罩没整块消失)"));
            }

            // 配对体检:采到的整屏尺寸必须等于"本屏几何 × DPR"。
            // 配错屏是多屏下最隐蔽的失效 —— 内容看着有,但全是别的屏。
            {
                const QImage m = core.captureMonitorMatched(screen);
                const QSize want(qRound(screen->geometry().width() * screen->devicePixelRatio()),
                                 qRound(screen->geometry().height() * screen->devicePixelRatio()));
                qInfo().nospace() << "STITCH-LIVE  matched capture =" << m.size()
                                  << " expected =" << want << " screen =" << screen->name();
                liveCheck(qAbs(m.width() - want.width()) <= 2
                              && qAbs(m.height() - want.height()) <= 2,
                          QStringLiteral("按屏配对捕获的尺寸与本屏几何自洽(没配到别屏)"));
            }

            const QColor before = sampleCenter("start");
            const int contentYBefore = (sel.center().y() - page->y()) + page->offset();
            const QColor wantBefore = LivePage::colorAt(contentYBefore);
            const bool pageVisible = qAbs(before.red() - wantBefore.red()) <= 12
                && qAbs(before.green() - wantBefore.green()) <= 12
                && qAbs(before.blue() - wantBefore.blue()) <= 12;
            liveCheck(pageVisible,
                      QStringLiteral("挖洞后选区里看到的是**真实页面**(不是遮罩残影) "
                                     "got=%1 want=%2").arg(before.name(), wantBefore.name()));

            // ---- 滚动页面,再采一次:内容必须跟着变 ----
            page->scrollBy(60);
            QApplication::processEvents();
            QTimer::singleShot(900, qApp, [overlay, page, sel, sampleCenter] {
                qInfo() << "STITCH-LIVE  scroll-lambda entered, page ptr=" << (void*)page;
                const QColor after = sampleCenter("after-scroll");
                qInfo() << "STITCH-LIVE  after-scroll sampled";
                const int contentYAfter = (sel.center().y() - page->y()) + page->offset();
                const QColor wantAfter = LivePage::colorAt(contentYAfter);
                const bool scrolled = qAbs(after.red() - wantAfter.red()) <= 12
                    && qAbs(after.green() - wantAfter.green()) <= 12
                    && qAbs(after.blue() - wantAfter.blue()) <= 12;
                liveCheck(scrolled,
                          QStringLiteral("页面滚动后抓到的内容**跟着变**(offset=%1 got=%2 want=%3)")
                              .arg(page->offset()).arg(after.name(), wantAfter.name()));

                qInfo() << "STITCH-LIVE  about-to-click-end";
                // ---- 点「结束」:应当完成合成并入库 ----
                const QRect panel2 = overlay::stitchPanelRectFor(
                    sel, overlay->size(),
                    QSize(overlay::kStitchPanelWidth, overlay::kStitchPanelHeight),
                    overlay::kBarMargin);
                const int bw2 = panel2.width() / overlay::kStitchButtons;
                const QPoint endBtn(panel2.x() + bw2 / 2, panel2.y() + panel2.height() / 2);
                sendPress(overlay, endBtn);
                sendRelease(overlay, endBtn);
                qInfo() << "STITCH-LIVE  end-clicked";
                QApplication::processEvents();
                qInfo() << "STITCH-LIVE  events-processed";

                qInfo() << "STITCH-LIVE  RESULT failures =" << g_liveFailures;
                QApplication::quit();
            });
        });
    });
}

} // namespace nb
