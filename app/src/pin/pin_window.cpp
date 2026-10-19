#include "pin_window.hpp"

#include "../app_font.hpp"
#include "host/image_save.hpp"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QWheelEvent>

namespace nb {

namespace {
constexpr int kTextPadding = 14;
constexpr int kTextMinWidth = 160;

/// 工具条的尺寸与留白(逻辑像素)。
constexpr int kToolbarButtonW = 24;
constexpr int kToolbarButtonH = 22;
constexpr int kToolbarSliderW = 74;

/// 贴图四周的阴影留白(逻辑像素)。窗口比图大一圈,多出的部分用来画阴影 ——
/// 贴图是无边框透明窗口,系统不会给它画阴影,只能自绘。
constexpr int kShadowMargin = 6;
/// 阴影由若干层低透明度圆角矩形自外向内叠加而成(纯 QPainter,不依赖
/// QGraphicsBlurEffect):近图处层数最多、最深,向外逐层变淡 → 柔和渐变。
constexpr int kShadowLayers = 3;
constexpr int kShadowLayerAlpha = 8;   // 单层 alpha(0-255)
/// 文本贴图的底板是 8px 圆角(见 renderText),阴影随之圆角;图片条目是直角。
constexpr qreal kTextPlateRadius = 8.0;

/// 顺时针旋转 90°:**像素级置换、无损**(不做插值 —— 反复旋转不会逐次糊化)。
/// 统一按 32 位 ARGB 处理,源图其他格式先转换。
QImage rotate90cw(const QImage& src) {
    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    const int w = in.width();
    const int h = in.height();
    QImage out(h, w, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            // 顺时针:源 (x, y) → 目标 (h-1-y, x)
            reinterpret_cast<QRgb*>(out.scanLine(x))[h - 1 - y] = line[x];
        }
    }
    return out;
}

/// 工具条底板配色(与文本贴图默认底板同色系;贴图多为深色浮层,浅底会"看不清")。
constexpr int kToolbarRadius = 6;
const QColor kToolbarBg(38, 39, 46);

/// 工具条容器:**自绘**深色圆角底板。
///
/// 不用 QSS 画自身背景的原因:工具条是顶层弹层 + `WA_TranslucentBackground`,
/// 这种组合下 QSS 给控件**自身**的 `background` 规则不生效 —— 表现为底板缺失,
/// 只剩半透明白按钮与浅色文字浮在桌面上,看起来"整条是白的、看不清"。
/// 自绘与 QSS 无关,任何平台/主题下都稳定。
class ToolbarPanel : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 255, 255, 46), 1));
        QColor fill = kToolbarBg;
        fill.setAlpha(244);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                kToolbarRadius, kToolbarRadius);
    }
};
} // namespace

QImage PinWindow::renderText(const QString& text, const PinTextStyle& style) {
    // 与界面/工具条同一份字体来源(app_font.hpp):以前这里硬编码
    // "Microsoft YaHei"(经典雅黑),而工具条按钮继承应用字体(雅黑 UI 变体),
    // 两者行高/字重不同,贴出来的图和工具条看着不是一套字。现在统一走 uiFont()。
    QFont font = nb::uiFont(style.fontSize);
    const int maxWidth = qMax(kTextMinWidth, style.maxWidth);
    const QFontMetrics metrics(font);
    // 先按最大宽度做换行排版测量,再定画布尺寸
    const QRect bounds = metrics.boundingRect(
        QRect(0, 0, maxWidth - 2 * kTextPadding, 10000),
        Qt::TextWordWrap | Qt::AlignLeft, text);
    const int width = qBound(kTextMinWidth, bounds.width() + 2 * kTextPadding, maxWidth);
    const int height = bounds.height() + 2 * kTextPadding;

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setBrush(style.background);
    painter.setPen(QPen(style.background.lighter(140), 1));
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1.0, height - 1.0), 8, 8);
    painter.setPen(style.textColor);
    painter.setFont(font);
    painter.drawText(QRect(kTextPadding, kTextPadding,
                           width - 2 * kTextPadding, height - 2 * kTextPadding),
                     Qt::TextWordWrap | Qt::AlignLeft, text);
    painter.end();
    return image;
}

