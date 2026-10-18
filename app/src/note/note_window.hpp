// 待办贴屏(桌面便签)窗口(docs/20-todo-sticky-note.md §5/§6)。
//
// 与贴图(PinWindow)同构但**不是同一个类**:PinWindow 的中心是一张 QImage
// (旋转/编辑回写/文本位图渲染),而那些对便签全是死代码;真正的复用是**机制** ——
// 无边框透明底 + 自绘阴影 + 8 向缩放命中 + DPR 变化延迟重算 + 顶层弹层菜单。
//
// 窗体 = 纯 Widgets(不用 QQuickWidget):便签可能同时开好几张,每张再挂一个
// QML 场景/RHI 上下文的代价与收益不匹配;且 QTextEdit 的 IME(中文输入法)行为
// 在置顶多窗口场景下比 QML TextEdit 稳。
#pragma once

#include <QDateTime>
#include <QPoint>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class QCheckBox;
class QMenu;
class QPropertyAnimation;
class QLabel;
class QTextEdit;
class QTimer;
class QToolButton;

namespace nb {

/// 一条便签的初始数据(todo_note_list 的一行)。
struct NoteData {
    QString id;
    /// 标题(text_content):便签标题条与"复制全文"用的署名行。
    QString text;
    /// 备注(noteText):正文区内容,可多行、可空(docs/21)。
    /// ⚠️ 字段名不能叫 note —— 下面已有 `QVariantMap note`(窗口状态),
    ///    两者含义完全不同,重名直接编译失败(C2371)。
    QString noteText;
    bool done = false;
    /// 截止时刻(unix 毫秒);0 = 无截止。
    qint64 dueAt = 0;
    bool overdue = false;
    /// note 对象原样转交(见 docs/20 §4 的字段表)。
    QVariantMap note;
};

class NoteWindow : public QWidget {
    Q_OBJECT
public:
    explicit NoteWindow(const NoteData& data, QWidget* parent = nullptr);
    ~NoteWindow() override;

    QString noteId() const { return m_id; }
    /// 外部(面板/托盘/到点提醒)把最新内容同步进窗口。
    /// 标题与备注分别比对(都规范化),避免自己刚写完又回灌导致光标跳动。
    void applyExternalState(const QString& title, const QString& note, bool done,
                            qint64 dueAt, bool overdue);

    /// 展开(若处于半隐藏态则滑回)。
    void expand(bool animate = true);
    /// 收起为半隐藏。默认只在 autoHide 且已吸附(非下边)时生效;
    /// force = true 时忽略 autoHide(托盘「全部收起」这类显式指令用 ——
    /// 用户点了"收起"却毫无反应,比不支持收起更费解)。
    void collapse(bool animate = true, bool force = false);
    bool isCollapsed() const { return m_collapsed; }
    bool isAutoHide() const { return m_autoHide; }
    void setAutoHide(bool on);
    /// 把未落库的正文改动立刻写盘(关窗/隐藏前用)。
    void flushBody();

    /// 屏幕增删/分辨率变化后重新定位:丢弃记录的屏名,按**当前几何**重新找屏
    /// 并夹回可用区(拔掉副屏后,窗口不该停在坐标 (3000, 200) 的虚空里)。
    void reclampToCurrentScreen();

    /// 这张便签的真实内存占用(与贴图同一口径:窗口后备缓冲 + 文本引擎估算)。
    qint64 memoryBytes() const;

signals:
    /// 用户点 ✕ / 菜单「关闭便签」:注册表负责落库 visible=0 并销毁窗口。
    void closeRequested(const QString& id);
    /// 状态(完成态/置顶/自动隐藏)变过,注册表据此刷新托盘与其它视图。
    void stateChanged(const QString& id);
    /// 右键「编辑待办…」:完整编辑(截止/标签)复用面板的待办编辑卡片 ——
    /// 便签只做正文,截止/提醒的复杂控件不在这里再实现一遍。
    void editRequested(const QString& id);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qint64* result) override;

private:
    void buildUi();
    void applyState(const QVariantMap& note, bool restoreCollapsed);
    void saveState(const QVariantMap& patch);
    void setTopMost(bool on);
    void applyOpacity();
    void refreshAffordances();
    void refreshFooter();
    void beginDrag(const QPoint& globalPos);
    void dragTo(const QPoint& globalPos);
    void endDrag();
    /// 松手后的吸附判定(docs/20 §6.2);返回是否吸附到了某条边。
    bool snapAfterMove();
    QPoint collapsedPosition() const;
    QPoint expandedPosition() const;
    /// 窗口**当前所在屏**的可用区。位置优先于记忆的屏名(见实现处注释)。
    QRect availableRect() const;
    /// 虚拟桌面(所有屏可用区的外接矩形):拖动夹取用 —— 单屏夹取等于把窗口
    /// 焊死在那一块屏上,多屏机上永远拖不过去。
    QRect virtualBounds() const;
    /// 按窗口当前位置更新 m_screen(位置是权威,屏名只是记忆)。
    void adoptScreenUnderWindow();
    void clampToScreen();
    void startSlide(const QPoint& target, bool animate);
    void saveGeometry();
    void scheduleBodySave();
    /// 立即把正文区写回**备注**(noteText)。
    void commitBodyNow();
    void onDoneToggled(bool done);
    void showToolbar(const QPoint& globalPos);
    /// 便签自己的瞬时提示(约 1.6s):主面板常常隐藏着,应用级 toast 看不见,
    /// 故复用页脚标签 —— 便签很小,再叠一层浮层会盖住正文。
    void showTransientHint(const QString& text);
    void setEdge(const QString& edge);
    void setOpacityPercent(int percent);

