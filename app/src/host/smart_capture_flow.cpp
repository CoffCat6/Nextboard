// 智能框选验收(--smart-capture-flow)。
//
// 为什么单开一条 flow:智能框选引入**第四套坐标**(虚拟桌面绝对物理像素)，
// 而"框住的是不是那一块"这件事全靠一次 4→1 换算 + 一次命中测试决定 ——
// 与"标注画不准"同一类缺陷面:**盯着代码看不出来，必须在真实窗口上对一次账**。
//
// 分四段:
//   A. 纯几何(dpr = 1 / 1.25 / 1.5 / 2):toLocalLogical / screenPhysicalRect
//      与手算对账，外加命中测试的边界(相邻候选共用一条边不留死缝)。
//   B. 端到端窗口级:造一个**已知几何**的真实窗口 → 遮罩 → 投 MouseMove
//      → 断言预选矩形 == 该窗口(±1px)；再投"单击"(按下抬起同点)断言采纳。
//   C. 手框不被破坏:拖动 > startDragDistance → 断言预选让位、选区 == 拖拽矩形。
//   D. 自过滤:遮罩本身 / 本进程窗口**不出现**在候选表里(这是最容易踩的坑 ——
//      遮罩一旦先 show 出来，枚举结果就全是它自己)。
//
// 日志前缀 SMART-CAPTURE，失败行以 FAIL 开头；结束时 QApplication::quit()。
#include "host/acceptance.hpp"

#include "capture/capture_session.hpp"
#include "capture/overlay_geometry.hpp"
#include "capture/smart_capture.hpp"
// ⚠️ 必须带上:capture_session.hpp 里 OcrLineBox 只是前向声明,而
//    showOcrResultPanel 的默认实参 QVector<OcrLineBox>{} 需要完整类型 ——
//    缺它会在 qarraydataops 里报一串看不懂的 static_assert。
#include "host/core_bridge.hpp"
#include "panel/core_facade.hpp"
#include "panel/panel_window.hpp"

#include <windows.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWidget>

