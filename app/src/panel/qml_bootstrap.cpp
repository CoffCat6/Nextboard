#include "qml_bootstrap.hpp"

#include "core_facade.hpp"
#include "clip_entry_model.hpp"
#include "panel_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QJSEngine>
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

    // 全局字体:统一 UI 字体族并开启抗锯齿。中文由 Qt 的回退链落到
    // "Microsoft YaHei UI"(显式列出,避免某些精简系统上回退到宋体)。
    // 未显式指定 family 的 QML Text / 各类控件都继承这里,是"字体显示"
    // 问题的唯一收敛点。
    QFont uiFont = QApplication::font();
    uiFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                        QStringLiteral("Microsoft YaHei"), QStringLiteral("Noto Sans CJK SC")});
    uiFont.setPixelSize(14);
    uiFont.setStyleStrategy(QFont::PreferAntialias);
    QApplication::setFont(uiFont);

    // ---- 单例注册(必须跨引擎可用) ----
    //
    // ⚠️ 不能用 qmlRegisterSingletonInstance:它把单例绑定到**单个** QQmlEngine。
    // 本项目有多个并存引擎(主面板 QQuickWidget + 独立翻译窗口 QQuickWidget),
    // 第二个引擎取到的是 null,日志表现为:
    //   "Singleton registered by registerSingletonInstance must only be accessed from one engine"
    //   "qmlRegisterSingletonType(): \"Core\" is not available because the callback returns a null pointer"
    //   "Theme.qml: Unable to assign [undefined] to bool" / "Core.translateText is not a function"
    // 即:翻译窗口里 Core.* 全部失效(无法翻译)。
    //
    // 改用回调式注册:回调在每个引擎解析该单例时被调用,返回同一个 C++ 实例;
    // CppOwnership 声明生命周期由 C++ 持有,引擎不得接管(否则多引擎会争抢析构)。
    // 该方案已由 tools/singleton_probe 实测验证(两引擎并存时均可用)。
    qmlRegisterSingletonType<CoreFacade>(
        "Nextboard.Core", 1, 0, "Core", [](QQmlEngine*, QJSEngine*) -> QObject* {
            auto* facade = &CoreFacade::instance();
            QQmlEngine::setObjectOwnership(facade, QQmlEngine::CppOwnership);
            return facade;
        });

    s_entries = new ClipEntryModel(qApp);
    // QML 单例类型名必须大写开头;QML 侧以 Entries.xxx 访问模型
    qmlRegisterSingletonType<ClipEntryModel>(
        "Nextboard.Core", 1, 0, "Entries", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QQmlEngine::setObjectOwnership(s_entries, QQmlEngine::CppOwnership);
            return s_entries;
        });

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
    // C++ 侧改动条目后(如右键「编辑」提交)也要刷新列表:
    // 编辑产物是新条目,不刷新的话列表看不到它。
    CoreFacade& facade = CoreFacade::instance();
    QObject::connect(&facade, &CoreFacade::clipChanged, &facade, [model] {
        model->refresh();
    });
    // 面板显示后立即重拉一次(隐藏期间积累的变化一次到位)
    panel->installEventFilter(new ShowRefresher(s_entries, panel));
}

} // namespace nb