PinWindow::PinWindow(PinContent content, const PinOptions& options, QWidget* parent)
    : QWidget(parent,
              Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
                  Qt::NoDropShadowWindowHint),
      m_image(std::move(content.image)),
      m_text(std::move(content.text)),
      m_idPrefix(std::move(content.idPrefix)),
      // 初始透明度取自设置(第一代 pinDefaultOpacity,30–100)
      m_opacity(qBound(0.3, options.opacityPercent / 100.0, 1.0)),
      m_doubleClickClose(options.doubleClickClose) {
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    // 需要接收键盘焦点,ESC 才能关闭(第一代语义:✕ 或 ESC 关闭)
    setFocusPolicy(Qt::StrongFocus);

    buildToolbar();

    // 落点 = 光标所在屏中央(第一代决策:四入口统一 at=None)
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    // 关键:按**目标屏**的 DPR 换算逻辑尺寸,而不是主屏 —— 窗口 show 之前的
    // devicePixelRatioF 仍是主屏值,拿它换算会在不同缩放比的屏上整体放大/缩小。
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    resize(logicalSizeFor(dpr));
    if (screen) {
        const QRect avail = screen->availableGeometry();
        // 图比屏大:退化为左上角起放(第一代 center_in 语义)
        move(avail.center() - rect().center());
    }
    // 工具条默认收起,由右键唤出;此处只需同步一次缩放值/不透明度
    updateToolbar();
}

// ---- 工具条构建 ----