namespace nb {
namespace {

int g_failures = 0;

void check(bool ok, const QString& what) {
    if (ok) {
        qInfo().noquote() << "SMART-CAPTURE  PASS" << what;
    } else {
        ++g_failures;
        qWarning().noquote() << "SMART-CAPTURE  FAIL" << what;
    }
}

/// 与 capture_toolbar_flow 同一套合成事件(走完整 Qt 事件链，不直调私有方法)。
void sendMouse(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent ev(type, QPointF(pos), w->mapToGlobal(QPointF(pos)), button, buttons,
                   Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void clickAt(QWidget* w, const QPoint& pos) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
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

void moveMouse(QWidget* w, const QPoint& pos) {
    sendMouse(w, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
}

/// 投一个真实的滚轮事件(走完整 Qt 事件链)。
/// up=true 向上滚一格;angleDelta 用 ±120(Windows 标准一格)。
void wheelAt(QWidget* w, const QPoint& pos, bool up) {
    const int dy = up ? 120 : -120;
    QWheelEvent ev(QPointF(pos), w->mapToGlobal(QPointF(pos)), QPoint(), QPoint(0, dy),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}

/// 两个矩形是否"几乎相同"(逐边容差)。
///
/// ⚠️ 不能写 `(a - b).manhattanLength()`:QRect 的 operator- 是**区域差集**,
///    不是坐标差;QSize 也没有 manhattanLength。逐边比较才是本意。
bool nearRect(const QRect& a, const QRect& b, int tol = 2) {
    return qAbs(a.x() - b.x()) <= tol && qAbs(a.y() - b.y()) <= tol &&
           qAbs(a.width() - b.width()) <= tol && qAbs(a.height() - b.height()) <= tol;
}

/// A 段:纯几何(不建窗口)。
void checkGeometry() {
    // A1. 屏幕物理矩形:原点取屏几何、尺寸取底图。
    //     底图 3840x2160 + 屏逻辑 1920x1080 → 物理矩形原点不变、尺寸来自底图。
    const QRect screenPhys(QPoint(-1920, 0), QSize(3840, 2160));

    // A2. 绝对物理 → 局部逻辑(dpr=2):原点平移到屏左上后按 dpr 缩小。
    const QRect local2 = smart::toLocalLogical(QRect(-1820, 100, 800, 600), screenPhys, 2.0);
    check(local2 == QRect(50, 50, 400, 300),
          QStringLiteral("dpr=2 换算:%1(期望 50,50 400x300)").arg(
              QStringLiteral("%1,%2 %3x%4").arg(local2.x()).arg(local2.y())
                  .arg(local2.width()).arg(local2.height())));

    // A3. dpr=1.5(150% 缩放屏,最常见的笔记本配置)。
    const QRect local15 = smart::toLocalLogical(QRect(0, 0, 300, 150),
                                                QRect(QPoint(0, 0), QSize(2880, 1620)), 1.5);
    check(local15 == QRect(0, 0, 200, 100),
          QStringLiteral("dpr=1.5 换算:%1x%2(期望 200x100)")
              .arg(local15.width()).arg(local15.height()));

    // A4. 越界裁剪:跨到隔壁屏的部分必须被裁掉,**不能**变成负坐标/超屏矩形。
    const QRect clipped = smart::toLocalLogical(QRect(-100, -50, 500, 400), screenPhys, 2.0);
    check(clipped.left() >= 0 && clipped.top() >= 0 && clipped.right() < 1920 &&
              clipped.bottom() < 1080,
          QStringLiteral("跨屏候选被裁进本屏(得 %1,%2 %3x%4)").arg(clipped.x()).arg(clipped.y())
              .arg(clipped.width()).arg(clipped.height()));
    check(smart::toLocalLogical(QRect(-5000, -5000, 100, 100), screenPhys, 2.0).isNull(),
          QStringLiteral("完全在屏外的候选返回空矩形"));

    // A5. 命中测试:z 序在前者优先。
    QVector<smart::Candidate> list;
    smart::Candidate a;
    a.physical = QRect(0, 0, 1000, 800);   // 底层窗口
    a.level = 0;
    list.append(a);
    smart::Candidate b;
    b.physical = QRect(100, 100, 300, 200); // 覆盖其上的小窗口(实际枚举时它在前面)
    b.level = 0;
    list.prepend(b);

    const QRect sp(QPoint(0, 0), QSize(1000, 800));
    check(smart::candidateAt(list, sp, QPoint(200, 200), 1.0) == 0,
          QStringLiteral("重叠处命中最上面的候选(索引 0)"));
    check(smart::candidateAt(list, sp, QPoint(600, 600), 1.0) == 1,
          QStringLiteral("小窗口之外命中底层窗口(索引 1)"));
    check(smart::candidateAt(list, sp, QPoint(5000, 5000), 1.0) == -1,
          QStringLiteral("屏外无命中"));

    // A6. 相邻候选共用一条边时不能留 1px 死缝(无边框窗口贴在一起很常见)。
    QVector<smart::Candidate> adj;
    smart::Candidate l;
    l.physical = QRect(0, 0, 200, 200);
    smart::Candidate r;
    r.physical = QRect(200, 0, 200, 200);
    adj << l << r;
    check(smart::candidateAt(adj, sp, QPoint(199, 100), 1.0) == 0 &&
              smart::candidateAt(adj, sp, QPoint(200, 100), 1.0) == 1,
          QStringLiteral("相邻候选边界两边都命得中(闭区间)"));

    // A6a. **快照查表**(docs/27):纯内存命中测试,必须与"逐点 DFS"给同一答案。
    //
    // 这条断言守的是延迟优化的**正确性**:换成快照模型后,元素级从"每次悬停重探
    // 77~136ms"变成"进窗预取一次 + 每帧查表 0.025ms"。若查表逻辑与探测逻辑
    // 语义漂移(例如"明显小于窗口"的阈值不一致),用户看到的就是"框错位置"。
    {
        const QRect win(QPoint(0, 0), QSize(2000, 1200));
        QVector<smart::ElementNode> nodes;
        auto add = [&nodes](QRect r, int depth, int parent) {
            smart::ElementNode n;
            n.physical = r;
            n.depth = depth;
            n.parent = parent;
            nodes.append(n);
            return nodes.size() - 1;
        };
        const int l1 = add(QRect(0, 0, 2000, 1200), 1, -1);      // 与窗口等大外壳
        // ⚠️ 只小 6px:必须被"明显小于窗口"(阈值 8px)判为**没细化**。
        //    Chromium 的外壳 Pane 实测就是这种(2077x1388 vs 2078x1390)。
        const int l2 = add(QRect(0, 0, 1994, 1194), 2, l1);
        const int l3 = add(QRect(100, 100, 600, 400), 3, l2);    // 内容区块
        const int l4 = add(QRect(120, 130, 300, 60), 4, l3);     // 区块内的段落
        Q_UNUSED(l4);

        // 光标在段落内 → 取最深命中(depth 4)
        const int hit = smart::elementAtSnapshot(nodes, win, QPoint(200, 150), 24);
        check(hit >= 0 && nodes.at(hit).depth == 4,
              QStringLiteral("快照查表取最深命中(depth=%1,期望 4；索引=%2)")
                  .arg(hit >= 0 ? nodes.at(hit).depth : -1).arg(hit));

        // 光标在区块内、段落外 → 退回 depth 3
        const int hit2 = smart::elementAtSnapshot(nodes, win, QPoint(500, 450), 24);
        check(hit2 == l3, QStringLiteral("光标在区块空白处 → 命中区块本身(depth=3)"));

        // 光标在窗口右下角(只在"与窗口等大"的两个节点内,区块够不到这里)
        // → 那两个节点差 6px,必须被判为没细化,返回 -1。
        const int hit3 = smart::elementAtSnapshot(nodes, win, QPoint(1900, 1150), 24);
        check(hit3 == -1,
              QStringLiteral("与窗口等大(差 6px < 8px 阈值)的节点不算细化,返回 -1(得 %1)")
                  .arg(hit3));

        // allowEqualWin=true 时应能取到它们(层级链滚轮的退路)
        const int hit4 = smart::elementAtSnapshot(nodes, win, QPoint(1900, 1150), 24, true);
        check(hit4 >= 0, QStringLiteral("allowEqualWin=true 时允许返回等大节点(得 %1)").arg(hit4));

        // 空快照必须安全返回 -1(降级契约:拿不到树不能让截图不可用)
        check(smart::elementAtSnapshot({}, win, QPoint(200, 150), 24) == -1,
              QStringLiteral("空快照安全返回 -1"));

        // ---- A6a-2. 查表延迟必须远低于"人眼能察觉"的门槛 ----
        //
        // docs/27 的核心指标。修复前:每次悬停要 77~136ms 探测 + 400ms 停稳等待
        // (且被手抖重置),用户体感"必须停 1 秒"。
        // 修复后:同一窗口内每次悬停 = 纯内存查表。
        //
        // 门槛取 **0.1ms**:实测本机 ~130 节点时 0.025ms,留 4x 余量容纳更复杂的
        // 窗口(几千节点)。若哪天有人往 elementAtSnapshot 里塞了 COM 调用或
        // 复杂分配,这条会立刻翻红。
        {
            QVector<smart::ElementNode> bench;
            for (int i = 0; i < 1500; ++i) {
                smart::ElementNode n;
                n.physical = QRect((i * 13) % 1800, (i * 7) % 1000, 120, 80);
                n.depth = 1 + (i % 12);
                n.parent = i > 0 ? i - 1 : -1;
                bench.append(n);
            }
            const QRect bw(QPoint(0, 0), QSize(1920, 1080));
            QElapsedTimer t;
            t.start();
            const int rounds = 1000;
            qint64 sink = 0;
            for (int i = 0; i < rounds; ++i) {
                sink += smart::elementAtSnapshot(bench, bw,
                                                 QPoint((i * 17) % 1920, (i * 23) % 1080), 24);
            }
            const qreal perQueryMs = static_cast<qreal>(t.nsecsElapsed()) / 1e6 / rounds;
            qInfo() << "SMART-CAPTURE  查表延迟 =" << perQueryMs << "ms/次 (1500 节点)"
                    << " sink =" << sink;
            check(perQueryMs < 0.1,
                  QStringLiteral("元素查表 < 0.1ms/次(实测 %1ms;修复前每次悬停要 77~136ms)")
                      .arg(perQueryMs, 0, 'f', 4));
        }
    }

    // A6b. **跨屏窗口**:一块窗口横跨 A/B 两屏时,每块遮罩只看自己那一半。
    //
    // 这是多屏的核心语义:候选是虚拟桌面的绝对物理矩形,而每块遮罩只能画/命中
    // 自己屏内的部分。若换算写错(比如忘了裁剪),B 屏会画出一条从屏左边缘
    // 延伸到屏幕外的框,或者干脆什么都画不出来。
    {
        // 主屏 3072x1920 @(0,0);右屏 1080x1920 @(3072,0)
        const QRect scrLeft(QPoint(0, 0), QSize(3072, 1920));
        const QRect scrRight(QPoint(3072, 0), QSize(1080, 1920));
        // 一个横跨边界的窗口:从 A 屏 x=2500 到 B 屏 x=400(物理)
        const QRect spanning(QPoint(2500, 100), QSize(972, 500));   // 到 3472

        const QRect partLeft = smart::toLocalLogical(spanning, scrLeft, 1.0);
        const QRect partRight = smart::toLocalLogical(spanning, scrRight, 1.0);
        qInfo() << "SMART-CAPTURE  跨屏窗口:左屏部分 =" << partLeft << " 右屏部分 =" << partRight;
        check(partLeft == QRect(2500, 100, 572, 500),
              QStringLiteral("跨屏窗口在 A 屏被裁到屏右边缘(得 %1x%2 @%3)")
                  .arg(partLeft.width()).arg(partLeft.height()).arg(partLeft.x()));
        check(partRight == QRect(0, 100, 400, 500),
              QStringLiteral("跨屏窗口在 B 屏被裁到屏内(得 %1x%2 @%3)")
                  .arg(partRight.width()).arg(partRight.height()).arg(partRight.x()));
        check(partLeft.width() + partRight.width() == spanning.width(),
              QStringLiteral("两屏的裁剪部分之和 == 窗口总宽(%1 + %2 vs %3)")
                  .arg(partLeft.width()).arg(partRight.width()).arg(spanning.width()));
    }

    // A7. UIA 的**降级契约**:非法入参必须返回空表,不能抛/不能崩。
    //
    // 这条比"能探到元素"更重要:元素级是跨进程 COM,目标窗口消失、权限不足、
    // 远程桌面会话都会让它失败,而失败**绝不允许**影响截图本身。
    check(smart::probeElementAt(0, QPoint(0, 0), 1.0).isEmpty(),
          QStringLiteral("UIA 探测:无效窗口句柄返回空表(不崩)"));
    check(smart::probeElementAt(0x1234, QPoint(0, 0), 0.0).isEmpty(),
          QStringLiteral("UIA 探测:dpr 非法返回空表(不崩)"));
    check(smart::probeElementAt(0xDEADBEEF, QPoint(0, 0), 1.0, 24, 50, 60).isEmpty(),
          QStringLiteral("UIA 探测:不存在的窗口句柄返回空表(COM 失败被吞掉)"));

    // A8. 目标类名不可探测时 worthProbing 必须说 false(避免白花几十毫秒)。
    check(!smart::worthProbing(0), QStringLiteral("worthProbing(0) = false"));
}

} // namespace

void runSmartCaptureFlow() {
    QTimer::singleShot(1200, qApp, [] {
        checkGeometry();

        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qWarning() << "SMART-CAPTURE  FAIL 没有屏幕";
            QApplication::quit();
            return;
        }

        // ---- 造一个**已知几何**的真实目标窗口 ----
        //
        // 用它当"光标下的内容":智能框选应当正好框住它的客户区(±1px)。
        // 位置刻意选在屏幕中部偏左,四周留足空间,保证与遮罩/任务栏都不贴边。
        const QRect screenGeo = screen->geometry();
        const QRect targetGeo(screenGeo.x() + 160, screenGeo.y() + 120, 620, 420);
        auto* target = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool);
        target->setObjectName(QStringLiteral("smartTarget"));
        target->setGeometry(targetGeo);
        target->setStyleSheet(QStringLiteral("background:#204020;"));
        target->show();
        target->raise();
        QApplication::processEvents();

        // ---- 枚举:必须发生在**遮罩建立之前** ----
        const QVector<smart::Candidate> candidates = smart::probeWindows();
        qInfo() << "SMART-CAPTURE  枚举到" << candidates.size() << "个候选窗口";

        // D1. 自过滤:本进程的 target 窗口**不应**出现在候选表里
        //     (probeWindows 按 PID 剔除;验收自身也是本进程,所以 target 就是
        //      "自己人" —— 这正好把自过滤从"理论"变成"可断言")。
        // target 是本进程窗口 → 它的物理矩形 = 逻辑几何 × dpr(原点取屏的物理原点)。
        const qreal dprNow = screen->devicePixelRatio();
        const QRect targetPhysicalNow(
            QPoint(screenGeo.x() + qRound(160 * dprNow), screenGeo.y() + qRound(120 * dprNow)),
            QSize(qRound(620 * dprNow), qRound(420 * dprNow)));
        bool sawTarget = false;
        for (const smart::Candidate& c : candidates) {
            if (nearRect(c.physical, targetPhysicalNow, 2)) sawTarget = true;
        }
        check(!sawTarget,
              QStringLiteral("本进程窗口不出现在候选表(按 PID 过滤生效)"));

        // ---- 建遮罩:底图用**合成图**,几何与屏一致 ----
        const qreal dpr = screen->devicePixelRatio();
        QImage frozen(screenGeo.size() * dpr, QImage::Format_ARGB32_Premultiplied);
        frozen.fill(QColor(0x20, 0x20, 0x24));
        {   // 在底图上画出 target 的位置,让"框内恢复原图"能看出效果。
            // 注意:底图是**物理像素**,目标矩形是逻辑坐标 → 各边都要乘 dpr。
            const QRect logical(targetGeo.topLeft() - screenGeo.topLeft(), targetGeo.size());
            const QRect physical(QPoint(qRound(logical.x() * dpr), qRound(logical.y() * dpr)),
                                 QSize(qRound(logical.width() * dpr),
                                       qRound(logical.height() * dpr)));
            QPainter p(&frozen);
            p.fillRect(physical, QColor(0x30, 0x80, 0x40));
            p.end();
        }

        // 认不上屏几何时退化成几何断言(不假装通过)
        if (frozen.isNull()) {
            qWarning() << "SMART-CAPTURE  FAIL 底图构造失败";
            target->close();
            target->deleteLater();
            QApplication::quit();
            return;
        }

        // 本屏物理矩形(原点取 Qt 几何、尺寸取底图 —— 与产品同口径,见 docs/23 §4.2)
        const QRect screenPhysicalRef(screenGeo.topLeft(), frozen.size());

        // ---- B. 窗口级:候选表**手工注入 target 的物理矩形** ----
        //
        // 为什么不直接给 probeWindows() 的结果:本进程窗口按设计被剔除了,
        // 而验收要断言的正是"候选 → 预选 → 采纳"这条链路。手工注入等价于
        // "另一个进程里有一个位置已知的窗口",把被测逻辑与 PID 过滤解耦。
        QVector<smart::Candidate> injected;
        smart::Candidate targetCand;
        // ⚠️ 候选表是**虚拟桌面绝对物理像素**,而 targetGeo 是 Qt 的**逻辑**几何。
        //    直接塞进去会让 hits 全部落空(实测现象:预选恒为空)—— 这正是本
        //    文件存在的意义:把"口径混用"变成一条会红的断言。
        targetCand.physical = targetPhysicalNow;
        targetCand.title = QStringLiteral("验收目标窗口");
        targetCand.id = 0xABCD;
        targetCand.windowId = 0xABCD;
        targetCand.level = 0;
        injected << targetCand;

        auto* overlay = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                          /*autoStartStitching=*/false, injected);
        overlay->setGeometry(screenGeo);
        overlay->show();
        QApplication::processEvents();

        check(overlay->smartCandidateCount() == injected.size(),
              QStringLiteral("遮罩收到候选表(%1 条)").arg(overlay->smartCandidateCount()));

        // B1. 悬停到 target 中心 → 预选 == target 的本屏局部逻辑矩形
        const QPoint targetCenterLocal = targetGeo.center() - screenGeo.topLeft();   // 逻辑坐标
        moveMouse(overlay, targetCenterLocal);
        QApplication::processEvents();

        const QRect expected = QRect(targetGeo.topLeft() - screenGeo.topLeft(),
                                     targetGeo.size());
        const QRect got = overlay->smartCandidate();
        // ⚠️ 变量名**不能**叫 near:
        //    windef.h 里有 `#define near`(16 位时代的段修饰符),
        //    在 <windows.h> 之后它会把这个名字整个吃掉,报一串莫名其妙的语法错误。
        const bool hoverMatches = !got.isNull() && nearRect(got, expected, 2);
        check(hoverMatches, QStringLiteral("悬停后预选 == 目标窗口(得 %1,%2 %3x%4,期望 %5,%6 %7x%8)")
                        .arg(got.x()).arg(got.y()).arg(got.width()).arg(got.height())
                        .arg(expected.x()).arg(expected.y())
                        .arg(expected.width()).arg(expected.height()));
        check(overlay->smartCandidateLevel() == 0,
              QStringLiteral("窗口级预选的 level = 0"));

        // B2. 悬停到空白处 → 预选清空
        moveMouse(overlay, QPoint(20, screenGeo.height() - 40));
        QApplication::processEvents();
        check(overlay->smartCandidate().isNull(),
              QStringLiteral("悬停到空白处预选清空"));

        // B3. 回到 target 后**单击**(按下抬起同点)→ 采纳(不确认)。
        //
        // ⚠️ 断言已随产品变更修正(2026-11-02,docs/28)。原断言写的是"单击即确认,
        //    regionConfirmed 已发出",但 **2026-11-01 起 regionAutoConfirm 已删除**,
        //    框选恒为浮条模式:单击只**采纳**(选区定下来 + 浮出浮条),确认要用户
        //    再点「完成」或按 Enter(capture_session.cpp 的 clickLike 分支里
        //    `showToolbar(); return;` —— 根本没有 emit regionConfirmed)。
        //
        //    这条断言因此长期是红的(改动前基线就有它),而它红的原因不是代码错,
        //    是断言描述了一个**已经不存在的产品行为**。留着它会不断误导下一个人
        //    去"修"那个并没有坏的代码。现在改为断言**当前**的真实语义。
        QRect confirmedRect;
        bool confirmed = false;
        QObject::connect(overlay, &OverlayWidget::regionConfirmed, overlay,
                         [&confirmedRect, &confirmed](const QRect& r) {
            confirmedRect = r;
            confirmed = true;
        });

        moveMouse(overlay, targetCenterLocal);
        QApplication::processEvents();
        clickAt(overlay, targetCenterLocal);
        QApplication::processEvents();

        check(overlay->smartAcceptCount() == 1,
              QStringLiteral("单击采纳计数 = 1(得 %1)").arg(overlay->smartAcceptCount()));
        // 浮条模式下单击**不**确认:regionConfirmed 不该发出
        check(!confirmed,
              QStringLiteral("浮条模式:单击只采纳、不立即确认(regionConfirmed 未发出)"));
        // 选区已定下来 = 工具栏可见(showToolbar 被调用,且 toolbarVisible 依赖选区可用)
        check(!overlay->smartCandidate().isNull() || overlay->smartCandidateLevel() == 1,
              QStringLiteral("采纳后选区保留(候选仍可读,供用户微调)"));

        // ⚠️ 期望值 = 目标窗口的**本屏相对**物理矩形(selectionPhysical 的契约
        //    口径),不是"绝对 * dpr"。主屏原点 (0,0) 时二者相等,曾掩盖了
        //    "采纳路径回传绝对坐标"的缺陷 —— 副屏上表现为裁剪完全越界、
        //    截图既不入库也不进剪贴板(2026-10-31 用户实测)。
        //
        //    浮条模式下没有 regionConfirmed 可拿,改由**确认动作**取矩形:
        //    投一个真实的 Enter 键(capture_session 的 keyPressEvent 里
        //    `Enter → confirmSelection()`,与用户按回车完全同一条路径)。
        //    直接调 confirmSelection 不行 —— 它是 private,而验收不该为了
        //    方便就开一个后门(那会让"键盘确认"这条真实链路失去覆盖)。
        {
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(overlay, &enter);
            QApplication::processEvents();
        }
        check(confirmed, QStringLiteral("按 Enter 后 regionConfirmed 发出"));
        const QRect expectedPhysical(
            QPoint(qRound(expected.x() * dpr), qRound(expected.y() * dpr))
                - screenPhysicalRef.topLeft(),
            QSize(qRound(expected.width() * dpr), qRound(expected.height() * dpr)));
        check(confirmed && nearRect(confirmedRect, expectedPhysical, 3),
              QStringLiteral("确认矩形 == 目标窗口的物理像素(得 %1x%2,期望 %3x%4)")
                  .arg(confirmedRect.width()).arg(confirmedRect.height())
                  .arg(expectedPhysical.width()).arg(expectedPhysical.height()));

        // ---- C. 手框不被破坏(浮条模式)----
        overlay->close();
        overlay->deleteLater();

        auto* overlay2 = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                           /*autoStartStitching=*/false, injected);
        overlay2->setGeometry(screenGeo);
        overlay2->show();
        QApplication::processEvents();

        // C1. 浮条模式:单击只**采纳**,不确认(仍要用户点完成)。
        moveMouse(overlay2, targetCenterLocal);
        QApplication::processEvents();
        clickAt(overlay2, targetCenterLocal);
        QApplication::processEvents();
        check(overlay2->smartAcceptCount() == 1,
              QStringLiteral("浮条模式:单击采纳计数 = 1"));
        qInfo() << "SMART-CAPTURE  浮条模式单击后候选 =" << overlay2->smartCandidate();

        // C2. 拖动(超过 startDragDistance)→ 走手框,选区 == 拖拽矩形。
        //
        // 这**必须**成立:智能框选是增益,不能把"我想手动画一个框"这条路堵死。
        overlay2->close();
        overlay2->deleteLater();

        auto* overlay3 = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                           /*autoStartStitching=*/false, injected);
        overlay3->setGeometry(screenGeo);
        overlay3->show();
        QApplication::processEvents();

        QRect draggingRect;
        QObject::connect(overlay3, &OverlayWidget::regionConfirmed, overlay3,
                         [&draggingRect](const QRect& r) { draggingRect = r; });

        const QPoint dragFrom(targetCenterLocal.x() - 120, targetCenterLocal.y() - 80);
        const QPoint dragTo(targetCenterLocal.x() + 180, targetCenterLocal.y() + 140);
        dragMouse(overlay3, dragFrom, dragTo);
        QApplication::processEvents();

        check(overlay3->smartAcceptCount() == 0,
              QStringLiteral("拖拽不走「单击采纳」(计数仍为 0)"));
        // 拖拽产生的选区 = 拖拽矩形;在浮条模式下不自动确认,所以这里断言
        // "选区 != 候选" 的最直接可观测形式就是:预选被让位(光标已不在候选内)且
        // 采纳计数为 0 —— 再加一条:此时**没有** regionConfirmed(浮条模式要用户确认)。
        check(draggingRect.isNull(),
              QStringLiteral("浮条模式下拖拽不自动确认(regionConfirmed 未发出)"));

        overlay3->close();
        overlay3->deleteLater();

        // ---- D. 滚轮在重叠候选之间切换,且切换结果**必须存活**(docs/29 §2) ----
        //
        // 回归对象(2026-11-02 用户反馈):"有些应用下滑轮重置框选窗口失效"。
        // 根因是 offset 的存活条件写成了 `pos != m_lastHoverPos` —— 任何 1 像素
        // 移动就清零。滚轮切到第 N 个后手一抖就弹回第 0 个,用户感知就是"滚了没用"。
        //
        // 本段不依赖真实 UIA/真机:注入两个**完全重叠**的候选,投真实滚轮事件,
        // 断言切换生效,再投一个**微小位移的 mousemove**,断言选择**不回退**。
        {
            const QRect screenPhysD(screenGeo.topLeft(),
                                    QSize(qRound(screenGeo.width() * dpr),
                                          qRound(screenGeo.height() * dpr)));
            // 两个同位置候选(z 序:小窗口在前)
            const QRect shared(screenPhysD.x() + qRound(300 * dpr),
                               screenPhysD.y() + qRound(200 * dpr),
                               qRound(500 * dpr), qRound(300 * dpr));
            QVector<smart::Candidate> overlap;
            smart::Candidate front;
            front.physical = shared;
            front.title = QStringLiteral("前窗口");
            front.id = 0x5001;
            front.windowId = 0x5001;
            front.level = 0;
            overlap << front;
            smart::Candidate back = front;
            back.title = QStringLiteral("后窗口");
            back.id = 0x5002;
            back.windowId = 0x5002;
            overlap << back;

            auto* ovW = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                          /*autoStartStitching=*/false, overlap);
            ovW->setGeometry(screenGeo);
            ovW->show();
            QApplication::processEvents();

            const QPoint inShared(qRound(shared.center().x() / dpr) - screenGeo.x(),
                                  qRound(shared.center().y() / dpr) - screenGeo.y());
            moveMouse(ovW, inShared);
            QApplication::processEvents();
            const QRect beforeWheel = ovW->smartCandidate();
            check(!beforeWheel.isNull(),
                  QStringLiteral("D:悬停重叠候选时预选非空(得 %1x%2)")
                      .arg(beforeWheel.width()).arg(beforeWheel.height()));

            // 滚一格 → 在重叠栈里前进一个(仍然是同一个 shared 矩形,但选择序号变了)
            wheelAt(ovW, inShared, /*up=*/true);
            QApplication::processEvents();
            const int offsetAfterWheel = ovW->smartHoverStackOffsetForTest();
            qInfo() << "SMART-CAPTURE  D 段:滚轮后 offset =" << offsetAfterWheel;
            check(offsetAfterWheel == 1,
                  QStringLiteral("D:滚轮切到重叠栈的下一个(offset=1,得 %1)")
                      .arg(offsetAfterWheel));

            // ---- 关键回归:微小位移后的 mousemove 不能把选择清零 ----
            //
            // 修复前:`pos != m_lastHoverPos` → offset 归零 → 用户觉得"滚了没用"。
            // 修复后:光标仍在**同一个候选矩形**内 → offset 保持。
            moveMouse(ovW, inShared + QPoint(1, 1));
            QApplication::processEvents();
            const int offsetAfterJitter = ovW->smartHoverStackOffsetForTest();
            qInfo() << "SMART-CAPTURE  D 段:微动 1px 后 offset =" << offsetAfterJitter;
            check(offsetAfterJitter == 1,
                  QStringLiteral("D:光标仍在同一候选内,滚轮选择不回退(offset=1,得 %1)")
                      .arg(offsetAfterJitter));

            // 再滚一格 → 回到 0(两个候选的循环)
            wheelAt(ovW, inShared, /*up=*/true);
            QApplication::processEvents();
            check(ovW->smartHoverStackOffsetForTest() == 0,
                  QStringLiteral("D:再滚一格循环回第 0 个"));

            // ---- 走出候选 → 选择作废(用户已换目标) ----
            wheelAt(ovW, inShared, /*up=*/true);   // offset = 1
            QApplication::processEvents();
            moveMouse(ovW, QPoint(screenGeo.width() - 30, screenGeo.height() - 30));
            QApplication::processEvents();
            check(ovW->smartHoverStackOffsetForTest() == 0,
                  QStringLiteral("D:光标移出候选后滚轮选择归零(得 %1)")
                      .arg(ovW->smartHoverStackOffsetForTest()));

            ovW->close();
            ovW->deleteLater();
        }

