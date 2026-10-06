#include "qml_bootstrap.hpp"

#include "core_facade.hpp"
#include "clip_entry_model.hpp"
#include "panel_window.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QVariant>

namespace nb {

static ClipEntryModel* s_entries = nullptr;

void setupQmlEnvironment() {
    // Basic 风格 + 全局暗色调色板:Basic 控件(Button/ComboBox/SpinBox 等)的
    // 默认配色跟随系统 QPalette,不注入则亮灰刺眼。
    QQuickStyle::setStyle("Basic");
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(0x1e1f24));
    dark.setColor(QPalette::WindowText, QColor(0xdfe1e8));
    dark.setColor(QPalette::Base, QColor(0x26272e));
    dark.setColor(QPalette::AlternateBase, QColor(0x2d2e37));
    dark.setColor(QPalette::Button, QColor(0x2d2e37));
    dark.setColor(QPalette::ButtonText, QColor(0xdfe1e8));
    dark.setColor(QPalette::Text, QColor(0xdfe1e8));
    dark.setColor(QPalette::PlaceholderText, QColor(0x9aa0ab));
    dark.setColor(QPalette::Highlight, QColor(0x4f8cff));
    dark.setColor(QPalette::HighlightedText, QColor(0xffffff));
    dark.setColor(QPalette::ToolTipBase, QColor(0x2d2e37));
    dark.setColor(QPalette::ToolTipText, QColor(0xdfe1e8));
    dark.setColor(QPalette::BrightText, QColor(0xffffff));
    dark.setColor(QPalette::Link, QColor(0x4f8cff));
    QGuiApplication::setPalette(dark);

    qmlRegisterSingletonInstance("Nextboard.Core", 1, 0, "Core", &CoreFacade::instance());

    s_entries = new ClipEntryModel(qApp);
    // QML 单例类型名必须大写开头;QML 侧以 Entries.xxx 访问模型
    qmlRegisterSingletonInstance("Nextboard.Core", 1, 0, "Entries", s_entries);
}

namespace {
class ShowRefresher : public QObject {
public:
    ShowRefresher(ClipEntryModel* m, QObject* parent) : QObject(parent), m_model(m) {}
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::Show)
            m_model->refreshNow();
        return QObject::eventFilter(obj, e);
    }
    ClipEntryModel* m_model;
};
} // namespace

void qmlAttachPanel(PanelWindow* panel) {
    if (!panel) return;
    // nbcore WM_APP 通知 → 模型刷新(信号中转,去抖在模型内);s_entries 是
    // 静态存储,lambda 内直接按名访问,不做简单捕获
    ClipEntryModel* model = s_entries;
    QObject::connect(panel, &PanelWindow::clipChanged, panel, [model] {
        model->refresh();
    });
    // 面板显示后立即重拉一次(隐藏期间积累的变化一次到位)
    panel->installEventFilter(new ShowRefresher(s_entries, panel));
}

} // namespace nb