void PinWindow::buildToolbar() {
    // 顶层弹层(同右键菜单机制),不是贴图的子控件 —— 子控件会被父窗口裁剪,
    // 贴图比工具条窄时右侧(滑杆/✕)会被直接切掉。弹层不受此限,并在 showToolbarAt
    // 里夹进屏幕可用区,任意大小的贴图都能完整显示。
    m_toolbar = new ToolbarPanel(this, Qt::Popup | Qt::FramelessWindowHint);
    m_toolbar->setObjectName(QStringLiteral("pinToolbar"));
    // 顶层窗口要透明底,自绘的圆角才不会被黑色/白色直角覆盖
    m_toolbar->setAttribute(Qt::WA_TranslucentBackground, true);
    // 兜底:万一透明底在该环境不生效,窗口底色也必须是深色(绝不能是白底)
    QPalette panelPalette = m_toolbar->palette();
    panelPalette.setColor(QPalette::Window, kToolbarBg);
    m_toolbar->setPalette(panelPalette);
    m_toolbar->installEventFilter(this);
    m_toolbar->hide();

    auto* lay = new QHBoxLayout(m_toolbar);
    lay->setContentsMargins(6, 4, 6, 4);
    lay->setSpacing(4);

    m_zoomLabel = new QLabel(m_toolbar);
    m_zoomLabel->setObjectName(QStringLiteral("pinZoom"));
    m_zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_zoomLabel->setMinimumWidth(34);
    lay->addWidget(m_zoomLabel);

    // 工具条控件一律 NoFocus:避免点一下就抢走窗口焦点,ESC 失效
    const auto addButton = [this, lay](const QString& text, const QString& tip,
                                       const QString& objectName, auto slot) {
        auto* btn = new QPushButton(text, m_toolbar);
        btn->setObjectName(objectName);
        btn->setToolTip(tip);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(kToolbarButtonW, kToolbarButtonH);
        connect(btn, &QPushButton::clicked, this, slot);
        lay->addWidget(btn);
        return btn;
    };
    // 旋转 / 编辑:仅图片贴图有意义(文本贴图是渲染出的位图,旋转/标注都不合适)
    const bool isImage = m_text.isEmpty();
    m_rotateButton = addButton(QStringLiteral("↻"), QStringLiteral("旋转 90°"),
                               QStringLiteral("pinButton"), &PinWindow::rotateImage);
    m_editButton = addButton(QStringLiteral("✎"), QStringLiteral("编辑标注"),
                             QStringLiteral("pinButton"), &PinWindow::requestEdit);
    m_rotateButton->setVisible(isImage);
    m_editButton->setVisible(isImage);

    addButton(QStringLiteral("⧉"), QStringLiteral("复制到剪贴板"),
              QStringLiteral("pinButton"), &PinWindow::copyToClipboard);
    addButton(QStringLiteral("💾"), QStringLiteral("另存为…"),
              QStringLiteral("pinButton"), &PinWindow::saveAs);
    // QWidget::close() 返回 bool,与成员指针签名不匹配,用 lambda 收口
    addButton(QStringLiteral("✕"), QStringLiteral("关闭贴图 (ESC)"),
              QStringLiteral("pinClose"), [this] { close(); });

    m_opacitySlider = new QSlider(Qt::Horizontal, m_toolbar);
    m_opacitySlider->setObjectName(QStringLiteral("pinSlider"));
    m_opacitySlider->setRange(30, 100);
    m_opacitySlider->setFixedWidth(kToolbarSliderW);
    m_opacitySlider->setFocusPolicy(Qt::NoFocus);
    m_opacitySlider->setToolTip(QStringLiteral("不透明度"));
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int value) {
        m_opacity = qBound(0.3, value / 100.0, 1.0);
        update();
    });
    lay->addWidget(m_opacitySlider);

    // 深色小工具条:底板由 ToolbarPanel 自绘,这里只负责**子控件**样式
    m_toolbar->setStyleSheet(QStringLiteral(
        "#pinZoom { color: #AEB4C0; font-size: 11px; }"
        "#pinButton { background: rgba(255,255,255,22); color: #DFE1E8;"
        " border: 1px solid rgba(255,255,255,38); border-radius: 4px; font-size: 12px; padding: 0; }"
        "#pinButton:hover { background: rgba(255,255,255,52); }"
        "#pinClose { background: rgba(214,76,76,205); color: #FFFFFF; border: none;"
        " border-radius: 4px; font-size: 12px; padding: 0; }"
        "#pinClose:hover { background: rgba(232,92,92,235); }"
        "QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,44);"
        " border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -4px 0;"
        " background: #7FA8FF; border-radius: 5px; }"));

    // 底部瞬时提示(已复制 / 已另存):自包含在贴图窗口内 —— 主面板隐藏时 toast 不可见
    m_hintLabel = new QLabel(this);
    m_hintLabel->setObjectName(QStringLiteral("pinHint"));
    m_hintLabel->setAttribute(Qt::WA_StyledBackground, true);
    m_hintLabel->setStyleSheet(QStringLiteral(
        "#pinHint { background: rgba(38,39,46,235); color: #DFE1E8;"
        " border: 1px solid rgba(255,255,255,38); border-radius: 4px; padding: 3px 8px;"
        " font-size: 12px; }"));
    m_hintLabel->hide();
    m_hintTimer = new QTimer(this);
    m_hintTimer->setSingleShot(true);
    m_hintTimer->setInterval(1600);
    connect(m_hintTimer, &QTimer::timeout, m_hintLabel, &QLabel::hide);
}

// ---- 右键工具条(对齐第一代 PinApp:缩放值 / 复制 / 另存 / 关闭 / 不透明度) ----

