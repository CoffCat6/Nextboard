// 桌面便签窗口实现。设计依据 docs/20-todo-sticky-note.md。
//
// 三条与其他窗口不同的约定(都踩过才写下来的):
//  1) **不用 startSystemMove()** 拖窗口:它进入系统模态移动循环,程序拿不到"松手"
//     这一刻,吸附就无从判定(docs/20 §6.5)。这里自己跟手移动,松手时做吸附。
//  2) **置顶用 SetWindowPos 动态切换**,不用 setWindowFlag():后者会销毁并重建原生
//     窗口,正在输入的便签会丢位置/焦点/编辑状态。
//  3) **底板自绘**:WA_TranslucentBackground 下 QSS 给控件自身画背景不生效
//     (pin_window.cpp:72 有原样记录),只给子控件用 QSS。

#include "note_window.hpp"

#include "../app_font.hpp"
#include "../host/memory_probe.hpp"
#include "../panel/core_facade.hpp"
#include "host/window_effects.hpp"

#include <QApplication>
#include <QClipboard>

#include "host/core_bridge.hpp"
#include <QGuiApplication>
#include <QPropertyAnimation>
#include <QCheckBox>
#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
// Linux(M0b)startSystemResize 需要 QWindow 完整类型(Windows 路径不经此处)
#include <QWindow>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace nb {

namespace {

/// 底板配色:跟随主题(亮/暗两套取自 CoreFacade 注入的 QPalette)。
/// 便签是"桌面陈设",对比度比主面板略低一档,避免在桌面上一片刺眼。
QColor plateColor(bool dark) {
    return dark ? QColor(0x1A, 0x20, 0x28, 246) : QColor(0xFF, 0xFF, 0xFF, 246);
}

bool isDarkPalette() {
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

constexpr int kHeaderH = 28;
constexpr int kFooterH = 20;
constexpr qreal kPlateRadius = 8.0;

} // namespace

QString normalizeNoteText(const QString& text) {
    QString out = text;
    // 换行统一(粘贴进来的 CRLF 不该在回灌比对时被当成"外部改了内容")
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // 只去尾部空白:用户正在输入的前导空格是有意义的
    while (out.endsWith(QLatin1Char('\n')) || out.endsWith(QLatin1Char(' '))
           || out.endsWith(QLatin1Char('\t'))) {
        out.chop(1);
    }
    return out;
}

NoteWindow::NoteWindow(const NoteData& data, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool | Qt::NoDropShadowWindowHint),
      m_id(data.id),
      m_done(data.done),
      m_dueAt(data.dueAt),
      m_overdue(data.overdue) {
    setObjectName(QStringLiteral("nbNoteWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    // 显示时**不抢前台**:开机自启恢复一批便签时抢焦点是灾难(docs/20 §7.5)。
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);
    // 需要键盘焦点:ESC 关闭、正文输入
    setFocusPolicy(Qt::StrongFocus);

    m_lastCommittedText = normalizeNoteText(data.text);
    m_lastCommittedNote = normalizeNoteText(data.noteText);
    buildUi();

    m_collapseTimer = new QTimer(this);
    m_collapseTimer->setSingleShot(true);
    m_collapseTimer->setInterval(700);
    connect(m_collapseTimer, &QTimer::timeout, this, [this] {
        // 两条保护(docs/20 §6.4):正在拖动/滑动中不收起;正在**输入**时不收起。
        //
        // ⚠️ 早先这里还有一条 `isActiveWindow()` —— 那是错的:鼠标移开窗口时窗口
        //    仍然持有焦点(没有别的窗口被激活),于是"鼠标早就离开、便签却永远收
        //    不起来"。自动收起看的是**鼠标**,不是激活态;输入保护由 hasFocus 承担
        //    (只有点了正文才会拿到焦点,而那时用户确实在编辑)。
        if (m_dragging || m_sliding) return;
        if (!m_autoHide || m_collapsed) return;
        // 鼠标又回到了窗口上(enterEvent 会停表,这里再兜一层,防竞态)
        if (rect().contains(mapFromGlobal(QCursor::pos()))) return;
        // 用户**正在打字**时不收起(3 秒内有过按键)。判据刻意不是 hasFocus:
        // 鼠标移开时窗口仍持有焦点(没有别的窗口被激活),用 hasFocus 会把
        // "边打字边移开鼠标"的便签永远钉在桌面上 —— 而它恰恰是该收起的。
        if (m_body && m_body->hasFocus() && QDateTime::currentMSecsSinceEpoch() - m_lastKeyMs < 3000) {
            m_collapseTimer->start();   // 过一会儿再看一次
            return;
        }
        collapse();
    });

    m_bodyTimer = new QTimer(this);
    m_bodyTimer->setSingleShot(true);
    m_bodyTimer->setInterval(800);
    connect(m_bodyTimer, &QTimer::timeout, this, &NoteWindow::commitBodyNow);

    applyState(data.note, /*restoreCollapsed=*/true);
    refreshFooter();
    refreshAffordances();
}

NoteWindow::~NoteWindow() {
    // 动画对象以 this 为 parent,析构会一并销毁;但若它正在运行,Qt 可能在销毁
    // 过程中回调 setPos → 访问已半死的窗口。显式停掉最省事。
    if (m_slide) m_slide->stop();
}

bool NoteWindow::eventFilter(QObject* obj, QEvent* e) {
    // 正文里的按键要冒泡到窗口级统计,否则"正在打字"完全观测不到
    // (QTextEdit 自己吃掉 KeyPress,窗口的 keyPressEvent 收不到)。
    if (e->type() == QEvent::KeyPress) m_lastKeyMs = QDateTime::currentMSecsSinceEpoch();
    return QWidget::eventFilter(obj, e);
}

void NoteWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    // 内容区 = 窗口去掉一圈阴影留白
    root->setContentsMargins(kNoteShadowMargin, kNoteShadowMargin,
                             kNoteShadowMargin, kNoteShadowMargin);
    root->setSpacing(0);

    // ---- 标题条:复选框 + 首行摘要(拖动热区)+ 钉住 + 关闭 ----
    auto* header = new QWidget(this);
    header->setFixedHeight(kHeaderH);
    header->setObjectName(QStringLiteral("nbNoteHeader"));
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(2, 0, 2, 0);
    headerLay->setSpacing(4);

    m_check = new QCheckBox(header);
    m_check->setObjectName(QStringLiteral("nbNoteCheck"));
    m_check->setChecked(m_done);
    m_check->setToolTip(QStringLiteral("标记完成"));
    connect(m_check, &QCheckBox::toggled, this, &NoteWindow::onDoneToggled);
    headerLay->addWidget(m_check);

    m_title = new QLabel(header);
    m_title->setObjectName(QStringLiteral("nbNoteTitle"));
    m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // 标题条是拖动热区:用 QLabel 的文本省略行为做首行摘要
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    headerLay->addWidget(m_title, 1);

    auto makeButton = [&](const QString& text, const QString& tip, const QString& name) {
        auto* button = new QToolButton(header);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(22, 20);
        headerLay->addWidget(button);
        return button;
    };
    m_pinButton = makeButton(QStringLiteral("📌"), QStringLiteral("钉住(不自动隐藏)"),
                             QStringLiteral("nbNotePin"));
    connect(m_pinButton, &QToolButton::clicked, this, [this] {
        setAutoHide(!m_autoHide);
    });
    m_closeButton = makeButton(QStringLiteral("✕"), QStringLiteral("关闭便签"),
                               QStringLiteral("nbNoteClose"));
    connect(m_closeButton, &QToolButton::clicked, this, [this] {
        flushBody();
        emit closeRequested(m_id);
    });

    root->addWidget(header);

    // ---- 正文 ----
    m_body = new QTextEdit(this);
    m_body->setObjectName(QStringLiteral("nbNoteBody"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setAcceptRichText(false);      // 数据模型是纯文本(text_content)
    m_body->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_body->setText(m_lastCommittedNote);
    m_body->installEventFilter(this);   // 统计按键(见 eventFilter)
    // ⚠️ QTextEdit 默认自带一套**英文**右键菜单(Copy/Paste/Select All),
    //    而且它会吃掉 contextMenuEvent —— 正文区右键**出不来便签菜单**,
    //    只有标题条/边距上右键才有效。改为自定义策略,统一走 showToolbar。
    m_body->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_body, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showToolbar(m_body->mapToGlobal(pos));
    });
    connect(m_body, &QTextEdit::textChanged, this, &NoteWindow::scheduleBodySave);
    root->addWidget(m_body, 1);

    // ---- 页脚:截止/逾期/提醒 ----
    m_footer = new QLabel(this);
    m_footer->setObjectName(QStringLiteral("nbNoteFooter"));
    m_footer->setFixedHeight(kFooterH);
    root->addWidget(m_footer);

    // 深色/浅色都由底板自绘负责,这里只给子控件定样式(见文件头第 3 条)
    const bool dark = isDarkPalette();
    const QString body = dark ? "#DFE1E8" : "#1B2430";
    const QString muted = dark ? "#8D99A6" : "#6B7785";
    const QString hover = dark ? "rgba(255,255,255,30)" : "rgba(0,0,0,18)";
    setStyleSheet(QStringLiteral(
        "#nbNoteTitle { color: %1; font-size: 12px; font-weight: 600; }"
        "#nbNoteBody { background: transparent; color: %2; selection-background-color: %3; }"
        "#nbNoteFooter { color: %4; font-size: 11px; }"
        "QToolButton { background: transparent; border: none; color: %4;"
        " border-radius: 4px; font-size: 11px; padding: 0; }"
        "QToolButton:hover { background: %5; }"
        "QCheckBox { spacing: 4px; }"
        "")
        .arg(dark ? "#F1F4F7" : "#17202A", body, dark ? "#4C9AFF" : "#3B82F6",
             muted, hover));

    // 字体:字号取 note.fontSize(0 = 主题默认)
    QFont bodyFont = nb::uiFont(m_fontSize > 0 ? m_fontSize : 13);
    m_body->setFont(bodyFont);
}

void NoteWindow::applyState(const QVariantMap& note, bool restoreCollapsed) {
    if (!note.isEmpty()) {
        m_edge = note.value(QStringLiteral("edge"), QStringLiteral("none")).toString();
        m_autoHide = note.value(QStringLiteral("autoHide"), false).toBool();
        m_topMost = note.value(QStringLiteral("topMost"), true).toBool();
        m_opacityPercent = note.value(QStringLiteral("opacity"), 100).toInt();
        m_fontSize = note.value(QStringLiteral("fontSize"), 0).toInt();
        m_screen = note.value(QStringLiteral("screen")).toString();
        // 上下边已从产品面下线(2026-10-19 用户裁定)。历史数据里若还存着
        // top/bottom,读入时归一成 none —— 否则窗口会按一条界面上根本没有的
        // 边去算收起几何,用户看到的是"便签自己跑到屏幕外"。
        if (m_edge != QLatin1String("none") && m_edge != QLatin1String("left")
            && m_edge != QLatin1String("right")) {
            m_edge = QStringLiteral("none");
        }
        if (restoreCollapsed) {
            m_collapsed = note.value(QStringLiteral("collapsed"), false).toBool();
        }
    }

    const int w = qMax(kNoteMinW, note.value(QStringLiteral("w"), kNoteDefaultW).toInt());
    const int h = qMax(kNoteMinH, note.value(QStringLiteral("h"), kNoteDefaultH).toInt());
    const int x = note.value(QStringLiteral("x"), 0).toInt();
    const int y = note.value(QStringLiteral("y"), 0).toInt();
    setMinimumSize(kNoteMinW + 2 * kNoteShadowMargin, kNoteMinH + 2 * kNoteShadowMargin);
    resize(w + 2 * kNoteShadowMargin, h + 2 * kNoteShadowMargin);
    move(x, y);
    if (m_screen.isEmpty()) {
        if (QScreen* s = QGuiApplication::screenAt(geometry().center())) {
            m_screen = s->name();
        }
    }
    // 恢复时一律夹回可用区:副屏被拔掉后,窗口坐标可能落在虚空中(docs/20 §6.4)
    if (!m_collapsed) clampToScreen();

    if (m_fontSize > 0) m_body->setFont(nb::uiFont(m_fontSize));
    setTopMost(m_topMost);
    applyOpacity();
    refreshFooter();
    refreshAffordances();
    // 恢复前先判一次:若这张便签正收在**屏幕交界处**,按旧数据恢复会得到一个
    // 跨在两块屏上的"半隐藏"窗口(它其实大半可见,鼠标一进就展开,再移出又收起,
    // 表现为闪烁)。这种情况直接展开并按当前屏的边重新吸附。
    if (m_collapsed && edgeOutsideIsAnotherScreen(screenForNote(), m_edge)) {
        m_collapsed = false;
        saveState({{QStringLiteral("collapsed"), false}});
    }
    if (m_collapsed) {
        // 收起态直接落到收起位置(不播动画:恢复布局时一片窗口同时滑动很吵)
        move(collapsedPosition());
    }
    // 归一后可能是"没有边却处于收起态"(历史 top/bottom 数据),落库纠正,
    // 免得下次启动又读到同一个非法组合。
    if (m_edge == QLatin1String("none") && m_collapsed) {
        m_collapsed = false;
        saveState({{QStringLiteral("edge"), QStringLiteral("none")},
                   {QStringLiteral("collapsed"), false}});
    }
}

// ---- 绘制与外观 ----

void NoteWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF plate(kNoteShadowMargin, kNoteShadowMargin,
                       width() - 2 * kNoteShadowMargin, height() - 2 * kNoteShadowMargin);

