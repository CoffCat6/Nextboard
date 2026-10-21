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
//           而不取消整次截图。真实采集不在这里验(要真滚窗口,交真机);
//        B5 **主题适配**(docs/theme.md §6):浮条底板必须跟随浅/深主题,
//           而不是恒定深色 —— 这是"无论什么主题工具栏都是深色"的回归哨兵。
//           判据是**抓图上的亮度中位数**:底板占浮条面积的绝大多数,文字是少数,
//           中位数就是底板色。比"取某个固定像素"稳(不受文字位置/抗锯齿影响)。
//
// 日志前缀 CAPTURE-TOOLBAR,失败行以 FAIL 开头;结束时 QApplication::quit()。
#include "host/acceptance.hpp"

#include "capture/capture_session.hpp"
#include "capture/overlay_geometry.hpp"
#include "ui_theme.hpp"
#include "ui/ocr_result_panel.hpp"
#include "host/core_bridge.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QJsonObject>
#include <QTextEdit>
#include <QStringList>
#include <QLabel>
#include <QPointer>
#include <QPalette>

#include <algorithm>

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

/// 区域内像素亮度中位数(0~255)。
///
/// 为什么用中位数而不是平均值或单点取样:
///  - 单点取样会撞上按钮文字、悬停高亮、分隔线(验收里鼠标可能在任意位置);
///  - 平均值会被少量高亮像素带偏;
///  - 底板占浮条面积的绝大多数,中位数就是底板色本身。
int medianLuma(const QImage& img, const QRect& region) {
    const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
    const QRect area = region.intersected(QRect(QPoint(0, 0), rgb.size()));
    if (area.isEmpty()) return -1;
    QVector<int> values;
    values.reserve(area.width() * area.height());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor c(line[x]);
            // Rec.601 亮度:人眼对绿最敏感,整数近似足够区分"深底/浅底"
            values.append((c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000);
        }
    }
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
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

        // ---- B8. OCR 结果面板必须**脱离宿主窗口**存活 ----
        //
        // 这是本轮 OcrResultPanel 的核心设计约束,也是最容易被改回去的地方:
        // 浮条取字后会话立即 cancelled → closeAllOverlays → 遮罩 close(),
        // 若面板是遮罩的子控件,它会"闪一下就不见"(编辑器早期踩过同一个坑)。
        // 断言方式:造一个 anchor 窗口 → 弹面板 → **销毁 anchor** → 面板仍可见。
        if (screen) {
            auto* anchorHost = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool);
            anchorHost->setGeometry(screen->geometry().adjusted(100, 100, -100, -100));
            anchorHost->show();
            QApplication::processEvents();

            const QString sample = QStringLiteral("第一行识别结果\n第二行文本");
            QPointer<OcrResultPanel> panel = OcrResultPanel::showResult(
                sample, QStringLiteral("已识别 16 个字符并复制到剪贴板"), QImage(),
                anchorHost, /*popup=*/false);
            QApplication::processEvents();
            check(panel && panel->isVisible(),
                  QStringLiteral("OCR 结果面板弹出后可见(驻留形态)"));

            // 宿主(遮罩)在这之后就会被销毁 —— 模拟真实链路
            anchorHost->close();
            delete anchorHost;
            QApplication::processEvents();
            check(panel && panel->isVisible(),
                  QStringLiteral("宿主销毁后 OCR 面板仍存活可见(不是遮罩子控件)"));
            if (panel) {
                // 可鼠标选取是"只取其中几行"的前提,值得钉住
                // 正文已从 QLabel 换成只读 QTextEdit(为了能滚动),所以这里要按
                // QTextEdit 查 —— 断言的是"能只取片段"这条**能力**,不是控件类型,
                // 但实现载体变了,查的对象就得跟着变。
                bool selectable = false;
                for (QTextEdit* t : panel->findChildren<QTextEdit*>()) {
                    if (t->textInteractionFlags() & Qt::TextSelectableByMouse) selectable = true;
                }
                check(selectable, QStringLiteral("OCR 面板文本可鼠标选取(能只取片段)"));

                // ---- 面板必须**有不透明底板** ----
                //
                // 用户报的现象:面板整体是透明的,文字直接浮在桌面上。
                // 根因:透明顶层窗口(WA_TranslucentBackground)+ stylesheet 的
                // QFrame background —— QFrame 不画 stylesheet 背景(要
                // WA_StyledBackground),于是什么都没画。
                //
                // 判据:取面板右下角一块(只有底板,没有文字/按钮)的亮度,
                // 必须接近主题 surface 而不是透出底下的桌面。
                {
                    // ⚠️ **两套主题都要测**。只在浅色下测是不够的:透明面板透出
                    //    白色桌面同样是 255,会和"白色 surface"给出一样的读数 ——
                    //    缺陷恰好能通过断言(假通过)。深色主题下透明会透出深色桌面,
                    //    与浅色 surface 差出一个数量级,才真正区分得开。
                    const QPalette savedPal = QGuiApplication::palette();
                    const auto plateLuma = [&](bool dark) {
                        QGuiApplication::setPalette(ui::paletteFor(dark));
                        QApplication::processEvents();
                        panel->update();
                        QApplication::processEvents();
                        const QImage img = panel->grab().toImage();
                        const qreal k = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
                        // 右侧中部:只有底板,避开文字与按钮行
                        const QRect probe(qRound(img.width() - 30 * k),
                                          qRound(img.height() * 0.45),
                                          qRound(16 * k), qRound(24 * k));
                        return medianLuma(img, probe);
                    };
                    const int lumaDark = plateLuma(true);
                    const int lumaLight = plateLuma(false);
                    QGuiApplication::setPalette(savedPal);
                    QApplication::processEvents();

                    const ui::ThemeTokens& td = ui::themeTokens(true);
                    const ui::ThemeTokens& tl = ui::themeTokens(false);
                    const int wantDark = qRound(0.299 * td.surface.red() + 0.587 * td.surface.green()
                                                + 0.114 * td.surface.blue());
                    const int wantLight = qRound(0.299 * tl.surface.red() + 0.587 * tl.surface.green()
                                                 + 0.114 * tl.surface.blue());
                    qInfo() << "CAPTURE-TOOLBAR  ocr panel plate luma: dark =" << lumaDark
                            << "(want" << wantDark << ") light =" << lumaLight
                            << "(want" << wantLight << ")";
                    // 容差取 ±12:底板是**纯色**填充,没有渐变/混合,实测值与主题色
                    // 只差抗锯齿边缘的几级。放宽到 ±40 会让"透明(0) vs 深色 surface(25)"
                    // 这种差别漏过 —— 反向验证时实测就是这样假通过的。
                    constexpr int kTol = 12;
                    check(lumaDark >= 0 && qAbs(lumaDark - wantDark) <= kTol,
                          QStringLiteral("深色主题下 OCR 面板有底板(实测 %1,期望 ~%2)")
                              .arg(lumaDark).arg(wantDark));
                    check(qAbs(lumaLight - wantLight) <= kTol,
                          QStringLiteral("浅色主题下 OCR 面板有底板(实测 %1,期望 ~%2)")
                              .arg(lumaLight).arg(wantLight));
                }

                // ---- 面板必须贴在**选区**旁边,而不是屏幕边角 ----
                //
                // 用户报的现象:面板跑到屏幕角落。根因是 anchor 传了整屏遮罩。
                // 判据:面板矩形与选区矩形**有重叠**,且距离不超过一个面板宽 ——
                // "贴旁边"的可用定义。太远 = 又跑偏了。
                {
                    const QRect selRect(200, 150, 400, 300);
                    const QSize ps(320, 160);
                    const QPoint p = OcrResultPanel::placeForTest(ps, selRect);
                    const QRect panelRect(p, ps);
                    const int dist = (panelRect.center() - selRect.center()).manhattanLength();
                    qInfo() << "CAPTURE-TOOLBAR  panel placed at" << p << "for selection" << selRect
                            << "distance" << dist;
                    // 选区内接的中心距上限:面板宽 + 选区半宽,足够宽松但能挡住"屏幕角落"
                    check(dist < ps.width() + selRect.width(),
                          QStringLiteral("OCR 面板贴紧选区(中心距 %1 在合理范围)").arg(dist));
                    // 更不能离屏
                    const QRect screenRect(QPoint(0, 0), QSize(1920, 1080));
                    check(screenRect.contains(panelRect),
                          QStringLiteral("OCR 面板完整落在屏幕内(点得到关闭)"));
                }
                panel->close();
            }

            // ---- B9. 长文本必须**可滚动**,不能被裁掉 ----
            //
            // 缺陷背景(用户报的):正文原先用 QLabel + setMaximumHeight(420),
            // 一屏放不下的识别结果被直接裁掉 —— 既看不到也拿不到,而"取字"恰恰
            // 常常来自长段落/整页文字。
            //
            // 判据两条,缺一不可:
            //  ① 长内容时**确实需要滚动**(bodyScrollable():滚动条 maximum > 0);
            //  ② 面板**没有被撑出屏幕**(否则"能滚"但按钮点不到,等于不可用)。
            {
                // 造 120 行:"短内容也要能滚"不成立,必须真的超长
                QStringList lines;
                for (int i = 1; i <= 120; ++i) {
                    lines << QStringLiteral("第 %1 行:用于验证长文本滚动与不被截断").arg(i);
                }
                const QString longText = lines.join(QLatin1Char('\n'));
                QPointer<OcrResultPanel> longPanel = OcrResultPanel::showResult(
                    longText, QStringLiteral("已识别 120 行"), QImage(),
                    QRect(screen->geometry().center() - QPoint(200, 150), QSize(400, 300)),
                    /*popup=*/false);
                QApplication::processEvents();

                // ⚠️ 这两条都是**弱断言**,单独用会假通过(反向验证实测):
                //   - bodyScrollable:QTextEdit 内容超长时无论多矮,maximum 都 > 0;
                //   - bodyLastLineReachable:QTextEdit 本来就能滚到末尾,与窗口多高无关。
                //     (把正文硬裁到 113px,两条照样 PASS。)
                // 保留它们 + 下面那条**高度**断言,才是完整判据:前者保证"能滚"这一能力
                // 存在,后者保证"面板真的为内容长高了"(而不是被压成一条缝)。
                check(longPanel && longPanel->bodyScrollable(),
                      QStringLiteral("长文本时正文可滚动"));
                check(longPanel && longPanel->bodyLastLineReachable(),
                      QStringLiteral("长文本可滚到最后一行"));

                const QRect avail = screen->availableGeometry();
                const QRect panelRect = longPanel ? longPanel->frameGeometry() : QRect();
                qInfo() << "CAPTURE-TOOLBAR  long-text panel rect" << panelRect
                        << "available" << avail;
                check(longPanel && avail.contains(panelRect),
                      QStringLiteral("长文本面板未被撑出屏幕(按钮点得到)"));

                // 短文本**不该**出现滚动条:否则每张小图取字都挂一条多余的条
                QPointer<OcrResultPanel> shortPanel = OcrResultPanel::showResult(
                    QStringLiteral("两行文字\n第二行"), QString(),
                    QImage(), QRect(screen->geometry().center(), QSize(300, 200)),
                    /*popup=*/false);
                QApplication::processEvents();
                check(shortPanel && !shortPanel->bodyScrollable(),
                      QStringLiteral("短文本不出现滚动条(高度按内容收缩)"));

                // ---- 关键断言:面板高度必须**随内容增长**并封顶 ----
                //
                // 这是"内容不被裁掉"唯一可外部观测的判据:文字多 → 面板高。
                // 阈值取 300px:120 行文本按 13px 字号远远超过它,而硬裁到 113px
                // 的旧行为必然低于它(反向验证实测 204px 也低于 300)。
                const int longH = longPanel ? longPanel->height() : 0;
                const int shortH = shortPanel ? shortPanel->height() : 0;
                qInfo() << "CAPTURE-TOOLBAR  panel heights: long =" << longH
                        << "short =" << shortH;
                check(longH > 300 && longH > shortH + 100,
                      QStringLiteral("长文本面板按内容长高(高 %1 > 300 且显著高于短文本 %2)")
                          .arg(longH).arg(shortH));

                if (shortPanel) shortPanel->close();
                if (longPanel) longPanel->close();

                // ---- B10. 面板可拖动(用户要求) ----
                //
                // 两类断言都要有,缺一不可:
                //  ① 在**标题行**按下并移动 → 窗口跟着走(这是"能拖");
                //  ② 在**正文**按下并移动 → 窗口**不动**(这是"拖动没抢走划选")。
                //     只验①会把"整块面板都能拖"写成正确行为,而那会让 QTextEdit
                //     永远选不中文字 —— 缺陷的镜像。
                {
                    QPointer<OcrResultPanel> dragPanel = OcrResultPanel::showResult(
                        QStringLiteral("拖动测试\n第二行"), QString(),
                        QImage(), QRect(QPoint(500, 400), QSize(300, 200)),
                        /*popup=*/false);
                    QApplication::processEvents();
                    const QPoint before = dragPanel->pos();

                    // ① 标题行拖动:目标点取标题行中心
                    QWidget* header = dragPanel->findChild<QWidget*>(QStringLiteral("ocrHeader"));
                    check(header != nullptr, QStringLiteral("面板有标题行(拖动把手)"));
                    if (header) {
                        const QPoint local = header->mapTo(dragPanel, header->rect().center());
                        const QPoint from = dragPanel->mapToGlobal(local);
                        const QPoint to = from + QPoint(60, 45);
                        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local),
                                          QPointF(from), Qt::LeftButton, Qt::LeftButton,
                                          Qt::NoModifier);
                        QApplication::sendEvent(dragPanel, &press);
                        QMouseEvent moveEv(QEvent::MouseMove, QPointF(local + QPoint(60, 45)),
                                           QPointF(to), Qt::NoButton, Qt::LeftButton,
                                           Qt::NoModifier);
                        QApplication::sendEvent(dragPanel, &moveEv);
                        QMouseEvent release(QEvent::MouseButtonRelease,
                                            QPointF(local + QPoint(60, 45)), QPointF(to),
                                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(dragPanel, &release);
                        QApplication::processEvents();

                        const QPoint after = dragPanel->pos();
                        qInfo() << "CAPTURE-TOOLBAR  drag: before =" << before
                                << "after =" << after
                                << "delta =" << (after - before);
                        check(after == before + QPoint(60, 45),
                              QStringLiteral("标题行拖动可移动面板(位移 = 鼠标位移)"));
                    }

                    // ② 正文区域的按下**不该**拖动窗口。
                    //    这里直接投给 QTextEdit:它接受左键(用于放置光标/划选),
                    //    事件不会冒泡到面板 —— 这正是我们要的行为。
                    const QPoint beforeBody = dragPanel->pos();
                    QTextEdit* body = dragPanel->findChild<QTextEdit*>(QStringLiteral("ocrText"));
                    if (body) {
                        // 投给 **viewport**:QTextEdit 真正的鼠标处理在 viewport 上,
                        // 投给 QTextEdit 本身等于绕过了它的命中逻辑(第一次就是这么错的)。
                        QWidget* vp = body->viewport();
                        const QPoint bFrom = vp->mapToGlobal(QPoint(10, 10));
                        QMouseEvent press(QEvent::MouseButtonPress, QPointF(QPoint(10, 10)),
                                          QPointF(bFrom), Qt::LeftButton, Qt::LeftButton,
                                          Qt::NoModifier);
                        QApplication::sendEvent(vp, &press);
                        QMouseEvent moveEv(QEvent::MouseMove, QPointF(QPoint(70, 55)),
                                           QPointF(bFrom + QPoint(60, 45)), Qt::NoButton,
                                           Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(vp, &moveEv);
                        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(QPoint(70, 55)),
                                            QPointF(bFrom + QPoint(60, 45)), Qt::LeftButton,
                                            Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(vp, &release);
                        QApplication::processEvents();
                        qInfo() << "CAPTURE-TOOLBAR  body press: pos before =" << beforeBody
                                << "after =" << dragPanel->pos();
                        check(dragPanel->pos() == beforeBody,
                              QStringLiteral("正文区按下不拖动窗口(划选文字不受影响)"));
                    }
                    if (dragPanel) dragPanel->close();
                }
            }
        }

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

            // ---- B5. 主题适配:浮条底板必须跟随浅/深主题 ----
            //
            // 缺陷原状(2026-10-21 前):底板硬编码 #26272E,与 QPalette / Theme.qml
            // 完全脱钩 —— 用户切到浅色主题后,工具栏仍是深色块。这条断言就是它的哨兵。
            //
            // 做法:显式切两套调色板(不依赖系统设置,否则不可自动化),各抓一次画面,
            // 比**底板亮度中位数**。深色主题 surface #151A21 ≈ 22,浅色 #FFFFFF = 255,
            // 两者相差一个数量级,判据取 90 / 150 留足余量(半透底板压在深色冻结
            // 底图上会被底图拉偏,取舍见下)。
            {
                const QPalette saved = QGuiApplication::palette();
                // 底板是半透的,压在冻结底图(#202020)上 —— 用**半透档**抓到的亮度
                // 会被底图往中间拽。为了让断言反映"底板本色",这里临时切到不透明档。
                const int savedAlpha = overlay->barAlpha();
                overlay->setBarAlpha(OverlayWidget::kBarAlphaSolid);

                const auto grabLuma = [&](bool dark) {
                    QGuiApplication::setPalette(ui::paletteFor(dark));
                    QApplication::processEvents();
                    const QImage shot = overlay->grab().toImage();
                    const qreal gd = shot.devicePixelRatio() > 0 ? shot.devicePixelRatio() : 1.0;
                    // 只取浮条中段:避开左右圆角(抗锯齿混入底图)与四边描边。
                    const QRect area(qRound((tb.x() + 12) * gd), qRound((tb.y() + 6) * gd),
                                     qRound((tb.width() - 24) * gd),
                                     qRound((tb.height() - 12) * gd));
                    return medianLuma(shot, area);
                };
                const int darkLuma = grabLuma(true);
                const int lightLuma = grabLuma(false);
                qInfo() << "CAPTURE-TOOLBAR  toolbar plate luma: dark =" << darkLuma
                        << "light =" << lightLuma;
                check(darkLuma >= 0 && darkLuma < 90,
                      QStringLiteral("深色主题浮条底板为深色(亮度中位数 < 90,得 %1)")
                          .arg(darkLuma));
                check(lightLuma > 150,
                      QStringLiteral("浅色主题浮条底板为浅色(亮度中位数 > 150,得 %1)")
                          .arg(lightLuma));
                // 两套主题必须**真的有差别** —— 只断言各自阈值时,若两者都恰好落在
                // 中间带(例如都在 150 附近)会漏掉"没跟主题走"的情况。
                check(lightLuma - darkLuma > 100,
                      QStringLiteral("浅/深主题下浮条底板明显不同(差值 > 100,得 %1)")
                          .arg(lightLuma - darkLuma));

                // 复位:后续断言要按"真实待截内容 + 半透底板"的口径跑
                QGuiApplication::setPalette(saved);
                overlay->setBarAlpha(savedAlpha);
                QApplication::processEvents();
            }

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

            // ---- B6. 标注态键盘撤销/重做(docs/17 §17.3 A 漏项)----
            //
            // 二级条上一直有撤销按钮,但键盘没有入口 —— 标注是"一手鼠标一手快捷键"
            // 的操作,画错一笔还得移回二级条点按钮。编辑器早有 Ctrl+Z/Y,浮条没有。
            // 这里用**真实按键**验,不直接调 undoStack():要证明的是"按键被接上了"。
            {
                QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
                QApplication::sendEvent(overlay, &undo);
                QApplication::processEvents();
                const QImage afterUndo = overlay->grab().toImage();
                const QRect stillThere = colorBounds(afterUndo, pen, insideSel);
                qInfo() << "CAPTURE-TOOLBAR  after Ctrl+Z stroke bbox:" << stillThere;
                check(stillThere.isNull(),
                      QStringLiteral("Ctrl+Z 撤销了刚画的那一笔(笔画消失)"));

                QKeyEvent redo(QEvent::KeyPress, Qt::Key_Y, Qt::ControlModifier);
                QApplication::sendEvent(overlay, &redo);
                QApplication::processEvents();
                const QImage afterRedo = overlay->grab().toImage();
                const QRect backAgain = colorBounds(afterRedo, pen, insideSel);
                qInfo() << "CAPTURE-TOOLBAR  after Ctrl+Y stroke bbox:" << backAgain;
                check(!backAgain.isNull(),
                      QStringLiteral("Ctrl+Y 重做出了那一笔(笔画回来)"));

                // Ctrl+Shift+Z 也必须能重做(与编辑器同一套三键)
                QKeyEvent undo2(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
                QApplication::sendEvent(overlay, &undo2);
                QKeyEvent redo2(QEvent::KeyPress, Qt::Key_Z,
                                Qt::ControlModifier | Qt::ShiftModifier);
                QApplication::sendEvent(overlay, &redo2);
                QApplication::processEvents();
                const QRect viaShift = colorBounds(overlay->grab().toImage(), pen, insideSel);
                check(!viaShift.isNull(),
                      QStringLiteral("Ctrl+Shift+Z 也能重做(与编辑器同一套按键)"));
            }

            // ---- B7. 标注偏好跨会话记忆(core settings round-trip)----
            //
            // 缺陷:每次进标注态都退回写死的红笔 4px,而同在浮条上的"底板不透明度"
            // 却会记住 —— 口吻不一致。这里验**真的落到设置里并能读回**,而不是
            // 只看成员变量(那只能证明"这轮记住了",证明不了"下次还能取到")。
            {
                const QJsonObject saved = CoreBridge::instance().call(
                    "save_settings",
                    {{"annotationColor", "#1C7ED6"},
                     {"annotationPenSize", 8},
                     {"annotationFontSize", 48}});
                qInfo() << "CAPTURE-TOOLBAR  save annotation prefs ok ="
                        << saved.value("ok").toBool();
                const QJsonObject readBack = CoreBridge::instance().call("get_settings");
                const QJsonObject d = readBack.value("data").toObject();
                qInfo() << "CAPTURE-TOOLBAR  readback color/size/font ="
                        << d.value("annotationColor").toString()
                        << d.value("annotationPenSize").toInt()
                        << d.value("annotationFontSize").toInt();
                check(d.value("annotationColor").toString() == QStringLiteral("#1C7ED6")
                          && d.value("annotationPenSize").toInt() == 8
                          && d.value("annotationFontSize").toInt() == 48,
                      QStringLiteral("标注偏好写进设置并能原样读回(跨会话记忆的前提)"));

                // 非档位值必须被 core 挡住 —— 否则存进去一个界面上选不到的值,
                // 下次进标注态就是"颜色/粗细与界面选中项对不上"。
                const QJsonObject bad = CoreBridge::instance().call(
                    "save_settings", {{"annotationPenSize", 7}});
                qInfo() << "CAPTURE-TOOLBAR  reject out-of-tier pen size ok ="
                        << !bad.value("ok").toBool();
                check(!bad.value("ok").toBool(),
                      QStringLiteral("档位外的粗细被 core 拒绝(界面不会出现选不中的值)"));
                // 复位成默认,避免影响后续断言与人工复核
                CoreBridge::instance().call("save_settings",
                                            {{"annotationColor", "#E03131"},
                                             {"annotationPenSize", 4},
                                             {"annotationFontSize", 36}});
            }

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

                // ---- v2:采集期遮罩整体隐藏 + 面板不进选区(docs/19 v2 §3.1/§3.2)----
                //
                // v1 在这里验的是"窗口区域挖洞"(GetWindowRgn / WindowFromPoint)。
                // v2 把那条整个删掉了,原因是两个死结:
                //  ① 窗口区域吃**设备像素**、选区是 Qt **逻辑像素**,150% 屏上洞按
                //     1:1 挖小 → 选区被遮罩盖住一部分 → 每帧都是静止画面 → 核心判
                //     duplicate → 长图只有一屏高(真机现象);
                //  ② 选区 = 整屏时"整窗减选区"是空区域 → 遮罩与面板一起消失。
                //
                // 现在的判据更直接:**遮罩必须整个不见了**(它还可见 = 页面点不到),
                // 且面板/描边不得与采集区重叠。
                {
                    const QSize panelSize(overlay::kStitchPanelWidth,
                                          overlay::kStitchPanelHeight);
                    // ---- 面板候选位置必须全部与选区零相交(v1 的 D1 正脸)----
                    //
                    // v1 在"上下都放不下"时把面板压进选区底部内侧,而那块区域在
                    // 采集期画不出来也点不到 —— 整屏选区下表现为"面板凭空消失"
                    // 且"点开始没反应"。
                    const QList<QRect> cands = overlay::stitchPanelCandidates(
                        sel, geo.size(), panelSize, overlay::kBarMargin);
                    bool allOutside = true;
                    for (const QRect& c : cands) {
                        if (!overlay::stitchPanelFitsOutside(c, sel)) allOutside = false;
                    }
                    qInfo() << "CAPTURE-TOOLBAR  panel candidates =" << cands.size()
                            << "allOutside =" << allOutside;
                    check(!cands.isEmpty() && allOutside,
                          QStringLiteral("面板候选位置全部与选区零相交(v1 的 D1 回归)"));

                    // ---- 面板矩形本身也不得落进选区 ----
                    const QRect panelRect = overlay::stitchPanelRectFor(
                        sel, geo.size(), panelSize, overlay::kBarMargin);
                    qInfo() << "CAPTURE-TOOLBAR  chosen panel" << panelRect
                            << "selection" << sel;
                    check(overlay::stitchPanelFitsOutside(panelRect, sel),
                          QStringLiteral("选定的面板位置与选区零相交"));

                    // ---- 整屏选区:面板必须仍然点得到 ----
                    //
                    // v1 在这个几何下必挂(空区域 → 遮罩与面板整块消失)。
                    const QRect fullSel(QPoint(0, 0), geo.size());
                    const QRect fullPanel = overlay::stitchPanelRectFor(
                        fullSel, geo.size(), panelSize, overlay::kBarMargin);
                    const QRect winRect(QPoint(0, 0), geo.size());
                    qInfo() << "CAPTURE-TOOLBAR  full-screen selection panel =" << fullPanel;
                    check(winRect.contains(fullPanel),
                          QStringLiteral("整屏选区下面板仍完整落在窗口内(点得到)"));

                    // ---- 描边四边必须在选区**外侧**(几何保证不进抓取区)----
                    const QList<QRect> frameRects = overlay::stitchFrameRects(sel, geo.size(), 2);
                    int outside = 0;
                    for (const QRect& r : frameRects) {
                        if (r.isNull()) continue;
                        if (!r.intersects(sel)) ++outside;
                    }
                    qInfo() << "CAPTURE-TOOLBAR  frame rects outside selection =" << outside
                            << "of" << frameRects.size();
                    check(outside > 0 && outside == frameRects.size(),
                          QStringLiteral("描边四边全部在选区之外(不与采集区重叠)"));

                    // ---- 采集期隐藏/恢复:用公开钩子直接验状态迁移 ----
                    //
                    // 真正的"点开始 → 隐藏遮罩"还要核心 begin 成功(依赖可用屏),
                    // 那是 --stitch-live-flow 的职责;这里只钉死"隐藏/恢复"这条
                    // 状态迁移本身不出错、且不会留下一个隐形的遮罩。
                    check(overlay->isVisible(), QStringLiteral("就绪态遮罩可见"));
                    overlay->setStitchOverlayHidden(true);
                    check(overlay->stitchOverlayHidden() && !overlay->isVisible(),
                          QStringLiteral("采集期遮罩整体隐藏(页面完全可交互)"));
                    overlay->setStitchOverlayHidden(false);
                    check(!overlay->stitchOverlayHidden() && overlay->isVisible(),
                          QStringLiteral("退出采集后遮罩恢复可见(不留隐形遮罩)"));
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