void PinWindow::showToolbarAt(const QPoint& globalPos) {
    if (!m_toolbar) return;
    m_toolbar->adjustSize();
    // 夹在**屏幕可用区**内(不是贴图窗口内):贴图再小,工具条也完整可见
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(globalPos, m_toolbar->size());
    const int x = qBound(avail.left(), globalPos.x(),
                         qMax(avail.left(), avail.right() - m_toolbar->width() + 1));
    const int y = qBound(avail.top(), globalPos.y(),
                         qMax(avail.top(), avail.bottom() - m_toolbar->height() + 1));
    m_toolbar->move(x, y);
    m_toolbar->show();
    m_toolbar->raise();
}

void PinWindow::hideToolbar() {
    if (m_toolbar) m_toolbar->hide();
}

void PinWindow::updateToolbar() {
    if (m_zoomLabel)
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(m_zoom * 100)));
    if (m_opacitySlider) {
        // 滑杆值可能来自滚轮(Alt+滚轮)变更,回写时屏蔽信号避免递归
        const QSignalBlocker blocker(m_opacitySlider);
        m_opacitySlider->setValue(qRound(m_opacity * 100));
    }
}

void PinWindow::copyToClipboard() {
    if (!m_text.isEmpty()) {
        QGuiApplication::clipboard()->setText(m_text);
        showNotice(QStringLiteral("已复制文本"));
    } else {
        QGuiApplication::clipboard()->setImage(m_image);
        showNotice(QStringLiteral("已复制图片"));
    }
}

void PinWindow::saveAs() {
    // 先收起弹层:它持有鼠标抓取,而文件对话框是模态窗口,开着弹层可能挡住对话框
    hideToolbar();
    const QString suffix = m_idPrefix.isEmpty()
                               ? QString::number(QDateTime::currentMSecsSinceEpoch())
                               : m_idPrefix;
    const bool isText = !m_text.isEmpty();
    if (isText) {
        // 文本贴图仍只有 txt 一种落盘形式(格式选择对文本没有意义)。
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("另存为"), QStringLiteral("文本_%1.txt").arg(suffix),
            QStringLiteral("文本文件 (*.txt)"));
        if (path.isEmpty()) {
            activateWindow();
            setFocus(Qt::OtherFocusReason);
            return;
        }
        bool ok = false;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            ok = file.write(m_text.toUtf8()) >= 0;
        showNotice(ok ? QStringLiteral("已另存为文本") : QStringLiteral("另存失败"));
    } else {
        // 图片走共享的另存通道:PNG / JPEG / WebP 由后缀决定。
        // 贴图内容可能带透明(text 渲染的边角),JPEG 分支会自动铺白底。
        QString error;
        const QString saved = image_save::saveImageWithDialog(
            this, m_image, QStringLiteral("贴图_%1").arg(suffix), &error);
        if (saved.isEmpty() && !error.isEmpty()) showNotice(error);
        else if (!saved.isEmpty()) showNotice(QStringLiteral("已另存为图片"));
    }
    // 文件对话框会夺走焦点:收回焦点,ESC 才能继续生效
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void PinWindow::rotateImage() {
    if (m_image.isNull()) return;
    // 以画面中心为锚点:旋转后宽高互换,重摆位使中心不动(否则会跳到别处)
    const QPoint globalCenter = mapToGlobal(rect().center());
    m_image = rotate90cw(m_image);
    applyScale();                               // 重算尺寸(宽高互换)+ 夹回工具条
    move(globalCenter - rect().center());
    showNotice(QStringLiteral("已旋转 90°"));
}

void PinWindow::requestEdit() {
    if (m_image.isNull()) return;
    hideToolbar();
    // 交给调用方(panel/CoreFacade)开编辑器;完成后经 setImage 回写
    emit editRequested(m_image);
}

void PinWindow::setImage(const QImage& image) {
    if (image.isNull()) return;
    const QPoint globalCenter = mapToGlobal(rect().center());
    m_image = image;
    applyScale();
    move(globalCenter - rect().center());
}

