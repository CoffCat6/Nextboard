#include "ui/ocr_result_panel.hpp"

#include "host/core_bridge.hpp"
#include "panel/core_facade.hpp"
#include "ui_theme.hpp"

#include <QApplication>
#include <QtMath>
#include <QClipboard>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextEdit>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

namespace nb {
namespace {

/// 面板定位:贴在**选区**旁边,而不是贴屏幕。
///
/// ## 缺陷背景(用户报的)
///
/// 之前 anchor 传的是遮罩窗口 —— 而遮罩是**整屏**窗口,frameGeometry() 就是整块屏,
/// 于是"贴锚点右下角"算出来是屏幕右下角,再被夹进可用区,面板就跑到屏幕边角,
/// 离用户刚框的选区十万八千里。
///
/// ## 摆位规则
///
/// 优先放选区**右下方外侧**(不挡刚截的内容),依次退让:
/// 右下 → 右上方 → 左下方 → 左上方 → 最后夹进屏内并允许与选区重叠。
/// 选区下方放不下时先试上方而不是直接贴屏幕底边 —— 保持"离选区近"这一要点。
QPoint placeNextTo(const QSize& panelSize, const QRect& anchor) {
    const QPoint aCenter = anchor.center();
    QScreen* screen = QGuiApplication::screenAt(aCenter);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry()
                               : QRect(QPoint(0, 0), QSize(1920, 1080));
    constexpr int kGap = 8;   // 与选区之间的缝

    // 候选:优先"右下(紧贴选区右下角外侧)",其次右侧贴顶/左侧贴底/左侧贴顶
    QList<QPoint> candidates;
    candidates << QPoint(anchor.right() + kGap, anchor.bottom() + kGap);   // 右下方
    candidates << QPoint(anchor.right() + kGap, anchor.bottom() - panelSize.height() + 1
                                              - kGap);                     // 右侧、底对齐
    candidates << QPoint(anchor.left() - panelSize.width() - kGap,
                         anchor.bottom() + kGap);                          // 左下方
    candidates << QPoint(anchor.left() - panelSize.width() - kGap,
                         anchor.bottom() - panelSize.height() + 1 - kGap); // 左侧、底对齐

    const auto fits = [&](const QPoint& p) {
        return QRect(p, panelSize).right() <= avail.right() - 4
               && QRect(p, panelSize).bottom() <= avail.bottom() - 4
               && p.x() >= avail.left() + 4 && p.y() >= avail.top() + 4;
    };
    for (const QPoint& p : candidates) {
        if (fits(p)) return p;
    }

    // 四个候选都放不下(选区几乎占满屏):贴选区右下角,允许压住选区边缘内容 ——
    // 但必须完整落在屏内,否则用户点不到「关闭」。
    QPoint p(anchor.right() - panelSize.width() + 1, anchor.bottom() + kGap);
    if (p.y() + panelSize.height() > avail.bottom())
        p.setY(anchor.bottom() - panelSize.height() + 1);   // 放进选区内部下侧
    p.setX(qBound(avail.left() + 4, p.x(), qMax(avail.left() + 4, avail.right() - panelSize.width() - 4)));
    p.setY(qBound(avail.top() + 4, p.y(), qMax(avail.top() + 4, avail.bottom() - panelSize.height() - 4)));
    return p;
}

} // namespace

OcrResultPanel::OcrResultPanel(const QString& text, const QString& reason,
                               const QImage& source, bool popup,
                               const QVector<OcrLineBox>& lines)
    : QFrame(nullptr, popup ? (Qt::Popup | Qt::FramelessWindowHint)
                            : (Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)),
      m_source(source),
      m_lines(lines) {
    setObjectName(QStringLiteral("ocrResultPanel"));
    setFrameShape(QFrame::NoFrame);
    // 顶层窗口要透明底,自绘圆角才不会被直角盖住(与贴图工具条同手法)。
    // ⚠️ 透明底 + stylesheet background 是**不生效**的组合:QFrame 不自绘 stylesheet
    //    背景(需 WA_StyledBackground),结果整块全透 —— 底板必须由 paintEvent 自己画。
    setAttribute(Qt::WA_TranslucentBackground);
    // popup 模式点外部即关;tool 模式必须显式关闭 —— 用 WA_DeleteOnClose 保证
    // 无论哪条关闭路径都不会留下一个不可见但常驻的窗口。
    setAttribute(Qt::WA_DeleteOnClose);
    if (!popup) setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);

