#include "editor/annotation_toolbar.hpp"

#include "ui_theme.hpp"

#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

namespace nb {
namespace {

/// 调色板色块的图标(与编辑器原先的 swatchIcon 同一造型)。
QIcon swatchIcon(const QColor& color) {
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    // 色块描边跟随主题:深色主题描深灰、浅色主题描浅灰(写死深灰时,浅色主题下
    // 每块色卡都像被黑框框住,整行显得很脏)。
    painter.setPen(QPen(ui::themeTokens().borderStrong, 1));
    painter.drawRoundedRect(QRectF(0.5, 0.5, 13, 13), 3, 3);
    painter.end();
    return QIcon(pixmap);
}

} // namespace

AnnotationToolbar::AnnotationToolbar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_toolsBar = new QToolBar(this);
    m_paramsBar = new QToolBar(this);
    for (QToolBar* bar : {m_toolsBar, m_paramsBar}) {
        bar->setMovable(false);
        bar->setIconSize(QSize(kIconPx, kIconPx));
        // 只显图标:文字进 ToolTip —— 这是"编辑页文字太多"的直接解法
        bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        // ⚠️ QToolBar 会**自己画底板**(PE_PanelToolBar,取 QPalette::Window)。两个宿主
        //    都要求"底板由外层画":遮罩浮条由 OverlayWidget::drawSubbar 画面板(主题
        //    surface + 用户档位 alpha),编辑器由窗口背景承担。不置透明的话:
        //      - 浮条上会多出一层不透明条,用户调"透明档"看不出变化;
        //      - 浅色主题下这层浅色板压在半透浮条上,底板描边与档位全部失效。
        bar->setStyleSheet(QStringLiteral("QToolBar{background:transparent;border:none;}"));
    }

    // 参数行 = 「粗细/字号/撤销重做(左)」+「宿主动作按钮(右)」。
    // 动机(用户反馈):原先所有按钮都堆在左下角,工具与"贴图/保存/取消"混在一起,
    // 分不清哪些是"选工具"、哪些是"执行动作"。改成工具在左、动作在右后,
    // 视线流向是"左侧选工具 → 右下角执行"。
    m_paramsRow = new QWidget(this);
    auto* paramsLayout = new QHBoxLayout(m_paramsRow);
    paramsLayout->setContentsMargins(0, 0, 0, 0);
    paramsLayout->setSpacing(0);
    paramsLayout->addWidget(m_paramsBar);
    paramsLayout->addStretch(1);   // 把右侧动作推到最右

    layout->addWidget(m_toolsBar);
    layout->addWidget(m_paramsRow);

    rebuildActions();
    refresh();
}

void AnnotationToolbar::setTrailingWidget(QWidget* trailing) {
    if (m_trailing == trailing) return;
    // 从旧位置摘除(不销毁 —— 所有权在宿主)
    if (m_trailing) m_trailing->setParent(nullptr);
    m_trailing = trailing;
    if (!m_trailing) return;
    // 挂到参数行末尾:布局里 addStretch(1) 在前,故这一项会被推到最右。
    if (m_paramsRow) {
        if (auto* hl = qobject_cast<QHBoxLayout*>(m_paramsRow->layout())) hl->addWidget(m_trailing);
    }
}

void AnnotationToolbar::setOverlayMode(bool on) {
    if (m_overlayMode == on) return;
    m_overlayMode = on;
    // 浮条没有可拖画布:手型无意义。直接不建这个按钮比"建了再隐藏"省事,
    // 且 rebuildActions 里据此跳过 → 工具行第一个就是画笔。
    if (m_panAction) { m_toolsBar->removeAction(m_panAction); m_panAction = nullptr; }
    m_panActive = false;   // 一进来就是可绘制态(遮罩上点工具就该画)
    rebuildActions();
    refresh();
}

void AnnotationToolbar::activatePan() {
    if (!m_panAction) return;
    m_panAction->setChecked(true);   // 互斥组:会自动取消绘图工具的选中
    m_panActive = true;
    emit drawingModeChanged(false);
}

