#include "qml_bootstrap.hpp"

#include "../app_font.hpp"
#include "../host/app_paths.hpp"

#include "core_facade.hpp"
#include "clip_entry_model.hpp"
#include "panel_window.hpp"

#include <QApplication>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLabel>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWidget>
#include <QStyleHints>
#include <QVariant>

namespace nb {

static ClipEntryModel* s_entries = nullptr;

void setupQmlEnvironment() {
    // ⚠️ 字体必须**最先**装,且必须在 QQuickStyle 之前:
    //   ① 缺字回退替换表要在任何 QFont 解析前生效(QFont 文档:替换后需重建
    //      QFont 才生效),晚一步装,已解析过的控件字体仍是旧的;
    //   ② Basic 控件的默认 font 取自应用字体,QApplication::setFont 晚装
    //      会让控件带着旧字体进场景图。
    // 字体族名的唯一来源是 app_font.hpp —— 详见那里关于"QML 的 font.family
    // 是单值、会覆盖应用字体的回退链,导致中文掉到宋体"的说明。
    installApplicationFont();

    // Basic 风格:控件(Button/ComboBox/SpinBox…)的配色跟随 QPalette,
    // 而 QPalette 现在由 CoreFacade::applyTheme 按设置注入(亮/暗两套),
    // 这里不再写死暗色。
    QQuickStyle::setStyle("Basic");

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

bool handleQmlLoadFailure(QQuickWidget* quick, QBoxLayout* layout) {
    if (!quick || quick->status() != QQuickWidget::Error) return false;

    // 日志留全量(便于定位),界面只显示第一条 —— 第一条通常就是根因。
    qWarning() << "QML load errors:" << quick->errors();
    if (!layout) return true;

    quick->hide();
    auto* label = new QLabel;
    label->setWordWrap(true);
    label->setMargin(20);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const QString first = quick->errors().isEmpty()
                              ? QStringLiteral("未知错误")
                              : quick->errors().first().toString();
    label->setText(QStringLiteral(
                       "<b>界面加载失败</b><br><br>%1<br><br>"
                       "<span style='color:#888'>完整信息见 %2</span>")
                       .arg(first.toHtmlEscaped(),
                            appDataFile(QStringLiteral("nextboard.log")).toHtmlEscaped()));
    layout->addWidget(label, 1);
    return true;
}

namespace {
/// 列表的"在线/离线"由两件事共同决定:**面板是否可见** 与 **当前停在哪一页**;
/// 只有"可见 且 停在列表页"才需要持有列表行,其余一律释放或冻结。
///
/// 动机(内存收口 S6.1/S6.3):列表行的图片纹理是本应用最大的一块常驻开销 ——
/// 面板大多数时间隐藏(托盘常驻 + 失焦即隐),进翻译页时列表更是用不到。
/// 隐藏期间剪贴板通知仍会刷新模型,若不拦住,释放/冻结就白做了。
///
/// 两种离线策略(实测对比,docs/06-memory-optimization.md §6):
///   release():清空行 → QML 委托整批销毁。**首个循环留下 +42MB 不可回落的堆增长**
///             (销毁 38 张复杂 Card 的 churn 撑大私有堆),且隐藏态 137MB 反而
///             高于显示态 122MB;纹理释放其实已由 setPersistentSceneGraph(false) 负责。
///   freeze() :保留行、只拦刷新。隐藏→显示若期间无变化则**不重载**(不销毁重建委托),
///             故无 churn。
class PanelListLifecycle : public QObject {
public:
    /// 列表"离线"(面板隐藏 **或** 停在翻译页)时保留行(冻结)而非清空(释放)。
    /// 两轮实测:
    ///   - 隐藏态 freeze 让读数对齐显示态,并消除首个循环的 +42MB 台阶;
    ///   - 原先"进翻译页仍 release"的分支是**唯一还在整批销毁委托**的路径 ——
    ///     列表⇄翻译每来回一轮私有字节 +3~4MB 且单调不回落(4 轮实测 100.8 →
    ///     107.3 → 109.3 → 111.1),正是 release 的堆 churn。翻译页并不需要一个
    ///     空列表来换取这份驻留(实测保留行只多 ~5MB)。
    /// 若将来在"图片条目极多"的压力场景下发现保留行更贵,改回 false 即可,
    /// 两种策略都已实测过,回退零成本。
    static constexpr bool kFreezeWhenOffline = true;

    PanelListLifecycle(PanelWindow* panel, ClipEntryModel* model)
        : QObject(panel), m_panel(panel), m_model(model) {}

    bool eventFilter(QObject* obj, QEvent* e) override {
        // 自己记可见性而不问 isVisible():事件处理器里它的取值时机不直观,
        // 而 Show/Hide 是本过滤器亲眼看到的,最可靠。
        if (e->type() == QEvent::Show) {
            m_visible = true;
            sync();
        } else if (e->type() == QEvent::Hide) {
            m_visible = false;
            sync();
        }
        return QObject::eventFilter(obj, e);
    }

    /// 形态切换后重新判定:回到"可见 + 列表页"才解冻重载,其余离线态一律冻结。
    void sync() {
        if (m_visible && m_panel->mode() == PanelWindow::Mode::List) {
            m_model->acquire();
        } else if (kFreezeWhenOffline) {
            m_model->freeze();
        } else {
            // 旧策略(实验开关关闭时):隐藏与进翻译页都清空行 —— 委托整批销毁,
            // 实测每来回一轮 +3~4MB 且单调不回落。
            m_model->release();
        }
    }

private:
    PanelWindow* m_panel;
    ClipEntryModel* m_model;
    bool m_visible = false;
};
} // namespace

void qmlAttachPanel(PanelWindow* panel) {    if (!panel) return;
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
    // 面板显隐 + 形态切换 → 列表释放/重拉(见 PanelListLifecycle)
    auto* lifecycle = new PanelListLifecycle(panel, s_entries);
    panel->installEventFilter(lifecycle);
    QObject::connect(panel, &PanelWindow::modeChanged, panel, [lifecycle] { lifecycle->sync(); });
    // 初始步对齐:**起飞态即离线**。
    // 过滤器只认自己看到的 Show/Hide,而 `--hidden` 开机自启压根不会 Show
    // (panel_window.cpp 的 prepareHiddenStart),于是 `m_visible` 保持初值 false
    // 却从不触发 sync() —— 模型既没冻结也没释放,隐藏期间的剪贴板通知会照常
    // 触发 reload,把整页行与纹理装进内存。开机后长时间无人呼出,这份驻留
    // 就白占了(与 S6.1 的收口目标相反)。
    // 这里补一次,让"从未显示过"与"显示后又隐藏"落在同一状态。
    lifecycle->sync();
    // 待办到点 → 列表与角标立即重拉(docs/05-todo.md §4.3)。
    // ⚠️ 走 refreshNow 绕开 200ms 去抖:提醒到点后角标/列表晚半秒,
    //    用户会读成"提示了但列表里没有"。
    QObject::connect(panel, &PanelWindow::todoDue, panel, [model] {
        model->refreshNow();
    });
}

} // namespace nb