    auto* layout = new QVBoxLayout(this);
    // 12/10 + 4px 底板内缩(见 paintEvent),视觉边距才是 12/10。
    // 上边距给标题行留位,所以比其它三边小一点。
    layout->setContentsMargins(16, 8, 16, 14);
    layout->setSpacing(8);

    // ---- 标题行 = 拖动把手 ----
    //
    // 用户要求"取字后的面板可以拖动"。无边框窗口默认没有标题栏,得自己给一个:
    //  - 它同时是**视觉暗示**(一条浅色横纹 + "识别结果"字样 + 抓手光标),
    //    没有这个暗示,用户不会想到面板能拖;
    //  - 它是**事件落点**:标题行自己不处理鼠标 → 冒泡到面板的 mousePressEvent
    //    (见头文件里对拖拽实现的说明)。
    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("ocrHeader"));
    m_header->setCursor(Qt::SizeAllCursor);   // 四向箭头 = "这个区域能拖"
    m_header->setFixedHeight(22);
    auto* headerLay = new QHBoxLayout(m_header);
    headerLay->setContentsMargins(0, 0, 0, 0);
    headerLay->setSpacing(6);
    auto* grip = new QWidget(m_header);
    grip->setObjectName(QStringLiteral("ocrGrip"));
    grip->setFixedSize(18, 3);      // 一道短横纹:足够暗示"把手",又不喧宾夺主
    headerLay->addWidget(grip, 0, Qt::AlignVCenter);
    auto* headerText = new QLabel(QStringLiteral("识别结果"), m_header);
    headerText->setObjectName(QStringLiteral("ocrHeaderText"));
    headerLay->addWidget(headerText, 0, Qt::AlignVCenter);
    headerLay->addStretch(1);
    layout->addWidget(m_header);

    // 正文用**只读 QTextEdit** 而不是 QLabel。
    //
    // 缺陷背景(用户报的):原先用 QLabel + setMaximumHeight(420),一屏放不下的
    // 识别结果被**直接裁掉**,既看不到也拿不到 —— 取字恰恰常常来自长段落/整页文字。
    //
    // 选 QTextEdit 而不是 QLabel+QScrollArea:它自带文本选择、软换行、键盘滚动
    // (PgUp/PgDn/Home/End),这三样都是"读长文本"的刚需,自己拼要写一堆。
    // 只读 + NoTextInteraction 之外保留 TextSelectableByMouse|ByKeyboard,
    // 用户仍可只挑其中几行复制。
    m_body = new QTextEdit(this);
    m_body->setObjectName(QStringLiteral("ocrText"));
    m_body->setReadOnly(true);
    m_body->setUndoRedoEnabled(false);
    m_body->setLineWrapMode(QTextEdit::WidgetWidth);
    m_body->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_body->setFrameShape(QFrame::NoFrame);       // 边框由底板承担,内框多余
    m_body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // 只纵向滚:横向靠换行
    m_body->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 纯文本:识别结果里的 <b> 之类不该被当标记解析
    m_body->setPlainText(text.isEmpty() ? reason : text);
    // 宽度固定:换行位置与"内容高度"的测量都依赖同一个宽度,浮动宽度会让两者脱节
    // (先按宽度 X 量出高度、再按宽度 Y 布局,就会出现"本该滚动却裁掉一行")。
    m_body->setFixedWidth(m_bodyWidth);
    layout->addWidget(m_body);

    if (!reason.isEmpty() && !text.isEmpty()) {
        m_status = new QLabel(reason, this);
        m_status->setObjectName(QStringLiteral("ocrStatus"));
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
    }

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    buttons->addStretch(1);

    // 「打开链接」:整个结果就是一个 URL 时给直达入口 —— 二维码场景的主诉求
    // (识别 → 复制 → 再开浏览器是三步,这里省到一步);OCR 识别出的纯链接
    // 同样受益。判定取"整段不含空白的 http(s) 地址":混在长文本里的链接不判,
    // 避免"打开的是半截 URL"。打开走 CoreFacade::openExternalUrl —— http(s)
    // 白名单与验收模式禁浏览器都在那一层,这里不重复做。
    const QString trimmed = text.trimmed();
    const bool isPureUrl =
        !trimmed.isEmpty() && !trimmed.contains(QRegularExpression(QStringLiteral("\\s")))
        && (trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
            || trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive));
    if (isPureUrl) {
        auto* openBtn = new QPushButton(QStringLiteral("打开链接"), this);
        openBtn->setObjectName(QStringLiteral("ocrOpenUrl"));
        openBtn->setCursor(Qt::PointingHandCursor);
        connect(openBtn, &QPushButton::clicked, this, [trimmed] {
            CoreFacade::instance().openExternalUrl(trimmed);
        });
        buttons->addWidget(openBtn);
    }

    // 「再次复制」:识别成功时文本已在剪贴板,但用户可能中途复制了别的东西
    // (面板是驻留的,期间去别处 Ctrl+C 很常见),给一个不用重跑的补拷入口。
    if (!text.isEmpty()) {
        auto* copyBtn = new QPushButton(QStringLiteral("再次复制"), this);
        copyBtn->setObjectName(QStringLiteral("ocrCopy"));
        copyBtn->setCursor(Qt::PointingHandCursor);
        connect(copyBtn, &QPushButton::clicked, this, [this, text] {
            // 经核心写:核心的 clipboard_win 带打开重试,别的进程占用时
            // Qt 剪贴板会静默失败(与浮条取字同一通道)。
            if (CoreBridge::instance().copyTextToClipboard(text)) emit copiedToClipboard();
        });
        buttons->addWidget(copyBtn);
    }

    m_closeBtn = new QPushButton(
        text.isEmpty() ? QStringLiteral("关闭") : QStringLiteral("关闭"), this);
    m_closeBtn->setObjectName(QStringLiteral("ocrClose"));
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::close);
    buttons->addWidget(m_closeBtn);
    layout->addLayout(buttons);

    // ---- 图中取字:行级交互 ----
    //
    // 事件装在 **viewport** 而不是 QTextEdit 本身:QTextEdit 把鼠标事件交给
    // 内部的 QWidget(QAbstractScrollArea::viewport),装在 m_body 上收不到 move/leave。
    // 行模型 = 文档 block:纯文本 setPlainText 按 \n 分块,块号即行号
    // (软换行不产生新块,与"识别行"口径一致)。
    if (m_body && !m_lines.isEmpty()) {
        m_body->viewport()->installEventFilter(this);
        m_body->setMouseTracking(true);          // 不按键也能收到 move(悬停高亮)
        m_body->viewport()->setMouseTracking(true);
    }
    buildPreview();

    applyTheme();
    fitBodyToContent();
    adjustSize();
}

