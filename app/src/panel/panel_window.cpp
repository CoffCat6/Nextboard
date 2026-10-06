#include "panel_window.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QScreen>
#include <QTimer>

#include <windows.h>

#include "sciter_view.hpp"

namespace nb {
namespace {
constexpr int BLUR_HIDE_DELAY_MS = 200; // 第一代结论:防拖动标题栏的虚假失焦
constexpr int PANEL_W = 800;
constexpr int PANEL_H = 600;
} // namespace

PanelWindow::PanelWindow(CoreBridge& core, QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool), m_core(core) {
    setWindowTitle("Nextboard");
    resize(PANEL_W, PANEL_H);
    setupUi();

    // 失焦自动隐藏(延迟复查,第一代 lib.rs 同参数)。
    // 仅在窗口**曾获得过焦点**之后启用:启动时 activateWindow 可能失败
    // (系统前台锁),此时若无脑 hide 会让应用"启动即退出"。
    m_blurTimer = new QTimer(this);
    m_blurTimer->setSingleShot(true);
    m_blurTimer->setInterval(BLUR_HIDE_DELAY_MS);
    connect(m_blurTimer, &QTimer::timeout, this, [this] {
        if (m_everFocused && !isActiveWindow()) hide();
    });
    connect(this, &QWidget::windowIconChanged, this, [this] { m_everFocused = isActiveWindow(); });
}

PanelWindow::~PanelWindow() = default;

void PanelWindow::setupUi() {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_sciter = new SciterView(this);
    layout->addWidget(m_sciter);
}

void PanelWindow::showAtCursor() {
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        move(avail.center() - rect().center());
    }
    show();
    raise();
    activateWindow();
}

HWND PanelWindow::nativeHandle() const {
    return reinterpret_cast<HWND>(winId());
}

void PanelWindow::applySearchText(const QString& text) {
    if (m_sciter) m_sciter->setSearchText(text);
}

void PanelWindow::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    m_blurTimer->stop();
    // showAtCursor 的 activateWindow 是异步的:give the WM a beat, then re-check
    QTimer::singleShot(120, this, [this] {
        if (isActiveWindow()) m_everFocused = true;
    });
}

void PanelWindow::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
}

void PanelWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
}

bool PanelWindow::nativeEvent(const QByteArray& eventType, void* message, qint64* result) {
    if (eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        // ⚠️ 此处禁止调用 winId():窗口创建期间(WM_NCCREATE 等)调用会递归触发
        // CreateWindowEx → 创建失败(Qt 报 "CreateWindowEx failed")。用缓存 HWND。
        if (msg->hwnd != m_cachedHwnd)
            return QWidget::nativeEvent(eventType, message, result);
        // nbcore 通知:剪贴板变化 → 通知 JS 刷新(经 SciterView 转发到文档)
        if (msg->message == nb::WM_APP_CLIP_CHANGED) {
            emit clipChanged();
            if (result) *result = 0;
            return true;
        }
        if (msg->message == nb::WM_APP_SETTINGS_CHANGED) {
            emit clipChanged(); // 设置变化也刷新一次(监听开关等影响列表)
            if (result) *result = 0;
            return true;
        }
        // 失焦自动隐藏
        if (msg->message == WM_ACTIVATE) {
            if (LOWORD(msg->wParam) == WA_INACTIVE) {
                m_blurTimer->start();
            } else {
                m_blurTimer->stop();
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

bool PanelWindow::event(QEvent* e) {
    if (e->type() == QEvent::WinIdChange) {
        // 缓存 HWND(nativeEvent 禁用 winId() 的替代)
        m_cachedHwnd = reinterpret_cast<HWND>(winId());
        // winId 就绪后再初始化 Sciter(需要 HWND)
        QTimer::singleShot(0, this, [this] {
            if (m_sciter) m_sciter->attach();
        });
    }
    return QWidget::event(e);
}

} // namespace nb