void AnnotationToolbar::setEngine(AnnotationEngine* engine) {
    m_engine = engine;
    setEnabled(engine != nullptr);
    refresh();
}

void AnnotationToolbar::reloadIcons() {
    // 主题切换后描边色要重取:QIcon 是即位图,不重建就还是旧色。
    // 做法与"按需重画"一致 —— 直接整体重建动作最省事(动作数量固定且很少)。
    rebuildActions();
    refresh();
}

void AnnotationToolbar::rebuildActions() {
    const QColor ink = iconInkFor(palette());

    m_toolsBar->clear();
    m_paramsBar->clear();

    // ---- 第一行:手型 + 工具 + 颜色 ----
    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);

    // 手型(画布模式,不是绘图工具):选中它 = 不画、只拖画面。
    // **这是"点了工具回不到抓手"的解法** —— 工具行里始终有一个退出绘制的入口。
    // 遮罩浮条模式跳过(遮罩上没有可拖画布,见 setOverlayMode)。
    m_panAction = m_overlayMode ? nullptr
                                : m_toolsBar->addAction(barIcon(BarIcon::Pan, kIconPx, ink),
                                                        QStringLiteral("拖动画面"));
    if (m_panAction) {
        m_panAction->setCheckable(true);
        m_panAction->setActionGroup(m_toolGroup);
        m_panAction->setToolTip(QStringLiteral("拖动画面(不绘制)"));
        m_panAction->setData(-1);   // -1 = 手型(与 EditorTool 的枚举值区分开)
        connect(m_panAction, &QAction::triggered, this, [this] {
            m_panActive = true;
            emit drawingModeChanged(false);
            refresh();
        });
    }

    for (EditorTool tool : toolbarTools()) {
        // 裁剪只在编辑窗暴露(与手型同一手法):浮条的"裁剪"就是重新框选,
        // 且裁剪的 Enter/Esc 确认键位会撞上浮条会话自身的完成/取消键位。
        if (m_overlayMode && tool == EditorTool::Crop) continue;
        QAction* action = m_toolsBar->addAction(toolIcon(tool, kIconPx, ink), toolLabel(tool));
        action->setCheckable(true);
        action->setActionGroup(m_toolGroup);
        action->setToolTip(toolLabel(tool));
        // 存枚举值:引擎侧程序化改工具(取色器采完回退)时据此同步选中态
        action->setData(int(tool));
        connect(action, &QAction::triggered, this, [this, tool] {
            if (!m_engine) return;
            m_panActive = false;          // 选了绘图工具 → 退出手型
            m_engine->setTool(tool);
            // 取色器选完会立刻回退到上一个绘图工具,故把实际生效的工具报出去
            emit toolChanged(m_engine->tool());
            emit drawingModeChanged(true);
            refresh();
        });
        // 颜色/粗细/字号随工具切换也要同步到浮条宿主(它自己存了一份成员值)
    }
    m_toolsBar->addSeparator();

    m_colorGroup = new QActionGroup(this);
    m_colorGroup->setExclusive(true);
    for (const QColor& color : annotationPalette()) {
        QAction* action = m_toolsBar->addAction(swatchIcon(color), color.name());
        action->setCheckable(true);
        action->setActionGroup(m_colorGroup);
        action->setToolTip(color.name());
        // 存色值:取色器采到调色板外的颜色时要据此清空全部选中
        action->setData(color);
        connect(action, &QAction::triggered, this, [this, color] {
            if (m_engine) m_engine->setColor(color);
            emit colorChanged(color);   // 浮条宿主据此同步 m_annoColor
            refresh();
        });
    }

    // ---- 第二行:粗细 + 字号 + 撤销/重做 ----
    m_sizeGroup = new QActionGroup(this);
    m_sizeGroup->setExclusive(true);
    for (int size : strokeSizes()) {
        const QString tip = QStringLiteral("粗细 %1").arg(size);
        QAction* action = m_paramsBar->addAction(strokeIcon(size, kIconPx, ink), tip);
        action->setCheckable(true);
        action->setActionGroup(m_sizeGroup);
        action->setToolTip(tip);
        action->setData(size);
        connect(action, &QAction::triggered, this, [this, size] {
            if (m_engine) m_engine->setStrokeWidth(size);
            emit strokeWidthChanged(size);   // 浮条宿主据此同步 m_penSize
            refresh();
        });
    }
    m_paramsBar->addSeparator();

    m_fontGroup = new QActionGroup(this);
    m_fontGroup->setExclusive(true);
    for (int size : textFontSizes()) {
        const QString tip = QStringLiteral("字号 %1").arg(size);
        QAction* action = m_paramsBar->addAction(fontSizeIcon(size, kIconPx, ink), tip);
        action->setCheckable(true);
        action->setActionGroup(m_fontGroup);
        action->setToolTip(tip);
        action->setData(size);
        connect(action, &QAction::triggered, this, [this, size] {
            if (m_engine) m_engine->setFontSize(size);
            emit fontSizeChanged(size);   // 浮条宿主据此同步 m_fontSize
            refresh();
        });
    }
    m_paramsBar->addSeparator();

    QAction* undo = m_paramsBar->addAction(barIcon(BarIcon::Undo, kIconPx, ink),
                                           QStringLiteral("撤销 (Ctrl+Z)"));
    undo->setToolTip(undo->text());
    connect(undo, &QAction::triggered, this, [this] {
        if (m_engine) m_engine->undoStack()->undo();
        refresh();
    });
    QAction* redo = m_paramsBar->addAction(barIcon(BarIcon::Redo, kIconPx, ink),
                                           QStringLiteral("重做 (Ctrl+Y)"));
    redo->setToolTip(redo->text());
    connect(redo, &QAction::triggered, this, [this] {
        if (m_engine) m_engine->undoStack()->redo();
        refresh();
    });

    // (参数按钮的启用态统一由 refresh() 决定:手型禁用 / 非文字工具禁用字号)
}

