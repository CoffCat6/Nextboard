#include "qml_bootstrap.hpp"

#include "core_facade.hpp"
#include "clip_entry_model.hpp"
#include "panel_window.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStyleHints>
#include <QVariant>

namespace nb {

static ClipEntryModel* s_entries = nullptr;

void setupQmlEnvironment() {
    // Basic 风格:控件(Button/ComboBox/SpinBox…)的配色跟随 QPalette,
    // 而 QPalette 现在由 CoreFacade::applyTheme 按设置注入(亮/暗两套),
    // 这里不再写死暗色。
    QQuickStyle::setStyle("Basic");

    qmlRegisterSingletonInstance("Nextboard.Core", 1, 0, "Core", &CoreFacade::instance());

    s_entries = new ClipEntryModel(qApp);
    // QML 单例类型名必须大写开头;QML 侧以 Entries.xxx 访问模型
    qmlRegisterSingletonInstance("Nextboard.Core", 1, 0, "Entries", s_entries);

    // 按 settings.theme 应用一次(QPalette + Theme 单例),并跟随系统配色变化
    // (仅 theme=system 会真正改变)
    CoreFacade& facade = CoreFacade::instance();
    facade.refreshThemeFromSettings();
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     &facade, &CoreFacade::reapplyTheme);
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