    // 阴影:自外向内叠三层低透明度圆角矩形(与贴图同一画法,近图处最深)。
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 8));
    const qreal step = qreal(kNoteShadowMargin) / 3.0;
    for (int i = 3; i >= 1; --i) {
        const qreal grow = i * step;
        painter.drawRoundedRect(plate.adjusted(-grow, -grow, grow, grow),
                                kPlateRadius, kPlateRadius);
    }

    const bool dark = isDarkPalette();
    QColor fill = plateColor(dark);
    // 已完成:整体压暗一档(与列表卡片"划线 + 0.55 透明"同一语义,但这里是底色)
    if (m_done) fill.setAlpha(dark ? 208 : 216);
    painter.setBrush(fill);
    painter.setPen(QPen(m_overdue && !m_done ? QColor(0xE5, 0x48, 0x4D, 160)
                                             : QColor(255, 255, 255, dark ? 36 : 18),
                        1));
    painter.drawRoundedRect(plate.adjusted(0.5, 0.5, -0.5, -0.5), kPlateRadius, kPlateRadius);

    // 半隐藏:露出的一条是"拉手" —— 画一枚居中短条 + 朝向屏幕内的三角箭头。
    // 颜色用 error 表示逾期未完成,其余用边框色(与 theme.md 的层次语汇一致)。
    if (m_collapsed) {
        const bool overdue = m_overdue && !m_done;
        painter.setPen(Qt::NoPen);
        painter.setBrush(overdue ? QColor(0xE5, 0x48, 0x4D) : QColor(0x8D, 0x99, 0xA6));
        const qreal cx = width() / 2.0;
        const qreal cy = height() / 2.0;
        // 只有左右两种收起方向 → 拉手恒为**竖条**(贴着屏幕侧的窄边)。
        // 横条是上下边时代的产物,已随那两个方向一起下线。
        const qreal w = 3, h = 34;
        painter.drawRoundedRect(QRectF(cx - w / 2, cy - h / 2, w, h), 1.5, 1.5);
    }
}

void NoteWindow::applyOpacity() {
    setWindowOpacity(qBound(0.3, m_opacityPercent / 100.0, 1.0));
}