    QString m_id;
    bool m_done = false;
    qint64 m_dueAt = 0;
    bool m_overdue = false;

    // ---- note 状态(与 todo_note 表一一对应)----
    QString m_edge = QStringLiteral("none");
    bool m_autoHide = false;
    bool m_collapsed = false;
    bool m_topMost = true;
    int m_opacityPercent = 100;
    int m_fontSize = 0;
    QString m_screen;

    // ---- 交互中的临时状态 ----
    bool m_dragging = false;
    QPoint m_dragOffset;
    bool m_sliding = false;
    /// 当前滑入/滑出动画(**同一时刻只能有一条**,见 startSlide 的注释)。
    QPropertyAnimation* m_slide = nullptr;
    QTimer* m_collapseTimer = nullptr;   ///< 鼠标离开后的自动收起计时
    QTimer* m_bodyTimer = nullptr;       ///< 正文写库去抖(800ms)
    bool m_suppressBodyDirty = false;    ///< 程序化回填正文时抑制"变脏"
    /// 已落库的**标题**(规范化后):与正文编辑解耦,去重/回灌判断用它。
    QString m_lastCommittedText;
    /// 已落库的**备注**(规范化后):正文区编辑对象(docs/21)。
    QString m_lastCommittedNote;
    /// 最近一次按键的时刻(msecs)。自动收起用它判"用户还在打字吗"。
    /// 只认按键而不是 hasFocus:光标移开窗口时窗口**仍持有焦点**(没有别的窗口被
    /// 激活),用 hasFocus 判会让便签永远收不起来。
    qint64 m_lastKeyMs = 0;
    QMenu* m_toolbar = nullptr;          ///< 右键菜单(顶层弹层)
    /// nativeEvent 用的 HWND 缓存(以 void* 持有,头文件不引 windows.h)。
    /// ⚠️ **绝不能在 nativeEvent 里调 winId()**:窗口创建期间会递归触发
    ///    CreateWindowEx → 创建失败,日志刷满 "Failed to create platform window"
    ///    且窗口永远出不来(panel_window.cpp 为此写过同一条警告)。用缓存值。
    void* m_cachedHwnd = nullptr;

    // ---- 子控件 ----
    QCheckBox* m_check = nullptr;
    QLabel* m_title = nullptr;
    QTextEdit* m_body = nullptr;
    QLabel* m_footer = nullptr;
    QToolButton* m_pinButton = nullptr;   ///< 钉住(不自动隐藏)
    QToolButton* m_closeButton = nullptr;
};

/// 正文规范化:统一换行 + 去掉**尾部**空白(只 trim 尾部 —— 用户正在输入的前导
/// 空格不该被吃掉)。回灌判断必须与写库同一口径,否则"刚打完字又被外部值覆盖"。
QString normalizeNoteText(const QString& text);

constexpr int kNoteShadowMargin = 8;
constexpr int kNoteDefaultW = 280;
constexpr int kNoteDefaultH = 220;
constexpr int kNoteMinW = 180;
constexpr int kNoteMinH = 120;
/// 吸附判定阈值(逻辑像素):松手时距可用区边缘 ≤ 此值即吸附。
constexpr int kNoteSnapPx = 12;
/// 半隐藏时露在屏幕内的拉手宽度(逻辑像素)。
constexpr int kNoteSliver = 8;

} // namespace nb