        // ---- E. 元素级:候选表里同时有"窗口"与"窗口内的元素"时,元素优先 ----
        //
        // 回归对象(真实缺陷,2026-10-26 修复):元素级候选是**异步**探测出来的,
        // 而每次 mousemove 都会重算窗口级候选 —— 于是元素框会在"下一帧鼠标动
        // 1px"时被窗口框覆盖,表现为"闪一下就没了"。修复后必须有断言钉住。
        {
            QVector<smart::Candidate> layered;
            smart::Candidate win = targetCand;               // level 0:整个窗口
            layered << win;
            smart::Candidate elem;
            // 元素 = 窗口内靠上的一块(模拟浏览器里的正文区块)
            elem.physical = QRect(win.physical.x() + 40, win.physical.y() + 30,
                                  qRound(win.physical.width() * 0.6),
                                  qRound(win.physical.height() * 0.25));
            elem.title = QStringLiteral("正文区块");
            elem.id = win.id;
            elem.windowId = win.windowId;
            elem.level = 1;
            layered << elem;

            auto* overlay4 = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                               /*autoStartStitching=*/false, layered);
            overlay4->setGeometry(screenGeo);
            overlay4->show();
            QApplication::processEvents();

            // 光标落在元素内部 → 应命中元素(更细),而不是窗口
            const QPoint inElemLocal =
                QPoint(qRound(elem.physical.center().x() / dpr) - screenGeo.x(),
                       qRound(elem.physical.center().y() / dpr) - screenGeo.y());
            moveMouse(overlay4, inElemLocal);
            QApplication::processEvents();

