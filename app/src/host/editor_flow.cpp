// 编辑器裁剪/旋转验收(--editor-flow)。
//
// 为什么单开一条验收:构图操作动的是**全部图元 + 底图**的坐标一致性 ——
// 变换矩阵写错一个符号,标注就会飞到图外或叠错位置,这类缺陷盯代码看不出来,
// 必须让"变换前后画在哪、底图像素落在哪"真实对一次账。两段:
//   A. 引擎级(不开窗口):AnnotationEngine 直测 —— 画笔/序号跟随旋转、
//      撤销重做还原、马赛克纹理随底图重建、裁剪后图元平移、
//      compose 不把裁剪覆盖层合成进输出;
//   B. 端到端:开真编辑器窗口 —— 真点「画笔」按钮、真投鼠标/键盘
//      (R 旋转 / Ctrl+Z 撤销 / C 裁剪 / Enter 应用与完成 / Esc 只退裁剪),
//      断言 committed 交出的合成图尺寸与内容。
//
// 合成底图四象限色(黄/蓝/绿/白)+ 标注默认红:任何一处颜色混叠都骗不过
// 像素断言(此前曾想用红象限,与画笔默认色 E03131 只差 g/b 通道,容差内
// 恒真 —— 换黄/蓝/绿后象限色与标注色两两距离都 > 100)。
//
// 日志前缀 EDITOR,失败行以 FAIL 开头;结束时 QApplication::quit()。
#include "host/acceptance.hpp"

#include "editor/annotation_engine.hpp"
#include "editor/annotation_toolbar.hpp"
#include "editor/editor_window.hpp"

#include <QAction>
#include <QApplication>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