void PinWindow::showNotice(const QString& text) {
    if (!m_hintLabel) return;
    m_hintLabel->setText(text);
    m_hintLabel->adjustSize();
    // 贴图片左下角(与工具条同理,避开阴影留白)
    const int y = qMax(kShadowMargin, height() - kShadowMargin - m_hintLabel->height() - 8);
    m_hintLabel->move(kShadowMargin + 8, y);
    m_hintLabel->show();
    m_hintLabel->raise();
    m_hintTimer->start();
}

// ---- 尺寸与绘制 ----

qreal PinWindow::currentDpr() const {
    // 用所属屏的 DPR:窗口跨屏后 Qt 会更新 screen(),据此保持物理尺寸不变。
    if (QScreen* s = screen()) return s->devicePixelRatio();
    return devicePixelRatioF();
}

qint64 PinWindow::memoryBytes() const {
    // 图片 + 窗口 backing store。软件渲染下每个置顶窗口都要一份与窗口物理尺寸
    // 等大的后备缓冲(DPR 生效,故乘 dpr²);实测这部分与图片本身差不多大
    // (1755×1097:图 7.7MB + 后备 ~7.7MB ≈ 进程增量 18.3MB)。
    const qreal dpr = currentDpr();
    const qint64 backing =
        static_cast<qint64>(qMax(1, width())) * qMax(1, height()) * dpr * dpr * 4;
    return m_image.sizeInBytes() + backing;
}

QSize PinWindow::logicalSizeFor(qreal dpr) const {
    if (dpr <= 0.0) dpr = 1.0;
    // 原图是**物理像素**,窗口坐标是**逻辑像素**:除以 DPR 后,窗口的物理尺寸
    // 恰为 原图 × 缩放,与所在屏的缩放比无关(跨屏拖来拖去不会变大变小)。
    // 再各留一圈阴影留白(见 paintEvent)。
    const int w = qMax(1, qRound(m_image.width() * m_zoom / dpr));
    const int h = qMax(1, qRound(m_image.height() * m_zoom / dpr));
    return QSize(w + 2 * kShadowMargin, h + 2 * kShadowMargin);
}

void PinWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setOpacity(m_opacity);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 图片占据窗口去掉一圈阴影留白后的区域
    const QRectF imageRect(kShadowMargin, kShadowMargin,
                           qMax(1, width() - 2 * kShadowMargin),
                           qMax(1, height() - 2 * kShadowMargin));

    // 阴影:自外向内叠加若干层低透明度圆角矩形,近图处最深、向外渐隐。
    // 先画阴影再画图,阴影只在图外一圈可见(图有透明角落时也会透出,更自然)。
    const qreal cornerRadius = m_text.isEmpty() ? 0.0 : kTextPlateRadius;
    const qreal step = qreal(kShadowMargin) / kShadowLayers;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, kShadowLayerAlpha));
    for (int i = kShadowLayers; i >= 1; --i) {
        const qreal grow = i * step;
        painter.drawRoundedRect(imageRect.adjusted(-grow, -grow, grow, grow),
                                cornerRadius, cornerRadius);
    }

    painter.drawImage(imageRect, m_image);
}

void PinWindow::applyScale() {
    resize(logicalSizeFor(currentDpr()));
    updateToolbar();
    update();
}

bool PinWindow::event(QEvent* e) {
    // 窗口被拖到不同缩放比的屏幕,或系统缩放变化:Qt 只重绘、不调整窗口几何,
    // 窗口逻辑尺寸不变 → 物理尺寸随 DPR 变化(忽大忽小)。这里按新 DPR 重算逻辑
    // 尺寸,使物理尺寸恒为 原图 × 缩放。
    //
    // 延到事件循环末尾再算:Windows 平台随后会在 WM_DPICHANGED 里 SetWindowPos
    // 原生调整窗口几何(且本事件在其之前同步送达),立即 resize 会被它覆盖;
    // 延后则保证最终尺寸由我们决定。
    const QEvent::Type type = e->type();
    if (type == QEvent::DevicePixelRatioChange || type == QEvent::ScreenChangeInternal) {
        QTimer::singleShot(0, this, [this] { applyScale(); });
    }
    return QWidget::event(e);
}