            const QRect elemLocal = overlay4->smartCandidate();
            const QRect winLocal = QRect(targetGeo.topLeft() - screenGeo.topLeft(),
                                         targetGeo.size());
            qInfo() << "SMART-CAPTURE  元素级预选 =" << elemLocal << " 窗口 =" << winLocal;
            check(!elemLocal.isNull() &&
                      elemLocal.width() < winLocal.width() &&
                      elemLocal.height() < winLocal.height(),
                  QStringLiteral("元素级候选优先于窗口级(框得更细 %1x%2 < %3x%4)")
                      .arg(elemLocal.width()).arg(elemLocal.height())
                      .arg(winLocal.width()).arg(winLocal.height()));

            // 再动几像素(**仍在元素内**)→ 元素预选不能退回窗口
            QMouseEvent jitter(QEvent::MouseMove, QPointF(inElemLocal + QPoint(3, 2)),
                               overlay4->mapToGlobal(QPointF(inElemLocal + QPoint(3, 2))),
                               Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(overlay4, &jitter);
            QApplication::processEvents();
            const QRect afterJitter = overlay4->smartCandidate();
            check(afterJitter.height() < winLocal.height(),
                  QStringLiteral("鼠标在元素内微动后仍保持元素级(不被窗口级覆盖,得 %1x%2)")
                      .arg(afterJitter.width()).arg(afterJitter.height()));

            overlay4->close();
            overlay4->deleteLater();
        }

        // ---- F. 「全屏窗口先占住,另一个应用就框不出来」----
        //
        // 用户实测反馈(2026-10-26):先框到一个占满全屏的应用窗口,再去框另一个
        // **非全屏**的应用,就框不出来了。
        //
        // 构造:z 序最上层是小窗口,它下面是一个**占满全屏**的窗口,而那个全屏
        // 窗口的 UIA 只报了一个"与窗口等大的根元素"(实测 Chrome/ZCode 就是这样)。
        //   → 小窗口完全落在"全屏元素"的矩形内部。
        //
        // 这暴露的是**层级优先度量的错误**:level 高不等于"更该被选中" ——
        // 元素属于哪个窗口才是前提。后台窗口的元素不该压过前台窗口。
        {
            QVector<smart::Candidate> mixed;
            const QRect screenPhysF(screenGeo.topLeft(),
                                    QSize(qRound(screenGeo.width() * dpr),
                                          qRound(screenGeo.height() * dpr)));

            // ① 小窗口(z 序最上,非全屏)
            smart::Candidate smallWin;
            smallWin.physical = QRect(screenPhysF.x() + qRound(200 * dpr),
                                      screenPhysF.y() + qRound(150 * dpr),
                                      qRound(420 * dpr), qRound(300 * dpr));
            smallWin.title = QStringLiteral("小窗口");
            smallWin.id = 0x1001;
            smallWin.windowId = 0x1001;
            smallWin.level = 0;
            mixed << smallWin;

            // ② 占满全屏的窗口(z 序在其下)
            smart::Candidate bigWin;
            bigWin.physical = screenPhysF;
            bigWin.title = QStringLiteral("全屏窗口");
            bigWin.id = 0x2002;
            bigWin.windowId = 0x2002;
            bigWin.level = 0;
            mixed << bigWin;

            // ③ 全屏窗口的 UIA 根元素:与窗口**等大**(实测形态)
            smart::Candidate bigElem;
            bigElem.physical = screenPhysF;
            bigElem.title = QStringLiteral("全屏窗口根元素");
            bigElem.id = 0x2002;
            bigElem.windowId = 0x2002;
            bigElem.level = 1;
            mixed << bigElem;

            auto* overlay5 = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                               /*autoStartStitching=*/false, mixed);
            overlay5->setGeometry(screenGeo);
            overlay5->show();
            QApplication::processEvents();

            const QPoint inSmallLocal(qRound(200 * dpr) + 40, qRound(150 * dpr) + 40);
            moveMouse(overlay5, inSmallLocal);
            QApplication::processEvents();

            const QRect picked = overlay5->smartCandidate();
            // ⚠️ 期望值是**逻辑**坐标:候选的 physical 已经乘过 dpr,这里换算回来
            //    才是遮罩内的局部矩形。写成 smallWin.physical 直接比会差一个 dpr
            //    (本机 175% → 期望值被放大成 350,263 735x525)。
            const QRect smallLocal(200, 150, 420, 300);
            qInfo() << "SMART-CAPTURE  全屏元素干扰下预选 =" << picked
                    << " 期望小窗口 =" << smallLocal;
            check(nearRect(picked, smallLocal, 2),
                  QStringLiteral("悬停小窗口时框住**小窗口**而不是后台全屏元素(得 %1x%2 @%3,%4)")
                      .arg(picked.width()).arg(picked.height())
                      .arg(picked.x()).arg(picked.y()));

            // ---- F2. 选中"占满整屏"的候选后,浮条必须仍然点得到 ----
            //
            // 这是"全屏应用"场景最容易翻车的地方:toolbarRectFor 在上下都放不下时
            // 会夹进窗口内,而选区就是整屏 —— 浮条会**压在内容上**并挡住用户视线。
            // 断言:浮条矩形完整落在窗口内(点得到),且选区 == 整屏(框对了)。
            {
                auto* ovFull = new OverlayWidget(frozen, 0, screen, /*rightClickCancel=*/true,
                                                 /*autoStartStitching=*/false, mixed);
                ovFull->setGeometry(screenGeo);
                ovFull->show();
                QApplication::processEvents();

                moveMouse(ovFull, QPoint(screenGeo.width() / 2, screenGeo.height() - 20));
                QApplication::processEvents();
                clickAt(ovFull, QPoint(screenGeo.width() / 2, screenGeo.height() - 20));
                QApplication::processEvents();

                const QRect fullSel = ovFull->smartCandidate();
                check(fullSel.width() >= screenGeo.width() - 2 &&
                          fullSel.height() >= screenGeo.height() - 2,
                      QStringLiteral("单击后选区 = 整屏窗口(%1x%2)")
                          .arg(fullSel.width()).arg(fullSel.height()));

                // 浮条必须在窗口内且可见(不越界 = 点得到)
                const QSize bar(nb::overlay::kToolbarWidth, nb::overlay::kButtonHeight);
                const QRect tb = nb::overlay::toolbarRectFor(fullSel, screenGeo.size(), bar,
                                                             nb::overlay::kBarMargin);
                check(tb.top() >= 0 && tb.bottom() < screenGeo.height() &&
                          tb.left() >= 0 && tb.right() < screenGeo.width(),
                      QStringLiteral("整屏选区下浮条仍完整落在窗口内(得 %1,%2 %3x%4)")
                          .arg(tb.x()).arg(tb.y()).arg(tb.width()).arg(tb.height()));
                // 上下都没有空白时必须贴底:压在正中会盖住窗口标题栏 ——
                // 而标题栏正是用户确认"框对了没有"的地方。
                check(tb.bottom() > screenGeo.height() - 60,
                      QStringLiteral("整屏选区下浮条贴住底边而非压在正中(得 bottom=%1,屏高=%2)")
                          .arg(tb.bottom()).arg(screenGeo.height()));

                ovFull->close();
                ovFull->deleteLater();
            }

