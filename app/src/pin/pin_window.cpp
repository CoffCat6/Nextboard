#include "pin_window.hpp"

#include <QClipboard>
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

/// 悬停工具条的尺寸与留白(逻辑像素)。
constexpr int kToolbarMargin = 6;
constexpr int kToolbarButtonW = 24;
constexpr int kToolbarButtonH = 22;
constexpr int kToolbarSliderW = 74;

/// 鼠标移出后延时隐藏工具条的宽限(毫秒):给"从图面移到工具条"留出时间。
constexpr int kToolbarHideDelayMs = 220;
} // namespace

QImage PinWindow::renderText(const QString& text, const PinTextStyle& style) {
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(style.fontSize);
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
    positionToolbar();
    updateToolbar();
}

// ---- 悬停工具条(对齐第一代 PinApp:缩放值 / 复制 / 另存 / 关闭 / 不透明度) ----

void PinWindow::buildToolbar() {
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("pinToolbar"));
    // 纯 QWidget 需显式开启,样式表里的 background/border 才会绘制
    m_toolbar->setAttribute(Qt::WA_StyledBackground, true);
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

    // 深色小工具条:与文本贴图默认底板同色系(贴图本身多为深色浮层)
    m_toolbar->setStyleSheet(QStringLiteral(
        "#pinToolbar { background: rgba(38,39,46,235); border: 1px solid rgba(255,255,255,40);"
        " border-radius: 6px; }"
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

    m_hideToolbarTimer = new QTimer(this);
    m_hideToolbarTimer->setSingleShot(true);
    m_hideToolbarTimer->setInterval(kToolbarHideDelayMs);
    connect(m_hideToolbarTimer, &QTimer::timeout, m_toolbar, &QWidget::hide);
}

void PinWindow::positionToolbar() {
    if (!m_toolbar) return;
    m_toolbar->adjustSize();
    const int x = qMax(kToolbarMargin, width() - m_toolbar->width() - kToolbarMargin);
    m_toolbar->move(x, kToolbarMargin);
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
    const QString suffix = m_idPrefix.isEmpty()
                               ? QString::number(QDateTime::currentMSecsSinceEpoch())
                               : m_idPrefix;
    if (!m_text.isEmpty()) {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("另存为"), QStringLiteral("文本_%1.txt").arg(suffix),
            QStringLiteral("文本文件 (*.txt)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            showNotice(QStringLiteral("另存失败"));
            return;
        }
        file.write(m_text.toUtf8());
        showNotice(QStringLiteral("已另存为文本"));
    } else {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("另存为"), QStringLiteral("贴图_%1.png").arg(suffix),
            QStringLiteral("PNG 图片 (*.png)"));
        if (path.isEmpty()) return;
        if (!m_image.save(path, "PNG")) {
            showNotice(QStringLiteral("另存失败"));
            return;
        }
        showNotice(QStringLiteral("已另存为图片"));
    }
}

void PinWindow::showNotice(const QString& text) {
    if (!m_hintLabel) return;
    m_hintLabel->setText(text);
    m_hintLabel->adjustSize();
    m_hintLabel->move(8, qMax(8, height() - m_hintLabel->height() - 8));
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

QSize PinWindow::logicalSizeFor(qreal dpr) const {
    if (dpr <= 0.0) dpr = 1.0;
    // 原图是**物理像素**,窗口坐标是**逻辑像素**:除以 DPR 后,窗口的物理尺寸
    // 恰为 原图 × 缩放,与所在屏的缩放比无关(跨屏拖来拖去不会变大变小)。
    return QSize(qMax(1, qRound(m_image.width() * m_zoom / dpr)),
                 qMax(1, qRound(m_image.height() * m_zoom / dpr)));
}

void PinWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setOpacity(m_opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(rect(), m_image);
}

void PinWindow::applyScale() {
    resize(logicalSizeFor(currentDpr()));
    positionToolbar();
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

void PinWindow::enterEvent(QEnterEvent* e) {
    QWidget::enterEvent(e);
    if (m_hideToolbarTimer) m_hideToolbarTimer->stop();
    if (m_toolbar) {
        positionToolbar();
        m_toolbar->show();
        m_toolbar->raise();
    }
}

void PinWindow::leaveEvent(QEvent* e) {
    QWidget::leaveEvent(e);
    // 从图面移到工具条(子控件)也会触发父窗 Leave:指针仍在工具条上时不隐藏
    if (m_toolbar && m_toolbar->isVisible() &&
        m_toolbar->rect().contains(m_toolbar->mapFromGlobal(QCursor::pos())))
        return;
    if (m_hideToolbarTimer) m_hideToolbarTimer->start();
}

void PinWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(e);
}

void PinWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
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