int OcrResultPanel::effectiveMaxBodyHeight() const {
    // 上限 = min(调用方给的上限, 屏可用高度的 55%)。
    // 留出 45% 给状态行、按钮行、边距与阴影 —— 正文独占过多会把窗口顶出屏幕。
    //
    // ⚠️ 屏幕来自 m_anchorScreen 而**不是** geometry():构造期窗口未定位,
    //    geometry() 给的是 (0,0) 附近的默认值,反查会拿到主屏(多屏时即错屏)。
    QScreen* screen = m_anchorScreen;
    if (!screen) screen = QGuiApplication::primaryScreen();
    const int availH = screen ? screen->availableGeometry().height() : 1080;
    return qBound(120, qMin(m_maxBodyHeight, availH * 55 / 100), 2000);
}

void OcrResultPanel::setAnchorScreen(QScreen* screen) {
    m_anchorScreen = screen;
    if (m_body) fitBodyToContent();
}

void OcrResultPanel::fitBodyToContent() {
    // 高度 = min(内容按**目标宽度**换行后需要的高度, 上限);不足一行按一行兜底。
    //
    // ## 两个坑(都实测踩过)
    //
    // ① **不能用 sizeHint**:只读 QTextEdit 的 sizeHint 是个与内容无关的常数,
    //    短文本也会撑成一大块空白。
    // ② **不能在 widget 布局前读 documentLayout()->documentSize()**:
    //    构造期 viewport 宽度还是默认值,换行结果与最终布局差很远 ——
    //    实测 120 行文本被算成 113px 高(应为封顶值),短文本反而溢出出现滚动条。
    //
    // 所以:显式给 document 设 textWidth 再读 size()。QTextDocument 的排版是
    // **同步**的,setTextWidth 之后 size() 立即就是"按该宽度换行后的真实尺寸",
    // 与 widget 是否已显示无关。
    const int scrollbarReserve = 12;   // 滚动条占位:不预留的话长文本必然横向溢出
    const int wrapWidth = qMax(80, m_bodyWidth - scrollbarReserve);
    m_body->document()->setTextWidth(wrapWidth);
    const int needed = qCeil(m_body->document()->size().height())
                       + 2 * 2;   // documentMargin 默认 4 上下各一,这里用保守值
    const int capped = qMin(needed, effectiveMaxBodyHeight());
    // 至少容纳一行(文本为空、只有 reason 时也不该塌成一条缝)
    const int oneLine = QFontMetrics(m_body->font()).lineSpacing() + 6;
    m_body->setFixedHeight(qMax(oneLine, capped));
}

