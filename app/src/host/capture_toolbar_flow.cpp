// 框选浮条 + 标注态验收(--capture-toolbar-flow)。
//
// 为什么单开一条验收:遮罩上叠了**三套坐标系**(逻辑像素 / 桌面全局物理像素 /
// 选区局部物理像素),而"标注画不准"先后出现过两次,两次都是换算错 —— 这类缺陷
// 盯着代码很难看出来,必须让"在哪儿画"与"画到哪儿"在真实遮罩上对一次账。
//
// 流程分三段:
//   A. 纯几何断言(overlay_geometry 的四个函数)—— 便宜、不依赖窗口;
//   B. 端到端:造一块**真**遮罩(合成冻结底图)→ 合成鼠标事件框选 → 点「标注」
//      → 在选区内画一笔 → 断言:
//        B1 grab() 出来的画面里,笔画落在**画的地方**(本次缺陷的正脸);
//        B2 「完成」交出的合成图:图幅 = 选区物理尺寸,且笔画落在对应场景坐标
//            (此前引擎场景被当"全局物理坐标"索引,合成图是错位 + 右下大片空白);
//        B3 笔画不出选区;
//        B4 长截图态(docs/19 §3):进入后手柄不画、面板落在窗口内、Esc 只退状态
//           而不取消整次截图。真实采集不在这里验(要真滚窗口,交真机)。
//
// 日志前缀 CAPTURE-TOOLBAR,失败行以 FAIL 开头;结束时 QApplication::quit()。
#include "host/acceptance.hpp"

#include "capture/capture_session.hpp"
#include "capture/overlay_geometry.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>

#include <windows.h>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>