            // 移出小窗口、只压在全屏窗口上 → 此时该框全屏(元素与窗口等大,等效)
            moveMouse(overlay5, QPoint(screenGeo.width() - 30, screenGeo.height() - 30));
            QApplication::processEvents();
            const QRect pickedFull = overlay5->smartCandidate();
            qInfo() << "SMART-CAPTURE  只压全屏窗口时预选 =" << pickedFull;
            check(pickedFull.width() >= screenGeo.width() - 2,
                  QStringLiteral("移出小窗口后框住全屏窗口(得 %1x%2)")
                      .arg(pickedFull.width()).arg(pickedFull.height()));

            overlay5->close();
            overlay5->deleteLater();
        }

        // ---- G. 非主屏端到端:悬停 → 单击 → 确认矩形必须是"本屏相对" ----
        //
        // 回归对象(2026-10-31 用户实测):非主屏上智能框选截图后**不入库、剪贴板
        // 为空**。根因:采纳路径把候选的"虚拟桌面绝对"物理矩形原样交给了核心
        // crop_rgba,而它要的是"本屏相对";非主屏原点非 (0,0),矩形在核心侧
        // 越界钳制后变成空 → 裁剪失败 → 链路静默终止。
        //
        // 为什么既有验收没抓到:B 段只在主屏(primaryScreen)跑,期望值又把
        // "绝对 * dpr"当成了正确答案 —— 主屏原点 (0,0) 时两个口径恰好相等。
        //
        // 做法:把验收遮罩搬到**另一块屏**(若有)上重建一次,单击采纳后断言
        //   ① regionConfirmed 发出;
        //   ② 确认矩形在 [0,0,屏物理尺寸] 之内(口径对了的必要条件);
        //   ③ 用确认矩形对**本屏底图**真实裁剪,产物非空且尺寸 == 矩形尺寸
        //     (口径错了的充分条件:核心会报"完全越界",裁出来是空图)。
        {
            QScreen* otherScreen = nullptr;
            for (QScreen* s : QGuiApplication::screens()) {
                if (s != screen) { otherScreen = s; break; }
            }
            if (!otherScreen) {
                qInfo() << "SMART-CAPTURE  (单屏环境,G 段跳过 —— 非主屏回归需要多屏真机)";
            } else {
                const QRect otherGeo = otherScreen->geometry();
                const qreal otherDpr = otherScreen->devicePixelRatio();
                // 在另一块屏上造一个"目标窗口"形状的候选(逻辑 300x200,屏内偏移固定)
                const QPoint otherOrigin(90, 80);
                const QSize otherSize(300, 200);
                // 本屏相对物理(注入候选时仍需**绝对**物理 —— 那是候选表的口径)
                const QRect relPhysical(
                    QPoint(qRound(otherOrigin.x() * otherDpr),
                           qRound(otherOrigin.y() * otherDpr)),
                    QSize(qRound(otherSize.width() * otherDpr),
                          qRound(otherSize.height() * otherDpr)));
                const QRect absPhysical = relPhysical.translated(otherGeo.topLeft());

                QVector<smart::Candidate> otherList;
                smart::Candidate oc;
                oc.physical = absPhysical;
                oc.title = QStringLiteral("副屏验收窗口");
                oc.id = 0x9001;
                oc.windowId = 0x9001;
                oc.level = 0;
                otherList << oc;

                QImage otherFrozen(otherGeo.size() * otherDpr,
                                   QImage::Format_ARGB32_Premultiplied);
                otherFrozen.fill(QColor(0x33, 0x44, 0x55));

                auto* ovOther = new OverlayWidget(otherFrozen, 0, otherScreen,
                                                  /*rightClickCancel=*/true,
                                                  /*autoStartStitching=*/false, otherList);
                ovOther->setGeometry(otherGeo);
                ovOther->show();
                QApplication::processEvents();

                QRect gotOther;
                bool confirmedOther = false;
                // ⚠️ connect 必须在**确认动作之前**:regionConfirmed 一发出会话就收尾。
                //    另注意:docs/28 起单击只**采纳**不确认(2026-11-01 起的浮条语义),
                //    真正的确认是用户按 Enter / 点浮条「完成」—— 这里投一个真实的
                //    Enter 键,走与用户完全同一条路径,而不是绕过 UI 直接调私有方法。
                QObject::connect(ovOther, &OverlayWidget::regionConfirmed, ovOther,
                                 [&gotOther, &confirmedOther](const QRect& r) {
                    gotOther = r;
                    confirmedOther = true;
                });

                const QPoint inOther(otherOrigin.x() + 40, otherOrigin.y() + 40);
                moveMouse(ovOther, inOther);
                QApplication::processEvents();
                clickAt(ovOther, inOther);          // 采纳(选区定下来 + 浮出浮条)
                QApplication::processEvents();
                check(ovOther->smartAcceptCount() == 1,
                      QStringLiteral("副屏:单击采纳计数 = 1"));
                {
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(ovOther, &enter);   // 用户按 Enter 确认
                    QApplication::processEvents();
                }

                qInfo() << "SMART-CAPTURE  副屏确认矩形 =" << gotOther
                        << " 期望(本屏相对)=" << relPhysical
                        << " 屏物理尺寸 =" << otherFrozen.size();
                check(confirmedOther, QStringLiteral("副屏:按 Enter 确认后 regionConfirmed 发出"));

                // ① 口径:必须落在本屏物理矩形之内(绝对坐标必越界)
                check(!gotOther.isNull() &&
                          QRect(QPoint(0, 0), otherFrozen.size()).contains(gotOther, true),
                      QStringLiteral("副屏:确认矩形在**本屏相对**口径内(得 %1,%2 %3x%4)")
                          .arg(gotOther.x()).arg(gotOther.y())
                          .arg(gotOther.width()).arg(gotOther.height()));
                check(!gotOther.isNull() && nearRect(gotOther, relPhysical, 3),
                      QStringLiteral("副屏:确认矩形 == 候选的本屏相对物理矩形"));

                // ② 终态验证:拿确认矩形对**本屏底图**真实裁剪 —— 修复前这里是空图
                if (!gotOther.isNull()) {
                    const QImage cropped =
                        CoreBridge::instance().captureCrop(otherFrozen, gotOther);
                    qInfo() << "SMART-CAPTURE  副屏真实裁剪产物 =" << cropped.size();
                    check(!cropped.isNull() &&
                              qAbs(cropped.width() - gotOther.width()) <= 2 &&
                              qAbs(cropped.height() - gotOther.height()) <= 2,
                          QStringLiteral("副屏:用确认矩形可正常裁剪入库(得 %1x%2,期望 %3x%4)")
                              .arg(cropped.width()).arg(cropped.height())
                              .arg(gotOther.width()).arg(gotOther.height()));
                }

                // ---- G2. 浅色底图 → 深色字(反向场景)----
                //
                // G 段的底图是深色(0x334455),只覆盖了"深底→白字"分支;
                // 用户报的场景恰是浅色页面(白底文档)上白字看不清,必须补浅底分支。
                // 判定:浅底图上 grab 提示底板区域,亮度中位数应为"亮"
                // (白板)且明显亮于深底场景的黑板 —— 两个分支都钉住。
                //
                // ⚠️ 2026-11-02(docs/28):尺寸提示**不再画在候选预览上**,只画在
                //    已确认的选区上。所以本段必须先**单击采纳**(选区定下来)再采样,
                //    否则抓到的是空白 —— 那正好也是本次要验的语义:悬停时不显示、
                //    采纳后才显示。
                QImage lightFrozen(otherGeo.size() * otherDpr,
                                   QImage::Format_ARGB32_Premultiplied);
                lightFrozen.fill(QColor(0xF2, 0xF2, 0xF2));   // 浅底
                auto* ovLight = new OverlayWidget(lightFrozen, 0, otherScreen,
                                                  /*rightClickCancel=*/true,
                                                  /*autoStartStitching=*/false, otherList);
                ovLight->setGeometry(otherGeo);
                ovLight->show();
                QApplication::processEvents();

                const QPoint inLight(otherOrigin.x() + 40, otherOrigin.y() + 40);

                // ---- G2-a. 悬停**未采纳**时:候选上不得出现尺寸提示 ----
                //
                // 这是 docs/28 的正面断言。判据:提示底板本该出现的那条区域,
                // 在只悬停(未单击)时应当是"没有白板"的 —— 浅底图上那块的亮度
                // 应与底图本身接近(0xF2 ≈ 242),而**不是**白板压在底图上的样子。
                // 更强的判据是"看不出有底板":直接比对"提示区"与"选区外同高度
                // 的一条",两者应几乎一致(都没有板)。
                moveMouse(ovLight, inLight);
                QApplication::processEvents();
                const QImage hoverGrab = ovLight->grab().toImage().convertToFormat(
                    QImage::Format_RGB32);
                // 采样坐标 = 窗口局部坐标。ovLight 的 setGeometry(otherGeo) 让窗口
                // 局部原点 == 屏内 (0,0),故 otherOrigin 可直接用。
                const QRect plateZone(otherOrigin.x() - 4,
                                      qMax(0, otherOrigin.y() - 40),
                                      140, 38);
                auto zoneLuma = [](const QImage& img, const QRect& z) {
                    int sum = 0;
                    int n = 0;
                    for (int y = z.top(); y < z.bottom(); ++y) {
                        for (int x = z.left(); x < z.right(); ++x) {
                            if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
                            const QColor c(img.pixel(x, y));
                            sum += (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
                            ++n;
                        }
                    }
                    return n ? sum / n : -1;
                };
                const int hoverLuma = zoneLuma(hoverGrab, plateZone);
                qInfo() << "SMART-CAPTURE  悬停未采纳:提示区亮度 =" << hoverLuma
                        << "(候选不画尺寸提示 → 不应有白板)";
                // 底图 0xF2 叠 90α 黑罩 ≈ 242*0.647 ≈ 157;有白板则显著更高。
                check(hoverLuma < 200,
                      QStringLiteral("悬停候选时**不**显示尺寸提示(提示区亮度 %1 < 200)")
                          .arg(hoverLuma));

                // ---- G2-b. 采纳后(选区 + 浮条出现):提示应当显现且为白板 ----
                clickAt(ovLight, inLight);
                QApplication::processEvents();
                const QImage selGrab = ovLight->grab().toImage().convertToFormat(
                    QImage::Format_RGB32);
                const int selLuma = zoneLuma(selGrab, plateZone);
                qInfo() << "SMART-CAPTURE  采纳后:提示区亮度 =" << selLuma
                        << " 采纳计数 =" << ovLight->smartAcceptCount()
                        << " 候选 =" << ovLight->smartCandidate()
                        << " 采样区 =" << plateZone
                        << " 抓图尺寸 =" << selGrab.size();
                // 深底场景黑板 α120 压在 0x334455 上 → 均值远低于 128;
                // 浅底场景白板 α120 压在 0xF2 上 → 均值必然高
                check(selLuma > 128 && selLuma > hoverLuma,
                      QStringLiteral("采纳后显示尺寸提示(亮度 %1 > 128 且 > 悬停时的 %2)")
                          .arg(selLuma).arg(hoverLuma));

                ovLight->close();
                ovLight->deleteLater();

                ovOther->close();
                ovOther->deleteLater();
            }
        }

        target->close();
        target->deleteLater();

        qInfo() << "SMART-CAPTURE  RESULT failures =" << g_failures;
        QApplication::quit();
    });
}