bool OcrResultPanel::bodyScrollable() const {
    if (!m_body) return false;
    // 判据用"内容是否需要滚动"而不是"滚动条是否可见":
    // 显式设置的滚动条策略在第一次布局前可能还没生效,拿 isVisible 会有一帧的假阴性。
    return m_body->verticalScrollBar()->maximum() > 0;
}

bool OcrResultPanel::bodyLastLineReachable() const {
    if (!m_body) return false;
    QScrollBar* bar = m_body->verticalScrollBar();
    if (!bar) return false;
    // 滚到底,再看文档最后一个可视块的底边是否落在 viewport 内。
    // ⚠️ 这里真的改滚动位置(而不是算数):算数等于把 QTextEdit 的排版逻辑
    //    在测试里再实现一遍,而这正是要验的东西。
    bar->setValue(bar->maximum());
    QTextCursor end = m_body->textCursor();
    end.movePosition(QTextCursor::End);
    const QRect lastRect = m_body->cursorRect(end);   // 相对 viewport
    const QRect viewportRect = m_body->viewport()->rect();
    return viewportRect.contains(lastRect.center());
}

void OcrResultPanel::setMaxBodyHeight(int px) {
    m_maxBodyHeight = qMax(80, px);
    if (m_body) fitBodyToContent();
}

void OcrResultPanel::applyTheme() {
    // 与 QML 侧 ThemedButton / Popup 同语言(docs/theme.md §24/§33):
    //   surface 底 + border 描边 + 8px 圆角;正文 textPrimary、状态 textSecondary;
    //   按钮常态 surfaceHover、悬停再深/浅一档。
    const ui::ThemeTokens& t = ui::themeTokens();
    const QColor btnHover = ui::isDarkTheme() ? t.surfaceSelected : t.surfaceHover.darker(104);
    // ⚠️ 这里**只管子控件**。底板(surface + border + 圆角)在 paintEvent 里画 ——
    //    见那里的说明:透明顶层窗口上,stylesheet 的 QFrame 背景不会生效。
    // 正文是 QTextEdit(可滚动),它的配色分两块:
    //   - 文字色走 palette / stylesheet,但**不能**给它 background:底板由 paintEvent 画,
    //     QTextEdit 再画一层不透明底会把圆角边缘盖成直角;
    //   - 滚动条必须显式主题化 —— 默认样式在深色主题下是一条亮白槽,极其刺眼。
    // 滚动条宽度 8px、无箭头(箭头在这么窄的条上没有可用性,纯噪音)。
    setStyleSheet(QStringLiteral(
        "QTextEdit#ocrText{color:%1;font-size:13px;background:transparent;"
        "border:none;padding:0;} "
        "QLabel#ocrStatus{color:%2;font-size:12px;background:transparent;} "
        "QPushButton{background:%3;color:%1;border:1px solid %4;border-radius:5px;"
        "padding:6px 12px;font-size:13px;} "
        "QPushButton:hover{background:%5;} "
        // 标题行:弱化但可辨 —— 它是"能拖"的唯一视觉线索,不能太抢眼
        "QWidget#ocrHeader{background:transparent;} "
        "QWidget#ocrGrip{background:%8;border-radius:1px;} "
        "QLabel#ocrHeaderText{color:%2;font-size:11px;background:transparent;} "
        "QScrollBar:vertical{background:transparent;width:8px;margin:2px 0 2px 4px;"
        "border:none;} "
        "QScrollBar::handle:vertical{background:%6;border-radius:3px;min-height:24px;} "
        "QScrollBar::handle:vertical:hover{background:%7;} "
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;border:none;} "
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}")
        .arg(t.textPrimary.name(), t.textSecondary.name(), t.surfaceHover.name(),
             t.border.name(), btnHover.name(),
             ui::withAlpha(t.textTertiary, 150).name(QColor::HexArgb),
             ui::withAlpha(t.textSecondary, 210).name(QColor::HexArgb),
             // 抓手横纹:比标题文字更弱,只做"结构提示"
             ui::withAlpha(t.textTertiary, 170).name(QColor::HexArgb)));
}