namespace nb {
namespace {

int g_failures = 0;
/// 端到端段用的"下层真实窗口"(见 WindowFromPoint 断言)。
QWidget* g_below = nullptr;
QImage g_composed;                 // 「完成」交出的合成图(静态:回调在动作之后才落地)

void check(bool ok, const QString& what) {
    if (ok) {
        qInfo().noquote() << "CAPTURE-TOOLBAR  PASS" << what;
    } else {
        ++g_failures;
        qWarning().noquote() << "CAPTURE-TOOLBAR  FAIL" << what;
    }
}

/// 合成一次鼠标动作(投给真实遮罩窗口,走完整事件链)。
void sendMouse(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent ev(type, QPointF(pos), w->mapToGlobal(QPointF(pos)), button, buttons,
                   Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void dragMouse(QWidget* w, const QPoint& from, const QPoint& to, int steps = 6) {
    sendMouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i) {
        const QPoint p(from.x() + (to.x() - from.x()) * i / steps,
                       from.y() + (to.y() - from.y()) * i / steps);
        sendMouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

/// 在指定区域内找目标色的包围盒;找不到返回空矩形。
/// ⚠️ 必须限定区域:浮条上的「取消」是红的、调色板里也有红块 —— 不排除它们的话
///    包围盒会被**撑大**,"画在哪"就恒为真,断言直接变成假通过(这一点比断言本身更重要)。
QRect colorBounds(const QImage& img, const QColor& target, const QRect& region,
                  int tolerance = 60) {
    const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
    const QRect area = region.intersected(QRect(QPoint(0, 0), rgb.size()));
    int x0 = rgb.width(), y0 = rgb.height(), x1 = -1, y1 = -1;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor c(line[x]);
            if (qAbs(c.red() - target.red()) <= tolerance
                && qAbs(c.green() - target.green()) <= tolerance
                && qAbs(c.blue() - target.blue()) <= tolerance) {
                x0 = qMin(x0, x); y0 = qMin(y0, y);
                x1 = qMax(x1, x); y1 = qMax(y1, y);
            }
        }
    }
    if (x1 < 0) return QRect();
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

/// A 段:坐标系换算 + 浮条摆位(不依赖窗口,便于快速回归)。
void checkGeometry() {
    const QRect sel(100, 80, 400, 300);

    // A1. 场景矩形 = 选区**局部**物理像素(原点 0,0;尺寸 = 逻辑 × dpr)。
    const QRect scene = overlay::annotationSceneRect(sel, 2.0);
    check(scene.topLeft() == QPoint(0, 0) && scene.size() == QSize(800, 600),
          QStringLiteral("场景矩形原点在选区左上、尺寸 = 逻辑×dpr(得 %1,%2 %3x%4)")
              .arg(scene.x()).arg(scene.y()).arg(scene.width()).arg(scene.height()));

    // A2. 逻辑 → 场景:选区左上 = (0,0);中心 = 半数物理尺寸。
    check(overlay::toScene(QPoint(100, 80), sel, 2.0) == QPointF(0, 0)
              && overlay::toScene(QPoint(300, 230), sel, 2.0) == QPointF(400, 300),
          QStringLiteral("逻辑→场景换算相对选区左上、按 dpr 放大"));

    // A3. 浮条默认压在选区下方;底部放不下翻到上方;**上方也放不下必须夹住**
    //     (原先不夹 → 整条跑出屏幕,表现为"框选后看不到工具栏/点不到")。
    const QSize bar(overlay::kToolbarWidth, overlay::kButtonHeight);
    const QRect below = overlay::toolbarRectFor(sel, QSize(1920, 1080), bar, overlay::kBarMargin);
    check(below.top() >= sel.bottom() && below.bottom() < 1080,
          QStringLiteral("浮条默认在选区下方(得 top=%1)").arg(below.top()));
    const QRect flipped = overlay::toolbarRectFor(QRect(100, 900, 400, 170), QSize(1920, 1080),
                                                  bar, overlay::kBarMargin);
    check(flipped.bottom() < 900, QStringLiteral("底部放不下时翻到选区上方"));
    const QRect clamped = overlay::toolbarRectFor(QRect(0, 0, 1920, 1080), QSize(1920, 1080),
                                                  bar, overlay::kBarMargin);
    check(clamped.top() >= 4 && clamped.bottom() < 1080,
          QStringLiteral("选区占满整屏时浮条仍夹在屏内(得 top=%1)").arg(clamped.top()));

    // A4. 二级条:不论叠在浮条上方还是下方,都夹在窗口内。
    const QSize subbar(overlay::kSubbarWidth, overlay::kSubbarHeight);
    const QRect sub = overlay::subbarRectFor(clamped, subbar, overlay::kBarGap, QSize(1920, 1080));
    check(sub.top() >= 4 && sub.bottom() < 1080,
          QStringLiteral("二级条夹在窗口内(得 top=%1 bottom=%2)").arg(sub.top()).arg(sub.bottom()));
}

} // namespace

void runCaptureToolbarFlow() {
    QTimer::singleShot(1500, qApp, [] {
        checkGeometry();

        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qWarning() << "CAPTURE-TOOLBAR  FAIL 无可用屏幕,跳过端到端段";
            ++g_failures;
            QApplication::quit();
            return;
        }
        // 造一个**真实的**下层窗口:挖洞是否奏效的唯一标准是"鼠标事件送到谁"。
        // 用 WindowFromPoint 直接问系统,比"发滚轮看计数器"稳定 —— 后者依赖前台/
        // 焦点状态,在本环境里不可靠。
        //
        // ⚠️ 只用作"选区下方确实有窗口"的保证,**不作为命中基准**:目标机上
        //    这块屏可能被别的应用(开发时实测是宿主自身的 Chrome)盖住,
        //    那样断言会变成对**环境**敏感 —— 换个桌面布局就红。
        g_below = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool);
        g_below->setStyleSheet(QStringLiteral("background:#123456"));
        g_below->setGeometry(screen->geometry().adjusted(200, 150, -200, -150));
        g_below->show();
        QApplication::processEvents();

        const QRect geo = screen->geometry();
        // 冻结底图**刻意造成 screen 的 2 倍**:dpr() 由"底图尺寸/逻辑尺寸"推导,
        // 于是本用例的 dpr 恒为 2(与真实屏 dpr 无关),换算偏差会被放大到一眼可见。
        QImage frozen(geo.size() * 2, QImage::Format_ARGB32_Premultiplied);
        frozen.fill(QColor(0x20, 0x20, 0x20));

        auto* overlay = new OverlayWidget(frozen, 0, screen, /*autoConfirm=*/false,
                                          /*rightClickCancel=*/true,
                                          /*autoStartStitching=*/false);
        overlay->setGeometry(geo);
        overlay->show();
        // 用信号收「完成」交出的图:验的是**真实确认链路**,不是内部状态
        QObject::connect(overlay, &OverlayWidget::annotatedConfirmed, overlay,
                         [](const QImage& img) { g_composed = img; });

        QTimer::singleShot(200, qApp, [overlay, geo] {
            const QRect sel(300, 220, 600, 400);   // 选在屏中部,给浮条留出下方空间
            dragMouse(overlay, sel.topLeft(), QPoint(sel.right(), sel.bottom()));

            const QSize bar(overlay::kToolbarWidth, overlay::kButtonHeight);
            const QRect tb = overlay::toolbarRectFor(sel, geo.size(), bar, overlay::kBarMargin);
            const int bw = tb.width() / overlay::kToolbarButtons;
            const auto click = [overlay](const QPoint& p) {
                sendMouse(overlay, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
                sendMouse(overlay, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
            };
            check(!tb.isNull(), QStringLiteral("框选后浮条可见(几何非空)"));

            // ---- 进标注态:「标注」按钮(下标 0)----
            click(QPoint(tb.x() + bw / 2, tb.y() + tb.height() / 2));

            // ---- B1. 在选区内画一笔 ----
            const QPoint from(sel.center().x() - 80, sel.center().y() + 60);
            const QPoint to(sel.center().x() + 90, sel.center().y() - 40);
            dragMouse(overlay, from, to, 8);

            const QColor pen(0xE0, 0x31, 0x31);
            const QImage shot = overlay->grab().toImage();
            const qreal gd = shot.devicePixelRatio() > 0 ? shot.devicePixelRatio() : 1.0;
            // 只查"选区内、且在两行条**之上**"的那块:条上的红字/红块若被算进来,
            // 包围盒恒覆盖笔画,断言就永远通过了(假通过,比漏检更糟)。
            const QRect sub = overlay::subbarRectFor(
                tb, QSize(overlay::kSubbarWidth, overlay::kSubbarHeight), overlay::kBarGap,
                geo.size());
            const int cutoff = qMin(sub.top(), tb.top()) - 2;
            const int searchH = qMax(1, cutoff - sel.top());
            const QRect insideSel(qRound(sel.left() * gd), qRound(sel.top() * gd),
                                  qRound(sel.width() * gd), qRound(searchH * gd));
            const QRect got = colorBounds(shot, pen, insideSel);
            const QRectF want = QRectF(from, to).normalized();
            const QRectF gotLogical(QPointF(got.topLeft()) / gd, QPointF(got.bottomRight()) / gd);
            qInfo() << "CAPTURE-TOOLBAR  draw at(logical)" << want
                    << "-> found at(logical)" << gotLogical << "grabDpr" << gd;
            check(!got.isNull() && gotLogical.adjusted(-8, -8, 8, 8).contains(want.topLeft())
                      && gotLogical.adjusted(-8, -8, 8, 8).contains(want.bottomRight()),
                  QStringLiteral("遮罩上笔画落在**画的地方**(标注渲染坐标正确)"));

            // ---- B3. 笔画横向不出选区 ----
            check(!got.isNull() && gotLogical.left() >= sel.left() - 8
                      && gotLogical.right() <= sel.right() + 8,
                  QStringLiteral("标注被裁在选区内(不出框)"));

            // ---- B2. 「完成」交出的合成图 ----
            // ⚠️ 索引跟着按钮集合走。当前 8 个按钮:
            //    0 标注 / 1 OCR取字 / 2 长截图 / 3 贴图 / 4 保存 / 5 透明 / 6 完成 / 7 取消
            //    写死旧索引会点到隔壁按钮,测试静默跑到另一条路径上还以为通过 ——
            //    这就是本节历史上两次"假通过"的来源(docs/14 §16 与 docs/19 §3.1)。
            const int doneIndex = 6;
            click(QPoint(tb.x() + bw * doneIndex + bw / 2, tb.y() + tb.height() / 2));
            const QSize wantSize(qRound(sel.width() * 2.0), qRound(sel.height() * 2.0));
            qInfo() << "CAPTURE-TOOLBAR  confirm -> image" << g_composed.size()
                    << "expected" << wantSize;
            check(!g_composed.isNull(), QStringLiteral("「完成」交出含标注的合成图"));
            check(g_composed.size() == wantSize,
                  QStringLiteral("合成图幅 = 选区物理尺寸(期望 %1x%2,得 %3x%4)")
                      .arg(wantSize.width()).arg(wantSize.height())
                      .arg(g_composed.width()).arg(g_composed.height()));
            const QRect inComposed = colorBounds(g_composed, pen,
                                                 QRect(QPoint(0, 0), g_composed.size()));
            const QPointF sceneFrom = overlay::toScene(from, sel, 2.0);
            const QPointF sceneTo = overlay::toScene(to, sel, 2.0);
            qInfo() << "CAPTURE-TOOLBAR  stroke in composed:" << inComposed
                    << "expected span" << sceneFrom << sceneTo;
            check(!inComposed.isNull()
                      && QRectF(inComposed).adjusted(-8, -8, 8, 8).contains(sceneFrom)
                      && QRectF(inComposed).adjusted(-8, -8, 8, 8).contains(sceneTo),
                  QStringLiteral("合成图里笔画落在场景坐标对应处(不再整体位移)"));

            // ---- 长截图**意图**(CaptureIntent::Scrolling):热键直达 ----
            //
            // 关键点:必须与 regionAutoConfirm **解耦**。旧写法下
            // "设了松手即入库" 的用户按 Ctrl+Shift+L 框完就直接入库了,
            // 永远到不了长截图态 —— 功能对一半用户不可达。
            {
                // 刻意造一个 autoConfirm=true 的遮罩 + 长截图意图:
                // 松手后应当**进长截图态**,而**不是** regionConfirmed。
                QImage dummy(geo.size() * 2, QImage::Format_ARGB32_Premultiplied);
                dummy.fill(QColor(0x30, 0x30, 0x30));
                // screen 不在本 lambda 的捕获列表里(外层 [overlay, geo] 只带了这两个);
                // 从 overlay 自己的屏取,避免为了一个指针去放大捕获列表。
                QScreen* scr = overlay->screen();
                auto* scrollOverlay = new OverlayWidget(dummy, 0, scr,
                                                        /*autoConfirm=*/true,
                                                        /*rightClickCancel=*/true,
                                                        /*autoStartStitching=*/true);
                scrollOverlay->setGeometry(geo);
                scrollOverlay->show();
                bool confirmedFired = false;
                QObject::connect(scrollOverlay, &OverlayWidget::regionConfirmed, scrollOverlay,
                                 [&confirmedFired](const QRect&) { confirmedFired = true; });

                const QRect ssel(320, 240, 560, 380);
                dragMouse(scrollOverlay, ssel.topLeft(), QPoint(ssel.right(), ssel.bottom()));
                const bool inStitch = scrollOverlay->stitching();
                const bool capturing = scrollOverlay->stitchCapturing();
                qInfo() << "CAPTURE-TOOLBAR  scrolling-intent: stitching =" << inStitch
                        << "capturing =" << capturing
                        << "regionConfirmed fired =" << confirmedFired;
                check(inStitch,
                      QStringLiteral("长截图意图:框完**直接进长截图态**(无需点浮条)"));
                check(!capturing,
                      QStringLiteral("长截图意图:进入后处于就绪态,等用户点「开始」"));
                check(!confirmedFired,
                      QStringLiteral("长截图意图**绕过** regionAutoConfirm=true 的"
                                     "(否则松手就入库,永远进不了长截图)"));

                // Esc 应回到**普通框选**(而不是取消整次截图):选区还在,用户可改用浮条
                QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(scrollOverlay, &esc2);
                check(!scrollOverlay->stitching(),
                      QStringLiteral("长截图意图下 Esc 退出长截图态"));
                check(scrollOverlay->isVisible() && !confirmedFired,
                      QStringLiteral("退出后遮罩仍在、未误触发入库"));

                scrollOverlay->close();
                scrollOverlay->deleteLater();
            }

            // ---- 长截图态(docs/19 §3)：面板摆位、Esc 分层、手柄隐藏 ----
            //
            // 这里只验**状态机与几何**,不验真实采集 —— 采集要真的滚窗口、
            // 等渲染,不可重复(docs/19 §8 A4 交给真机)。真采集的拼接正确性
            // 已由 --stitch-flow 用构造帧覆盖。
            {
                const QString snapPath = qEnvironmentVariable("NB_STITCH_SNAP");
                // 重新框选出一个干净选区(上一轮的标注已随「完成」结束)
                dragMouse(overlay, sel.topLeft(), QPoint(sel.right(), sel.bottom()));
                // 点浮条「长截图」(下标 2)
                click(QPoint(tb.x() + bw * 2 + bw / 2, tb.y() + tb.height() / 2));

                check(overlay->stitching(), QStringLiteral("点「长截图」后进入长截图态"));
                check(!overlay->stitchCapturing(),
                      QStringLiteral("进入长截图态时**不**自动开始采集(等用户点开始)"));

                // 面板几何:必须落在窗口内(否则用户点不到)
                const QRect panel = overlay::stitchPanelRectFor(
                    sel, geo.size(),
                    QSize(overlay::kStitchPanelWidth, overlay::kStitchPanelHeight),
                    overlay::kBarMargin);
                const QRect winRect(QPoint(0, 0), geo.size());
                qInfo() << "CAPTURE-TOOLBAR  stitch panel" << panel << "window" << winRect;
                check(winRect.contains(panel),
                      QStringLiteral("长截图面板完整落在窗口内(不会被裁到屏外)"));

                // 手柄必须**不画**:选区已锁定为采集窗口,让用户以为还能拖就会拖错
                const QImage shot2 = overlay->grab().toImage();
                const qreal gd2 = shot2.devicePixelRatio() > 0 ? shot2.devicePixelRatio() : 1.0;
                // 手柄是纯白小方块(QColor(255,255,255)),在选区内边角找
                const QRect corner(qRound((sel.left() + 1) * gd2), qRound((sel.top() + 1) * gd2),
                                   qRound(24 * gd2), qRound(24 * gd2));
                const QRect handleHit = colorBounds(shot2, QColor(255, 255, 255), corner);
                qInfo() << "CAPTURE-TOOLBAR  handle probe (expect null):" << handleHit;
                check(handleHit.isNull(),
                      QStringLiteral("长截图态不绘制 8 向手柄(选区已锁定)"));

                // 面板是否**真的画出来了**:数一下面板矩形内的非底色像素。
                // 只断言"几何落在窗口内"是不够的 —— 面板完全不画也能通过那条。
                const QRect panelPx(qRound(panel.x() * gd2), qRound(panel.y() * gd2),
                                    qRound(panel.width() * gd2), qRound(panel.height() * gd2));
                int painted = 0;
                for (int y = panelPx.top(); y <= panelPx.bottom(); ++y) {
                    if (y < 0 || y >= shot2.height()) continue;
                    const QRgb* line = reinterpret_cast<const QRgb*>(shot2.scanLine(y));
                    for (int x = panelPx.left(); x <= panelPx.right(); ++x) {
                        if (x < 0 || x >= shot2.width()) continue;
                        const QRgb p = line[x];
                        // 底板是 #26272E 半透压在底图上,与"框选外的黑罩 + 冻结底图"
                        // 明显不同;这里只排除"完全相同的整片背景"。
                        if (qRed(p) > 40 || qGreen(p) > 120) ++painted;
                    }
                }
                qInfo() << "CAPTURE-TOOLBAR  stitch panel painted px =" << painted
                        << "of" << panelPx.width() * panelPx.height();
                check(painted > 40,
                      QStringLiteral("长截图面板**真的画出来了**(非背景像素数 > 40)"));

                if (!snapPath.isEmpty()) {
                    shot2.save(snapPath);
                    // 另存一份只含面板的小图,便于人工核对视觉
                    shot2.copy(panelPx).save(snapPath + QStringLiteral(".panel.png"));
                    qInfo() << "CAPTURE-TOOLBAR  stitch snapshot ->" << snapPath;
                }

                // ---- 挖洞是否真的生效(docs/19 §7 R2 的核心)----
                //
                // 这是"长截图能不能用"的前提:遮罩不挖洞就会吞掉滚轮,页面不滚,
                // 每一帧都是同一张图。这里直接问系统拿窗口区域,验证
                // ① 选区中心点在区域**之外**(洞真的挖掉了)
                // ② 选区外的一个点在区域**之内**(遮罩还在,按钮仍可点)
                {
                    // 点「开始」进入采集中(挖洞在这一刻生效)
                    const QRect panelNow = overlay::stitchPanelRectFor(
                        sel, geo.size(),
                        QSize(overlay::kStitchPanelWidth, overlay::kStitchPanelHeight),
                        overlay::kBarMargin);
                    const int pbw = panelNow.width() / overlay::kStitchButtons;
                    click(QPoint(panelNow.x() + pbw / 2, panelNow.y() + panelNow.height() / 2));
                    check(overlay->stitchCapturing(),
                          QStringLiteral("点「开始」后进入采集中"));

                    const HWND hwnd = reinterpret_cast<HWND>(overlay->winId());
                    const HRGN rgn = ::CreateRectRgn(0, 0, 0, 0);
                    const int kind = ::GetWindowRgn(hwnd, rgn);
                    // kind: NULLREGION/ SIMPLEREGION/ COMPLEXREGION/ ERROR
                    const QPoint inside(sel.center());
                    const QPoint outside(sel.left() / 2, sel.top() / 2);
                    const bool holeIsHole = ::PtInRegion(rgn, inside.x(), inside.y()) == 0;
                    const bool maskAlive = ::PtInRegion(rgn, outside.x(), outside.y()) != 0;
                    qInfo() << "CAPTURE-TOOLBAR  window region kind =" << kind
                            << "hole(center)=" << inside << "inRegion=" << !holeIsHole
                            << "mask(outside)=" << outside << "inRegion=" << maskAlive;
                    ::DeleteObject(rgn);
                    check(kind != ERROR,
                          QStringLiteral("采集期间窗口区域已设置(SetWindowRgn 生效)"));
                    check(holeIsHole,
                          QStringLiteral("选区被**挖掉**(滚轮可直达下层窗口,页面能滚)"));
                    check(maskAlive,
                          QStringLiteral("选区外遮罩仍在(「结束」按钮可点)"));

                    // ---- 挖洞的**真正目的**:鼠标事件要送到下层窗口 ----
                    //
                    // 这条是长截图能否成立的分水岭:遮罩不挖洞时,选区上的滚轮被
                    // 遮罩自己接收,底下的浏览器/记事本**根本不会滚**,于是每一帧
                    // 采到的都是同一张图 —— 功能静默失效,且没有任何报错。
                    //
                    // 用 WindowFromPoint 直接问系统"这个屏幕坐标上的顶层窗口是谁":
                    // 挖洞生效 → 返回下层窗口;未挖洞 → 返回遮罩自己。
                    // ⚠️ 必须在**补洞之前**断言 —— 补洞后命中遮罩才是正确的。
                    const QPoint screenCenter = overlay->mapToGlobal(sel.center());
                    const HWND hitHole = ::WindowFromPoint(
                        POINT{screenCenter.x(), screenCenter.y()});
                    const QPoint screenOutside =
                        overlay->mapToGlobal(QPoint(sel.left() / 2, sel.top() / 2));
                    const HWND hitMask = ::WindowFromPoint(
                        POINT{screenOutside.x(), screenOutside.y()});
                    const auto className = [](HWND h) {
                        wchar_t buf[256] = {0};
                        ::GetClassNameW(h, buf, 255);
                        return QString::fromWCharArray(buf);
                    };
                    qInfo() << "CAPTURE-TOOLBAR  WindowFromPoint hole =" << (void*)hitHole
                            << "class =" << className(hitHole)
                            << "| mask =" << (void*)hitMask << "class =" << className(hitMask)
                            << "| overlay hwnd =" << (void*)hwnd
                            << "| below hwnd =" << (void*)(g_below ? g_below->winId() : 0);
                    check(hitHole != hwnd,
                          QStringLiteral("挖洞处命中**不是遮罩**(滚轮可直达下层,页面能滚)"));
                    // ⚠️ 这一条只**打印不断言**。
                    //
                    // 原因:WindowFromPoint 反映的是**当前桌面层级**,而本用例跑在
                    // 无人值守/可能锁屏的机器上 —— 一旦系统覆盖层(锁屏
                    // LockScreenBackstopFrame、UAC 暗幕、其它置顶窗口)压在最上面,
                    // 洞内外会**同时**返回那个覆盖层,于是"洞内外命中不同"必然不成立,
                    // 尽管挖洞本身完全正常(上面两条 PtInRegion 断言已经证明了区域形状)。
                    //
                    // 把环境相关的现象判成失败,只会制造假红、并让真正的回归淹没在
                    // 噪声里。故此处降级为信息:人工在正常桌面上跑时看这一行即可。
                    qInfo() << "CAPTURE-TOOLBAR  (info) 洞内外命中目标:"
                            << "hole =" << (void*)hitHole << "mask =" << (void*)hitMask
                            << "-> 洞内外不同:" << (hitHole != hitMask)
                            << "(锁屏/覆盖层下会相同,属环境现象)";
                    // 命中的应当是一个**真实的应用窗口**(本环境实测是
                    // Chrome_RenderWidgetHostHWND —— 那是宿主 GUI 自己占着这块屏)。
                    // 不要求它等于我们造的 g_below:它在别的窗口下面,是否露在
                    // 最上层取决于桌面现状,拿它做基准会让用例对**环境**敏感。
                    // 真正要证明的是"事件交给了下层应用,而不是被遮罩吞掉"。
                    check(!className(hitHole).isEmpty() && hitHole != hwnd,
                          QStringLiteral("挖洞处命中的是**真实应用窗口**"
                                         "(证明事件已交给下层,而非被遮罩吞掉)"));

                    // 点「结束」→ 挖洞必须补回,否则遮罩会破一块
                    click(QPoint(panelNow.x() + pbw / 2, panelNow.y() + panelNow.height() / 2));
                    const HRGN rgn2 = ::CreateRectRgn(0, 0, 0, 0);
                    const int kind2 = ::GetWindowRgn(hwnd, rgn2);
                    ::DeleteObject(rgn2);
                    // 补洞后区域应恢复整窗 → 仍是非空区域,且中心点回到区域内
                    const HRGN rgn3 = ::CreateRectRgn(0, 0, 0, 0);
                    ::GetWindowRgn(hwnd, rgn3);
                    const bool centerBack = ::PtInRegion(rgn3, sel.center().x(),
                                                         sel.center().y()) != 0;
                    ::DeleteObject(rgn3);
                    qInfo() << "CAPTURE-TOOLBAR  after finish: region kind =" << kind2
                            << "center back in region =" << centerBack;
                    check(centerBack,
                          QStringLiteral("结束采集后挖洞已补回(遮罩恢复完整)"));
                }

                // Esc 分层:就绪态按 Esc → 退出长截图态(**不**取消整次截图)
                QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(overlay, &esc);
                check(!overlay->stitching(),
                      QStringLiteral("就绪态 Esc 退出长截图态(而非取消整次截图)"));
                // 遮罩仍在(没被取消)
                check(overlay->isVisible(),
                      QStringLiteral("Esc 只退长截图态,遮罩与选区仍在"));
            }

            overlay->close();
            overlay->deleteLater();
            // 验收窗口必须收回:它是普通顶层窗口,留着会挡住后续人工验证
            if (g_below) {
                g_below->close();
                g_below->deleteLater();
                g_below = nullptr;
            }
            qInfo() << "CAPTURE-TOOLBAR  RESULT failures =" << g_failures;
            QApplication::quit();
        });
    });
}

} // namespace nb