namespace nb {
namespace {

int g_failures = 0;
QImage g_committed;   // finish() 交出的合成图

void check(bool ok, const QString& what) {
    if (ok) {
        qInfo().noquote() << "EDITOR  PASS" << what;
    } else {
        ++g_failures;
        qWarning().noquote() << "EDITOR  FAIL" << what;
    }
}

bool colorClose(const QColor& a, const QColor& b, int tolerance = 60) {
    return qAbs(a.red() - b.red()) <= tolerance && qAbs(a.green() - b.green()) <= tolerance
           && qAbs(a.blue() - b.blue()) <= tolerance;
}

/// 合成一次鼠标动作(投给真实控件,走完整事件链)。
void sendMouse(QWidget* w, QEvent::Type type, const QPointF& pos, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent ev(type, pos, w->mapToGlobal(pos.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void dragMouse(QWidget* w, const QPointF& from, const QPointF& to, int steps = 6) {
    sendMouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i) {
        const QPointF p(from.x() + (to.x() - from.x()) * i / steps,
                        from.y() + (to.y() - from.y()) * i / steps);
        sendMouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

void clickButton(QWidget* button) {
    const QPointF c(button->rect().center());
    sendMouse(button, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    sendMouse(button, QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);
}

void sendKey(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent ev(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &ev);
}

/// 四象限底图:左上黄 / 右上蓝 / 左下绿 / 右下白(与标注红两两可分辨)。
QImage makeBase(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.fillRect(QRect(0, 0, w / 2, h / 2), QColor(0xF0, 0xC0, 0x00));
    p.fillRect(QRect(w / 2, 0, w - w / 2, h / 2), QColor(0x00, 0x70, 0xD0));
    p.fillRect(QRect(0, h / 2, w / 2, h - h / 2), QColor(0x00, 0xB0, 0x50));
    p.end();
    return img;
}

/// region 内两图不同的像素数(同尺寸前提;尺寸不同返回 -1)。
qint64 countDifferent(const QImage& a, const QImage& b, const QRect& region) {
    if (a.size() != b.size()) return -1;
    const QImage ia = a.convertToFormat(QImage::Format_RGB32);
    const QImage ib = b.convertToFormat(QImage::Format_RGB32);
    const QRect area = region.intersected(QRect(QPoint(0, 0), ia.size()));
    qint64 diff = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const QRgb* la = reinterpret_cast<const QRgb*>(ia.constScanLine(y));
        const QRgb* lb = reinterpret_cast<const QRgb*>(ib.constScanLine(y));
        for (int x = area.left(); x <= area.right(); ++x)
            if (la[x] != lb[x]) ++diff;
    }
    return diff;
}

const QColor kPen(0xE0, 0x31, 0x31);   // 引擎默认标注色(画笔/序号未改色时的落笔色)

// ---------- A. 引擎级 ----------

void runEngineSection() {
    qInfo().noquote() << "EDITOR  SECTION A 引擎级(AnnotationEngine 直测)";
    auto* engine = new AnnotationEngine(makeBase(400, 300));

    // A1 画一笔:入栈 + 像素落点
    engine->setTool(EditorTool::Pen);
    engine->onCanvasPressed(QPointF(100, 50));
    engine->onCanvasMoved(QPointF(150, 80));
    engine->onCanvasReleased(QPointF(200, 120));
    check(engine->undoStack()->count() == 1, QStringLiteral("画笔一笔 = 一条撤销记录"));
    check(engine->compose().size() == QSize(400, 300), QStringLiteral("初始合成图 400×300"));
    check(colorClose(engine->compose().pixelColor(100, 50), kPen),
          QStringLiteral("笔画像素落在落点(100,50)"));

    // A2 顺时针旋转:尺寸互换 + 笔画跟随 + 象限色对位
    engine->rotate90(true);
    check(engine->compose().size() == QSize(300, 400),
          QStringLiteral("顺时针旋转后 300×400(宽高互换)"));
    check(colorClose(engine->compose().pixelColor(250, 100), kPen),
          QStringLiteral("笔画跟随旋转 (100,50)→(250,100)"));
    // 黄象限 [0,200)×[0,150) → CW 后 (300−y, x):x'∈(150,300], y'∈[0,200)
    check(colorClose(engine->compose().pixelColor(250, 50), QColor(0xF0, 0xC0, 0x00)),
          QStringLiteral("黄象限旋转后落在右上(250,50)"));

    // A3 撤销/重做:绝对还原(尺寸与笔画都对回去)
    const int countAfterRotate = engine->undoStack()->count();
    engine->undoStack()->undo();
    check(engine->compose().size() == QSize(400, 300) && colorClose(engine->compose().pixelColor(100, 50), kPen),
          QStringLiteral("撤销旋转:尺寸与笔画像素原位还原"));
    engine->undoStack()->redo();
    check(engine->compose().size() == QSize(300, 400),
          QStringLiteral("重做旋转:再次 300×400"));

    // A4 序号徽标跟随逆时针旋转
    engine->setColor(QColor(0x00, 0x00, 0xFF));
    engine->setTool(EditorTool::Number);
    engine->onCanvasPressed(QPointF(60, 80));   // 落 1 号徽标,半径约 18
    check(engine->undoStack()->count() == countAfterRotate + 1,
          QStringLiteral("序号一笔 = 一条撤销记录"));
    engine->rotate90(false);   // CCW 于 300×400:(60,80) → (y, W−x) = (80, 240)
    const QImage composed4 = engine->compose();
    check(composed4.size() == QSize(400, 300), QStringLiteral("逆时针旋转回 400×300"));
    check(colorClose(composed4.pixelColor(80 + 12, 240), QColor(0x00, 0x00, 0xFF)),
          QStringLiteral("徽标跟随逆时针旋转 (60,80)→(80,240)"));

    // A5 马赛克纹理随底图重建:旋转时正用马赛克,纹理失效后必须立刻重建,
    // 否则空纹理笔画静默无效(笔画前后无像素差 = 缺陷)。
    engine->setTool(EditorTool::Mosaic);
    engine->rotate90(true);   // 300×400;笔画跨 x=150 颜色分界线
    const QImage beforeMosaic = engine->compose();
    engine->onCanvasPressed(QPointF(140, 150));
    engine->onCanvasMoved(QPointF(160, 250));
    engine->onCanvasReleased(QPointF(170, 350));
    const QImage afterMosaic = engine->compose();
    check(countDifferent(beforeMosaic, afterMosaic, QRect(110, 120, 100, 260)) > 0,
          QStringLiteral("旋转后马赛克笔画可见(纹理已按新底图重建)"));

    // A6 裁剪:框交互 → compose 不含覆盖层 → 应用 → 底图与图元平移
    engine->setTool(EditorTool::Crop);
    engine->onCanvasPressed(QPointF(50, 40));
    engine->onCanvasMoved(QPointF(150, 120));
    engine->onCanvasReleased(QPointF(250, 200));
    check(engine->cropReady()
              && qAbs(engine->cropRect().x() - 50) < 0.5
              && qAbs(engine->cropRect().y() - 40) < 0.5
              && qAbs(engine->cropRect().width() - 200) < 0.5
              && qAbs(engine->cropRect().height() - 160) < 0.5,
          QStringLiteral("拖拽得到裁剪框 (50,40) 200×160"));
    // 覆盖层守卫:裁剪中间态下 compose() 的框外像素**不得**被遮罩压暗
    check(colorClose(engine->compose().pixelColor(5, 5), QColor(0x00, 0xB0, 0x50), 50),
          QStringLiteral("裁剪中间态 compose 框外未被遮罩压暗(覆盖层已隐藏)"));
    engine->applyCrop();
    const QImage cropped = engine->compose();
    check(cropped.size() == QSize(200, 160), QStringLiteral("应用裁剪后 200×160"));
    check(colorClose(cropped.pixelColor(0, 0), QColor(0x00, 0xB0, 0x50)),
          QStringLiteral("裁剪后 (0,0) = 原图 (50,40) 的绿"));
    // 笔画经历 CW→CCW→CW(净一次 CW):(100,50)→(250,100),裁剪平移后 → (200,60)。
    // ⚠️ 采样点取 (198,60):笔画起点恰在图右边线上(x=200 越界,pixelColor 返回
    //    无效色恒 FAIL),往圆头内侧退 2px 才是真实落点。
    check(colorClose(cropped.pixelColor(198, 60), kPen),
          QStringLiteral("笔画随裁剪平移到 (200,60)"));
    check(engine->tool() != EditorTool::Crop, QStringLiteral("应用裁剪后退出裁剪工具"));
    engine->undoStack()->undo();
    check(engine->compose().size() == QSize(300, 400),
          QStringLiteral("撤销裁剪:底图尺寸还原"));

    // A8 裁剪确认/取消按钮:引擎绘制的屏幕常量按钮(✓ 绿 / ✕ 红),
    // 按下 + 松手都在按钮上才触发;移出松手 = 放弃(系统按钮同语义)
    engine->setTool(EditorTool::Crop);
    engine->onCanvasPressed(QPointF(60, 50));
    engine->onCanvasMoved(QPointF(140, 115));
    engine->onCanvasReleased(QPointF(220, 180));
    check(engine->cropReady(), QStringLiteral("A8 拖出第二个裁剪框 (60,50) 160×130"));
    const QRectF cancelBtn = engine->cropButtonRect(1);
    const QRectF confirmBtn0 = engine->cropButtonRect(0);
    check(confirmBtn0.isValid() && cancelBtn.isValid() && !confirmBtn0.intersects(cancelBtn),
          QStringLiteral("确认/取消按钮均存在且互不重叠"));
    check(QRectF(0, 0, 300, 400).contains(cancelBtn) && QRectF(0, 0, 300, 400).contains(confirmBtn0),
          QStringLiteral("按钮完整落在底图内(永远点得到)"));
    // 点「取消」:框丢弃、退出裁剪,底图不动
    engine->onCanvasPressed(cancelBtn.center());
    engine->onCanvasReleased(cancelBtn.center());
    check(engine->compose().size() == QSize(300, 400) && engine->tool() != EditorTool::Crop
              && !engine->cropReady(),
          QStringLiteral("点「取消」:丢弃裁剪框并退出,尺寸未变"));
    // 再拖框 → 点「确认」:等价于 Enter 应用
    engine->setTool(EditorTool::Crop);
    engine->onCanvasPressed(QPointF(60, 50));
    engine->onCanvasMoved(QPointF(140, 115));
    engine->onCanvasReleased(QPointF(220, 180));
    const QRectF confirmBtn = engine->cropButtonRect(0);
    engine->onCanvasPressed(confirmBtn.center());
    engine->onCanvasReleased(confirmBtn.center());
    check(engine->compose().size() == QSize(160, 130) && engine->tool() != EditorTool::Crop,
          QStringLiteral("点「确认」:应用裁剪 160×130 并退出裁剪工具"));
    engine->undoStack()->undo();   // 回到 300×400,给下一断言留稳定底图
    // 按下按钮后移出松手 = 不触发(裁剪态保持)
    engine->setTool(EditorTool::Crop);
    engine->onCanvasPressed(QPointF(60, 50));
    engine->onCanvasMoved(QPointF(140, 115));
    engine->onCanvasReleased(QPointF(220, 180));
    const QRectF cancelBtn2 = engine->cropButtonRect(1);
    engine->onCanvasPressed(cancelBtn2.center());
    engine->onCanvasMoved(QPointF(10, 10));   // 按住不放拖出按钮
    engine->onCanvasReleased(QPointF(10, 10));
    check(engine->tool() == EditorTool::Crop && engine->cropReady()
              && engine->compose().size() == QSize(300, 400),
          QStringLiteral("按钮上按下、框外松手:不触发,仍在裁剪态"));

    // A7 析构安全:撤销栈里同时存在 AddItem 与 Transform 命令时的销毁顺序
    // (此刻还留着活动裁剪覆盖层,顺带覆盖"裁剪态直接销毁"的路径)
    delete engine;
    check(true, QStringLiteral("引擎析构无崩溃(混合命令栈 + 活动裁剪态)"));
}

// ---------- B. 端到端(真窗口) ----------

void runWindowSection() {
    qInfo().noquote() << "EDITOR  SECTION B 端到端(真窗口 + 真事件)";
    auto* win = new EditorWindow(makeBase(400, 300));
    QObject::connect(win, &EditorWindow::committed, [](const QImage& img) { g_committed = img; });
    win->show();

    QTimer::singleShot(400, qApp, [win] {
        auto* canvas = win->findChild<QGraphicsView*>(QStringLiteral("editorCanvas"));
        check(canvas != nullptr, QStringLiteral("画布可按 objectName 定位"));
        if (!canvas) {
            qInfo() << "EDITOR  RESULT failures =" << g_failures;
            QApplication::quit();
            return;
        }
        QWidget* viewport = canvas->viewport();
        const auto scenePos = [canvas](qreal x, qreal y) {
            const QPointF v = canvas->mapFromScene(QPointF(x, y));
            return v;
        };

        // B1 真点「画笔」按钮 → 真拖一笔
        auto* bar = win->findChild<AnnotationToolbar*>();
        QAction* penAction = nullptr;
        if (bar) {
            const auto bars = bar->findChildren<QToolBar*>();
            for (QToolBar* tb : bars) {
                for (QAction* a : tb->actions()) {
                    if (a->text() == QStringLiteral("画笔")) {
                        penAction = a;
                        break;
                    }
                }
                if (penAction) break;
            }
        }
        QWidget* penButton = penAction && bar ? bar->findChildren<QToolBar*>().value(0)->widgetForAction(penAction) : nullptr;
        check(penButton != nullptr, QStringLiteral("「画笔」按钮可定位"));
        if (penButton) {
            clickButton(penButton);
            dragMouse(viewport, scenePos(100, 50), scenePos(200, 120));
            check(win->compose().size() == QSize(400, 300)
                      && colorClose(win->compose().pixelColor(100, 50), kPen),
                  QStringLiteral("点画笔 + 拖拽:笔画落在 (100,50)"));
        }

        // B2 R 键旋转 → Ctrl+Z 撤销
        sendKey(win, Qt::Key_R);
        check(win->compose().size() == QSize(300, 400), QStringLiteral("R 键顺时针旋转为 300×400"));
        sendKey(win, Qt::Key_Z, Qt::ControlModifier);
        check(win->compose().size() == QSize(400, 300), QStringLiteral("Ctrl+Z 撤销旋转回 400×300"));

        // B3 C 键裁剪 → Esc 只退裁剪不关窗
        sendKey(win, Qt::Key_C);
        dragMouse(viewport, scenePos(50, 40), scenePos(250, 200));
        sendKey(win, Qt::Key_Escape);
        check(win->isVisible() && win->compose().size() == QSize(400, 300),
              QStringLiteral("裁剪态 Esc 只退裁剪(窗口仍在、尺寸未变)"));

        // B4 再进裁剪 → Enter 应用(不关窗)
        sendKey(win, Qt::Key_C);
        dragMouse(viewport, scenePos(50, 40), scenePos(250, 200));
        sendKey(win, Qt::Key_Return);
        check(win->isVisible() && win->compose().size() == QSize(200, 160),
              QStringLiteral("Enter 应用裁剪:窗口仍在、合成图 200×160"));

        // B5 真鼠标点「确认」按钮:按钮画在覆盖层上,真实视口坐标点击走完整事件链
        // (用户没有键盘也能完成裁剪 —— 这正是本轮补按钮的目的)
        sendKey(win, Qt::Key_C);
        dragMouse(viewport, scenePos(30, 30), scenePos(100, 90));
        check(win->engine() && win->engine()->cropReady(),
              QStringLiteral("B5 第二个裁剪框已就绪"));
        const QRectF confirmScene = win->engine() ? win->engine()->cropButtonRect(0) : QRectF();
        check(confirmScene.isValid(), QStringLiteral("确认按钮场景矩形可取得"));
        if (confirmScene.isValid()) {
            const QPointF c = canvas->mapFromScene(confirmScene.center());
            sendMouse(viewport, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
            sendMouse(viewport, QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);
            check(win->isVisible() && win->compose().size() == QSize(70, 60),
                  QStringLiteral("点「确认」按钮应用裁剪:70×60(不经键盘)"));
        }
        g_committed = QImage();
        sendKey(win, Qt::Key_Return);
        check(!g_committed.isNull() && g_committed.size() == QSize(70, 60),
              QStringLiteral("Enter 完成:committed 交出 70×60"));

        QTimer::singleShot(200, qApp, [] {
            qInfo() << "EDITOR  RESULT failures =" << g_failures;
            QApplication::quit();
        });
    });
}

} // namespace

void runEditorFlow() {
    runEngineSection();
    QTimer::singleShot(0, qApp, [] { runWindowSection(); });
}

} // namespace nb
