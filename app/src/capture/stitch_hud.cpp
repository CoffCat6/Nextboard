#include "capture/stitch_hud.hpp"

#include "capture/overlay_geometry.hpp"
#include "ui_theme.hpp"

#include <QApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace nb {
namespace {

/// 全局 Esc 热键 id(RegisterHotKey 的作用域是本线程;与宿主的热键集合不冲突,
/// 因为宿主用的是另一套 id 且本 id 只在采集期存在)。
constexpr int kEscHotkeyId = 0x4E42;

/// 描边厚度(逻辑像素)。2px 足够指示采集范围,又不会盖住页面内容。
constexpr int kFrameThickness = 2;

/// 面板尺寸(逻辑像素):两个按钮 + 一行进度文案。
constexpr int kPanelWidth = 208;
constexpr int kPanelHeight = 30;
constexpr int kPanelButtonsHeight = 30;
constexpr int kPanelStatusHeight = 22;

/// 浮窗样式:不抢焦点、不进 Alt-Tab、不进任务栏。
/// ⚠️ 描边窗额外加 WS_EX_TRANSPARENT(鼠标穿透);面板**不加**(它要能点)。
Qt::WindowFlags hudWindowFlags() {
    return Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
           | Qt::WindowDoesNotAcceptFocus;
}

} // namespace

/// 描边窗口:只画那一圈,鼠标穿透。
class StitchFrameWindow : public QWidget {
public:
    explicit StitchFrameWindow(const QRect& globalRect, const QRect& selectionGlobal)
        : QWidget(nullptr, hudWindowFlags() | Qt::WindowTransparentForInput),
          m_selectionGlobal(selectionGlobal) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setGeometry(globalRect);
#ifdef Q_OS_WIN
        // 显式置 WS_EX_TRANSPARENT:Qt 的 WindowTransparentForInput 在部分后端上
        // 只设了 WA_TransparentForMouseEvents(同一进程内生效),跨进程的命中测试
        // 仍可能把这块当成窗口。加扩展样式才是系统级穿透。
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        LONG_PTR ex = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        // 二道保险:让本窗口不进截图(失败只记日志 —— 几何已保证描边在选区外)。
        if (!::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
            qInfo() << "长截图:描边窗口 WDA_EXCLUDEFROMCAPTURE 不可用(不影响正确性)";
        }
#endif
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        // 描边 = 主题 accent(与遮罩上的选区边框同色,视觉上还是"同一个选区")。
        p.setPen(Qt::NoPen);
        p.setBrush(ui::themeTokens().accent);
        // 只画选区**外侧**那一圈:本窗口 = 选区外扩 kFrameThickness。
        const QRect sel = m_selectionGlobal.translated(-geometry().topLeft());
        const QRect outer = rect();
        p.drawRect(QRect(outer.left(), outer.top(), outer.width(), kFrameThickness));
        p.drawRect(QRect(outer.left(), outer.bottom() - kFrameThickness + 1, outer.width(),
                         kFrameThickness));
        p.drawRect(QRect(outer.left(), sel.top(), kFrameThickness, sel.height()));
        p.drawRect(QRect(outer.right() - kFrameThickness + 1, sel.top(), kFrameThickness,
                         sel.height()));
    }

private:
    QRect m_selectionGlobal;
};

/// 面板窗口:两个可点按钮 + 一行进度文案。
class StitchPanelWindow : public QWidget {
public:
    explicit StitchPanelWindow(StitchHud* hud)
        : QWidget(nullptr, hudWindowFlags()), m_hud(hud) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(kPanelWidth, kPanelHeight + kPanelStatusHeight + 4);
        setCursor(Qt::ArrowCursor);
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        LONG_PTR ex = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
        // 面板可能被迫浮在选区内(整屏选区时无处可放),此时必须保证它不进画面。
        // 优先 WDA_EXCLUDEFROMCAPTURE;不可用时靠分层窗口 + 无 CAPTUREBLT 的 BitBlt
        // (见 docs/19 v2 §3.2)。两条都不成立时验收的哨兵色断言会失败 —— 不静默。
        if (!::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
            qInfo() << "长截图:面板 WDA_EXCLUDEFROMCAPTURE 不可用,依赖分层窗口";
        }
#endif
    }

    QString status;
    bool escUnavailable = false;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 面板跟随主题(surface 底 + border 描边),与遮罩浮条同一套令牌。
        // 采集期这块面板浮在**页面内容**上,浅色主题下若还是深色底,会和整机观感割裂。
        const ui::ThemeTokens& t = ui::themeTokens();
        const QRect btnArea(0, 0, width(), kPanelButtonsHeight);
        p.setPen(QPen(ui::panelBorder(), 1));
        p.setBrush(ui::panelPlate(242));
        p.drawRoundedRect(QRectF(btnArea).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);

        const int bw = width() / 2;
        // 「结束」= warning(收尾动作),「取消」= error(丢弃)
        const QRect endBtn(0, 0, bw, kPanelButtonsHeight);
        const QRect abortBtn(bw, 0, width() - bw, kPanelButtonsHeight);
        p.setPen(t.warning);
        p.drawText(endBtn, Qt::AlignCenter, QStringLiteral("结束"));
        p.setPen(t.error);
        p.drawText(abortBtn, Qt::AlignCenter, QStringLiteral("取消"));
        p.setPen(ui::withAlpha(t.borderStrong, 170));
        p.drawLine(QPoint(bw, 8), QPoint(bw, kPanelButtonsHeight - 8));

        // 进度 / 提示文案
        if (!status.isEmpty() || escUnavailable) {
            const QRect bar(0, kPanelButtonsHeight + 2, width(), kPanelStatusHeight);
            p.setPen(QPen(ui::panelBorder(), 1));
            p.setBrush(ui::panelPlate(242));
            p.drawRoundedRect(QRectF(bar).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            p.setPen(t.textPrimary);
            const QString text =
                escUnavailable ? QStringLiteral("%1(点「结束」收尾)").arg(status)
                               : status;
            p.drawText(bar.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !m_hud) return;
        const int bw = width() / 2;
        if (e->pos().x() < bw) {
            emit m_hud->finishRequested();
        } else {
            emit m_hud->abortRequested();
        }
    }

private:
    StitchHud* m_hud = nullptr;
};