void NoteWindow::setTopMost(bool on) {
    m_topMost = on;
    // ⚠️ 不用 setWindowFlag(Qt::WindowStaysOnTopHint):它会**销毁并重建原生窗口**,
    //    正在输入的便签会丢位置/焦点/编辑状态。SetWindowPos 只改 Z 序。
    // 未建原生窗口时(构造期)先不动 Z 序:showEvent 会再调一次。
    // 这里刻意**不**为了拿句柄去 winId() —— 那会在构造链上创建窗口。
#if defined(Q_OS_WIN)
    const HWND hwnd = reinterpret_cast<HWND>(m_cachedHwnd);
    if (!hwnd) return;
    ::SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    // Linux(M0b):置顶暂缓 —— 同 panel_window 的口径(X11 property M2 补;
    // Wayland 降级;不重建窗口)。
    qDebug() << "setTopMost: Linux 暂未接入置顶(M2/X11 property), on =" << on;
#endif
}

void NoteWindow::refreshAffordances() {
    // 标题条 = **标题**(docs/21):以前取"正文首行",加了备注之后首行不再等于标题。
    // 截断口径与托盘「今日待办」一致(20 字 + 省略号)。
    QString firstLine = m_lastCommittedText;
    if (firstLine.isEmpty()) firstLine = QStringLiteral("(空便签)");
    if (firstLine.size() > 20) {
        firstLine = firstLine.left(20) + QStringLiteral("…");
    }
    m_title->setText(firstLine);
    // 钉住按钮:仅当吸附在某条边(且不是下边)时才有意义 —— 没吸附时"不自动隐藏"
    // 是恒真的,按钮亮着会让人以为点了没反应。
    const bool atJunction = m_edge != QLatin1String("none")
                            && edgeOutsideIsAnotherScreen(screenForNote(), m_edge);
    const bool canAutoHide = m_edge != QLatin1String("none") && !atJunction;
    m_pinButton->setEnabled(canAutoHide);
    m_pinButton->setText(m_autoHide ? QStringLiteral("📌") : QStringLiteral("📍"));
    m_pinButton->setToolTip(
        canAutoHide
            ? (m_autoHide ? QStringLiteral("已钉住:鼠标移开不收起")
                          : QStringLiteral("贴边自动收起(点击钉住)"))
            : (atJunction ? QStringLiteral("这块屏的这一侧紧接着另一块屏,交界处不做自动收起")
                          : QStringLiteral("先贴到屏幕边缘,才能自动收起")));
    update();
}

void NoteWindow::refreshFooter() {
    QStringList parts;
    if (m_dueAt > 0) {
        const QDateTime due = QDateTime::fromMSecsSinceEpoch(m_dueAt);
        if (m_done) {
            parts << QStringLiteral("已完成");
        } else if (m_overdue) {
            parts << QStringLiteral("已逾期 · %1").arg(due.toString(QStringLiteral("MM-dd HH:mm")));
        } else if (due.date() == QDate::currentDate()) {
            parts << QStringLiteral("今天 %1 截止").arg(due.toString(QStringLiteral("HH:mm")));
        } else {
            parts << QStringLiteral("截止 %1").arg(due.toString(QStringLiteral("MM-dd HH:mm")));
        }
    } else if (m_done) {
        parts << QStringLiteral("已完成");
    }
    m_footer->setText(parts.join(QStringLiteral("   ")));
    m_footer->setVisible(!parts.isEmpty());

    // 逾期用 error 色,其余用 muted(颜色见 styleSheet 的 footer 段)
    const bool overdue = m_overdue && !m_done;
    const bool dark = isDarkPalette();
    m_footer->setStyleSheet(overdue
                                ? QStringLiteral("color: %1; font-size: 11px;")
                                      .arg(dark ? "#FF8A8F" : "#C0272D")
                                : QString());
    if (!overdue) {
        m_footer->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                    .arg(dark ? "#8D99A6" : "#6B7785"));
    }

    // 正文与标题随完成态变淡/划线(与列表卡片的已完成视觉一致)
    QFont titleFont = m_title->font();
    titleFont.setStrikeOut(m_done);
    m_title->setFont(titleFont);
    m_title->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
                               .arg(m_done ? (dark ? "#77828F" : "#8A95A1")
                                           : (dark ? "#F1F4F7" : "#17202A")));
    m_body->setStyleSheet(QStringLiteral("background: transparent; color: %1;")
                              .arg(m_done ? (dark ? "#9AA4B0" : "#5E6873")
                                          : (dark ? "#DFE1E8" : "#1B2430")));
    if (m_check) {
        const QSignalBlocker blocker(m_check);
        m_check->setChecked(m_done);
    }
}

// ---- 几何 / 吸附 / 半隐藏 ----

/// 窗口的"归属点":用来判断这张便签**属于哪块屏**。
///
/// 收起态窗口有一大半在屏幕外,中心点已经不属于任何屏。
/// ⚠️ 必须取**留在屏内**的那一端,而不是"贴边那一端"(2026-10-19 修,用户实测):
///    原实现按 edge 取的是贴边侧的坐标 —— 左贴时取 geo.left()+4,而收起后窗口
///    是**往左推出屏幕**的,于是这个点落在左邻屏上;多屏相邻时便签会在两块屏
///    之间来回认主(每认一次就按新屏的边重算收起几何),表现为反复横跳。
///    正确语义:贴左边 → 屏内那一端是**右**边;贴右边 → 屏内那一端是**左**边。
static QPoint anchorFor(const QRect& geo, bool collapsed, const QString& edge) {
    if (!collapsed) return geo.center();
    if (edge == QLatin1String("left")) return QPoint(geo.right() - 4, geo.center().y());
    if (edge == QLatin1String("right")) return QPoint(geo.left() + 4, geo.center().y());
    // 上/下边已从产品面下线(见 docs/20 §6.2 修订),这里保留镜像逻辑只为兼容
    // 历史数据里可能还存着 edge=top/bottom 的便签 —— 读到时按"屏内那一端"纠正。
    if (edge == QLatin1String("top")) return QPoint(geo.center().x(), geo.bottom() - 4);
    if (edge == QLatin1String("bottom")) return QPoint(geo.center().x(), geo.top() + 4);
    return geo.center();
}

/// 这张便签**属于哪块屏**。
///
/// 优先级(按"谁更可信"排):
///   ① 展开态:窗口中心在哪块屏,就属于哪块屏 —— 位置是权威,屏名只是记忆;
///   ② 收起态:**记忆的屏名优先**。收起后窗口被**故意**推到屏外,几何不再能代表
///      "用户把它贴在哪块屏",此时拿几何去找屏必然认错(多屏相邻时尤其致命:
///      一认错就按新屏的边重算收起几何,于是两块屏之间反复横跳)。
///   ③ 兜底:光标屏 → 主屏。
QScreen* NoteWindow::screenForNote() const {
    if (m_collapsed) {
        for (QScreen* candidate : QGuiApplication::screens()) {
            if (!m_screen.isEmpty() && candidate->name() == m_screen) return candidate;
        }
        // 记忆的屏没了(副屏被拔):退到锚点,再退到光标屏
        if (QScreen* byAnchor = QGuiApplication::screenAt(
                anchorFor(geometry(), true, m_edge))) {
            return byAnchor;
        }
        if (QScreen* byCursor = QGuiApplication::screenAt(QCursor::pos())) return byCursor;
        return QGuiApplication::primaryScreen();
    }
    if (QScreen* here = QGuiApplication::screenAt(geometry().center())) return here;
    for (QScreen* candidate : QGuiApplication::screens()) {
        if (!m_screen.isEmpty() && candidate->name() == m_screen) return candidate;
    }
    if (QScreen* byCursor = QGuiApplication::screenAt(QCursor::pos())) return byCursor;
    return QGuiApplication::primaryScreen();
}

bool NoteWindow::edgeOutsideIsAnotherScreen(QScreen* screen, const QString& edge) const {
    if (!screen) return false;
    const QRect avail = screen->availableGeometry();
    // 取样点:贴边那条线上、纵向取窗口中点(纵向落在窗外也算 —— 只要邻屏在这一侧
    // 有像素,就说明这一侧不是桌面外缘)。
    const int y = avail.top() + avail.height() / 2;
    QPoint probe;
    if (edge == QLatin1String("left")) {
        probe = QPoint(avail.left() - 2, y);
    } else if (edge == QLatin1String("right")) {
        probe = QPoint(avail.right() + 2, y);
    } else {
        return false;
    }
    // ⚠️ 用 screenAt 而不是"遍历所有屏的矩形 contains":
    //    screenAt 走的是系统口径(含缩放/排列),自己比矩形容易在混合 DPI 下错位。
    // ⚠️ 命中的是自己 → 说明这条边外侧仍是自己(不该发生),按"不是连接处"处理。
    QScreen* other = QGuiApplication::screenAt(probe);
    return other != nullptr && other != screen;
}