// ---- UIA 元素级正向验收(--smart-uia-flow)----
//
// 元素级的价值是"框得比窗口更细"(浏览器里框住正文区块、IDE 里框住编辑区)。
// 但它**依赖目标应用的 UIA 实现**,不能假定任何一台机器上都有 —— 所以这里的
// 断言分两层:
//   ① 硬断言:**有 UIA 树的窗口**必须能探出元素,且元素严格小于窗口;
//   ② 软断言:整机上至少有一个窗口探得出(探不出只记日志,不判失败)——
//      纯游戏机/精简系统上探不出是正常的,不该让验收红。
//
// 目标窗口的选择:枚举候选 → 逐个 worthProbing → 取其中心点探测,
// 命中第一个能探出元素的就断言。全程只读,不动目标窗口。
void runSmartUiaFlow() {
    QTimer::singleShot(1500, qApp, [] {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qInfo() << "SMART-UIA  RESULT failures =" << g_failures;
            QApplication::quit();
            return;
        }
        const QRect screenPhys(screen->geometry().topLeft(), screen->geometry().size());
        const qreal dpr = screen->devicePixelRatio();

        const QVector<smart::Candidate> windows = smart::probeWindows();
        int probed = 0;
        int found = 0;
        /// Chromium 系窗口的探测结果(docs/26:本 flow 的**核心回归对象**)。
        int chromiumProbed = 0;
        int chromiumRefined = 0;

        int tested = 0;
        for (const smart::Candidate& w : windows) {
            if (!smart::worthProbing(w.id)) continue;
            // 前 5 个窗口都探一遍:单看一个窗口无法区分"这个应用不给树"与
            // "我们的下沉逻辑停了"—— 两条诊断信息一起看才有用。
            if (tested++ >= 5) break;
            // 只用窗口中心探测(离边缘越近越容易命中滚动条/边框元素)
            const QPoint centerPhysical = w.physical.center();
            // 必须在窗口内(候选矩形是物理像素,虚桌面坐标)
            if (!w.physical.contains(centerPhysical)) continue;
            ++probed;
            // Chromium 系(Chrome / Edge / Electron:VSCode、MarkText…)用同一套
            // 外壳结构:**顶层两个同尺寸 Pane,第 0 个是空壳**。这是 docs/26 那次
            // 缺陷的复发面,单独计数并**硬断言**(见本段末尾)。
            wchar_t cls[256] = {0};
            ::GetClassNameW(reinterpret_cast<HWND>(w.id), cls, 255);
            const bool isChromium = QString::fromWCharArray(cls) == QStringLiteral("Chrome_WidgetWin_1");
            const QStringList tree = smart::dumpElementTree(w.id, 4, 4);
            for (const QString& line : tree) qInfo() << "SMART-UIA   tree" << line;
            smart::ProbeStats st;
            const QVector<smart::Candidate> elems =
                smart::probeElementAt(w.id, centerPhysical, dpr, 24, 4000, 500, &st);
            qInfo() << "SMART-UIA  探测" << w.title.left(30) << "→" << elems.size() << "个元素"
                    << (elems.isEmpty() ? QRect() : elems.first().physical)
                    << QStringLiteral(" [depth=%1 visited=%2 %3ms view=%4]")
                           .arg(st.depth).arg(st.visited).arg(st.elapsedMs)
                           .arg(st.viewUsed == 0 ? QStringLiteral("Control") : QStringLiteral("Raw"));
            if (isChromium) {
                ++chromiumProbed;
                if (!elems.isEmpty()) ++chromiumRefined;
            }
            if (elems.isEmpty()) continue;
            ++found;
            const QRect e = elems.first().physical;
            check(e.width() <= w.physical.width() && e.height() <= w.physical.height(),
                  QStringLiteral("元素不超出窗口(%1x%2 vs %3x%4)")
                      .arg(e.width()).arg(e.height())
                      .arg(w.physical.width()).arg(w.physical.height()));

            // ---- 硬断言:必须真的"往下走了",不能停在第 1 层 ----
            //
            // 这是本 flow 存在的**全部理由**(docs/26 §6.2)。原实现在这里软放行
            // ("元素 == 窗口就只 qInfo"),于是"算法每层只挑一个兄弟"这个致命
            // 缺陷在 Chrome 上 100% 复现却连续 6 天全绿。
            //
            // 判据分两条,任一条成立即认为"探到了":
            //   ① 元素明显小于窗口(差 ≥ 8px,Chromium 外壳 Pane 只差 2px);
            //   ② 遍历深度 ≥ 5(证明我们确实下沉过,而不是停在根/第 1 层)。
            // 两条都不成立 → 判失败。
            const bool shrinkW = e.width() < w.physical.width() - 8;
            const bool shrinkH = e.height() < w.physical.height() - 8;
            const bool deep = st.depth >= 5;
            if (shrinkW || shrinkH) {
                check(true, QStringLiteral("探到比窗口更细的元素(%1x%2 < %3x%4,depth=%5)")
                                .arg(e.width()).arg(e.height())
                                .arg(w.physical.width()).arg(w.physical.height())
                                .arg(st.depth));
            } else if (deep) {
                check(true, QStringLiteral("遍历深度足够(depth=%1),元素与窗口同尺寸是目标应用的形态")
                                .arg(st.depth));
            } else {
                // 既没细化、也没下沉 —— 这就是本 flow 要抓的那个失效。
                check(false,
                      QStringLiteral("元素停在很粗的层:depth=%1(期望 ≥5 或矩形明显小于窗口 %2x%3)")
                          .arg(st.depth).arg(w.physical.width()).arg(w.physical.height()));
            }
            // 继续看下一个窗口(多留几条树结构证据)
        }

        qInfo() << "SMART-UIA  探测" << probed << "个窗口,命中" << found << "个";
        // 软断言:探不到不判失败(无 UIA 的机器是合法的)
        if (found == 0) {
            qInfo() << "SMART-UIA  (本机没有窗口给出可用的 UIA 元素 —— 合法,不判失败)";
        }

        // ---- Chromium 硬断言(docs/26 那次事故的直接回归)----
        //
        // 为什么单列一条:Chrome/Edge/Electron 的 UIA 顶层是**两个同尺寸 Pane**,
        // 第 0 个是空壳。原算法每层只挑"面积最小"的孩子,必然选中空壳,于是
        // 元素级恒等于整窗 —— 而当时本 flow 对"元素 == 窗口"只 qInfo,四条验收
        // 全绿,缺陷存活 6 天。
        //
        // 这条断言把"Chromium 必须能细化"钉死:探了 N 个 Chromium 窗口,至少要有
        // 一个探出元素。多个里可能有个别(如某些 Electron 壳)确实不给树,故用
        // "至少一个"而不是"全部" —— 既抓住回归,又不至于在特殊机器上误红。
        if (chromiumProbed > 0) {
            check(chromiumRefined > 0,
                  QStringLiteral("Chromium 系窗口能探出元素级候选(%1/%2 —— 外壳空壳必须被穿透)")
                      .arg(chromiumRefined).arg(chromiumProbed));
        } else {
            qInfo() << "SMART-UIA  (本机没有 Chromium 窗口,跳过 Chromium 回归断言)";
        }

        qInfo() << "SMART-UIA  RESULT failures =" << g_failures;
        QApplication::quit();
    });
}