StitchHud::StitchHud(QScreen* screen, const QRect& regionDevice, qreal dpr, QObject* parent)
    : QObject(parent), m_screen(screen), m_regionDevice(regionDevice), m_dpr(dpr) {
    if (!m_screen) return;
    // 设备像素 → 全局逻辑坐标:屏原点(逻辑) + 设备矩形 / dpr
    const QRect geo = m_screen->geometry();
    const qreal k = m_dpr > 0.01 ? m_dpr : 1.0;
    m_regionLogical = QRect(geo.topLeft() +
                                QPoint(qRound(m_regionDevice.x() / k),
                                       qRound(m_regionDevice.y() / k)),
                            QSize(qRound(m_regionDevice.width() / k),
                                  qRound(m_regionDevice.height() / k)));

    auto* frame = new StitchFrameWindow(
        m_regionLogical.adjusted(-kFrameThickness, -kFrameThickness, kFrameThickness,
                                 kFrameThickness),
        m_regionLogical);
    m_frame = frame;

    auto* panel = new StitchPanelWindow(this);
    m_panel = panel;

    // 面板位置:用与遮罩同一套纯几何(它那边的候选表已经保证"与选区零相交"优先)
    const QSize panelSize(panel->width(), panel->height());
    const QList<QRect> candidates = overlay::stitchPanelCandidates(
        m_regionLogical, m_screen->geometry().size(), panelSize, 8);
    if (!candidates.isEmpty()) {
        // 候选是"屏内局部逻辑坐标",转成全局
        m_panelRectGlobal = candidates.first().translated(geo.topLeft());
        m_panelOutside = true;
    } else {
        // 无处可放:浮在选区右下角。此时依赖 WDA_EXCLUDEFROMCAPTURE / 分层窗口。
        m_panelRectGlobal = QRect(m_regionLogical.right() - panelSize.width() + 1,
                                  m_regionLogical.bottom() + 8, panelSize.width(),
                                  panelSize.height());
        m_panelOutside = false;
        qWarning() << "长截图:面板在屏内找不到选区外的位置,将浮在选区内(依赖排除捕获)";
    }
    layoutWindows();
}

StitchHud::~StitchHud() {
    unregisterEsc();
    // 窗口用 WA_DeleteOnClose 会被 Qt 延后删除;析构里直接 delete 更干脆 ——
    // 它们此时已隐藏(见 hideHud),不存在"半隐藏状态下被销毁"的可见残影。
    if (m_frame) {
        m_frame->hide();
        m_frame->deleteLater();
        m_frame = nullptr;
    }
    if (m_panel) {
        m_panel->hide();
        m_panel->deleteLater();
        m_panel = nullptr;
    }
}

void StitchHud::layoutWindows() {
    if (m_frame) {
        m_frame->setGeometry(
            m_regionLogical.adjusted(-kFrameThickness, -kFrameThickness, kFrameThickness,
                                     kFrameThickness));
    }
    if (m_panel) m_panel->setGeometry(m_panelRectGlobal);
}

void StitchHud::showHud() {
    if (m_frame) m_frame->show();
    if (m_panel) m_panel->show();
    registerEsc();
}

void StitchHud::hideHud() {
    unregisterEsc();
    if (m_frame) m_frame->hide();
    if (m_panel) m_panel->hide();
}

void StitchHud::setStatus(const QString& text) {
    if (auto* panel = static_cast<StitchPanelWindow*>(m_panel)) {
        panel->status = text;
        panel->escUnavailable = m_escUnavailable;
        panel->update();
    }
}

void StitchHud::registerEsc() {
    if (m_escRegistered) return;
#ifdef Q_OS_WIN
    const HWND hwnd = m_panel ? reinterpret_cast<HWND>(m_panel->winId()) : nullptr;
    if (!hwnd) {
        m_escUnavailable = true;
        return;
    }
    // 采集期页面持有焦点,本进程收不到 Esc —— 只能临时抢一个全局热键。
    if (!::RegisterHotKey(hwnd, kEscHotkeyId, 0, VK_ESCAPE)) {
        m_escUnavailable = true;
        qWarning() << "长截图:全局 Esc 注册失败(可能被其它程序占用),"
                      "用户需用面板按钮收尾";
        return;
    }
    qApp->installNativeEventFilter(this);
    m_escRegistered = true;
#endif
}

bool StitchHud::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(eventType);
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    auto* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_HOTKEY && msg->wParam == kEscHotkeyId) {
        qInfo() << "长截图:Esc 收尾(全局热键)";
        emit finishRequested();
        return true;
    }
#else
    Q_UNUSED(message);
#endif
    return false;
}

void StitchHud::unregisterEsc() {
    if (!m_escRegistered) return;
#ifdef Q_OS_WIN
    qApp->removeNativeEventFilter(this);
    if (m_panel) ::UnregisterHotKey(reinterpret_cast<HWND>(m_panel->winId()), kEscHotkeyId);
#endif
    m_escRegistered = false;
}

} // namespace nb
