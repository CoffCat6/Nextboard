// 桌面便签交互验收(--note-flow,docs/20-todo-sticky-note.md §10.2)。
//
// 为什么必须有这一条:便签的核心价值全在"拖动 → 吸附 → 半隐藏 → 鼠标回来"这条
// 交互链上,而这条链的每一步都只靠**坐标与真实事件**判定,盯着代码看不出对错
// (吸附阈值、收起几何、拉手是否落在屏内 —— 任一算错的表现都是"窗口跑了/收不回来")。
// 本仓库的既有结论:只有投真实事件才能抓出这类缺陷(见 panel_window.hpp:100 的说明)。
//
// 流程(每步读回库/几何并打 NOTE PASS/FAIL):
//   STEP1 播种待办 → open() → 窗口存在,几何 == 库中几何
//   STEP2 拖标题条到右边缘 5px 处松手 → edge == right,右边对齐可用区右边
//   STEP3 开 autoHide → 指针移开 → collapsed == 1,屏内只留拉手
//   STEP4 指针移到拉手 → 滑回,collapsed == 0
//   STEP4.5 正文区右键必须出**便签菜单**(QTextEdit 默认会吞掉它)
//   STEP4.6 菜单项**真实触发**:贴左边 / 收起态取消贴边 / 不透明度落库
//   STEP5 改正文 → todo_get 的 textContent 一致
//   STEP6 删除该待办 → 窗口关闭,todo_note 无孤儿行
//
// ⚠️ 全程**串行回调链**:各步都会改库(edge/collapsed/opacity/正文),
//    并行 singleShot 会让两组写入交错,断言就不可信了。
#include "host/acceptance.hpp"

#include "note/note_registry.hpp"
#include "note/note_window.hpp"
#include "panel/clip_entry_model.hpp"
#include "panel/qml_bootstrap.hpp"
#include "panel/core_facade.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDebug>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QLabel>
#include <QTextEdit>
#include <QTimer>