// ---- 交互 ----

void PinWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // 取焦:ESC 关闭、工具条可点。贴图刚创建时是用户主动触发的,取焦可接受。
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void PinWindow::contextMenuEvent(QContextMenuEvent* e) {
    // 右键唤出/收起工具条(取代原"悬停显示")。工具条是顶层弹层,定位用屏幕坐标。
    const QPoint globalPos = e->globalPos();
    // 工具条自身的右键不参与开关(弹层持有鼠标抓取,一般收不到;保底判断)
    if (m_toolbar && m_toolbar->isVisible() && m_toolbar->frameGeometry().contains(globalPos)) {
        e->accept();
        return;
    }
    if (m_toolbar && m_toolbar->isVisible())
        hideToolbar();
    else
        showToolbarAt(globalPos);
    e->accept();
}

bool PinWindow::eventFilter(QObject* obj, QEvent* e) {
    if (obj == m_toolbar && e->type() == QEvent::Hide && isVisible()) {
        // 弹层开着时键盘焦点在弹层上;关闭后收回贴图,ESC 才能继续生效
        activateWindow();
        setFocus(Qt::OtherFocusReason);
    }
    return QWidget::eventFilter(obj, e);
}

void PinWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        // 工具条已展开时先收起工具条,再按一次才关窗
        if (m_toolbar && m_toolbar->isVisible())
            hideToolbar();
        else
            close();
        return;
    }
    // 方向键微调位置(docs/18 §3.6;Snipaste 习惯):1px,按住 Shift 为 10px。
    // 直接 move() 而**不走** m_dragging —— 拖动状态由鼠标独占,键盘不该改动它,
    // 否则松手时的 mouseReleaseEvent 会把"键盘挪过的那张"当成还在拖。
    QPoint delta;
    switch (e->key()) {
        case Qt::Key_Left:  delta = QPoint(-1, 0); break;
        case Qt::Key_Right: delta = QPoint(1, 0); break;
        case Qt::Key_Up:    delta = QPoint(0, -1); break;
        case Qt::Key_Down:  delta = QPoint(0, 1); break;
        default:
            QWidget::keyPressEvent(e);
            return;
    }
    const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    move(pos() + delta * step);
    // 键盘移动后贴图仍要能用 ESC 关:某些环境下 move() 会打断焦点(工具条弹层
    // 关闭后的焦点回收见 eventFilter),这里不做额外处理,只保证窗口不被带走。
    e->accept();
}

void PinWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        // 左键点图面 = 收起工具条并开始拖动
        hideToolbar();
        // 点击后确保窗口仍持有焦点(ESC 才能生效)
        activateWindow();
        setFocus(Qt::MouseFocusReason);
        m_dragging = true;
        m_dragOffset = e->globalPosition().toPoint() - pos();
    }
}

void PinWindow::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging) move(e->globalPosition().toPoint() - m_dragOffset);
}

void PinWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) m_dragging = false;
}

void PinWindow::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::AltModifier) {
        // Alt + 滚轮:透明度(第一代语义)
        m_opacity = qBound(0.3, m_opacity + (e->angleDelta().y() > 0 ? 0.05 : -0.05), 1.0);
        updateToolbar();   // 回写工具条滑杆
    } else {
        // 滚轮:缩放(1.0 = 1:1 物理像素);下限 0.1 防止缩成一个点
        m_zoom = qBound(0.1, m_zoom * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1), 4.0);
        applyScale();
    }
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* e) {
    Q_UNUSED(e);
    // 双击关闭:第一代 Phase 9 默认(Snipaste 习惯);设置里可关掉
    if (m_doubleClickClose) close();
}

} // namespace nb