// ---- 图中取字:行级交互 ----

namespace {

/// 预览浮窗里源图的**放大倍数**:行框太小时(单行 12~40px 高)直接显示看不清;
/// 统一放大到 4 倍封顶、宽 240 起步 —— "看清这一行"够了,再大就成了第二块面板。
constexpr int kPreviewMinWidth = 240;
constexpr int kPreviewMaxScale = 4;

} // namespace

/// 悬停预览浮窗:一个小型无框顶层窗口,内容 = 源图该行区域(放大) + 说明行。
/// 独立顶层而不是面板子控件:它要**盖在面板外**(贴行旁边),子控件画不出父窗口边界。
void OcrResultPanel::buildPreview() {
    m_preview = new QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
    m_preview->setObjectName(QStringLiteral("ocrLinePreview"));
    auto* lay = new QVBoxLayout(m_preview);
    lay->setContentsMargins(1, 1, 1, 1);
    lay->setSpacing(0);
    m_previewImage = new QLabel(m_preview);
    m_previewImage->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_previewImage);
    m_previewCaption = new QLabel(m_preview);
    m_previewCaption->setObjectName(QStringLiteral("ocrLinePreviewCaption"));
    m_previewCaption->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_previewCaption);
    // 浮窗配色跟面板底板走(themeTokens 与 paintEvent 同源),风格不漂移。
    const ui::ThemeTokens& t = ui::themeTokens();
    m_preview->setStyleSheet(
        QStringLiteral("#ocrLinePreview{background:%1;border:1px solid %2;}")
            .arg(t.surface.name(), ui::withAlpha(t.border, 230).name()));
    m_previewCaption->setStyleSheet(
        QStringLiteral("color:%1;font-size:11px;padding:2px 6px 3px;background:transparent;")
            .arg(t.textSecondary.name()));
}

void OcrResultPanel::hidePreview() {
    m_hoverLine = -1;
    m_previewAnchorGlobal = QRect();
    if (m_preview) m_preview->hide();
}

QRect OcrResultPanel::sourceRectForLine(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= m_lines.size()) return QRect();
    return m_lines.at(lineIndex).rect;
}

bool OcrResultPanel::lineCopyable(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= m_lines.size()) return false;
    return !m_lines.at(lineIndex).text.trimmed().isEmpty();
}

int OcrResultPanel::lineIndexForDocumentPosition(int position) const {
    if (!m_body || position < 0) return -1;
    QTextCursor c = m_body->textCursor();
    c.setPosition(qBound(0, position, m_body->document()->characterCount() - 1));
    return c.blockNumber();   // 纯文本:block 号 == \n 拆分的行号
}

QPoint OcrResultPanel::previewTopLeftForLine(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= m_lines.size() || m_source.isNull()) return QPoint();
    // 锚 = 该行文字在**面板全局坐标**下的可视矩形(cursorRect 口径,含滚动换行)。
    if (m_previewAnchorGlobal.isEmpty()) return QPoint();
    const QSize size = m_preview ? m_preview->size() : QSize(kPreviewMinWidth, 80);
    // 默认放锚点**右侧** 12px;放不下(屏幕右缘)退到左侧。
    QPoint p(m_previewAnchorGlobal.right() + 12, m_previewAnchorGlobal.top());
    QScreen* screen = QGuiApplication::screenAt(p);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        if (p.x() + size.width() > avail.right() - 4) {
            p.setX(m_previewAnchorGlobal.left() - size.width() - 12);
        }
        p.setY(qBound(avail.top() + 4, p.y(),
                      qMax(avail.top() + 4, avail.bottom() - size.height() - 4)));
    }
    return p;
}