// ---- 真实链路验收(--smart-live-flow)----
//
// 与 --smart-capture-flow 的分工:
//   * 前者验**算法**(候选 → 预选 → 采纳),候选表手工注入,位置精确可控;
//   * 本入口验**接线**:真的走 startCaptureSession,真的枚举桌面上现有的窗口,
//     断言"遮罩建起来时确实带着候选、且光标所在屏的那块能命中"。
//
// 单开一条的理由:接线的关键约束是"枚举必须发生在遮罩 show 之前"。这条约束
// 一旦被改坏,症状是"永远框住整屏"—— 而它在单元断言里完全看不出来(注入的
// 候选表绕过了这个时机),只有跑真实链路才会暴露。
void runSmartLiveFlow() {
    QTimer::singleShot(1500, qApp, [] {
        // 先看裸枚举(此时还没有任何遮罩窗口):应当至少有一个非本进程窗口
        const QVector<smart::Candidate> raw = smart::probeWindows();
        check(!raw.isEmpty(),
              QStringLiteral("裸枚举得到候选窗口 %1 个(桌面并非空)").arg(raw.size()));

        // ---- z 序交叉验证(此刻还没有遮罩,WindowFromPoint 给的是真值)----
        //
        // EnumWindows 的枚举顺序是否等于 z 序,是这个功能的**地基**。若它错了,
        // 表现就是"悬停到 A 却框住 B"或"某个应用永远框不出来"。
        // 判据:对每个候选取中心点,问系统"这个屏幕点上最上层是哪个窗口",
        // 再与"候选表里第一个包含该点的窗口"比对。
        //
        // ⚠️ 必须排除**本进程自己的窗口**(2026-11-02 修)。验收跑起来时面板是
        //    可见的,`WindowFromPoint` 会老老实实报告"最上层是 Nextboard";
        //    而候选表按设计**已过滤本进程 PID** —— 于是每个被面板盖住的候选都
        //    被判成"不一致",boom,断言必红。这不是产品的 z 序错了,是验收自己
        //    制造了遮挡。判据:命中窗口的 PID == 本进程 → 这条样本跳过(记 skipped),
        //    因为"候选表里没有自己"正是被测的**另一条**正确行为。
        {
            const DWORD selfPid = ::GetCurrentProcessId();
            int agree = 0;
            int disagree = 0;
            int skipped = 0;
            for (int i = 0; i < raw.size(); ++i) {
                const QPoint pt = raw.at(i).physical.center();
                POINT p{pt.x(), pt.y()};
                HWND hit = ::WindowFromPoint(p);
                if (hit) hit = ::GetAncestor(hit, GA_ROOT);
                if (!hit) { ++skipped; continue; }
                DWORD hitPid = 0;
                ::GetWindowThreadProcessId(hit, &hitPid);
                if (hitPid == selfPid) {
                    // 命中的是本进程面板/便签:候选表按设计不含自己,无从比对
                    ++skipped;
                    continue;
                }
                // 候选表里第一个包含该点的窗口 = 我们以为的"最上层"
                quintptr expected = 0;
                for (const smart::Candidate& c : raw) {
                    if (c.physical.contains(pt)) { expected = c.id; break; }
                }
                if (expected == 0) { ++skipped; continue; }
                if (expected == reinterpret_cast<quintptr>(hit)) {
                    ++agree;
                } else {
                    ++disagree;
                    const QString sysTitle = [hit] {
                        wchar_t b[256] = {0};
                        ::GetWindowTextW(hit, b, 255);
                        return QString::fromWCharArray(b);
                    }();
                    qWarning() << "SMART-LIVE  z序不一致 @" << pt
                               << " 我们以为 =" << raw.at(i).title.left(20)
                               << " 系统说 =" << sysTitle.left(20);
                }
            }
            qInfo() << "SMART-LIVE  z序交叉验证:一致" << agree << "不一致" << disagree
                    << "跳过" << skipped;
            check(disagree == 0,
                  QStringLiteral("EnumWindows 顺序与系统 WindowFromPoint 一致(不一致 %1)")
                      .arg(disagree));
        }

        // 真起一次会话。框选恒为浮条模式(regionAutoConfirm 已删),只看状态。
        nb::startCaptureSession(nb::CaptureIntent::Region);
        QTimer::singleShot(1200, qApp, [raw] {
            // ⚠️ 必须收集**全部**可见遮罩:startCaptureSession 是"每屏一块"
            //    (受 captureAllScreens 影响)。只看第一块会让"不在本屏"的候选
            //    被误判成漏框/遮挡 —— 第一版就是这么写的,诊断日志自相矛盾。
            QList<OverlayWidget*> overlays;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* o = qobject_cast<OverlayWidget*>(w)) {
                    if (o->isVisible()) overlays.append(o);
                }
            }
            OverlayWidget* overlay = overlays.isEmpty() ? nullptr : overlays.first();
            if (!overlay) {
                check(false, QStringLiteral("会话已建立遮罩窗口"));
                qInfo() << "SMART-LIVE  RESULT failures =" << g_failures;
                QApplication::quit();
                return;
            }
            check(true, QStringLiteral("会话已建立遮罩窗口"));

            const int count = overlay->smartCandidateCount();
            qInfo() << "SMART-LIVE  遮罩持有候选" << count << "条(裸枚举" << raw.size() << ")";
            check(count > 0,
                  QStringLiteral("遮罩拿到了候选(接线:枚举早于遮罩 show)"));

            // ⚠️ 关键回归:候选里**不能**含遮罩自己。
            //
            // 判据**不能**用"预测到整屏 = 枚举到自己":真机上桌面里本来就常有
            // 最大化窗口(实测 VSCodium 就是 3072x1920),那样判会把正确行为判成
            // 缺陷 —— 第一版就是这么写的,被这条真实数据当场证伪。
            // 正确判据:遮罩的**原生句柄**是否出现在候选表里。
            const quintptr selfHwnd = static_cast<quintptr>(overlay->winId());
            bool sawSelf = false;
            for (const smart::Candidate& c : raw) {
                if (c.id == selfHwnd) sawSelf = true;
            }
            qInfo() << "SMART-LIVE  遮罩 hwnd =" << Qt::hex << selfHwnd << Qt::dec
                    << " 候选里是否含它:" << sawSelf;
            check(!sawSelf, QStringLiteral("候选表不含遮罩自己的 HWND"));

            // ---- 多屏地基:断言"屏幕物理矩形"的三方一致性 ----
            //
            // 智能框选的换算完全建立在**一个假设**上:
            //     QScreen::geometry().topLeft() == 该屏在 Win32 虚拟桌面里的物理原点
            //
            // 项目里这条假设有历史依据(capture.rs 的 match_monitor 注释),但它只在
            // **缩放屏恰好位于原点**时天然成立 —— 主屏 dpr=1.75 且原点 (0,0) 时,
            // 逻辑原点与物理原点恰好重合,错了也看不出来。多屏混合缩放下很容易破。
            //
            // 判据用 Win32 真值(与 EnumWindows 同一坐标空间):
            //   ① 遮罩自身窗口矩形(GetWindowRect)必须等于该屏的显示器矩形;
            //   ② 我们算出来的 screenPhysical 必须与它一致。
            // 两者任一不符,候选就会整体偏移 —— 表现是"框住的是隔壁屏那块"。
            for (OverlayWidget* ov : overlays) {
                QScreen* sc = ov->screen();
                const HMONITOR mon = ::MonitorFromWindow(
                    reinterpret_cast<HWND>(ov->winId()), MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                ::GetMonitorInfoW(mon, &mi);
                const QRect win32Monitor(mi.rcMonitor.left, mi.rcMonitor.top,
                                         mi.rcMonitor.right - mi.rcMonitor.left,
                                         mi.rcMonitor.bottom - mi.rcMonitor.top);

                RECT wr{};
                ::GetWindowRect(reinterpret_cast<HWND>(ov->winId()), &wr);
                const QRect win32Window(wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);

                const QRect qtOriginBased(
                    sc->geometry().topLeft(),
                    smart::screenPhysicalSize(sc, QImage()));

                qInfo() << "SMART-LIVE  屏" << sc->name()
                        << "\n   win32 monitor =" << win32Monitor
                        << "\n   win32 window  =" << win32Window
                        << "\n   qt  origin    =" << sc->geometry().topLeft()
                        << " logical size =" << sc->geometry().size()
                        << " dpr =" << sc->devicePixelRatio();

                check(nearRect(win32Window, win32Monitor, 2),
                      QStringLiteral("屏 %1:遮罩窗口矩形 == 显示器物理矩形(w=%2 vs %3)")
                          .arg(sc->name()).arg(win32Window.width()).arg(win32Monitor.width()));
                check(sc->geometry().topLeft() == win32Monitor.topLeft(),
                      QStringLiteral("屏 %1:Qt 几何原点 == Win32 物理原点(Qt=%2,%3 Win32=%4,%5)")
                          .arg(sc->name())
                          .arg(sc->geometry().x()).arg(sc->geometry().y())
                          .arg(win32Monitor.x()).arg(win32Monitor.y()));
                Q_UNUSED(qtOriginBased);
            }

            // ---- 多屏:鼠标离开某屏后,该屏的候选框必须消失 ----
            //
            // 场景:三屏各一块遮罩。用户从 A 屏滑到 B 屏时,系统给 A 屏发 Leave、
            // 给 B 屏发 Move。若 A 屏不清候选,**A 屏上会永远留着一个虚线框** ——
            // 用户看到两块屏同时在"预选",不知道点哪块才是真的。
            //
            // 这是多屏独有的缺陷(单屏时鼠标永远不会"离开"遮罩,遮罩就是整屏)。
            // 挑一块**真的有窗口可框**的遮罩(实测第一块常是空的,直接跳过就白测)
            OverlayWidget* a = nullptr;
            QPoint aPoint(-1, -1);
            for (OverlayWidget* ov : overlays) {
                const QRect aScreen(ov->screen()->geometry().topLeft(),
                                    ov->screen()->geometry().size() * ov->devicePixelRatioF());
                for (const smart::Candidate& c : raw) {
                    const QRect local =
                        smart::toLocalLogical(c.physical, aScreen, ov->devicePixelRatioF());
                    if (!local.isEmpty()) { a = ov; aPoint = local.center(); break; }
                }
                if (a) break;
            }
            if (overlays.size() >= 2 && a) {
                moveMouse(a, aPoint);
                QApplication::processEvents();
                const bool had = !a->smartCandidate().isNull();
                // 模拟"鼠标滑到另一块屏":系统会给 A 屏发 Leave
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(a, &leave);
                QApplication::processEvents();
                qInfo() << "SMART-LIVE  离开屏" << a->screen()->name()
                        << " 前有候选 =" << had
                        << " 离开后候选 =" << a->smartCandidate();
                check(!had || a->smartCandidate().isNull(),
                      QStringLiteral("鼠标离开屏 %1 后预选被清空(不留幽灵虚线框)")
                          .arg(a->screen()->name()));
            } else {
                qInfo() << "SMART-LIVE  没有可测的屏(或只有一个屏),跳过离开测试";
            }

            // ---- 多屏:两块遮罩的预选互不串台 ----
            //
            // 三块遮罩共用**同一份候选表**(绝对物理像素),各自的命中只应落在
            // 自己屏内。若换算里忘了减屏原点,A 屏的窗口会在 B 屏上被"框住",
            // 或者 B 屏永远框不到东西。
            if (overlays.size() >= 2) {
                // 找两块各自都有候选的屏
                QVector<QPair<OverlayWidget*, QPoint>> picks;
                for (OverlayWidget* ov : overlays) {
                    const QRect sp(ov->screen()->geometry().topLeft(),
                                   ov->screen()->geometry().size() * ov->devicePixelRatioF());
                    for (const smart::Candidate& c : raw) {
                        const QRect local = smart::toLocalLogical(c.physical, sp,
                                                                  ov->devicePixelRatioF());
                        if (!local.isEmpty()) {
                            picks.append({ov, local.center()});
                            break;
                        }
                    }
                }
                if (picks.size() >= 2) {
                    OverlayWidget* first = picks.at(0).first;
                    OverlayWidget* second = picks.at(1).first;
                    moveMouse(first, picks.at(0).second);
                    QApplication::processEvents();
                    const QRect firstCand = first->smartCandidate();
                    moveMouse(second, picks.at(1).second);
                    QApplication::processEvents();
                    const QRect secondCand = second->smartCandidate();
                    qInfo() << "SMART-LIVE  串台检查:屏" << first->screen()->name()
                            << "候选 =" << firstCand << " 屏" << second->screen()->name()
                            << "候选 =" << secondCand;
                    check(!secondCand.isNull(),
                          QStringLiteral("第二块屏也能框住自己的窗口(得 %1x%2)")
                              .arg(secondCand.width()).arg(secondCand.height()));
                    // 各自都必须**完整落在自己屏内**(换算方向错会超界)
                    check(secondCand.left() >= 0 && secondCand.top() >= 0 &&
                              secondCand.right() < second->width() &&
                              secondCand.bottom() < second->height(),
                          QStringLiteral("屏 %1 的预选完整落在自己屏内(%2,%3 %4x%5)")
                              .arg(second->screen()->name())
                              .arg(secondCand.x()).arg(secondCand.y())
                              .arg(secondCand.width()).arg(secondCand.height()));
                    check(firstCand.left() >= 0 && firstCand.top() >= 0 &&
                              firstCand.right() < first->width() &&
                              firstCand.bottom() < first->height(),
                          QStringLiteral("屏 %1 的预选完整落在自己屏内")
                              .arg(first->screen()->name()));
                } else {
                    qInfo() << "SMART-LIVE  不足两块屏有候选,串台检查跳过";
                }
            }

            // ---- 真实桌面回归:逐个候选悬停,断言"每个候选都能被框住" ----
            //
            // 用户反馈的现象是"某个应用的窗口框不出来"。这一条把"框不出来"
            // 变成可判定的:**枚举到的每个候选,把光标移到它内部一个不被更上层
            // 候选覆盖的点上,必须能命中它**。
            //
            // 覆盖判定用"该点是否也落在 z 序更前的候选里"——落在前面的是遮挡,
            // 不算漏。这样才能区分"我们的命中逻辑漏了"与"它确实被别的窗口压着"。
            for (OverlayWidget* ov : overlays) {
                const QVector<smart::Candidate>& list = raw;
                const QRect screenPhysLive(
                    ov->screen()->geometry().topLeft(),
                    ov->screen()->geometry().size() * ov->devicePixelRatioF());
                const qreal dprLive = ov->devicePixelRatioF();
                int boxed = 0;
                int occluded = 0;
                int skippedOffscreen = 0;
                int missed = 0;
                qInfo() << "SMART-LIVE  验证屏" << ov->screen()->name() << screenPhysLive;
                for (int ci = 0; ci < list.size(); ++ci) {
                    const smart::Candidate& c = list.at(ci);
                    const QRect local = smart::toLocalLogical(c.physical, screenPhysLive, dprLive);
                    if (local.isEmpty()) { ++skippedOffscreen; continue; }
                    // 找一个不被更上层候选覆盖的采样点
                    QPoint sample;
                    bool found = false;
                    for (const QPoint& cand : {local.center(),
                                               local.topLeft() + QPoint(6, 6),
                                               local.bottomRight() - QPoint(6, 6)}) {
                        bool blocked = false;
                        for (int zi = 0; zi < ci; ++zi) {   // 只查 z 序更前的
                            const QRect o = smart::toLocalLogical(
                                list.at(zi).physical, screenPhysLive, dprLive);
                            if (o.contains(cand)) { blocked = true; break; }
                        }
                        if (!blocked) { sample = cand; found = true; break; }
                    }
                    if (!found) {
                        ++occluded;
                        qInfo() << "SMART-LIVE  被完全遮挡:" << c.title.left(24) << local;
                        continue;
                    }
                    moveMouse(ov, sample);
                    QApplication::processEvents();
                    const QRect got = ov->smartCandidate();
                    if (got.isEmpty()) {
                        ++missed;
                        qWarning() << "SMART-LIVE  漏框:" << c.title.left(24) << "采样点" << sample;
                    } else {
                        ++boxed;
                        qInfo() << "SMART-LIVE  框住:" << c.title.left(24) << "→" << got;
                    }
                }
                qInfo() << "SMART-LIVE  屏" << ov->screen()->name() << ":框住" << boxed
                        << "被遮挡" << occluded << "不在本屏" << skippedOffscreen
                        << "漏框" << missed;
                check(missed == 0,
                      QStringLiteral("屏 %1:可见候选都能被框住(漏框 %2 个)")
                          .arg(ov->screen()->name()).arg(missed));
            }

            // 清场
            //
            // ⚠️ 只 hide()，**不要 close()**（docs/29 §3）。
            //
            // 遮罩带 WA_DeleteOnClose：`close()` 会同步触发 delete（析构）。
            // 而多屏时每块屏一块遮罩，事件队列里几乎总有残留的 paint ——
            // Qt 随后会拿**已销毁的 paint device** 去画：
            //   QPainter::begin → engine == 0 → 一串 "Painter not active"
            //   → QPainter::end 访问已释放的备用缓冲 → 0xC0000005
            // 这就是本 flow 间歇性崩溃（实测 2/3~2/5 概率）的成因。
            //
            // 「hide() + processEvents() + close()」试过，不够：两块屏的遮罩会
            // 互相触发重绘，跑完一轮又有新的排队。既然紧接着就要 quit()，
            // 最稳的做法是**根本不主动析构** —— 让进程退出一并回收，
            // 既没有窗口生命周期竞态，也不会漏掉什么（验收不验析构顺序）。
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* o = qobject_cast<OverlayWidget*>(w)) o->hide();
            }
            QTimer::singleShot(500, qApp, [] {
                qInfo() << "SMART-LIVE  RESULT failures =" << g_failures;
                QApplication::quit();
            });
        });
    });
}