void AnnotationToolbar::refresh() {
    if (!m_engine) return;

    if (m_toolGroup) {
        // 手型激活时选中的是手型按钮(data = -1),否则按引擎当前工具选中
        const int want = m_panActive ? -1 : int(m_engine->tool());
        for (QAction* action : m_toolGroup->actions()) {
            if (action->data().toInt() == want) {
                action->setChecked(true);   // 互斥组:选中一个会自动取消其余
                break;
            }
        }
    }
    // 手型模式下颜色/粗细/字号无意义 → 禁用(不隐藏,避免工具条宽度跳动)。
    // 裁剪同理:它不消费这些参数。字号另有"仅文字工具"的启用条件。
    const bool drawing = !m_panActive && m_engine->tool() != EditorTool::Crop;
    if (m_sizeGroup) for (QAction* a : m_sizeGroup->actions()) a->setEnabled(drawing);
    if (m_colorGroup) for (QAction* a : m_colorGroup->actions()) a->setEnabled(drawing);
    if (m_fontGroup) for (QAction* a : m_fontGroup->actions())
        a->setEnabled(drawing && m_engine->tool() == EditorTool::Text);
    if (m_colorGroup) {
        // 互斥组无法直接取消选中:临时关掉互斥再逐项设置,最后恢复
        const bool exclusive = m_colorGroup->isExclusive();
        m_colorGroup->setExclusive(false);
        for (QAction* action : m_colorGroup->actions())
            action->setChecked(action->data().value<QColor>() == m_engine->color());
        m_colorGroup->setExclusive(exclusive);
    }
    if (m_sizeGroup) {
        for (QAction* action : m_sizeGroup->actions())
            action->setChecked(action->data().toInt() == m_engine->strokeWidth());
    }
    if (m_fontGroup) {
        // 启用态已在上方统一处理(手型禁用 / 非文字工具禁用);这里只同步选中值
        for (QAction* action : m_fontGroup->actions())
            action->setChecked(action->data().toInt() == m_engine->fontSize());
    }
}

} // namespace nb