/// 悬停行 → 显示预览浮窗(内容 = 源图该行区域放大)。
/// 每次都整窗重建内容:行切换频繁,局部更新(QPixmap 原位换)反而要处理大小抖动。
void OcrResultPanel::showPreviewForLine(int lineIndex) {
    if (!m_preview || !m_previewImage || !m_previewCaption) return;
    if (lineIndex < 0 || lineIndex >= m_lines.size() || m_source.isNull()) {
        hidePreview();
        return;
    }
    const QRect srcRect = m_lines.at(lineIndex).rect;
    if (srcRect.isEmpty() || !m_source.rect().intersects(srcRect)) {
        hidePreview();
        return;
    }
    // 上下各放 2px 余量,避免字形贴边;并与源图求交(行框理论上不出图,防御)
    const QRect clipped = (srcRect.adjusted(-2, -2, 2, 2)).intersected(m_source.rect());
    const QImage crop = m_source.copy(clipped);
    if (crop.isNull()) {
        hidePreview();
        return;
    }
    // 放大倍数:把行框高度撑到 ~64px,封顶 4 倍;宽度同倍率,再封到 520(约一屏行宽)。
    const int targetH = 64;
    const qreal scale = qMin<qreal>(kPreviewMaxScale,
                                    qMax<qreal>(1.0, qreal(targetH) / qMax(1, crop.height())));
    QImage scaled = crop.scaled(qRound(crop.width() * scale), qRound(crop.height() * scale),
                                Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (scaled.width() > 520) {
        scaled = scaled.scaledToWidth(520, Qt::SmoothTransformation);
    }
    if (scaled.width() < kPreviewMinWidth) {
        scaled = scaled.scaledToWidth(kPreviewMinWidth, Qt::SmoothTransformation);
    }
    m_previewImage->setPixmap(QPixmap::fromImage(scaled));
    m_previewCaption->setText(m_lines.at(lineIndex).text);
    m_preview->adjustSize();
    const QPoint p = previewTopLeftForLine(lineIndex);
    if (p.isNull()) {
        hidePreview();
        return;
    }
    m_preview->move(p);
    m_preview->show();
    m_preview->raise();
}

bool OcrResultPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == (m_body ? m_body->viewport() : nullptr) && m_body) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            // 悬停行:找鼠标所在 block,有行框数据就高亮 + 预览。
            const QPoint vpPos = static_cast<QMouseEvent*>(event)->pos();
            QTextCursor cursor = m_body->cursorForPosition(vpPos);
            const int line = cursor.blockNumber();
            if (line >= 0 && line < m_lines.size() && lineCopyable(line)) {
                if (m_hoverLine != line) {
                    m_hoverLine = line;
                    // 锚 = 该 block 在 viewport 中的可视矩形 → 面板 → 全局。
                    const QRect blockRect = m_body->cursorRect(cursor);
                    const QRect vpRect(blockRect.topLeft() + m_body->viewport()->pos(),
                                       blockRect.size());
                    const QRect panelRect = QRect(m_body->mapTo(this, vpRect.topLeft()), vpRect.size());
                    m_previewAnchorGlobal = QRect(mapToGlobal(panelRect.topLeft()), panelRect.size());
                    showPreviewForLine(line);
                }
            } else {
                hidePreview();
            }
            break;
        }
        case QEvent::Leave:
            hidePreview();
            break;
        case QEvent::MouseButtonDblClick: {
            // **点行复制**:双击某行 = 只复制该行(整体复制仍由「再次复制」承担)。
            // 双击而不是单击:单击是 QTextEdit 划选/定位文本的原生手势,不能抢;
            // "取这一行"是低频明确动作,双击的确认感正好。
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                const int line = m_body->cursorForPosition(me->pos()).blockNumber();
                if (line >= 0 && line < m_lines.size() && lineCopyable(line)) {
                    if (CoreBridge::instance().copyTextToClipboard(
                            m_lines.at(line).text)) {
                        if (m_status) {
                            m_status->setText(
                                QStringLiteral("已复制第 %1 行").arg(line + 1));
                        }
                    }
                    me->accept();
                    hidePreview();
                    return true;   // 已消费:不再让 QTextEdit 处理这次双击
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void OcrResultPanel::closeEvent(QCloseEvent* e) {
    // 预览浮窗是独立顶层(无父对象):面板走 WA_DeleteOnClose 后它若只 hide,
    // 会留一个**看不见但仍存在**的顶层窗口 —— 面板本身销毁了,这个窗口没有
    // 任何人持有它的指针,再也没机会删。
    //
    // ⚠️ 2026-11-03 前这里只调 hidePreview(),于是一个 QWidget + 2 个 QLabel +
    //    一份 stylesheet 随每个 OCR 结果面板永久泄漏;OCR 是高频动作,长驻托盘
    //    应用里这份泄漏单调累积。
    // 现在:deleteLater() 并置空三个指针(deleteLater 而非 delete:本函数还在
    // QWidget 的事件处理栈上,立即删除自己创建的兄弟窗口容易踩到半销毁状态)。
    if (m_preview) {
        m_preview->hide();
        m_preview->deleteLater();
        m_preview = nullptr;
        m_previewImage = nullptr;      // 是 m_preview 的子对象,随它一起析构
        m_previewCaption = nullptr;    // 同上
    }
    // 状态一并复位:否则下一次 buildPreview 之前若有代码读 m_hoverLine/anchor,
    // 会拿到上一轮的残留值(hidePreview 里做的就是这件事)。
    m_hoverLine = -1;
    m_previewAnchorGlobal = QRect();
    QFrame::closeEvent(e);
}

// ---- 拖动 ----

void OcrResultPanel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QFrame::mousePressEvent(e);
        return;
    }
    // 走到这里的事件来自:标题行 / 状态行 / 按钮行外侧的边距 / 上下内边距
    // —— 这些区域不消费鼠标,按下会冒泡到面板。
    //
    // 正文区**显式排除**:实测 QTextEdit 的 viewport 本来就会吃掉按下(反向验证:
    // 把这层排除删掉,"正文区按下不拖动"的断言仍然 PASS),所以这条不是修缺陷,
    // 是**防御**:它把"拖动"与"划选"的边界写死在代码里,不依赖"某个子控件恰好
    // 会消费事件"这一假设。假设若在将来某个控件上不成立,症状是"拖着拖着文字
    // 选不中了",极难定位 —— 值得多这五行。
    if (m_body) {
        // 用 body 在面板内的几何判断:事件已冒泡到面板,pos() 是面板局部坐标
        const QRect bodyRect(m_body->mapTo(this, QPoint(0, 0)), m_body->size());
        if (bodyRect.contains(pos())) {
            QFrame::mousePressEvent(e);
            return;
        }
    }
    // 排除正文后,其余空白(标题行、状态行、按钮行外侧的边距、上下内边距)都能拖。
    // 不强制"只能在标题行里拖":面板很窄,强迫用户瞄准 22px 高的条是坏体验;
    // 而正文是唯一"按下有其它语义"的区域,排除它就够了。
    m_dragging = true;
    // ⚠️ 两个量都要用**全局坐标**口径:
    //    - m_dragStartPos 记窗口左上角的**全局**位置(Esc 取消时移回这里);
    //    - m_dragOffset 记"鼠标全局点 - 窗口左上角",之后 move 时一减即得新位置。
    //    若把 m_dragStartPos 记成 pos()(面板局部坐标),Esc 恢复会把窗口移到屏幕左上角。
    m_dragStartPos = frameGeometry().topLeft();
    m_dragOffset = e->globalPosition().toPoint() - m_dragStartPos;
    e->accept();
}