QRect NoteWindow::availableRect() const {
    // ⚠️ 用 availableGeometry 而不是 geometry:后者含任务栏区域,
    //    贴上去会被任务栏盖住(docs/20 §6.2 第 1 条)。
    //
    // ⚠️ 归属屏只看**便签所在的那一块**,不看虚拟桌面:边缘 = 当前屏幕的边缘,
    //    不是所有显示器拼起来的外轮廓。用户实测的"贴到一边后自动收起来回横跳"
    //    就出在这条上 —— 详见 screenForNote 的注释。
    QScreen* screen = screenForNote();
    return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

QRect NoteWindow::virtualBounds() const {
    // 虚拟桌面(所有屏可用区的外接矩形):拖动时用它夹取,而不是单屏可用区 ——
    // 单屏夹取等于"把窗口焊死在这一块屏上"。
    QRect bounds;
    for (QScreen* screen : QGuiApplication::screens())
        bounds = bounds.united(screen->availableGeometry());
    if (bounds.isNull()) bounds = QRect(0, 0, 1920, 1080);
    return bounds;
}

void NoteWindow::adoptScreenUnderWindow() {
    // ⚠️ **收起态不改归属屏**(2026-10-19 修,用户实测"两块屏之间来回横跳")。
    //    收起是程序把窗口推到屏外的动作,不是用户把便签挪了屏 —— 这时按几何
    //    重新认主必然认到邻屏,下一次展开/收起就按邻屏的边算,于是来回跳。
    //    归属屏只在两种时刻更新:用户拖动/缩放结束、屏幕布局变化。
    if (m_collapsed) return;
    QScreen* screen = QGuiApplication::screenAt(geometry().center());
    if (!screen) return;
    if (screen->name() != m_screen) {
        m_screen = screen->name();
        qInfo() << "note-screen 便签换屏 ->" << m_screen;
    }
}

void NoteWindow::clampToScreen() {
    const QRect avail = availableRect();
    QPoint pos = m_collapsed ? collapsedPosition() : this->pos();
    pos.setX(qBound(avail.left() - kNoteShadowMargin,
                    pos.x(),
                    qMax(avail.left(), avail.right() - width() + kNoteShadowMargin + 1)));
    pos.setY(qBound(avail.top() - kNoteShadowMargin,
                    pos.y(),
                    qMax(avail.top(), avail.bottom() - height() + kNoteShadowMargin + 1)));
    move(pos);
}

QPoint NoteWindow::expandedPosition() const {
    const QRect avail = availableRect();
    QPoint pos = this->pos();
    // 吸附边决定哪一侧对齐(用户可能刚改了 edge 而窗口还没挪)。
    // 只有左右:上下边已从产品面下线,走到这里说明数据异常 —— 保持原位即可,
    // 不再按一条界面上没有的边去摆窗口(那正是"便签跑到屏幕外"的成因之一)。
    if (m_edge == QLatin1String("left")) {
        pos.setX(avail.left() - kNoteShadowMargin);
    } else if (m_edge == QLatin1String("right")) {
        pos.setX(avail.right() - width() + kNoteShadowMargin + 1);
    }
    return pos;
}

QPoint NoteWindow::collapsedPosition() const {
    const QRect avail = availableRect();
    QPoint pos = expandedPosition();
    // 只露 kNoteSliver + 阴影留白 的一条在屏内(docs/20 §6.3):
    // 收起后窗口仍有一截**在屏内**是硬要求 —— 否则鼠标进不来,永远收不回来。
    const int visible = kNoteSliver + kNoteShadowMargin;
    if (m_edge == QLatin1String("left")) {
        pos.setX(avail.left() - width() + visible);
    } else if (m_edge == QLatin1String("right")) {
        pos.setX(avail.right() - visible + 1);
    }
    // 只有左右可收起(collapse() 已挡住其余情况),这里不再有纵向分支。
    return pos;
}

void NoteWindow::startSlide(const QPoint& target, bool animate) {
    // ⚠️ 必须**独占一条**动画:两个 QPropertyAnimation 同时驱动同一个 "pos"
    //    属性会互相覆盖 —— 前一条按自己的缓动曲线继续写到结束,最后落在它自己的
    //    终点上。实测表现是"鼠标进入拉手后窗口滑到一半又弹回去/停在半路",
    //    而且时快时慢(取决于两次动画的时间差),极难复现。
    //    收起/展开在吸附动画期间被反复触发(鼠标正好停在动画路径上)是常态,
    //    所以这不是理论问题。
    if (m_slide) {
        // 先断开再 stop:stop() 会 emit finished,而 finished 的槽会把 m_sliding
        // 置回 false —— 紧接着我们要开始新动画,状态会被这条旧信号污染。
        m_slide->disconnect(this);
        m_slide->stop();
        m_slide->deleteLater();
        m_slide = nullptr;
    }
    if (!animate) {
        m_sliding = false;
        move(target);
        return;
    }
    if (pos() == target) {
        m_sliding = false;
        return;
    }
    m_sliding = true;
    // 140ms OutCubic(theme.md §31 的 micro/normal 档):足够看出"滑",又不拖沓
    m_slide = new QPropertyAnimation(this, "pos", this);
    m_slide->setDuration(140);
    m_slide->setEasingCurve(QEasingCurve::OutCubic);
    m_slide->setStartValue(pos());
    m_slide->setEndValue(target);
    connect(m_slide, &QPropertyAnimation::finished, this, [this] { m_sliding = false; });
    m_slide->start();
}

void NoteWindow::expand(bool animate) {
    if (!m_collapsed) return;
    m_collapseTimer->stop();
    m_collapsed = false;
    saveState({{QStringLiteral("collapsed"), false}});
    startSlide(expandedPosition(), animate);
    refreshAffordances();
}

void NoteWindow::collapse(bool animate, bool force) {
    if (m_collapsed) return;
    if (!m_autoHide && !force) return;
    // 屏幕连接处**不做半隐藏**(2026-10-19 用户实测:连接处贴边会闪烁且收不回去)。
    // 理由见 edgeOutsideIsAnotherScreen:这里往边外推不是"藏起来",而是"推到邻屏上",
    // 鼠标随即又落回窗口 → expand → 弹回 → leave → collapse → 死循环。
    if (edgeOutsideIsAnotherScreen(screenForNote(), m_edge)) {
        // 用户可能刚把它拖到连接处,而库里还留着 autoHide=true —— 明确告知一次,
        // 否则"勾了自动收起却没反应"会被当成坏功能。
        if (!m_junctionNoticed) {
            m_junctionNoticed = true;
            showTransientHint(QStringLiteral("屏幕交界处不做自动收起"));
        }
        return;
    }
    // 没贴边就没有"收起方向"可言 —— 强行收起只会把窗口挪到一个说不通的位置,
    // 这条即使 force 也保留。(上下边已下线,left/right 是唯一可收起的两种。)
    if (m_edge == QLatin1String("none")) return;
    m_collapsed = true;
    saveState({{QStringLiteral("collapsed"), true}});
    startSlide(collapsedPosition(), animate);
    refreshAffordances();
}

void NoteWindow::setAutoHide(bool on) {
    if (m_autoHide == on) return;
    // 屏幕连接处开启自动收起没有意义(见 collapse):明确拒绝并说明,
    // 而不是让用户勾上之后"什么都不发生"。
    if (on && edgeOutsideIsAnotherScreen(screenForNote(), m_edge)) {
        showTransientHint(QStringLiteral("屏幕交界处不做自动收起"));
        refreshAffordances();
        return;
    }
    m_autoHide = on;
    if (!on && m_collapsed) expand();
    saveState({{QStringLiteral("autoHide"), on}});
    refreshAffordances();
    emit stateChanged(m_id);
    // 开启时鼠标往往**本来就在窗口外**(用户是右键菜单点的):那一刻不会再有
    // leaveEvent,如果只依赖 Enter/Leave,便签会一直摊着不动 —— 用户看到的是
    // "勾了自动收起但毫无反应"。这里补一次计时。
    if (on && !m_collapsed && m_edge != QLatin1String("none")
        && !rect().contains(mapFromGlobal(QCursor::pos()))) {
        m_collapseTimer->start();
    }
}

bool NoteWindow::snapAfterMove() {
    const QRect avail = availableRect();
    const QRect geo = geometry();
    // 只判**左右**两条边(2026-10-19 按用户实测裁定下线上下边):
    //   上边是标题栏/全屏区,贴上去之后几乎无法稳定触发(松手位置稍高就把窗口
    //   顶出可用区);下边是任务栏,半隐藏会与任务栏自动隐藏抢鼠标。
    //   实际使用以左右为主,只留左右也让"贴边"这一动作有唯一确定的方向。
    const int dLeft = qAbs(geo.left() - avail.left());
    const int dRight = qAbs(geo.right() - avail.right());

    int best = kNoteSnapPx + 1;
    QString edge = QStringLiteral("none");
    for (const auto& [distance, name] :
         {qMakePair(dLeft, QStringLiteral("left")),
          qMakePair(dRight, QStringLiteral("right"))}) {
        if (distance <= kNoteSnapPx && distance < best) {
            best = distance;
            edge = name;
        }
    }
    if (edge == QLatin1String("none")) {
        // 没落在左右边上:若此前贴的是上下边(历史数据),这里一并解除 ——
        // 产品面已无这两个入口,留着只会让状态机带着一个不可达分支跑。
        if (m_edge == QLatin1String("none")) return false;
        m_edge = QStringLiteral("none");
        m_collapsed = false;
        m_collapseTimer->stop();
        saveState({{QStringLiteral("edge"), QStringLiteral("none")},
                   {QStringLiteral("collapsed"), false}});
        refreshAffordances();
        return true;
    }

    m_edge = edge;
    // 吸附后同时夹回可用区:避免"贴了右边却有一半在屏幕外"。
    // ⚠️ 纵向也必须夹 —— 纯左右吸附时纵向是用户自己摆的,他可能把便签拖到了
    //    屏幕上下方之外(多屏竖向排列时尤其容易),夹一下才能贴边即完整可见。
    QPoint target = pos();
    if (edge == QLatin1String("left")) target.setX(avail.left() - kNoteShadowMargin);
    if (edge == QLatin1String("right")) {
        target.setX(avail.right() - width() + kNoteShadowMargin + 1);
    }
    target.setY(qBound(avail.top() - kNoteShadowMargin, target.y(),
                       qMax(avail.top(), avail.bottom() - height() + kNoteShadowMargin + 1)));
    move(target);
    // 若吸附到的这条边**外侧是另一块屏**(屏幕交界处),主动关掉自动收起并告知:
    // 半隐藏在那里不成立(推出去 = 推到邻屏上,见 edgeOutsideIsAnotherScreen),
    // 留着 autoHide=1 会变成"勾着却永不收起"的假状态。
    if (edgeOutsideIsAnotherScreen(screenForNote(), edge)) {
        if (m_autoHide || m_collapsed) {
            m_autoHide = false;
            m_collapsed = false;
            showTransientHint(QStringLiteral("屏幕交界处不做自动收起"));
        }
    }
    saveState({{QStringLiteral("edge"), edge},
               {QStringLiteral("autoHide"), m_autoHide},
               {QStringLiteral("collapsed"), m_collapsed},
               {QStringLiteral("topMost"), m_topMost}});
    refreshAffordances();
    emit stateChanged(m_id);
    return true;
}

void NoteWindow::setEdge(const QString& edge) {
    // 上下边已下线(2026-10-19 用户裁定):菜单不再出现,历史数据里残留的
    // top/bottom 在读入时会被归一成 none(见 applyState),这里也兜一层,
    // 免得任何调用方把不可达状态写回库里。
    QString want = edge;
    if (want != QLatin1String("none") && want != QLatin1String("left")
        && want != QLatin1String("right")) {
        want = QStringLiteral("none");
    }
    // 取消贴边时**必须先退出收起态**:收起位置的几何是按边算的,没有边还停在
    // 收起位置 = 窗口留在屏幕外,鼠标永远进不来(便签就此消失)。
    // 库侧 normalized() 也会纠正这个组合,这里同步纠正内存态。
    if (want == QLatin1String("none") && m_collapsed) {
        m_collapsed = false;
        saveState({{QStringLiteral("collapsed"), false}});
    }
    m_edge = want;
    saveState({{QStringLiteral("edge"), want}});
    // 贴边会改窗口位置:几何必须一起落库,否则重启回到旧位置 + 旧 edge 自相矛盾。
    // 先确认归属屏:saveState 会把 m_screen 一起写下去,顺序上先 adopt 能保证存的是
    // "窗口此刻真在的那块屏"(展开态才 adopt,收起态见 adoptScreenUnderWindow)。
    if (!m_collapsed) adoptScreenUnderWindow();
    // 显式贴边也可能贴到屏幕交界处:那里不做半隐藏(见 edgeOutsideIsAnotherScreen)
    if (want != QLatin1String("none")
        && edgeOutsideIsAnotherScreen(screenForNote(), want)) {
        if (m_autoHide || m_collapsed) {
            m_autoHide = false;
            m_collapsed = false;
            saveState({{QStringLiteral("autoHide"), false},
                       {QStringLiteral("collapsed"), false}});
            showTransientHint(QStringLiteral("屏幕交界处不做自动收起"));
        }
    }
    startSlide(expandedPosition(), true);
    saveGeometry();
    refreshAffordances();
    emit stateChanged(m_id);
}

void NoteWindow::saveGeometry() {
    // 先确认归属屏:saveState 会把 m_screen 一起写下去(它自己也会补一次),
    // 顺序上先 adopt 能保证存的是"窗口此刻真在的那块屏",而不是建窗时那块。
    adoptScreenUnderWindow();
    saveState({{QStringLiteral("x"), pos().x()},
               {QStringLiteral("y"), pos().y()},
               {QStringLiteral("w"), width() - 2 * kNoteShadowMargin},
               {QStringLiteral("h"), height() - 2 * kNoteShadowMargin}});
}

void NoteWindow::saveState(const QVariantMap& patch) {
    QVariantMap req = patch;
    req.insert(QStringLiteral("id"), m_id);
    if (!m_screen.isEmpty()) req.insert(QStringLiteral("screen"), m_screen);
    // 走 Core 单例的同步 JSON 门面(本地 SQLite,毫秒级)
    const QVariantMap resp = CoreFacade::instance().call(QStringLiteral("todo_note_save"), req);
    if (!resp.value(QStringLiteral("ok")).toBool()) {
        qWarning() << "便签状态保存失败:" << m_id
                   << resp.value(QStringLiteral("error")).toMap()
                          .value(QStringLiteral("message")).toString();
    }
}

qint64 NoteWindow::memoryBytes() const {
    // 与贴图同一口径(只算图片会低估一半,docs/06 §4):窗口后备缓冲
    // = 物理尺寸 ×4;文本引擎/字体缓存的增量按 0.5MB 估(实测这个量级)。
    const qreal dpr = devicePixelRatioF();
    const qint64 backing =
        static_cast<qint64>(qMax(1, width())) * qMax(1, height()) * dpr * dpr * 4;
    return backing + 512 * 1024;
}

// ---- 交互:拖动 / 缩放 / 菜单 / 键盘 ----

void NoteWindow::beginDrag(const QPoint& globalPos) {
    m_dragging = true;
    m_collapseTimer->stop();
    m_dragOffset = globalPos - pos();
}

void NoteWindow::dragTo(const QPoint& globalPos) {
    if (!m_dragging) return;
    // 拖动中若是收起态,先展开(用户已经在操作它了,不能再藏一半)
    if (m_collapsed) {
        m_collapsed = false;
        saveState({{QStringLiteral("collapsed"), false}});
        refreshAffordances();
    }
    // 跟手移动时就要夹住:不夹的话用户能把窗口拖到屏幕外一半,松手后既看不到
    // 也点不到(而吸附判定也永远够不到边缘 —— 边界恰好是"贴住"的那一格)。
    //
    // ⚠️ 夹取范围是**虚拟桌面**而不是"当前屏可用区"(2026-10-18 修):
    //    单屏夹取会让窗口拖到屏幕边界就停住,多屏机上永远去不了副屏。
    const QRect bounds = virtualBounds();
    QPoint next = globalPos - m_dragOffset;
    next.setX(qBound(bounds.left() - kNoteShadowMargin, next.x(),
                     qMax(bounds.left(), bounds.right() - width() + kNoteShadowMargin + 1)));
    next.setY(qBound(bounds.top() - kNoteShadowMargin, next.y(),
                     qMax(bounds.top(), bounds.bottom() - height() + kNoteShadowMargin + 1)));
    move(next);
    // 拖动过程中就更新归属屏:吸附判定与收起几何都按"当前所在屏"算,
    // 拖到副屏松手时要贴的是**副屏**的边。
    adoptScreenUnderWindow();
}

void NoteWindow::endDrag() {
    if (!m_dragging) return;
    m_dragging = false;
    // ⚠️ 落库**必须在吸附之后无条件执行**(2026-10-18 修)。
    //    原实现是 `if (!snapped) saveGeometry();` —— 吸附成功那条路径只写了
    //    edge,没写 x/y,于是"拖到左边吸附 → 重启"会回到**上一次存过几何的位置**,
    //    而 edge=left 还在 → 窗口自己瞬移到左边。用户看到的是"位置记不住/乱跳"。
    snapAfterMove();
    adoptScreenUnderWindow();
    saveGeometry();
    // 拖动结束后按新位置决定是否重新计时自动收起
    if (m_autoHide && !m_collapsed) m_collapseTimer->start();
}

void NoteWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
#if defined(Q_OS_WIN)
        // 缩放把手:四边/四角(无边框窗口没有非客户区,自己算命中)
        const int hit = framelessResizeHitTest(this, reinterpret_cast<HWND>(winId()),
                                               [&] {
                                                   const QPoint p = e->globalPosition().toPoint();
                                                   return MAKELPARAM(p.x(), p.y());
                                               }());
        if (hit != -1) {
            // 交给系统做缩放(WM_NCLBUTTONDOWN),松手后由 nativeEvent 收尾
            ::ReleaseCapture();
            ::SendMessageW(reinterpret_cast<HWND>(winId()), WM_NCLBUTTONDOWN, hit, 0);
            e->accept();
            return;
        }
#else
        // Linux(M0b):边缘缩放走 QWindow::startSystemResize —— 命中判据与
        // Windows 的 framelessResizeHitTest 同一套(6 逻辑像素边框),
        // 但 Qt 版本在本地面坐标系,无 MAKELPARAM/物理坐标换算。
        // 松手收尾:Windows 由 WM_EXITSIZEMOVE 收尾;Linux 的系统缩放结束后
        // 由 Resize 事件自然落位,snap 在下一次拖动/重夹时补偿(M0b 可接受)。
        if (!isMaximized()) {
            const int b = 6;
            const QPoint p = e->position().toPoint();
            Qt::Edges edges;
            if (p.x() < b) edges |= Qt::LeftEdge;
            if (p.x() >= width() - b) edges |= Qt::RightEdge;
            if (p.y() < b) edges |= Qt::TopEdge;
            if (p.y() >= height() - b) edges |= Qt::BottomEdge;
            if (edges != 0 && windowHandle() && windowHandle()->startSystemResize(edges)) {
                e->accept();
                return;
            }
        }
#endif
        // 标题条 = 拖动热区;正文区不拖动(否则选字/点击都会被当成拖窗口)
        if (e->position().y() < kNoteShadowMargin + kHeaderH) {
            beginDrag(e->globalPosition().toPoint());
            activateWindow();
            e->accept();
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void NoteWindow::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging) {
        dragTo(e->globalPosition().toPoint());
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void NoteWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging) {
        endDrag();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void NoteWindow::enterEvent(QEnterEvent* e) {
    // 排查"收不回来/滑不出去"时先看这两行(便签只有几张,不需要按需开关):
    // 它们能直接区分"Enter 没到"与"到了但状态机没动"。
    qDebug() << "note-enter" << m_id.left(8) << "collapsed:" << m_collapsed
             << "geo:" << geometry();
    m_collapseTimer->stop();
    // 收起态:鼠标进入拉手立即滑回(docs/20 §6.3)
    if (m_collapsed) expand();
    QWidget::enterEvent(e);
}

bool NoteWindow::isCursorOverWindow() const {
    // ⚠️ 用**窗口矩形**而不是 rect():rect() 是窗口内坐标,窗口在屏外时
    //    mapFromGlobal 仍能算出值,但含义容易读错。这里直接比全局坐标,意图明确。
    return frameGeometry().contains(QCursor::pos());
}

void NoteWindow::leaveEvent(QEvent* e) {
    qDebug() << "note-leave" << m_id.left(8) << "collapsed:" << m_collapsed
             << "cursorOverWindow:" << isCursorOverWindow();
    // ⚠️ 抖振保护(2026-10-19):滑入/滑出动画会让窗口在鼠标底下移动,
    //    Windows 于是连发 leave→enter→leave…,每次 leave 都起一遍计时器,
    //    动画一结束就收起、刚收起又展开 —— 用户看到的是**闪烁**。
    //    只在"光标真的不在窗口上"时才起表。
    if (m_autoHide && !m_collapsed && !m_dragging && !isCursorOverWindow()) {
        m_collapseTimer->start();
    }
    QWidget::leaveEvent(e);
}

void NoteWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    applyRoundedCorners(this);
    setTopMost(m_topMost);
    logMemory("note-open");
    if (m_collapsed) move(collapsedPosition());
}

void NoteWindow::keyPressEvent(QKeyEvent* e) {
    m_lastKeyMs = QDateTime::currentMSecsSinceEpoch();
    if (e->key() == Qt::Key_Escape) {
        // ESC 的语义:输入中先退出输入焦点,否则关闭便签 —— 与贴图(ESC 直接关)
        // 的差异是刻意的:便签上很可能正在打字。
        if (m_body && m_body->hasFocus()) {
            flushBody();
            setFocus(Qt::OtherFocusReason);
            return;
        }
        flushBody();
        emit closeRequested(m_id);
        return;
    }
    // Ctrl+S:立即落库(与全局习惯一致)
    if (e->key() == Qt::Key_S && (e->modifiers() & Qt::ControlModifier)) {
        commitBodyNow();
        return;
    }
    QWidget::keyPressEvent(e);
}

void NoteWindow::contextMenuEvent(QContextMenuEvent* e) {
    showToolbar(e->globalPos());
    e->accept();
}

void NoteWindow::showToolbar(const QPoint& globalPos) {
    if (!m_toolbar) {
        // ⚠️ 不要动 QMenu 的 windowFlags:它是 Qt::Popup,额外加
        //    FramelessWindowHint / NoDropShadowWindowHint 会打乱弹层的抓取与
        //    关闭语义(实测表现为点了外部不关、或菜单闪一下就消失)。
        //    要贴合主题只需 QSS —— 与托盘菜单同一做法(见 tray.cpp:83)。
        m_toolbar = new QMenu(this);
        m_toolbar->setObjectName(QStringLiteral("nbNoteMenu"));
        m_toolbar->setFont(nb::uiFont(kTrayMenuFontPixelSize));
        m_toolbar->setStyleSheet(QStringLiteral(
            "QMenu#nbNoteMenu, QMenu#nbNoteSubMenu"
            " { background: %1; color: %2; border: 1px solid %3; padding: 2px; }"
            "QMenu#nbNoteMenu::item, QMenu#nbNoteSubMenu::item"
            " { padding: 4px 8px; border-radius: 4px; }"
            "QMenu#nbNoteMenu::item:selected, QMenu#nbNoteSubMenu::item:selected"
            " { background: %4; color: %2; }"
            "QMenu#nbNoteMenu::item:disabled, QMenu#nbNoteSubMenu::item:disabled"
            " { color: %5; }"
            "QMenu#nbNoteMenu::separator, QMenu#nbNoteSubMenu::separator"
            " { height: 1px; background: %3; margin: 4px 8px; }"
            // 勾选列:与托盘菜单不同,这里有可勾选项(setCheckable),必须让出列宽
            // 并给出可见的勾选标记,否则"置顶/自动收起"的当前状态看不出来。
            "QMenu#nbNoteMenu::indicator, QMenu#nbNoteSubMenu::indicator"
            " { width: 14px; height: 14px; }")
            .arg(isDarkPalette() ? "#1A2028" : "#FFFFFF",
                 isDarkPalette() ? "#DFE1E8" : "#17202A",
                 isDarkPalette() ? "#2C343F" : "#D9E0E7",
                 isDarkPalette() ? "rgba(255,255,255,28)" : "#E8F1FF",
                 isDarkPalette() ? "#6B7785" : "#B6BEC8"));
    }
    // 每次弹出重建:菜单里大量条目带**当前状态**(置顶/自动收起/贴边/不透明度),
    // 复用同一批 QAction 就得逐条 setChecked,漏一条就显示错状态;
    // 重建的代价只是一次 QMenu::clear() + 十余个 QAction。
    m_toolbar->clear();

    const auto addSub = [this](const QString& title) {
        auto* sub = new QMenu(title, m_toolbar);
        sub->setObjectName(QStringLiteral("nbNoteSubMenu"));
        sub->setFont(m_toolbar->font());
        m_toolbar->addMenu(sub);
        return sub;
    };

    // ---- 组 0:内容(改的是待办本身,不是窗口外观)----
    m_toolbar->addAction(QStringLiteral("编辑待办…"), this, [this] {
        // 截止/提醒/标签的完整控件只在面板的编辑卡片里实现一份,便签不重复造
        emit editRequested(m_id);
    });
    m_toolbar->addAction(m_done ? QStringLiteral("取消完成") : QStringLiteral("标记完成"),
                         this, [this] { onDoneToggled(!m_done); });
    // 复制:标题 + 备注(docs/21)。只复制标题会丢细节,只复制备注又没有署名行 ——
    // 两条都有时用空行连接,与用户手写便签的形态一致。
    m_toolbar->addAction(QStringLiteral("复制内容"), this, [this] {
        flushBody();
        QString text = m_lastCommittedText;
        if (!m_lastCommittedNote.isEmpty()) {
            text = text.isEmpty() ? m_lastCommittedNote
                                  : text + QStringLiteral("\n\n") + m_lastCommittedNote;
        }
        // 经核心写入(带打开重试),理由同 pin_window::copyToClipboard:
        // Qt 直写在剪贴板被占用时静默失败,而这里紧接着会提示"已复制内容"。
        const bool ok = CoreBridge::instance().copyTextToClipboard(text);
        if (!ok) {
            showTransientHint(QStringLiteral("复制失败:剪贴板被占用,请重试"));
            return;
        }
        // 提示必须落在便签上:主面板可能隐藏着,应用级 toast 看不见
        // (与贴图 showNotice 同一取舍,见 pin_window.cpp 的注释)。
        showTransientHint(QStringLiteral("已复制内容"));
    });

    // ---- 组 1:窗口行为 ----
    m_toolbar->addSeparator();
    QAction* const topAction = m_toolbar->addAction(QStringLiteral("置顶"));
    topAction->setCheckable(true);
    topAction->setChecked(m_topMost);
    connect(topAction, &QAction::toggled, this, [this](bool on) {
        setTopMost(on);
        saveState({{QStringLiteral("topMost"), m_topMost}});
        emit stateChanged(m_id);
    });

    // 自动收起只在吸附后才有意义(未贴边时"不自动隐藏"恒真,勾选框会误导)
    const bool atJunction = m_edge != QLatin1String("none")
                            && edgeOutsideIsAnotherScreen(screenForNote(), m_edge);
    const bool canAutoHide = m_edge != QLatin1String("none") && !atJunction;
    QAction* const autoAction = m_toolbar->addAction(QStringLiteral("贴边自动收起"));
    autoAction->setCheckable(true);
    autoAction->setChecked(m_autoHide);
    autoAction->setEnabled(canAutoHide);
    if (!canAutoHide) {
        autoAction->setToolTip(atJunction
            ? QStringLiteral("这块屏的这一侧紧接着另一块屏,交界处不做自动收起")
            : QStringLiteral("先贴到屏幕左/右边缘,才能自动收起"));
    }
    connect(autoAction, &QAction::toggled, this, [this](bool on) { setAutoHide(on); });

    // 贴边:显式入口。用户往往先想到"贴左/贴右",而不是"拖到边上"——
    // 只靠拖动 12px 的阈值,在小屏上很容易拖过头而没吸附,会被当成"贴边坏了"。
    QMenu* const edgeMenu = addSub(QStringLiteral("贴到屏幕边缘"));
    // ⚠️ 只有左右(2026-10-19 用户裁定):上下边在实际使用中无法稳定触发
    //    (上边是标题栏/全屏区,下边是任务栏),留着只会让人点了没反应。
    const struct { const char* id; const char* label; } edges[] = {
        {"left", "贴左边"}, {"right", "贴右边"},
    };
    for (const auto& e : edges) {
        const QString id = QString::fromLatin1(e.id);
        QAction* const a = edgeMenu->addAction(QString::fromUtf8(e.label));
        a->setCheckable(true);
        a->setChecked(m_edge == id);
        connect(a, &QAction::triggered, this, [this, id] { setEdge(id); });
    }
    edgeMenu->addSeparator();
    QAction* const detach = edgeMenu->addAction(QStringLiteral("取消贴边"));
    detach->setEnabled(m_edge != QLatin1String("none"));
    connect(detach, &QAction::triggered, this, [this] { setEdge(QStringLiteral("none")); });

    QMenu* const opacityMenu = addSub(QStringLiteral("不透明度"));
    const int levels[] = {100, 85, 70, 60};
    for (int level : levels) {
        QAction* const a = opacityMenu->addAction(QStringLiteral("%1%").arg(level));
        a->setCheckable(true);
        a->setChecked(m_opacityPercent == level);
        connect(a, &QAction::triggered, this, [this, level] { setOpacityPercent(level); });
    }

    // ---- 组 2:关闭(危险项置底,与列表右键菜单同一排布)----
    m_toolbar->addSeparator();
    m_toolbar->addAction(QStringLiteral("关闭便签"), this, [this] {
        flushBody();
        emit closeRequested(m_id);
    });

    // 夹进屏幕可用区:贴着屏幕右/下角右键时,菜单会跑出屏幕外点不到
    QPoint pos = globalPos;
    const QRect avail = availableRect();
    const QSize hint = m_toolbar->sizeHint();
    pos.setX(qBound(avail.left(), pos.x(), qMax(avail.left(), avail.right() - hint.width() + 1)));
    pos.setY(qBound(avail.top(), pos.y(), qMax(avail.top(), avail.bottom() - hint.height() + 1)));
    // 弹出前先拿到焦点:菜单是 Qt::Popup,失焦会自动关闭;便签是 WA_ShowWithoutActivating,
    // 不先激活会让"右键菜单刚出来就被自己判成外部点击"。
    activateWindow();
    m_toolbar->popup(pos);
}

void NoteWindow::showTransientHint(const QString& text) {
    // 便签自己的瞬时提示:主面板往往隐藏着,CoreFacade 的 toast 看不见。
    // 复用页脚标签而不是新建浮层 —— 便签很小,再叠一层会盖住正文。
    if (!m_footer) return;
    m_footer->setText(text);
    m_footer->setVisible(true);
    QTimer::singleShot(1600, this, [this] {
        // 提示结束后恢复真实页脚(可能已被外部同步改过,故重算而不是存快照)
        refreshFooter();
    });
}

void NoteWindow::setOpacityPercent(int percent) {
    m_opacityPercent = qBound(30, percent, 100);
    applyOpacity();
    saveState({{QStringLiteral("opacity"), m_opacityPercent}});
}

// ---- 内容:完成态 / 正文写回 / 外部同步 ----

void NoteWindow::onDoneToggled(bool done) {
    m_done = done;
    if (m_done) m_overdue = false;
    refreshFooter();
    refreshAffordances();
    update();
    const QVariantMap resp = CoreFacade::instance().call(
        QStringLiteral("todo_set_done"),
        {{QStringLiteral("id"), m_id}, {QStringLiteral("done"), m_done}});
    if (!resp.value(QStringLiteral("ok")).toBool()) {
        qWarning() << "便签完成态写入失败:" << m_id;
    }
    emit stateChanged(m_id);
}

void NoteWindow::scheduleBodySave() {
    if (m_suppressBodyDirty) return;
    m_bodyTimer->start();
}

void NoteWindow::commitBodyNow() {
    m_bodyTimer->stop();
    if (!m_body) return;
    const QString note = normalizeNoteText(m_body->toPlainText());
    if (note == m_lastCommittedNote) return;
    // ⚠️ 备注**允许清空**:它是补充说明,删光就是"没有备注",不像标题那样是必填项。
    //    所以这里不做"空值恢复"——那是标题的规则(docs/21)。
    //    下发空串而不是缺键:core 的三态里缺键 = 不改,根本清不掉。
    const QVariantMap resp = CoreFacade::instance().call(
        QStringLiteral("todo_update"),
        {{QStringLiteral("id"), m_id}, {QStringLiteral("noteText"), note}});
    if (!resp.value(QStringLiteral("ok")).toBool()) {
        qWarning() << "便签备注写入失败:" << m_id
                   << resp.value(QStringLiteral("error")).toMap()
                          .value(QStringLiteral("message")).toString();
        return;
    }
    m_lastCommittedNote = note;
    emit stateChanged(m_id);
}

void NoteWindow::flushBody() { commitBodyNow(); }

void NoteWindow::applyExternalState(const QString& title, const QString& note, bool done,
                                    qint64 dueAt, bool overdue) {
    // 标题:只影响标题条(不碰正文框,避免把光标打到行首)。
    const QString normalizedTitle = normalizeNoteText(title);
    if (normalizedTitle != m_lastCommittedText) {
        m_lastCommittedText = normalizedTitle;
        refreshAffordances();
    }
    // 备注:正文区的主人。只在**真的不一致**时才回灌,否则每次列表刷新都会把
    // 光标打到行首(docs/20 §7.3 与 R5)。
    const QString normalizedNote = normalizeNoteText(note);
    if (m_bodyTimer && m_bodyTimer->isActive()
        && normalizedNote != m_lastCommittedNote) {
        // 用户正在便签里打字(去抖窗口内还没落库):这一刻**不回灌**。
        // 回灌会把这几个字直接冲掉,而用户根本没机会看到发生了什么。
        // 本地改动会在去抖到期时落库,"最后写入者获胜"—— 与用户预期一致。
        qDebug() << "note-sync 跳过回灌(本地有未落库的编辑)" << m_id.left(8);
    } else if (normalizedNote != m_lastCommittedNote) {
        m_lastCommittedNote = normalizedNote;
        if (m_body) {
            const int cursor = m_body->textCursor().position();
            m_suppressBodyDirty = true;
            m_body->setPlainText(normalizedNote);
            m_suppressBodyDirty = false;
            QTextCursor c = m_body->textCursor();
            c.setPosition(qMin(cursor, normalizedNote.size()));
            m_body->setTextCursor(c);
        }
    }
    if (m_done != done) {
        m_done = done;
        refreshFooter();
        refreshAffordances();
        update();
    }
    if (m_dueAt != dueAt || m_overdue != overdue) {
        m_dueAt = dueAt;
        m_overdue = overdue;
        refreshFooter();
        update();
    }
}

bool NoteWindow::event(QEvent* e) {
    // 跨屏/系统缩放变化:重算逻辑尺寸并夹回新屏可用区(与贴图同一坑:
    // Windows 会在 WM_DPICHANGED 里原生调几何,必须延后一拍再摆)。
    const QEvent::Type type = e->type();
    if (type == QEvent::WinIdChange) {
        // 句柄就绪/重建都走这里:缓存下来供 nativeEvent 用(见那里的警告)
        m_cachedHwnd = reinterpret_cast<void*>(winId());
    } else if (type == QEvent::DevicePixelRatioChange
               || type == QEvent::ScreenChangeInternal) {
        QTimer::singleShot(0, this, [this] {
            clampToScreen();
            saveGeometry();
        });
    } else if (type == QEvent::WindowDeactivate) {
        // 失焦即落库:用户点别处后不该还留一份"未写回"的正文
        commitBodyNow();
        // 失焦时鼠标可能**已经在窗口外**(输入时移开的情形):那一刻 leaveEvent 被
        // hasFocus 保护挡掉了,不会再触发第二次,所以这里补一次收起计时 ——
        // 否则"边打字边把鼠标移开"的便签会一直摊在桌面上。
        if (m_autoHide && !m_collapsed && !m_dragging
            && !rect().contains(mapFromGlobal(QCursor::pos()))) {
            m_collapseTimer->start();
        }
    } else if (type == QEvent::Hide) {
        commitBodyNow();
    }
    return QWidget::event(e);
}

bool NoteWindow::nativeEvent(const QByteArray& eventType, void* message, qint64* result) {
#if !defined(Q_OS_WIN)
    // Linux(M0b):WM_EXITSIZEMOVE 收尾(系统缩放后夹回+吸附)是 Windows 路径 ——
    // Linux 的系统缩放经 startSystemResize,落位由 Resize 事件自然完成;
    // snap 补偿口径见 mousePressEvent 的 Linux 分支注释。
    Q_UNUSED(message);
    Q_UNUSED(result);
    return QWidget::nativeEvent(eventType, message, result);
#else
    if (eventType != "windows_generic_MSG") {
        return QWidget::nativeEvent(eventType, message, result);
    }
    auto* msg = static_cast<MSG*>(message);
    // ⚠️ 这里**不能**调 winId():窗口创建期间(WM_NCCREATE 等)调用会递归触发
    //    CreateWindowEx → 创建失败,日志刷 "Failed to create platform window"
    //    且窗口永远建不出来。用缓存句柄(panel_window.cpp 的同一教训)。
    const HWND self = reinterpret_cast<HWND>(m_cachedHwnd);
    // 缩放结束/移动结束:落库(系统缩放不走 mouseReleaseEvent)
    if (self && msg->hwnd == self && (msg->message == WM_EXITSIZEMOVE)) {
        QTimer::singleShot(0, this, [this] {
            if (!m_dragging) {
                // 系统缩放(WM_NCLBUTTONDOWN 进入的循环)结束后同样要夹回 + 吸附
                snapAfterMove();
                saveGeometry();
            }
        });
    }
    return QWidget::nativeEvent(eventType, message, result);
#endif  // Q_OS_WIN
}

void NoteWindow::reclampToCurrentScreen() {
    // 先按可见锚点找屏:窗口若还在某块现存屏上,就属于那块屏(用户可能刚把它
    // 拖过去);找不到再退回光标屏/主屏 —— 顺序要紧,反过来会把跨屏窗口全拉回主屏。
    // ⚠️ 用 anchorFor 而不是 geometry().center():收起态的中心在屏幕外。
    QScreen* screen = QGuiApplication::screenAt(anchorFor(geometry(), m_collapsed, m_edge));
    if (!screen) screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) m_screen = screen->name();
    if (m_collapsed) {
        move(collapsedPosition());
    } else {
        clampToScreen();
        if (m_edge != QLatin1String("none")) move(expandedPosition());
    }
    saveGeometry();
}

} // namespace nb
