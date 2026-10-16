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
//        B3 笔画不出选区。
//
// 日志前缀 CAPTURE-TOOLBAR,失败行以 FAIL 开头;结束时 QApplication::quit()。
#include "host/acceptance.hpp"

#include "capture/capture_session.hpp"
#include "capture/overlay_geometry.hpp"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>

namespace nb {
namespace {

int g_failures = 0;
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
        const QRect geo = screen->geometry();
        // 冻结底图**刻意造成 screen 的 2 倍**:dpr() 由"底图尺寸/逻辑尺寸"推导,
        // 于是本用例的 dpr 恒为 2(与真实屏 dpr 无关),换算偏差会被放大到一眼可见。
        QImage frozen(geo.size() * 2, QImage::Format_ARGB32_Premultiplied);
        frozen.fill(QColor(0x20, 0x20, 0x20));

        auto* overlay = new OverlayWidget(frozen, 0, screen, /*autoConfirm=*/false,
                                          /*rightClickCancel=*/true);
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
            // ⚠️ 索引跟着按钮集合走:加入「透明」档位后 完成 = 5、取消 = 6(此前 4/5)。
            //    写死旧索引会点到「透明」,测试会静默跑到另一条路径上还以为通过。
            const int doneIndex = 5;
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

            overlay->close();
            overlay->deleteLater();
            qInfo() << "CAPTURE-TOOLBAR  RESULT failures =" << g_failures;
            QApplication::quit();
        });
    });
}

} // namespace nb