void OcrResultPanel::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) {
        QFrame::mouseMoveEvent(e);
        return;
    }
    // 不设最小/最大边界:拖动时就该跟手。拖动期间允许出屏,用户松手前可以拉回来
    // (强行夹取会出现"鼠标到了但窗口卡住"的割裂感)。
    move(e->globalPosition().toPoint() - m_dragOffset);
    e->accept();
}

void OcrResultPanel::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        e->accept();
        return;
    }
    QFrame::mouseReleaseEvent(e);
}

void OcrResultPanel::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && m_dragging) {
        // 拖动途中反悔:回到按下时的位置并结束拖动(Esc 不关面板 —— 关面板由用户
        // 点「关闭」或再按 Esc 决定,这里先只撤销这次拖动)。
        m_dragging = false;
        move(m_dragStartPos);
        e->accept();
        return;
    }
    QFrame::keyPressEvent(e);
}

void OcrResultPanel::paintEvent(QPaintEvent* e) {
    Q_UNUSED(e);
    // 底板自绘(不靠 stylesheet,理由见头文件与构造函数的注释)。
    // 视觉与 QML 侧 Popup 对齐(docs/theme.md §33):surface 底 + 1px border +
    // 8px 圆角;因为背景可能是任意截图/桌面内容,再加一层柔和阴影把面板"托起来"。
    QPainter p(this);
    if (!p.isActive()) return;   // 首帧后备缓冲未就绪(begin 失败)时丢弃这一帧

    const ui::ThemeTokens& t = ui::themeTokens();
    constexpr int kRadius = 8;

    p.setRenderHint(QPainter::Antialiasing, true);

    // 1) 阴影:在面板矩形**内缩**绘制一层半透明圆角,外圈由窗口透明区承担。
    //    不用 QGraphicsDropShadowEffect —— 它会把整棵树走离屏渲染,对这种
    //    "显示几秒就关"的小窗不划算,且与透明顶层窗口叠加时易出黑边。
    for (int i = 3; i >= 1; --i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 8 * i));
        p.drawRoundedRect(QRectF(rect()).adjusted(i, i + 1, -i, -i + 1).adjusted(0.5, 0.5, -0.5, -0.5),
                          kRadius, kRadius);
    }

    // 2) 底板 + 描边
    const QRectF plate = QRectF(rect()).adjusted(4.5, 4.5, -4.5, -4.5);
    p.setPen(QPen(ui::withAlpha(t.border, 230), 1));
    p.setBrush(t.surface);   // 不透明:这里是**阅读内容**,底下的截图不该透进来
    p.drawRoundedRect(plate, kRadius, kRadius);
}