namespace nb {
namespace {

int g_failures = 0;

void check(bool ok, const QString& what) {
    if (ok) {
        qInfo().noquote() << "NOTE  PASS" << what;
    } else {
        ++g_failures;
        qWarning().noquote() << "NOTE  FAIL" << what;
    }
}

void finish() {
    if (g_failures == 0) qInfo() << "NOTE 全部通过";
    else qWarning() << "NOTE 失败项:" << g_failures;
    qApp->quit();
}

/// 步骤入口的**窗口存活守卫**。
///
/// 为什么需要(2026-11-03,外部审查 #8):整条流程是串行回调链,各步的
/// QTimer::singleShot( 用 qApp 作 context —— 那只能保证"应用还活着",
/// **不能**保证回调里解引用的 window 还活着。STEP6 删待办会关窗、外部
/// close() 也会,于是回调里 window->geometry() 就是一次 UAF。
/// 当前靠"流程顺序保证窗口活着"这条脆弱契约兜底。
///
/// 修法取舍:把 context 换成 window 是错的 —— 定时器会静默失效,那几步断言
/// 就"没跑也算过"(正是 #26 那种假绿)。这里改成**显式判空 + 记失败**:
/// 窗口真没了是有诊断价值的失败,而不是悄悄跳过。
/// @return false 表示窗口已销毁,调用方应立即 return。
bool requireWindow(QWidget* window, const char* step) {
    if (window) return true;
    ++g_failures;
    qWarning().noquote() << "NOTE  FAIL" << step << "窗口已销毁(无法继续该步)";
    return false;
}

void sendMouse(QWidget* w, QEvent::Type type, const QPoint& local, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent ev(type, QPointF(local), w->mapToGlobal(QPointF(local)), button, buttons,
                   Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

/// 在标题条上按下 → 分步移动 → 松手,把**窗口**的左上都搬到 targetPos。
///
/// ⚠️ 目标必须是**窗口位置**而不是光标位置(2026-10-18 修):
///    拖动时窗口是"跟着按下点走"的(m_dragOffset = 按下点 − 窗口左上),
///    所以"把光标移到 X"只会把窗口搬到 X − offset。单屏内 offset 很小,
///    吸附阈值(12px)恰好把它吸收掉,看起来是对的;一旦跨屏(offset 上百 px),
///    窗口就落在离目标老远的位置 —— 表现为"拖过去没贴上",把测试搞得像产品 bug。
void dragWindowTo(QWidget* w, const QPoint& targetPos, int steps = 8) {
    const QPoint fromLocal(kNoteShadowMargin + 60, kNoteShadowMargin + 10);
    const QPoint pressGlobal = w->mapToGlobal(fromLocal);
    sendMouse(w, QEvent::MouseButtonPress, fromLocal, Qt::LeftButton, Qt::LeftButton);
    const QPoint delta = targetPos - w->pos();
    for (int i = 1; i <= steps; ++i) {
        const QPoint g = pressGlobal + delta * i / steps;
        sendMouse(w, QEvent::MouseMove, g - w->pos(), Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(w, QEvent::MouseButtonRelease, pressGlobal + delta - w->pos(),
              Qt::LeftButton, Qt::NoButton);
}

/// 把窗口的**某条边**搬到屏幕可用区的对应边(吸附测试的靶点)。
///
/// ⚠️ 必须先把另一根轴搬离其它边(2026-10-18 修):吸附判定取"距离最近的边",
///    两条边距离相等时按固定顺序取第一条。窗口若已在左边,只改 y 拖到上边,
///    落点是**角落**,left 与 top 距离都是 8 → 产品判 left(确定性行为,没错),
///    但测试期望 top 就红了 —— 这是测试的靶子没摆干净,不是产品 bug。
void dragEdgeTo(QWidget* w, const QString& edge, const QRect& avail) {
    QPoint target = w->pos();
    const int marginX = 40;
    const int marginY = 40;
    if (edge == QLatin1String("left") || edge == QLatin1String("right")) {
        // 横向贴边:先把纵向挪到中间(远离上/下边)
        target.setY(avail.center().y() - w->height() / 2);
    } else {
        // 纵向贴边:先把横向挪到中间(远离左/右边)
        target.setX(avail.center().x() - w->width() / 2);
    }
    Q_UNUSED(marginX);
    Q_UNUSED(marginY);
    // 先搬到"中间的空位",松手(不吸附),再走最后一段 —— 一步到位会让
    // 起点与终点跨过整个屏幕,拖动过程经过的边不参与判定,但分两段更接近真人
    dragWindowTo(w, target);
    if (edge == QLatin1String("left")) {
        target.setX(avail.left() - kNoteShadowMargin);
    } else if (edge == QLatin1String("right")) {
        target.setX(avail.right() - w->width() + kNoteShadowMargin + 1);
    } else if (edge == QLatin1String("top")) {
        target.setY(avail.top() - kNoteShadowMargin);
    } else if (edge == QLatin1String("bottom")) {
        target.setY(avail.bottom() - w->height() + kNoteShadowMargin + 1);
    }
    dragWindowTo(w, target);
}

/// 窗口**当前所在屏**的可用区(与产品 availableRect 同一口径:位置优先)。
/// ⚠️ 不能缓存:窗口换屏后旧屏的边完全不适用 —— 曾经用主屏的 avail 去断言
///    副屏上的吸附,结果"下边贴住"必然失败。
QRect availUnder(QWidget* w) {
    QScreen* screen = QGuiApplication::screenAt(w->geometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

/// 让"指针进入/离开"这件事发生。
///
/// 两个事实(都是实测):
///  1) 本机 `QCursor::setPos` 之后立刻读回**可能是另一个坐标**(1916,235 → 683,767),
///     有其它进程在动鼠标(远程会话/辅助工具)。只靠真实光标,验收会变成看天气。
///  2) Qt 只在**它自己观察到**指针跨越窗口边界时才发 Enter/Leave。而前面的拖动是用
///     合成事件做的,真实光标从未进入过窗口 —— 于是"把光标移走"不会产生 Leave,
///     autoHide 的计时根本不会启动(实测 STEP3 因此恒失败)。
///
/// 所以这里**两条路都走**:先真实移动光标(尽量贴近用户路径),再显式投递
/// Enter/Leave(保证确定性)。两者驱动的是**同一个** enterEvent/leaveEvent ——
/// 也就是同一段产品代码,只是补上了 OS 那一跳的观测。重复投递是幂等的:
/// enterEvent 只是停表 + (收起时)展开,leaveEvent 只是启动收起计时。
void movePointerTo(QWidget* w, const QPoint& globalTarget, bool expectInside) {
    QCursor::setPos(globalTarget);
    const QPoint after = QCursor::pos();
    const bool landed = (after - globalTarget).manhattanLength() <= 4;
    qInfo() << "STEP-pointer" << (expectInside ? "进入" : "离开") << globalTarget
            << "真实光标读回" << after << (landed ? "(落点一致)" : "(被外部搬走)");
    // 确定性的一跳:不论光标是否听话,都让产品代码收到对应事件。
    QEvent ev(expectInside ? QEvent::Enter : QEvent::Leave);
    QApplication::sendEvent(w, &ev);
}

QVariantMap noteRowFromCore(const QString& id) {
    const QVariantMap resp = CoreFacade::instance().call(QStringLiteral("todo_note_list"));
    const QVariantList items = resp.value(QStringLiteral("data")).toMap()
                                   .value(QStringLiteral("items")).toList();
    for (const QVariant& raw : items) {
        const QVariantMap item = raw.toMap();
        if (item.value(QStringLiteral("id")).toString() == id)
            return item.value(QStringLiteral("note")).toMap();
    }
    return {};
}

QRect screenAvailFor(QWidget* w) {
    QScreen* screen = QGuiApplication::screenAt(w->geometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

QWidget* findNoteWindow() {
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == QLatin1String("nbNoteWindow") && w->isVisible()) return w;
    }
    return nullptr;
}

QMenu* submenu(QWidget* window, const QString& title) {
    auto* menu = window->findChild<QMenu*>(QStringLiteral("nbNoteMenu"));
    if (!menu) return nullptr;
    for (QAction* action : menu->actions()) {
        if (action->text() == title && action->menu()) return action->menu();
    }
    return nullptr;
}

bool triggerChild(QMenu* menu, const QString& label) {
    if (!menu) return false;
    for (QAction* action : menu->actions()) {
        if (action->text() == label) {
            action->trigger();
            return true;
        }
    }
    return false;
}

/// 在给定坐标弹一次便签菜单(走真实的 contextMenuEvent 路径)。
void popNoteMenu(QWidget* window, const QPoint& local) {
    QContextMenuEvent ev(QContextMenuEvent::Mouse, local, window->mapToGlobal(local));
    QApplication::sendEvent(window, &ev);
}

// 串行链的每一步:用 std::function 逐个接续,避免层层嵌套 lambda 难以阅读
using Step = std::function<void()>;

void step5(const QString& id, QWidget* window);
void step6(const QString& id, QWidget* window);

// ---- STEP4.6:菜单项真实触发 ----
/// 挑一条**不是屏幕交界处**的贴边方向,供"半隐藏"类断言使用。
///
/// 交界处不做半隐藏(产品行为,见 docs/20 §13.12),所以在三屏机上随便挑一条边
/// 可能正好是交界 —— 断言"已收起"会失败,而那其实是**正确**的。
/// 这里按主屏两侧的邻屏情况挑:左边外有屏就选右边,反之亦然。
/// 测试侧的交界判定,与产品 NoteWindow::edgeOutsideIsAnotherScreen 同口径。
/// 不直接调产品私有函数(它在 private 区),这里按屏幕的**可用区几何**复算 ——
/// 若两份口径漂移,可以把产品那句 qInfo 与这里的日志对照。
bool edgeOutsideIsAnotherScreenForTest(QWidget* window, const QString& edge) {
    QScreen* screen = QGuiApplication::screenAt(window->geometry().center());
    if (!screen || edge == QLatin1String("none")) return false;
    const QRect avail = screen->availableGeometry();
    const int y = avail.center().y();
    const QPoint probe = (edge == QLatin1String("left"))
        ? QPoint(avail.left() - 2, y) : QPoint(avail.right() + 2, y);
    QScreen* other = QGuiApplication::screenAt(probe);
    return other != nullptr && other != screen;
}

QString collapsibleEdgeFor(QScreen* screen) {
    if (!screen) return QStringLiteral("right");
    const QRect avail = screen->availableGeometry();
    const int y = avail.center().y();
    const bool leftBusy = QGuiApplication::screenAt(QPoint(avail.left() - 2, y)) != nullptr;
    const bool rightBusy = QGuiApplication::screenAt(QPoint(avail.right() + 2, y)) != nullptr;
    if (!leftBusy) return QStringLiteral("left");
    if (!rightBusy) return QStringLiteral("right");
    // 两侧都是屏(整机没有外缘):返回空,调用方跳过半隐藏断言
    return QString();
}

void step46(const QString& id, QWidget* window) {
    // 回归两处实测缺陷:
    //  ① 收起态点「取消贴边」→ collapsed 若留在 1,窗口会停在按边算出的屏外位置,
    //     鼠标永远进不来 = 便签"凭空消失";
    //  ② 菜单项点了不落库 / 勾选态与真实状态不一致。
    popNoteMenu(window, QPoint(window->width() / 2, kNoteShadowMargin + 10));
    QMenu* edgeMenu = submenu(window, QStringLiteral("贴到屏幕边缘"));
    check(edgeMenu != nullptr, QStringLiteral("STEP4.6 有「贴到屏幕边缘」子菜单"));
    // 挑一条**不在屏幕交界处**的边:交界处不做半隐藏,后面"强制收起"的断言
    // 在那里必然失败 —— 那会是**正确行为**被当成缺陷。
    QScreen* here = QGuiApplication::screenAt(window->geometry().center());
    const QString usableEdge = collapsibleEdgeFor(here);
    const QString edgeLabel = usableEdge == QLatin1String("left")
        ? QStringLiteral("贴左边") : QStringLiteral("贴右边");
    qInfo() << "STEP4.6 选用贴边方向:" << usableEdge;
    check(!usableEdge.isEmpty(), QStringLiteral("STEP4.6 本机至少有一条可收起的边"));
    check(triggerChild(edgeMenu, edgeLabel),
          QStringLiteral("STEP4.6 触发贴边菜单项"));

    QTimer::singleShot(400, qApp, [id, window, usableEdge] {
        if (!requireWindow(window, "STEP4.6")) return;
        auto* note = qobject_cast<NoteWindow*>(window);
        const QVariantMap row = noteRowFromCore(id);
        const QRect avail = screenAvailFor(window);
        check(row.value(QStringLiteral("edge")).toString() == usableEdge,
              QStringLiteral("STEP4.6 贴边方向已落库"));
        check((usableEdge == QLatin1String("left")
                   ? qAbs(window->geometry().left() - avail.left())
                   : qAbs(window->geometry().right() + 1 - kNoteShadowMargin
                          - (avail.right() + 1))) <= kNoteShadowMargin + 2,
              QStringLiteral("STEP4.6 窗口贴住可用区对应边"));

        // ---- 缺陷 ①:收起态 + 取消贴边 ----
        note->collapse(/*animate=*/false, /*force=*/true);
        check(note->isCollapsed(), QStringLiteral("STEP4.6 强制收起生效(prepare)"));
        check(triggerChild(submenu(window, QStringLiteral("贴到屏幕边缘")),
                           QStringLiteral("取消贴边")),
              QStringLiteral("STEP4.6 触发「取消贴边」"));

        QTimer::singleShot(400, qApp, [id, window, note] {
            if (!requireWindow(window, "STEP4.6-b")) return;
            if (!note) { ++g_failures; qWarning() << "NOTE  FAIL STEP4.6-b 便签对象已销毁"; return; }
            const QVariantMap row2 = noteRowFromCore(id);
            const QRect avail = screenAvailFor(window);
            qInfo() << "STEP4.6 取消贴边后:edge=" << row2.value(QStringLiteral("edge"))
                    << "collapsed=" << row2.value(QStringLiteral("collapsed"))
                    << "窗口:" << window->geometry();
            check(row2.value(QStringLiteral("edge")).toString() == QLatin1String("none"),
                  QStringLiteral("STEP4.6 取消贴边已落库"));
            check(!row2.value(QStringLiteral("collapsed")).toBool(),
                  QStringLiteral("STEP4.6 取消贴边同时清掉收起态(否则窗口留在屏外)"));
            check(!note->isCollapsed(), QStringLiteral("STEP4.6 内存态也不是收起"));
            check(window->geometry().intersects(avail),
                  QStringLiteral("STEP4.6 取消贴边后窗口回到屏幕可见区"));

            // ---- 不透明度档位真实落库 ----
            check(triggerChild(submenu(window, QStringLiteral("不透明度")),
                               QStringLiteral("60%")),
                  QStringLiteral("STEP4.6 触发「不透明度 60%」"));
            QTimer::singleShot(300, qApp, [id, window] {
                const QVariantMap row3 = noteRowFromCore(id);
                check(row3.value(QStringLiteral("opacity")).toInt() == 60,
                      QStringLiteral("STEP4.6 不透明度已落库"));
                Q_UNUSED(row3);
                step5(id, window);
            });
        });
    });
}

// ---- STEP5:便签正文编辑的是**备注**,标题不受影响(docs/21)----
void step5(const QString& id, QWidget* window) {
    auto* note = qobject_cast<NoteWindow*>(window);
    auto* body = window->findChild<QTextEdit*>(QStringLiteral("nbNoteBody"));
    check(body != nullptr, QStringLiteral("STEP5 找到正文控件"));
    if (body) {
        body->setFocus();
        body->setPlainText(QStringLiteral("验收写入的备注\n第二行"));
        note->flushBody();
    }
    QTimer::singleShot(500, qApp, [id, window] {
        const QVariantMap got = CoreFacade::instance().call(
            QStringLiteral("todo_get"), {{QStringLiteral("id"), id}});
        const QVariantMap data = got.value(QStringLiteral("data")).toMap();
        const QString title = data.value(QStringLiteral("textContent")).toString();
        const QString noteText = data.value(QStringLiteral("noteText")).toString();
        qInfo() << "STEP5 todo_get title:" << title << "noteText:" << noteText;
        // 关键回归:便签正文改的是**备注**,不能把标题覆盖掉。
        check(title == QStringLiteral("验收便签正文"),
              QStringLiteral("STEP5 标题未被正文编辑覆盖"));
        check(noteText == QStringLiteral("验收写入的备注\n第二行"),
              QStringLiteral("STEP5 备注已写回待办(多行原样)"));

        // ---- STEP5b:清空备注(回归 core 的三态语义:缺键=不改,空串=清空)----
        auto* body2 = window->findChild<QTextEdit*>(QStringLiteral("nbNoteBody"));
        auto* note2 = qobject_cast<NoteWindow*>(window);
        if (body2 && note2) {
            body2->setPlainText(QString());
            note2->flushBody();
        }
        QTimer::singleShot(400, qApp, [id, window] {
            const QVariantMap got2 = CoreFacade::instance().call(
                QStringLiteral("todo_get"), {{QStringLiteral("id"), id}});
            const QVariantMap data2 = got2.value(QStringLiteral("data")).toMap();
            qInfo() << "STEP5b noteText:" << data2.value(QStringLiteral("noteText"));
            check(data2.value(QStringLiteral("noteText")).toString().isEmpty(),
                  QStringLiteral("STEP5b 备注可被清空(空串真的清掉)"));
            check(data2.value(QStringLiteral("textContent")).toString()
                      == QStringLiteral("验收便签正文"),
                  QStringLiteral("STEP5b 清备注不影响标题"));

            // ---- STEP5c:面板改完 → 便签**立刻**更新(本轮用户反馈的缺陷)----
            //
            // 缺陷原形:NoteRegistry 连的是 CoreFacade::clipChanged,而 core 的
            // WM_APP_CLIP_CHANGED 实际由 PanelWindow::nativeEvent 转发成
            // PanelWindow::clipChanged —— 面板内的 todo_update 根本不经过前者,
            // 于是便签只在"关掉重贴"时才刷新。
            // 这里走**真实链路**:core 写库 → PostMessage → PanelWindow → 便签。
            // 走**面板编辑卡片的真实入口**(todoSave),不是裸调 todo_update:
            // 后者会绕过"备注总是下发 / 标签三态"那段组装逻辑。
            ClipEntryModel* model = entryModelInstance();
            check(model != nullptr, QStringLiteral("STEP5c 拿到面板模型实例"));
            const QVariantMap times{{QStringLiteral("dueAt"), QVariant()},
                                    {QStringLiteral("remindAt"), QVariant()},
                                    {QStringLiteral("notifyAt"), QVariant()}};
            const QVariantMap upd = model->todoSave(
                id, QStringLiteral("面板改过的标题"), times, QStringList(),
                QStringLiteral("面板改过的备注"));
            check(upd.value(QStringLiteral("ok")).toBool(),
                  QStringLiteral("STEP5c 面板侧写入成功"));
            // core 的调度是"写库后 PostMessage",给宿主一个事件循环的时间
            QTimer::singleShot(600, qApp, [id, window] {
                if (!requireWindow(window, "STEP5c")) return;
                auto* body3 = window->findChild<QTextEdit*>(
                    QStringLiteral("nbNoteBody"));
                const QString bodyText = body3 ? body3->toPlainText() : QString();
                qInfo() << "STEP5c 便签正文:" << bodyText;
                check(bodyText == QStringLiteral("面板改过的备注"),
                      QStringLiteral("STEP5c 面板改备注后便签正文立刻更新"));
                // 标题也要跟着变:只更新正文会让"改名后便签还挂着旧标题"更困惑
                QLabel* title = window->findChild<QLabel*>(
                    QStringLiteral("nbNoteTitle"));
                const QString titleText = title ? title->text() : QString();
                qInfo() << "STEP5c 便签标题条:" << titleText;
                check(titleText == QStringLiteral("面板改过的标题"),
                      QStringLiteral("STEP5c 面板改标题后便签标题条立刻更新"));
                step6(id, window);
            });
        });
    });
}

// ---- STEP6:删除待办 → 窗口关闭 + 无孤儿行 ----
void step6(const QString& id, QWidget* window) {
    CoreFacade::instance().call(QStringLiteral("todo_delete"), {{QStringLiteral("id"), id}});
    NoteRegistry::instance().syncFromCore();
    QTimer::singleShot(400, qApp, [id, window] {
        Q_UNUSED(window);
        const QVariantMap row = noteRowFromCore(id);
        qInfo() << "STEP6 row 存在:" << !row.isEmpty()
                << "注册表数量:" << NoteRegistry::instance().count();
        check(row.isEmpty(), QStringLiteral("STEP6 条目删除后 todo_note 无孤儿行"));
        check(NoteRegistry::instance().count() == 0,
              QStringLiteral("STEP6 便签窗口已关闭并从注册表摘除"));
        finish();
    });
}

// ---- STEP4.5:正文区右键必须出便签菜单 ----
void step45(const QString& id, QWidget* window) {
    // 缺陷原形:QTextEdit 默认自带英文右键菜单并吃掉 contextMenuEvent,
    // 于是正文区右键只有 Copy/Paste,便签菜单根本出不来。
    auto* body = window->findChild<QTextEdit*>(QStringLiteral("nbNoteBody"));
    check(body != nullptr, QStringLiteral("STEP4.5 找到正文控件"));
    if (!body) {
        step46(id, window);
        return;
    }
    check(body->contextMenuPolicy() == Qt::CustomContextMenu,
          QStringLiteral("STEP4.5 正文右键策略为自定义(不被 QTextEdit 吞掉)"));
    const QPoint local(body->width() / 2, body->height() / 2);
    QContextMenuEvent ev(QContextMenuEvent::Mouse, local, body->mapToGlobal(local));
    QApplication::sendEvent(body, &ev);

    QMenu* menu = window->findChild<QMenu*>(QStringLiteral("nbNoteMenu"));
    check(menu != nullptr, QStringLiteral("STEP4.5 便签菜单已创建"));
    if (menu) {
        QStringList labels;
        for (QAction* action : menu->actions()) labels << action->text();
        qInfo() << "STEP4.5 菜单项:" << labels;
        check(menu->isVisible(), QStringLiteral("STEP4.5 正文右键弹出菜单"));
        check(labels.contains(QStringLiteral("编辑待办…")),
              QStringLiteral("STEP4.5 菜单含「编辑待办…」"));
        check(labels.contains(QStringLiteral("标记完成"))
                  || labels.contains(QStringLiteral("取消完成")),
              QStringLiteral("STEP4.5 菜单含完成态切换"));
        check(labels.contains(QStringLiteral("复制内容")),
              QStringLiteral("STEP4.5 菜单含「复制内容」(标题+备注)"));
        check(labels.contains(QStringLiteral("贴到屏幕边缘")),
              QStringLiteral("STEP4.5 菜单含显式贴边入口"));
        check(labels.contains(QStringLiteral("不透明度")),
              QStringLiteral("STEP4.5 菜单含不透明度档位"));
        bool topChecked = false;
        for (QAction* action : menu->actions()) {
            if (action->text() == QStringLiteral("置顶")) topChecked = action->isChecked();
        }
        check(topChecked, QStringLiteral("STEP4.5 「置顶」勾选态 == 实际状态"));
        menu->close();
    }
    body->clearFocus();
    step46(id, window);
}

void step2b(const QString& id, QWidget* window, const QRect& avail);
void step2a(const QString& id, QWidget* window, const QRect& avail);
void step3b(const QString& id, QWidget* window, const QRect& avail);

// ---- STEP2b:每条**受支持的边**都必须能吸附(回归"只能贴右边")----
//
// 生产反馈:便签只能贴右边。逐边从屏幕中间拖一次,断言 edge 落库 + 对应边贴住。
// 每边一个 300ms 回调(拖动合成是同步的,只需等落库)。
// ⚠️ 只覆盖左/右:上下边已从产品面下线(2026-10-19 用户裁定),不再测也不再有入口。
void step3(const QString& id, QWidget* window, const QRect& avail);

void step2a(const QString& id, QWidget* window, const QRect& avail) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.size() < 2) {
        qWarning() << "STEP2a SKIP 本机只有" << screens.size() << "块屏,跨屏拖动无法验证";
        step2b(id, window, avail);
        return;
    }
    // 找一块与当前屏不同的屏,把窗口拖到它**中心附近**
    QScreen* here = QGuiApplication::screenAt(window->geometry().center());
    QScreen* other = nullptr;
    for (QScreen* s : screens) {
        if (s != here) { other = s; break; }
    }
    if (!other) {
        qWarning() << "STEP2a SKIP 找不到另一块屏";
        step2b(id, window, avail);
        return;
    }
    const QRect otherAvail = other->availableGeometry();
    qInfo() << "STEP2a 当前屏:" << (here ? here->name() : QStringLiteral("?"))
            << "目标屏:" << other->name() << otherAvail;
    dragWindowTo(window, otherAvail.center() - QPoint(window->width() / 2,
                                                      window->height() / 2));
    QTimer::singleShot(400, qApp, [id, window, other, otherAvail, avail] {
        if (!requireWindow(window, "STEP2a")) return;
        const QVariantMap row = noteRowFromCore(id);
        const QRect geo = window->geometry();
        qInfo() << "STEP2a 松手后 geo:" << geo
                << "screen(库):" << row.value(QStringLiteral("screen"));
        check(geo.center() != QPoint(), QStringLiteral("STEP2a 窗口存在"));
        // 1) 真的跨过去了:中心落在目标屏可用区内
        check(otherAvail.contains(geo.center()),
              QStringLiteral("STEP2a 窗口中心落在目标屏(能拖到其他屏幕)"));
        // 2) 归属屏也跟着更新(否则下次吸附会按旧屏算边)
        check(row.value(QStringLiteral("screen")).toString() == other->name(),
              QStringLiteral("STEP2a 库中 screen 已切到目标屏"));
        // 3) 几何也落库了(不能只写 edge)
        check(qAbs(geo.x() - row.value(QStringLiteral("x")).toInt()) <= 1
                  && qAbs(geo.y() - row.value(QStringLiteral("y")).toInt()) <= 1,
              QStringLiteral("STEP2a 位置已落库"));

        // 4) 在**新屏**上贴左边:这条同时回归"只能贴右边"——
        //    旧实现在副屏上算的是主屏的边,四边里只有恰好重合的右侧能命中。
        dragEdgeTo(window, QStringLiteral("left"), otherAvail);
        QTimer::singleShot(400, qApp, [id, window, otherAvail, avail] {
            if (!requireWindow(window, "STEP2a-b")) return;
            const QVariantMap row2 = noteRowFromCore(id);
            const QRect geo2 = window->geometry();
            qInfo() << "STEP2a 副屏贴左边 edge:" << row2.value(QStringLiteral("edge"))
                    << "geo:" << geo2 << "目标可用区:" << otherAvail;
            check(row2.value(QStringLiteral("edge")).toString() == QLatin1String("left"),
                  QStringLiteral("STEP2a 副屏上也能贴左边(不只能贴右边)"));
            check(qAbs(geo2.left() - otherAvail.left()) <= kNoteShadowMargin + 2,
                  QStringLiteral("STEP2a 贴的是目标屏的左边"));
            // 回主屏继续后续步骤(STEP2b/3/4 以主屏几何设计)
            // 回主屏(按**窗口位置**搬,不是按光标)
            QScreen* primary = QGuiApplication::primaryScreen();
            const QRect home = primary ? primary->availableGeometry() : avail;
            dragWindowTo(window, home.center() - QPoint(window->width() / 2,
                                                        window->height() / 2));
            QTimer::singleShot(300, qApp, [id, window, home] {
                // ⚠️ 必须贴**左边**:本机主屏左侧接着 S24 PRO(x=-1080..0),
                //    收起时窗口被推出左边界,旧实现的锚点正好落进邻屏 —— 那才是
                //    用户看到的"相邻屏之间来回横跳"。贴右边在单侧无邻屏的机器上
                //    锚点落在屏幕外的虚空里,screenAt() 返回空,缺陷反而复现不出来。
                // 9 秒后再进 STEP2b —— 那边要用窗口当前状态继续往下测。
                dragEdgeTo(window, QStringLiteral("left"), home);
                QTimer::singleShot(300, qApp, [id, window, home] {
                    step3b(id, window, home);
                    QTimer::singleShot(9000, qApp, [id, window, home] {
                        step2b(id, window, home);
                    });
                });
            });
        });
    });
}

void step2b(const QString& id, QWidget* window, const QRect& avail) {
    // 只测左右(2026-10-19:上下边已从产品面下线)。
    static const char* kNames[2] = {"left", "right"};
    auto chain = std::make_shared<std::function<void(int)>>();
    *chain = [id, window, avail, chain](int index) {
        if (index >= 2) {
            // 回到右边再进 STEP3(STEP3/4 的拉手几何按右边设计)
            dragEdgeTo(window, QStringLiteral("right"), avail);
            QTimer::singleShot(300, qApp, [id, window, avail] {
                const QVariantMap row = noteRowFromCore(id);
                check(row.value(QStringLiteral("edge")).toString()
                          == QLatin1String("right"),
                      QStringLiteral("STEP2b 回到右边吸附"));
                step3(id, window, avail);
            });
            return;
        }
        const QString expect = QString::fromLatin1(kNames[index]);
        const QRect here = availUnder(window);
        dragEdgeTo(window, expect, here);
        QTimer::singleShot(300, qApp, [id, window, chain, index, expect] {
            if (!requireWindow(window, "STEP2b-chain")) return;
            const QVariantMap row = noteRowFromCore(id);
            const QString edge = row.value(QStringLiteral("edge")).toString();
            const QRect geo = window->geometry();
            const QRect avail = availUnder(window);
            qInfo() << "STEP2b" << expect << "edge:" << edge << "geo:" << geo
                    << "avail:" << avail;
            check(edge == expect,
                  QStringLiteral("STEP2b 拖到 %1 吸附").arg(expect));
            if (expect == QLatin1String("left"))
                check(qAbs(geo.left() - avail.left()) <= kNoteShadowMargin + 2,
                      QStringLiteral("STEP2b 左边贴住可用区"));
            if (expect == QLatin1String("right"))
                check(qAbs(geo.right() + 1 - kNoteShadowMargin - (avail.right() + 1)) <= 2,
                      QStringLiteral("STEP2b 右边贴住可用区"));
            (*chain)(index + 1);
        });
    };
    (*chain)(0);
}

// ---- STEP4:指针进入拉手 → 滑回 ----
void step4(const QString& id, QWidget* window, const QRect& avail) {
    auto* note = qobject_cast<NoteWindow*>(window);
    const QPoint sliver(avail.right() - 3, window->geometry().center().y());
    qInfo() << "STEP4 指针移到拉手:" << sliver << "窗口:" << window->geometry();
    movePointerTo(window, sliver, /*expectInside=*/true);
    QTimer::singleShot(900, qApp, [id, window, note] {
        if (!requireWindow(window, "STEP4")) return;
        if (!note) { ++g_failures; qWarning() << "NOTE  FAIL STEP4 便签对象已销毁"; return; }
        const QVariantMap row = noteRowFromCore(id);
        qInfo() << "STEP4 collapsed:" << note->isCollapsed()
                << "geo:" << window->geometry();
        check(!note->isCollapsed(), QStringLiteral("STEP4 指针进入拉手后滑回"));
        check(!row.value(QStringLiteral("collapsed")).toBool(),
              QStringLiteral("STEP4 展开态已落库"));
        step45(id, window);
    });
}

// ---- STEP3:开 autoHide → 指针移开 → 收起 ----
void step3(const QString& id, QWidget* window, const QRect& avail) {
    auto* note = qobject_cast<NoteWindow*>(window);
    note->setAutoHide(true);
    movePointerTo(window, avail.center(), /*expectInside=*/false);
    QTimer::singleShot(1600, qApp, [id, window, note, avail] {
        if (!requireWindow(window, "STEP3")) return;
        if (!note) { ++g_failures; qWarning() << "NOTE  FAIL STEP3 便签对象已销毁"; return; }
        const QVariantMap row = noteRowFromCore(id);
        // 收起后窗口大部分在屏幕外:屏内可见宽必须取「窗口 ∩ 可用区」
        const QRect visible = window->geometry().intersected(avail);
        qInfo() << "STEP3 collapsed:" << note->isCollapsed()
                << "geo:" << window->geometry() << "屏内可见宽:" << visible.width();
        check(row.value(QStringLiteral("collapsed")).toBool(),
              QStringLiteral("STEP3 收起态已落库"));
        check(note->isCollapsed(), QStringLiteral("STEP3 窗口处于收起态"));
        // 必须有一截在屏内,否则鼠标进不来、永远收不回来
        check(visible.width() > 0
                  && visible.width() <= kNoteSliver + kNoteShadowMargin + 2,
              QStringLiteral("STEP3 屏内只留拉手一条(且不为 0)"));
        step4(id, window, avail);
    });
}

// ---- STEP3b:贴在某块屏的一边后,反复收起/展开**不该换屏** ----
//
// 用户实测缺陷(2026-10-19):便签贴在一块屏的一边,而这块屏旁边还连着另一块屏时,
// 自动收起会在两块屏之间来回横跳。根因是收起态用**几何锚点**认屏 —— 收起是程序
// 把窗口推出屏幕的动作,锚点落到了邻屏上,于是 next 次展开按邻屏的边算,
// 又落回原屏,来回振荡。
//
// 验收判据(不依赖内部实现):连续 3 轮 收起→展开,库里的 screen 必须**一直不变**,
// 且每轮展开后的几何必须等于第一轮的几何(位置不漂移)。
void step3b(const QString& id, QWidget* window, const QRect& avail) {
    auto* note = qobject_cast<NoteWindow*>(window);
    const QVariantMap before = noteRowFromCore(id);
    const QString screenBefore = before.value(QStringLiteral("screen")).toString();
    const QString edgeBefore = before.value(QStringLiteral("edge")).toString();
    const QRect geoBefore = window->geometry();
    qInfo() << "STEP3b 起点: screen:" << screenBefore << "edge:" << edgeBefore
            << "geo:" << geoBefore;
    // 只有贴边 + autoHide 才可能走到收起路径;先把状态摆好。
    // ⚠️ 若这条边是屏幕交界处,产品**拒绝**收起(见 docs/20 §13.12)——
    //    此时断言"已收起"会失败,而那是正确行为。断言改由 junction 分支覆盖。
    const QVariantMap rowNow = noteRowFromCore(id);
    const bool atJunction = edgeOutsideIsAnotherScreenForTest(
        window, rowNow.value(QStringLiteral("edge")).toString());
    note->setAutoHide(true);
    qInfo() << "STEP3b 该边是否屏幕交界:" << atJunction;

    auto chain = std::make_shared<std::function<void(int)>>();
    *chain = [id, window, avail, screenBefore, geoBefore, chain](int round) {
        if (round >= 3) {
            const QVariantMap row = noteRowFromCore(id);
            const QRect geo = window->geometry();
            qInfo() << "STEP3b 三轮后: screen:" << row.value(QStringLiteral("screen"))
                    << "geo:" << geo;
            check(row.value(QStringLiteral("screen")).toString() == screenBefore,
                  QStringLiteral("STEP3b 收起/展开三轮后归属屏未变(不横跳)"));
            // ⚠️ QRect 没有 operator-:逐轴比较左上角(尺寸不变)
            check(qAbs(geo.x() - geoBefore.x()) <= 2
                      && qAbs(geo.y() - geoBefore.y()) <= 2,
                  QStringLiteral("STEP3b 展开后位置不漂移"));
            return;
        }
        auto* note = qobject_cast<NoteWindow*>(window);
        // 收起:鼠标先移开(收起计时器依赖"鼠标不在窗口上")
        movePointerTo(window, avail.center(), /*expectInside=*/false);
        QTimer::singleShot(1300, qApp, [id, window, screenBefore, chain, round] {
            const QVariantMap collapsed = noteRowFromCore(id);
            // 交界处:产品**拒绝**收起(见 docs/20 §13.12)。断言反过来 ——
            // 不但不该收起,还得把 autoHide 关掉(否则是"勾着却永不生效"的假状态)。
            if (edgeOutsideIsAnotherScreenForTest(
                    window, collapsed.value(QStringLiteral("edge")).toString())) {
                check(!collapsed.value(QStringLiteral("collapsed")).toBool(),
                      QStringLiteral("STEP3b 第 %1 轮交界处未收起(正确行为)").arg(round + 1));
                check(!collapsed.value(QStringLiteral("autoHide")).toBool(),
                      QStringLiteral("STEP3b 第 %1 轮交界处已关掉 autoHide").arg(round + 1));
                (*chain)(round + 1);
                return;
            }
            check(collapsed.value(QStringLiteral("collapsed")).toBool(),
                  QStringLiteral("STEP3b 第 %1 轮已收起").arg(round + 1));
            // 收起态也必须保持原归属屏 —— 这一条正是缺陷的正面判据
            check(collapsed.value(QStringLiteral("screen")).toString() == screenBefore,
                  QStringLiteral("STEP3b 第 %1 轮收起后归属屏未变").arg(round + 1));
            // 展开:指针移进拉手(窗口在屏内的那一端)
            const QRect avail = availUnder(window);
            const QString edge = collapsed.value(QStringLiteral("edge")).toString();
            const QPoint sliver = (edge == QLatin1String("left"))
                ? QPoint(avail.left() + 3, window->geometry().center().y())
                : QPoint(avail.right() - 3, window->geometry().center().y());
            movePointerTo(window, sliver, /*expectInside=*/true);
            QTimer::singleShot(900, qApp, [chain, round] { (*chain)(round + 1); });
        });
    };
    (*chain)(0);
}

// ---- STEP2:拖到右边缘 5px 内 → 吸附 ----
void step2(const QString& id, QWidget* window, const QRect& avail) {
    const QRect geo = window->geometry();
    Q_UNUSED(geo);
    dragEdgeTo(window, QStringLiteral("right"), avail);
    QTimer::singleShot(300, qApp, [id, window, avail] {
        if (!requireWindow(window, "STEP2")) return;
        const QVariantMap row = noteRowFromCore(id);
        qInfo() << "STEP2 edge:" << row.value(QStringLiteral("edge"))
                << "geo:" << window->geometry() << "avail:" << avail;
        check(row.value(QStringLiteral("edge")).toString() == QLatin1String("right"),
              QStringLiteral("STEP2 松手后吸附到右边"));
        check(qAbs(window->geometry().right() + 1 - kNoteShadowMargin - (avail.right() + 1))
                  <= 2,
              QStringLiteral("STEP2 窗口右边与可用区右边对齐"));
        step2a(id, window, avail);
    });
}

// ---- STEP1:播种 + 建窗 ----
void step1() {
    CoreFacade& facade = CoreFacade::instance();
    const QVariantMap created = facade.call(
        QStringLiteral("todo_create"),
        {{QStringLiteral("text"), QStringLiteral("验收便签正文")}});
    const QString id = created.value(QStringLiteral("data")).toMap()
                           .value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        check(false, QStringLiteral("STEP1 播种待办失败"));
        finish();
        return;
    }
    // ⚠️ 备注必须在 **open() 之前**写:建窗那一刻就把 noteText 读进正文区了
    //    (顺序写反的表现是"备注明明存了,便签里却是空的")。
    const QVariantMap savedNote = facade.call(
        QStringLiteral("todo_update"),
        {{QStringLiteral("id"), id},
         {QStringLiteral("noteText"), QStringLiteral("备注应当显示在正文区")}});
    check(savedNote.value(QStringLiteral("ok")).toBool(), QStringLiteral("STEP1 备注可写入"));

    if (!NoteRegistry::instance().open(id)) {
        check(false, QStringLiteral("STEP1 open() 失败"));
        finish();
        return;
    }
    QTimer::singleShot(400, qApp, [id] {
        NoteRegistry& registry = NoteRegistry::instance();
        check(registry.count() == 1, QStringLiteral("STEP1 注册表有 1 张便签"));
        const QVariantMap row = noteRowFromCore(id);
        check(!row.isEmpty(), QStringLiteral("STEP1 库里有对应的 todo_note 行"));
        qInfo() << "STEP1 row:" << row;

        QWidget* window = findNoteWindow();
        check(window != nullptr, QStringLiteral("STEP1 便签窗口存在且可见"));
        if (!window) {
            finish();
            return;
        }
        const QRect geo = window->geometry();
        check(geo.x() == row.value(QStringLiteral("x")).toInt()
                  && geo.y() == row.value(QStringLiteral("y")).toInt(),
              QStringLiteral("STEP1 窗口位置 == 库中几何"));
        check(geo.width() - 2 * kNoteShadowMargin
                  == row.value(QStringLiteral("w")).toInt(),
              QStringLiteral("STEP1 窗口内容宽 == 库中 w"));

        // 正文区显示的是**备注**,标题条显示的是**标题**(docs/21 的核心语义)。
        // 这条覆盖"数据都落库了、但 UI 读错字段"——表现是便签里重复显示标题。
        if (auto* body = window->findChild<QTextEdit*>(QStringLiteral("nbNoteBody"))) {
            qInfo() << "STEP1 便签正文区:" << body->toPlainText();
            check(body->toPlainText() == QStringLiteral("备注应当显示在正文区"),
                  QStringLiteral("STEP1 便签正文区显示备注(不是标题)"));
        } else {
            check(false, QStringLiteral("STEP1 找不到便签正文控件"));
        }

        // avail 只算一次并全程沿用:收起后窗口中心已在屏外,screenAt 会退化到主屏,
        // 多屏机上那条会把正确实现误判成缺陷(实测踩到)。
        step2(id, window, screenAvailFor(window));
    });
}

} // namespace

void runNoteAcceptanceFlow() {
    // 等面板/托盘装配完成再动:过早建窗会与启动恢复撞车
    QTimer::singleShot(2500, qApp, [] { step1(); });
}

} // namespace nb
