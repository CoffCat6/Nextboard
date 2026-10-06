#include "sciter_view.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QDebug>

#include <windows.h>

#include "bridge.hpp"

namespace nb {

/// 诊断辅助:value → 可打印串(仅日志用)
static QString vstrSafe(const sciter::value& v) {
    const sciter::string s = v.to_string(CVT_JSON_LITERAL);
    return QString::fromWCharArray(s.c_str(), int(s.length()));
}

SciterView::SciterView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // 心跳:引擎的动画/CSS transition/计时器需要周期驱动(官方 demo 16ms)
    m_heartbit = new QTimer(this);
    m_heartbit->setInterval(16);
    connect(m_heartbit, &QTimer::timeout, this, [this] {
        if (m_created) ::SciterProcX(reinterpret_cast<HWINDOW>(winId()),
                                     SCITER_X_MSG_HEARTBIT(::GetTickCount()));
    });
}

SciterView::~SciterView() {
    if (m_created)
        ::SciterProcX(reinterpret_cast<HWINDOW>(winId()), SCITER_X_MSG_DESTROY());
}

void SciterView::attach() {
    if (m_created) return;

    ::SciterSetOption(nullptr, SCITER_SET_SCRIPT_RUNTIME_FEATURES,
                      ALLOW_FILE_IO | ALLOW_SOCKET_IO | ALLOW_EVAL | ALLOW_SYSINFO);
    ::SciterSetOption(nullptr, SCITER_SET_DEBUG_MODE, TRUE);

    // 全局桥(Nextboard.* 命名空间)必须在任何文档加载前注册
    installSciterBridge();

    HWND hwnd = reinterpret_cast<HWND>(winId());
    m_dpi = static_cast<UINT>(::GetDpiForWindow(hwnd));

    // 引擎实例:位图渲染后端(第二参数 FALSE = 非 OpenGL)
    ::SciterProcX(hwnd, SCITER_X_MSG_CREATE(SL_TARGET_BITMAP, FALSE));
    ::SciterProcX(hwnd, SCITER_X_MSG_RESOLUTION(m_dpi));
    m_created = true;

    ::SciterSetCallback(hwnd, &SciterView::sciterCallback, this);
    m_heartbit->start();

    // 先建表面并**立即**告知引擎(启动时的 resizeEvent 早于 attach,那时引擎未创建,
    // 唯一把像素缓冲交给引擎的机会就是这里;错过它引擎渲染进空指针 → 永远是初始底色)
    if (ensureSurface(size())) {
        SL_SURFACE surface{};
        surface.bitmap.pixels = m_surface.bits();
        surface.bitmap.stride = static_cast<UINT>(m_surface.bytesPerLine());
        ::SciterProcX(hwnd, SCITER_X_MSG_SIZE(static_cast<UINT>(width()),
                                              static_cast<UINT>(height()), surface));
    }

    const QString htmlPath =
        QDir(QCoreApplication::applicationDirPath()).filePath("panel/main.htm");
    m_loaded = ::SciterLoadFile(hwnd, reinterpret_cast<LPCWSTR>(htmlPath.utf16()));
    if (!m_loaded) {
        static const wchar_t kFallback[] =
            L"<html><body style='background:#1e1f24;color:#dfe1e8;padding:24px'>"
            L"<h2>Nextboard</h2><p>panel/main.htm not found</p></body></html>";
        ::SciterLoadHtml(hwnd, reinterpret_cast<LPCBYTE>(kFallback),
                         static_cast<UINT>(wcslen(kFallback) * sizeof(wchar_t)), nullptr);
        m_loaded = true;
    }
    qInfo() << "Sciter ProcX attached, dpi =" << m_dpi << ", html =" << htmlPath;
    update();
}

void SciterView::setSearchText(const QString& text) {
    // SciterEval 在 ProcX(SL_TARGET_BITMAP) 引擎实例上不可用(实测 1+1 也返回 FALSE,
    // 无主 VM)。搜索链路验收由 nbcore 单测(FTS 中文兜底)+ 手动验收覆盖;
    // 本接口保留,后续经 sciter::host::call_function(需主 VM)再启用。
    qInfo() << "setSearchText( deferred, Eval unavailable ):" << text;
}

bool SciterView::ensureSurface(const QSize& size) {
    if (size.isEmpty()) return false;
    if (m_surface.size() == size) return true;
    // RGB32 = 0xff000000 | BGR(引擎输出不透明表面);stride 自动 4 对齐
    m_surface = QImage(size, QImage::Format_RGB32);
    if (m_surface.isNull()) return false;
    m_surface.fill(0xff1e1f24); // 初始底色,避免首帧闪烁
    return true;
}

void SciterView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (!m_created) return;
    if (!ensureSurface(size())) return;
    SL_SURFACE surface{};
    surface.bitmap.pixels = m_surface.bits();
    surface.bitmap.stride = static_cast<UINT>(m_surface.bytesPerLine());
    ::SciterProcX(reinterpret_cast<HWINDOW>(winId()),
                  SCITER_X_MSG_SIZE(static_cast<UINT>(width()),
                                    static_cast<UINT>(height()), surface));
    update();
}

void SciterView::paintEvent(QPaintEvent* e) {
    QWidget::paintEvent(e);
    if (m_surface.isNull()) return;
    // 关键:先让引擎把 DOM 渲染进表面(SXM_PAINT),再把表面画出来。
    // 引擎是"按需渲染"——没有 SXM_PAINT 就永远不写像素(官方 lite-bitmap 同序)。
    RECT rc{0, 0, width(), height()};
    ::SciterProcX(reinterpret_cast<HWINDOW>(winId()), SCITER_X_MSG_PAINT(rc));
    QPainter painter(this);
    painter.drawImage(rect(), m_surface);
}