QPoint OcrResultPanel::placeForTest(const QSize& panelSize, const QRect& anchorGlobal) {
    return placeNextTo(panelSize, anchorGlobal);
}

OcrResultPanel* OcrResultPanel::showResult(const QString& text, const QString& reason,
                                           const QImage& source, const QRect& anchorRectGlobal,
                                           bool popup,
                                           const QVector<OcrLineBox>& lines) {
    auto* panel = new OcrResultPanel(text, reason, source, popup, lines);

    // 顺序有讲究:
    //   ① 先按**当前屏可用高度**定正文上限(内容长时封顶,剩下的交给滚动条);
    //   ② fitBodyToContent 把正文定到"够用且不超上限"的高度;
    //   ③ 再 adjustSize 得到最终窗口尺寸,用于摆位。
    // 若先 adjustSize 再限高,窗口会先撑到屏幕外,摆位夹取后按钮仍可能被切。
    QScreen* screen = QGuiApplication::screenAt(anchorRectGlobal.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    panel->setAnchorScreen(screen);
    if (screen) {
        // 预留:状态行 + 按钮行 + 边距 + 阴影,合计约 130px
        panel->setMaxBodyHeight(screen->availableGeometry().height() - 130);
    }
    panel->fitBodyToContent();
    panel->adjustSize();
    panel->move(placeNextTo(panel->size(), anchorRectGlobal));
    panel->show();
    panel->raise();
    return panel;
}

OcrResultPanel* OcrResultPanel::showResult(const QString& text, const QString& reason,
                                           const QImage& source, QWidget* anchor, bool popup,
                                           const QVector<OcrLineBox>& lines) {
    // 便捷重载:退化成"贴这个控件"(编辑器用)。
    // ⚠️ 调用方**不要**把整屏窗口(截图遮罩)传进来 —— 那会算成屏幕边角,
    //    截图链路必须走 anchorRectGlobal 重载传选区矩形。
    const QRect r = anchor ? anchor->frameGeometry() : QRect();
    return showResult(text, reason, source, r, popup, lines);
}

} // namespace nb