// ---- 设置页开关验收(--smart-settings-flow)----
//
// 为什么单开一条而不是塞进 --settings-flow:那条 flow 的 lambda 嵌套已经到 30 层,
// 再插一段就没人能读了;而"智能框选开关"要验的东西很简单独立:
//   ① 切到「截图」分类后控件真的存在(分类页是 Loader 懒装,漏 import 只有
//      切过去才会炸 —— 这一点 docs/22 记录过真实事故);
//   ② 真实点击能翻转并**落盘**(scheduleSave 去抖,读太早会拿到旧值);
//   ③ 再点一次能恢复(不给后续步骤留副作用)。
void runSmartSettingsFlow(PanelWindow& panel) {
    QTimer::singleShot(1200, qApp, [&panel] {
        panel.showSettings();
        QTimer::singleShot(900, &panel, [&panel] {
            const bool navClicked =
                panel.clickByObjectName(QStringLiteral("settingsNavCapture"));
            qInfo() << "SMART-SETTINGS  切到截图分类:" << navClicked;
            check(navClicked, QStringLiteral("侧栏「截图」分类可点击"));

            QTimer::singleShot(800, &panel, [&panel] {
                panel.setRootProperty("acceptanceScrollTarget",
                                      QVariant(QStringLiteral("smartCaptureCheck")));
                const bool scrolled = panel.invokeAcceptance("acceptanceSettingsScrollToTarget");
                qInfo() << "SMART-SETTINGS  滚入视口:" << scrolled;

                const bool before = CoreFacade::instance()
                                        .call("get_settings")
                                        .value("data").toMap()
                                        .value("smartCapture").toBool();
                const bool clicked =
                    panel.clickByObjectName(QStringLiteral("smartCaptureCheck"));
                qInfo() << "SMART-SETTINGS  点击开关:" << clicked << "before =" << before;

                QTimer::singleShot(800, &panel, [&panel, before, clicked] {
                    const bool after = CoreFacade::instance()
                                           .call("get_settings")
                                           .value("data").toMap()
                                           .value("smartCapture").toBool();
                    qInfo() << "SMART-SETTINGS  after =" << after;
                    check(clicked, QStringLiteral("「智能框选」开关可真实点击(控件存在且可命中)"));
                    check(before != after,
                          QStringLiteral("点一下即翻转并落盘(%1 -> %2)").arg(before).arg(after));

                    // 复原
                    panel.clickByObjectName(QStringLiteral("smartCaptureCheck"));
                    QTimer::singleShot(800, &panel, [&panel, before] {
                        const bool restored = CoreFacade::instance()
                                                  .call("get_settings")
                                                  .value("data").toMap()
                                                  .value("smartCapture").toBool();
                        check(restored == before,
                              QStringLiteral("再点一次恢复原值(得 %1,期望 %2)")
                                  .arg(restored).arg(before));
                        qInfo() << "SMART-SETTINGS  RESULT failures =" << g_failures;
                        QApplication::quit();
                    });
                });
            });
        });
    });
}

} // namespace nb