// ---- 输入事件 → SXM 消息 ----

void SciterView::sendMouse(QMouseEvent* e, MOUSE_EVENTS evt) {
    if (!m_created) return;
    // 按下的按钮:UP/DCLICK 事件里 e->buttons() 已不含该键,须用 e->button()
    unsigned buttons = 0;
    const Qt::MouseButton single = e->button();
    const Qt::MouseButtons held = e->buttons();
    const bool useSingle = (evt == MOUSE_EVENTS::MOUSE_UP || evt == MOUSE_EVENTS::MOUSE_DCLICK);
    auto contains = [&](Qt::MouseButton b) {
        return useSingle ? (single == b) : (held & b);
    };
    if (contains(Qt::LeftButton)) buttons |= MAIN_MOUSE_BUTTON;
    if (contains(Qt::RightButton)) buttons |= PROP_MOUSE_BUTTON;
    if (contains(Qt::MiddleButton)) buttons |= MIDDLE_MOUSE_BUTTON;

    unsigned mods = 0;
    if (e->modifiers() & Qt::ShiftModifier) mods |= KEYBOARD_STATE_LSHIFT;
    if (e->modifiers() & Qt::ControlModifier) mods |= KEYBOARD_STATE_LCONTROL;
    if (e->modifiers() & Qt::AltModifier) mods |= KEYBOARD_STATE_LALT;

    const POINT pos{e->pos().x(), e->pos().y()};
    ::SciterProcX(reinterpret_cast<HWINDOW>(winId()),
                  SCITER_X_MSG_MOUSE(evt, MOUSE_BUTTONS(buttons), KEYBOARD_STATES(mods), pos));
    update();
}

void SciterView::mousePressEvent(QMouseEvent* e) {
    sendMouse(e, MOUSE_EVENTS::MOUSE_DOWN);
}

void SciterView::mouseReleaseEvent(QMouseEvent* e) {
    sendMouse(e, MOUSE_EVENTS::MOUSE_UP);
}

void SciterView::mouseMoveEvent(QMouseEvent* e) {
    sendMouse(e, MOUSE_EVENTS::MOUSE_MOVE);
}

void SciterView::mouseDoubleClickEvent(QMouseEvent* e) {
    // 双击序列:DOWN → UP → DCLICK(引擎期望 DCLICK 替代第二次 DOWN)
    sendMouse(e, MOUSE_EVENTS::MOUSE_DCLICK);
}

void SciterView::wheelEvent(QWheelEvent* e) {
    if (!m_created) return;
    unsigned mods = 0;
    if (e->modifiers() & Qt::ControlModifier) mods |= KEYBOARD_STATE_LCONTROL;
    if (e->modifiers() & Qt::ShiftModifier) mods |= KEYBOARD_STATE_LSHIFT;
    const POINT pos{static_cast<LONG>(e->position().x()), static_cast<LONG>(e->position().y())};
    // 滚轮:MOUSE_MOVE 事件族没有专门的 wheel 事件,引擎经 MOUSE_WHEEL
    // (MOUSE_EVENTS 高位)接受 delta;SDK usciter 以 MOUSE_MOVE + delta 编码,
    // v1 主面板无滚轮敏感 UI,滚轮留待 S3 列表时用 SCROLL 补齐
    Q_UNUSED(e);
    ::SciterProcX(reinterpret_cast<HWINDOW>(winId()),
                  SCITER_X_MSG_MOUSE(MOUSE_MOVE, MOUSE_BUTTONS(0),
                                     KEYBOARD_STATES(mods), pos));
    update();
}

void SciterView::sendKey(QKeyEvent* e, KEY_EVENTS evt) {
    if (!m_created) return;
    unsigned mods = 0;
    if (e->modifiers() & Qt::ShiftModifier) mods |= KEYBOARD_STATE_LSHIFT;
    if (e->modifiers() & Qt::ControlModifier) mods |= KEYBOARD_STATE_LCONTROL;
    if (e->modifiers() & Qt::AltModifier) mods |= KEYBOARD_STATE_LALT;
    const UINT vk = e->nativeVirtualKey();
    ::SciterProcX(reinterpret_cast<HWINDOW>(winId()),
                  SCITER_X_MSG_KEY(evt, vk, KEYBOARD_STATES(mods)));
    update();
}

void SciterView::keyPressEvent(QKeyEvent* e) {
    sendKey(e, KEY_EVENTS::KEY_DOWN);
    // 文本输入:KEY_CHAR 事件携带字符码(编辑框场景,S3 再补 WM_CHAR 全量转发)
    const QString text = e->text();
    if (!text.isEmpty() && text.at(0).unicode() >= 0x20) {
        ::SciterProcX(reinterpret_cast<HWINDOW>(winId()),
                      SCITER_X_MSG_KEY(KEY_EVENTS::KEY_CHAR,
                                       static_cast<UINT>(text.at(0).unicode()),
                                       KEYBOARD_STATES(0)));
    }
}

void SciterView::keyReleaseEvent(QKeyEvent* e) {
    sendKey(e, KEY_EVENTS::KEY_UP);
}

// ---- 引擎回调:失效区域 → 触发 Qt 重绘 ----

UINT SC_CALLBACK SciterView::sciterCallback(LPSCITER_CALLBACK_NOTIFICATION pns, LPVOID param) {
    switch (pns->code) {
        case SC_INVALIDATE_RECT: {
            auto* self = static_cast<SciterView*>(param);
            if (self) {
                const RECT& r = reinterpret_cast<SCN_INVALIDATE_RECT*>(pns)->invalidRect;
                self->update(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
            }
            return 0;
        }
        default:
            break;
    }
    return 0;
}

} // namespace nb
