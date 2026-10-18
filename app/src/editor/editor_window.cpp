#include "editor_window.hpp"

#include "annotation_engine.hpp"
#include "annotation_toolbar.hpp"
#include "tool_icons.hpp"

#include "host/memory_probe.hpp"
#include "host/core_bridge.hpp"
#include "host/image_save.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QPushButton>
#include <QLabel>
#include <QtConcurrent/QtConcurrentRun>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLineF>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QResizeEvent>
#include <QScreen>
#include <QToolBar>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <functional>

namespace nb {

namespace {


/// 工具按钮上的色块图标。
QIcon swatchIcon(const QColor& color) {
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(QPen(QColor(0x33, 0x35, 0x3D), 1));
    painter.drawRoundedRect(QRectF(0.5, 0.5, 13, 13), 3, 3);
    painter.end();
    return QIcon(pixmap);
}

} // namespace

/// 画布视图:把鼠标位置换算成**场景坐标**(= 底图物理像素)后转给窗口处理。
/// 文字编辑期间打开 passthrough,让事件回到 QGraphicsView,光标才能正常交互。
/// (带 Q_OBJECT,故不能放进匿名命名空间——AUTOMOC 需要外部链接。)
class EditorCanvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit EditorCanvas(QGraphicsScene* scene, QWidget* parent = nullptr)
        : QGraphicsView(scene, parent) {}

    void setPassthrough(bool on) { m_passthrough = on; }

    /// 当前工具的光标。**存 QCursor 而不是 Qt::CursorShape** ——
    /// 绘图工具用的是自定义位图光标(crosshairCursor,见 tool_icons),
    /// 它没有对应的形状枚举;形状枚举只能表达 Qt 内置那几种(用户觉得不好看的就是它)。
    void setToolCursor(const QCursor& cursor) {
        m_toolCursor = cursor;
        if (!m_passthrough) viewport()->setCursor(cursor);
    }

    /// 适配显示:重置为指定缩放,并清除"用户已手动缩放"标记。
    void resetZoom(qreal zoom) {
        m_userZoomed = false;
        m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
        resetTransform();
        scale(m_zoom, m_zoom);
    }

    bool userZoomed() const { return m_userZoomed; }
    /// 清掉"用户已手动缩放"标记 → 恢复随窗口自动适配。
    void clearUserZoom() { m_userZoomed = false; }
    /// 标记"用户已手动缩放" → 不再自动适配(显式选定比例后,窗口变化不该覆盖它)。
    void markUserZoomed() { m_userZoomed = true; }

    /// 抓手平移开关(空格按住 / 中键拖)。
    /// 动机:画布原先 setDragMode(NoDrag) —— 放大后只能拖滚动条挪画面,
    /// 左键一按就是在画。平移是"放大看细节"的必要配套。
    void setPanning(bool on) {
        if (m_panning == on) return;
        m_panning = on;
        setDragMode(on ? QGraphicsView::ScrollHandDrag : QGraphicsView::NoDrag);
        // 平移用内置"张开的手"(它就是最贴切的表达),绘制用工具光标
        viewport()->setCursor(on ? QCursor(Qt::OpenHandCursor) : m_toolCursor);
    }
    /// 进入"可绘制"状态(选中了绘图工具)。默认是抓手,选中工具后才可画。
    void enterDrawingMode() { setPanning(false); }
    /// 回到抓手态(未选工具 / 文字这段已确认)。
    void enterPanMode() { setPanning(true); }
    bool panning() const { return m_panning; }
    /// 空格键:按住临时进平移态(与主流图像编辑器一致)。
    /// 需 public —— EditorWindow 的 keyPress/keyRelease 会调它。
    void setSpaceHeld(bool held) {
        m_spaceHeld = held;
        setPanning(held);
    }

    /// 文字点击探针(§18.2):返回 true = 这次按下落在"正在编辑的文字**之外**",
    /// 应当结束这一段、并就地再落一段。
    ///
    /// 用注入式回调而不是让画布直接引用引擎:画布只管"问一下",状态判断与烘焙
    /// 都在 EditorWindow 一侧(与 setToolCursor/passthrough 同一分工)。
    void setTextClickProbe(std::function<bool(const QPointF&)> probe) {
        m_textProbe = std::move(probe);
    }

signals:
    void canvasPressed(const QPointF& scenePos);
    void canvasMoved(const QPointF& scenePos);
    void canvasReleased(const QPointF& scenePos);
    /// 视图尺寸变化(布局定型 / 窗口缩放)→ 窗口据此重新适配底图。
    void canvasResized();

protected:
    void resizeEvent(QResizeEvent* e) override {
        QGraphicsView::resizeEvent(e);
        emit canvasResized();
    }

    void wheelEvent(QWheelEvent* e) override {
        // 滚轮缩放(以光标为锚点):编辑小字/细节必需。
        // 一旦手动缩放,就不再随窗口尺寸自动适配,免得把用户选好的比例重置掉。
        const qreal factor = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        const qreal next = qBound(kMinZoom, m_zoom * factor, kMaxZoom);
        if (!qFuzzyCompare(next, m_zoom)) {
            m_userZoomed = true;
            setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
            scale(next / m_zoom, next / m_zoom);
            m_zoom = next;
        }
        e->accept();
    }

    void mousePressEvent(QMouseEvent* e) override {
        // ① 文字编辑态:点在"这段文字之外" → 先请宿主收尾(烘焙本段),再按普通
        //    按下继续 → 引擎用当前工具(仍是「文字」)就地落下一段。
        //    这是"一次点选、连续落多个文字"(§18.2)的实现点。
        //    点在文字**内**则不进这里,交给下面的 QGraphicsView 定位光标/选中。
        if (m_passthrough && e->button() == Qt::LeftButton && m_textProbe
            && m_textProbe(mapToScene(e->pos()))) {
            m_passthrough = false;   // 宿主已烘焙;下面按普通按下走
        }
        // ② 平移态(或中键)交给 QGraphicsView 自己拖画面,不发画笔事件
        if (m_panning || e->button() == Qt::MiddleButton) {
            setPanning(true);
            QGraphicsView::mousePressEvent(e);
            return;
        }
        if (m_passthrough || e->button() != Qt::LeftButton) {
            QGraphicsView::mousePressEvent(e);
            return;
        }
        emit canvasPressed(mapToScene(e->pos()));
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_passthrough) emit canvasMoved(mapToScene(e->pos()));
        QGraphicsView::mouseMoveEvent(e);
        // ⚠️ 必须放在 base 调用**之后**:QGraphicsView 的移动处理会按"鼠标下的图元"
        //    重设视口光标(图元没设 cursor 就 unset),先设会被它冲掉 ——
        //    表现就是"光标时对时错、动不动变回箭头"。
        if (!m_passthrough) viewport()->setCursor(m_toolCursor);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (m_panning || e->button() == Qt::MiddleButton) {
            QGraphicsView::mouseReleaseEvent(e);
            // 空格松开前保持平移态;由 keyRelease 收尾(见 EditorWindow::keyReleaseEvent)
            if (e->button() == Qt::MiddleButton && !m_spaceHeld) setPanning(false);
            return;
        }
        if (m_passthrough || e->button() != Qt::LeftButton) {
            QGraphicsView::mouseReleaseEvent(e);
            return;
        }
        emit canvasReleased(mapToScene(e->pos()));
        e->accept();
    }

private:
    static constexpr qreal kMinZoom = 0.05;
    static constexpr qreal kMaxZoom = 8.0;

    qreal m_zoom = 1.0;
    bool m_userZoomed = false;
    bool m_passthrough = false;
    bool m_panning = false;      // 见 setPanning
    bool m_spaceHeld = false;    // 空格是否按住(决定松手后是否退出平移态)
    std::function<bool(const QPointF&)> m_textProbe;   // 见 setTextClickProbe
    QCursor m_toolCursor = Qt::CrossCursor;   // 见 setToolCursor(默认值会被立即覆盖)
};

// ---------- EditorWindow ----------

EditorWindow::EditorWindow(QImage base, QWidget* parent) : QWidget(parent), m_base(std::move(base)) {
    setWindowTitle(QStringLiteral("Nextboard 截图编辑"));
    setAttribute(Qt::WA_DeleteOnClose);
    // 窗口几何记忆(§17.3 D):用户按习惯调过尺寸,下次打开不该被重置。
    // 没有记录时按默认尺寸;有记录且仍在某块屏幕可见区域内才采用
    // (外接屏拔掉后,记录里的坐标可能已经在可见区之外)。
    restoreGeometryFromSettings();

    // 标注引擎:scene/undo/图元/纹理全在此(docs/14 §16.2,与遮罩浮条共用)
    m_engine = new AnnotationEngine(m_base, this);
    QGraphicsScene* m_scene_local = m_engine->scene();

    auto* canvas = new EditorCanvas(m_engine->scene(), this);
    canvas->setRenderHint(QPainter::Antialiasing);
    canvas->setRenderHint(QPainter::SmoothPixmapTransform);
    canvas->setDragMode(QGraphicsView::NoDrag);
    canvas->setBackgroundBrush(QColor(0x1E, 0x1F, 0x24));
    canvas->setAlignment(Qt::AlignCenter);
    canvas->setMouseTracking(true);
    m_view = canvas;
    canvas->enterPanMode();   // 默认抓手(§18.1):选工具前先能拖动画面
    // 文字"连续落多个"的判据(§18.2):点在当前文字之外 → 烘焙本段,继续落下一段。
    // 判断在窗口侧(引擎状态在这里),画布只负责问一句。
    canvas->setTextClickProbe([this](const QPointF& scenePos) -> bool {
        if (!m_engine || !m_engine->editingText()) return false;
        if (m_engine->editingTextContains(scenePos)) return false;   // 点在文字内:交给文本控件
        // 点在文字外:确认这一段 —— 烘焙后 editingText() 变假,画布随之退出穿透态,
        // 紧接着的同一次按下会以「文字」工具落到新位置(见 EditorCanvas::mousePressEvent)
        bakeTextItem();
        return true;
    });
    syncCanvasState();   // 初值:光标 = 当前工具(真正保持靠 onCanvasMoved)

    connect(canvas, &EditorCanvas::canvasPressed, this, &EditorWindow::onCanvasPressed);
    connect(canvas, &EditorCanvas::canvasMoved, this, &EditorWindow::onCanvasMoved);
    connect(canvas, &EditorCanvas::canvasReleased, this, &EditorWindow::onCanvasReleased);
    // 关键:按**视图**尺寸变化来适配,而不是按窗口 resizeEvent —— 后者可能在布局
    // 把视图撑开之前就触发,那时视口只有几十像素,算出来的缩放会让底图缩成一小点。
    connect(canvas, &EditorCanvas::canvasResized, this, &EditorWindow::fitCanvas);

    buildToolBar();

    // 工具栏放**底部**(形态 B,docs/14 §15.1):贴图/OCR 等"完成动作"靠近拇指,
    // 且与主流截图工具一致;标注画布保持最大可视面积。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, 1);
    // 共享标注工具条(与浮条同类)。宿主的动作按钮(另存/贴图/OCR/缩放/完成/取消)
    // 作为**尾部控件**挂进它的参数行右侧 —— 工具在左、动作在右(用户反馈的布局要求)。
    layout->addWidget(m_annotationBar);

    // 初始落在光标所在屏中央(与贴图窗口同一落点语义)
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        move(avail.center() - rect().center());
    }

    // ⚠️ 本条读数**仍然含上一环的遮罩**,不能当作编辑器成本判读:构造编辑器的那段
    // 代码正处在遮罩 regionConfirmed 的信号发射栈上,deleteLater 的遮罩要到事件循环
    // 转一圈才析构,所以在这里移到构造末尾也一样读到混合值(2026-10-14 实测:
    // 本条 170.6MB,而编辑器真实持有只有 98.2MB —— 见 docs/06 §0 坑 3)。
    // 判读编辑器持有量用:采样峰值,或 `capture-end − editor-closed`。
    logMemory("editor-open");
}

EditorWindow::~EditorWindow() {
    // ⚠️ 析构顺序是这里的关键:QObject 子对象按**创建顺序**析构,而 m_scene 创建早于
    // m_undo —— 若放任不管,场景会先析构并删掉场景内的图元,随后 AddItemCommand
    // 析构再 delete 一次 → **双重释放,进程崩溃**(只要画过标注,点「完成」或「取消」
    // 都必崩)。这里显式按正确顺序拆:
    //   ① 先断开视图与场景(视图析构晚于场景,否则会解引用已释放的场景);
    //   ② 再销毁撤销栈(图元所有权在命令手里,此时场景仍在,removeItem 是安全的)。
    if (m_view) m_view->setScene(nullptr);
    // 引擎是 QObject 子对象,随窗口析构;scene/undo 的析构顺序陷阱已封装在引擎内部
    // 析构体执行时成员(底图/纹理)尚未释放,此刻读数是"还没回收"的水位;
    // 延后一拍(此时对象与成员都已析构)再读,才是编辑器的真实回落值。
    QTimer::singleShot(0, qApp, [] { logMemory("editor-closed"); });
}

namespace {
/// 工具条图标边长(逻辑像素)。20 在 1.5px 描边下仍清晰,且两行工具条不至于过高。
constexpr int kIconPx = 20;
} // namespace

void EditorWindow::buildToolBar() {
    // 工具/颜色/粗细/字号/撤销重做 = **共享控件**(§17.3 A):
    // 遮罩浮条嵌的是同一个类,所以两处 UI 天然一致,不再是"各写一份"。
    m_annotationBar = new AnnotationToolbar(this);
    m_annotationBar->setEngine(m_engine);
    // 工具切换后同步鼠标光标(窗口级状态,控件不该管)
    connect(m_annotationBar, &AnnotationToolbar::toolChanged, this, [this](EditorTool) {
        syncCanvasState();
    });
    // ⚠️ 画布模式**由工具条驱动**:drawing=true 表示选了绘图工具,false 表示手型。
    //    原先这里写成"选过工具就永远 m_drawingTool=true",于是**点过工具就回不到抓手**
    //    (用户反馈:默认手型后点了工具,再也切不回来)。手型按钮使该状态可逆。
    connect(m_annotationBar, &AnnotationToolbar::drawingModeChanged, this,
            [this](bool drawing) {
        m_drawingTool = drawing;
        syncCanvasState();
    });

    // 动作条(窗口专属:另存 / 贴图 / OCR / 缩放 / 完成 / 取消)—— 不进共享控件。
    // 挂到共享工具条的**参数行最右侧**(见 setTrailingWidget):工具在左、动作在右。
    auto* actions = new QToolBar(this);
    actions->setMovable(false);
    actions->setIconSize(QSize(AnnotationToolbar::kIconPx, AnnotationToolbar::kIconPx));
    actions->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const QColor ink = iconInkFor(palette());
    const auto addAction = [&](BarIcon icon, const QString& tip, auto slot) {
        QAction* action = actions->addAction(barIcon(icon, AnnotationToolbar::kIconPx, ink), tip);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    addAction(BarIcon::SaveAs, QStringLiteral("另存为… (Ctrl+S)"), &EditorWindow::saveToFile);
    addAction(BarIcon::Pin, QStringLiteral("贴图"), &EditorWindow::pinToScreen);
    addAction(BarIcon::Ocr, QStringLiteral("OCR 取字"), &EditorWindow::ocrTakeText);
    addAction(BarIcon::ZoomFit, QStringLiteral("适应窗口 (Ctrl+0)"), &EditorWindow::zoomToFit);
    addAction(BarIcon::ZoomOriginal, QStringLiteral("实际大小 (Ctrl+1)"), &EditorWindow::zoomToOriginal);
    actions->addSeparator();
    addAction(BarIcon::Done, QStringLiteral("完成 (Enter):入库并写剪贴板"), &EditorWindow::finish);
    addAction(BarIcon::Cancel, QStringLiteral("取消 (Esc)"), &EditorWindow::cancel);

    // 交给共享工具条放到参数行右侧(不再自己占一整行)。
    // 所有权随 addWidget 转给参数行 → 随窗口析构,这里不留指针。
    m_annotationBar->setTrailingWidget(actions);
}

void EditorWindow::zoomToFit() {
    // ⚠️ 必须**先清掉**"用户已手动缩放"标记再适配:fitCanvas 见到 userZoomed()
    //    会直接返回,否则用户点了「适应窗口」却毫无反应 —— 这正是原先
    //    "滚轮缩过一次就再也回不去"的成因。
    auto* canvas = qobject_cast<EditorCanvas*>(m_view);
    if (!canvas) return;
    canvas->clearUserZoom();
    fitCanvas();
}

void EditorWindow::zoomToOriginal() {
    // 1:1(实际大小):底图物理像素直接对应屏幕像素,便于逐像素看细节。
    auto* canvas = qobject_cast<EditorCanvas*>(m_view);
    if (!canvas) return;
    canvas->resetZoom(1.0);
    canvas->markUserZoomed();   // 不再自动适配(用户显式选了比例)
}

void EditorWindow::fitCanvas() {
    if (!m_engine || !m_view || !m_view->viewport()) return;
    auto* canvas = qobject_cast<EditorCanvas*>(m_view);
    // 用户手动滚轮缩放过就不再自动适配,免得把选好的比例重置掉
    if (canvas && canvas->userZoomed()) return;
    const QRectF rect = m_engine->scene()->sceneRect();
    if (rect.isEmpty()) return;
    const QSizeF viewport = m_view->viewport()->size();
    // 视口尚未定型(布局没跑完)时**不要**计算:否则会按一个极小的视口把图缩到看不清
    if (viewport.width() < 2 || viewport.height() < 2) return;
    const qreal fit = qMin(viewport.width() / rect.width(), viewport.height() / rect.height());
    const qreal zoom = qMin(fit, 1.0);   // 只在图比窗口大时缩小;小图保持 1:1 不放大
    if (canvas) {
        canvas->resetZoom(zoom);
    } else {
        m_view->resetTransform();
        m_view->scale(zoom, zoom);
    }
}

void EditorWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    fitCanvas();
}

void EditorWindow::closeEvent(QCloseEvent* e) {
    // 所有关闭路径(完成/取消/贴图/OCR/Esc/✕)都经过这里 → 几何只在此写回一次,
    // 不必在每个出口各写一遍(写多了必然漏一处)。
    saveGeometryToSettings();
    QWidget::closeEvent(e);
}

void EditorWindow::keyReleaseEvent(QKeyEvent* e) {
    // 空格松开 → 退出平移态(按下在 keyPressEvent,见 EditorCanvas::setSpaceHeld)
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        if (auto* canvas = qobject_cast<EditorCanvas*>(m_view)) canvas->setSpaceHeld(false);
        return;
    }
    QWidget::keyReleaseEvent(e);
}

void EditorWindow::syncCanvasState() {
    auto* canvas = qobject_cast<EditorCanvas*>(m_view);
    if (!canvas) return;
    const bool editing = m_engine && m_engine->editingText();
    // 文字编辑期间穿透:把鼠标事件还给 QGraphicsView/QGraphicsTextItem,
    // 文本才能正常框选/点选;其余时候必须关掉,否则画布收不到画笔事件。
    canvas->setPassthrough(editing);
    // ⚠️ 默认抓手(§18.1):没选绘图工具时画布就是"可拖动"的,左键拖动平移画面;
    //    一旦选了绘图工具(含文字)才切成可绘制 —— 否则用户左键一按就是一笔,
    //    想挪画面只能拖滚动条(这正是"放大后移不动"的由来)。
    //    文字编辑中保持穿透(事件交给文本控件),由上面的 passthrough 负责。
    if (m_drawingTool) canvas->enterDrawingMode();
    else canvas->enterPanMode();
    // 光标**只由画布拥有**:形状存在画布上,由它在每次移动处理之后统一应用 ——
    // QGraphicsView 自己会按"鼠标下的图元"重设视口光标,只有压在其后才能生效
    // (文字态它不碰,交给文本控件给 IBeam)。
    // 光标选择:文字保留系统竖线(文本控件本就该是 IBeam)、取色用系统手型
    // (Qt 无吸管形状);**其余绘图工具**换成自定义准星(内置十字不好看,见 tool_icons)。
    const EditorTool tool = m_engine->tool();
    if (tool == EditorTool::Text) {
        canvas->setToolCursor(QCursor(Qt::IBeamCursor));
    } else if (tool == EditorTool::Picker) {
        canvas->setToolCursor(QCursor(Qt::PointingHandCursor));
    } else {
        // 深浅主题由**当前调色板**判定(与 iconInkFor 同一依据),不额外存状态:
        // 窗口底色暗 → 准星取浅色描边,反之取深色 —— 保证在任何主题下都看得清。
        const QColor bg = palette().color(QPalette::Window);
        const bool dark = bg.lightness() < 128;
        canvas->setToolCursor(crosshairCursor(dark, devicePixelRatioF()));
    }
}

void EditorWindow::onCanvasPressed(const QPointF& scenePos) {
    m_engine->onCanvasPressed(scenePos);
    if (m_engine->editingText()) m_view->setFocus();   // 焦点交给文字图元
    // ⚠️ 取色器采样后引擎会**自行**把工具切回上一个绘图工具(见引擎 onCanvasPressed),
    //    工具条不刷新就会停在"取色"高亮上,与引擎实际工具不符。这里统一回正。
    if (m_annotationBar) m_annotationBar->refresh();
    syncCanvasState();
}

void EditorWindow::onCanvasMoved(const QPointF& scenePos) {
    m_engine->onCanvasMoved(scenePos);
    // 每次移动都对一次:QGraphicsView 会用"图元自己的 cursor"覆盖视口光标
    // (图元没设 = unset),只设一次会被它冲掉 —— 表现为"设了没用/光标变回箭头"。
    syncCanvasState();
}

void EditorWindow::onCanvasReleased(const QPointF& scenePos) {
    m_engine->onCanvasReleased(scenePos);
}

void EditorWindow::bakeTextItem() {
    m_engine->bakeTextItem();
    // 文字编辑结束 → 立刻退出穿透态并恢复工具光标(见 syncCanvasState 注释)
    syncCanvasState();
}

QImage EditorWindow::compose() const {
    return m_engine->compose();
}

void EditorWindow::saveToFile() {
    bakeTextItem();
    // 格式(PNG/JPEG/WebP)与白底、质量等细节统一交给 image_save。
    // ⚠️ 编辑器产物是 ARGB32_Premultiplied + fill(transparent):存 JPEG 时若
    //    不铺白底,Qt 会把透明区写成黑色(实测)。这件事由 image_save 兜住。
    const QString base = QStringLiteral("Nextboard-%1").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString error;
    const QString saved = image_save::saveImageWithDialog(this, compose(), base, &error);
    if (saved.isEmpty() && !error.isEmpty())
        QMessageBox::warning(this, QStringLiteral("另存为"), error);
}

void EditorWindow::finish() {
    bakeTextItem();
    emit committed(compose());
    close();
}

void EditorWindow::cancel() {
    emit cancelled();
    close();
}

void EditorWindow::saveGeometryToSettings() const {
    const QRect g = geometry();
    // 只存四元组;合法性由设置校验兜底(宽高区间),x/y 允许为负(多屏在负坐标区)
    CoreBridge::instance().call(
        "save_settings",
        {{"editorGeometry", QJsonArray{g.x(), g.y(), g.width(), g.height()}}});
}

void EditorWindow::restoreGeometryFromSettings() {
    const QJsonObject resp = CoreBridge::instance().call("get_settings");
    const QJsonArray arr =
        resp.value("ok").toBool()
            ? resp.value("data").toObject().value("editorGeometry").toArray()
            : QJsonArray{};
    if (arr.size() == 4) {
        const QRect want(arr.at(0).toInt(), arr.at(1).toInt(), arr.at(2).toInt(),
                         arr.at(3).toInt());
        // 仍在某块屏幕(含可用区域)内才采用:否则用户会把窗口"打开在看不见的地方"
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen && screen->availableGeometry().intersects(want)) {
                setGeometry(want);
                return;
            }
        }
    }
    resize(960, 720);   // 默认尺寸(原先的行为)
}

void EditorWindow::pinToScreen() {
    bakeTextItem();
    const QImage composed = compose();
    if (composed.isNull()) return;
    // 决策(docs/14 §15.6-4):贴图后关闭编辑器 —— 贴图窗口接管后续(编辑回写走
    // PinWindow::editRequested 的既有闭环),编辑器不再驻留占内存(实测 ~98MB)。
    emit pinRequested(composed);
    close();
}

void EditorWindow::ocrTakeText() {
    bakeTextItem();
    const QImage composed = compose();
    if (composed.isNull()) return;
    // OCR 必须**异步**:det 推理 0.4~5s(全屏),UI 线程跑会冻结工具栏。
    // 模式照抄 CoreFacade::ocrEntry(QThreadPool + 结果回 UI 线程)。
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QString text = watcher->result();
        // 「取字」的落点是**系统剪贴板**(与浮条同一语义,对齐第一代:
        // 见 docs/progress.md「OCR 结果」一条)。此前这里只弹层展示、从不写剪贴板,
        // 用户点了「OCR取字」再 Ctrl+V 什么都没有。
        // 经核心写入:核心 clipboard_win 带打开重试,别的进程占用时 Qt 写会静默失败。
        QString reason;
        if (text.isEmpty()) {
            reason = QStringLiteral("未识别到文字(图片可能不含可识别文本)");
        } else if (CoreBridge::instance().copyTextToClipboard(text)) {
            reason = QStringLiteral("已识别 %1 个字符并复制到剪贴板").arg(text.size());
        } else {
            reason = QStringLiteral("识别成功,但写入剪贴板失败");
        }
        showOcrResult(text, reason);
    });
    connect(watcher, &QFutureWatcher<QString>::canceled, watcher, &QObject::deleteLater);
    watcher->setFuture(QtConcurrent::run([image = composed]() -> QString {
        return CoreBridge::instance().ocrImage(image);
    }));
    // 弹层先以"识别中"占位,完成后由 showOcrResult 填充(全屏 4~5s 不能没反馈)
    showOcrResult(QString(), QStringLiteral("识别中…"));
}

void EditorWindow::showOcrResult(const QString& text, const QString& reason) {
    // OCR 结果面板(§17.3 D6)。
    //
    // 原实现两点不好用:①文本只"展示",用户看完就没了(虽然已进剪贴板却看不出);
    // ②面板是编辑器子窗口,而识别完成后 singleShot(0) 立刻 close → 面板一帧都
    // 看不到(表现为"点了取字,啥也没发生")。
    // 现改为:面板**驻留**,明确提示"已复制,可 Ctrl+V",文本可鼠标选取(便于只取片段)。
    //
    // ⚠️ 2026-10-17 移除「插入为标注文字」(曾把 OCR 文本直接落成画布文字标注),两条理由:
    //    ①场景受限 —— 取字链路本就以"把文本带走"为主,"留在本图"属小众需求;
    //    ②文字布局难处理 —— 只能落在可见区中心(引擎只回纯文本、没有逐行框),
    //      字号/颜色/行距都与原图无关,多行必然压字或溢出,"看着不对"比不打这句还差。
    //    故 OCR 文本的去处收敛为**剪贴板**一条(要留在图上就自己用文字工具输入)。
    if (m_ocrPopup) { m_ocrPopup->deleteLater(); m_ocrPopup = nullptr; }

    // "识别中…" 仍用轻量提示(此时没有可操作的文本)
    if (text.isEmpty() && !reason.isEmpty() && reason.endsWith(QStringLiteral("…"))) {
        if (m_ocrPopup) { m_ocrPopup->deleteLater(); m_ocrPopup = nullptr; }
        auto* popup = new QLabel(this, Qt::ToolTip);
        popup->setText(reason);
        popup->setStyleSheet(QStringLiteral(
            "QLabel{background:#26272E;color:#DFE1E8;padding:8px 12px;border-radius:6px;}"));
        popup->adjustSize();
        popup->move(mapToGlobal(QPoint(width() - popup->width() - 24,
                                       height() - popup->height() - 72)));
        popup->show();
        m_ocrPopup = popup;
        return;
    }

    auto* panel = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    panel->setObjectName("ocrResultPanel");
    panel->setFrameShape(QFrame::NoFrame);
    panel->setStyleSheet(QStringLiteral(
        "QFrame#ocrResultPanel{background:#26272E;border:1px solid #3A3D46;"
        "border-radius:8px;} "
        "QLabel#ocrText{color:#DFE1E8;font-size:13px;} "
        "QLabel#ocrStatus{color:#8FA0B4;font-size:12px;} "
        "QPushButton{background:#33363F;color:#DFE1E8;border:none;border-radius:5px;"
        "padding:6px 12px;font-size:13px;} "
        "QPushButton:hover{background:#3E424D;}"));

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* body = new QLabel(panel);
    body->setObjectName("ocrText");
    body->setWordWrap(true);
    body->setTextFormat(Qt::PlainText);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 允许手动选取部分文本
    body->setMaximumWidth(520);
    body->setMaximumHeight(qMax(120, height() - 200));
    body->setText(text.isEmpty() ? reason : text);
    layout->addWidget(body);

    if (!reason.isEmpty() && !text.isEmpty()) {
        // 成功时提示"已进剪贴板";失败时显示原因
        auto* status = new QLabel(reason, panel);
        status->setObjectName("ocrStatus");
        layout->addWidget(status);
    }

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    // 只剩「关闭」一个动作:文本已进剪贴板,面板的职责是"让用户看见识别到了什么"
    // 并能选取片段 Ctrl+C,不再提供落画布的入口(见上方 2026-10-17 说明)。
    auto* closeBtn = new QPushButton(
        text.isEmpty() ? QStringLiteral("关闭") : QStringLiteral("关闭(已在剪贴板)"), panel);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, [this] {
        if (m_ocrPopup) m_ocrPopup->hide();
    });
    buttons->addWidget(closeBtn);
    layout->addLayout(buttons);

    panel->adjustSize();
    panel->move(mapToGlobal(QPoint(qMax(8, width() - panel->width() - 24),
                                   qMax(8, height() - panel->height() - 72))));
    panel->show();
    m_ocrPopup = panel;

    // ⚠️ 不再自动关编辑器:面板要留给用户阅读/选取片段。
    //    关闭由用户点按钮(或直接关编辑器)完成 —— 原先识别完就 singleShot 关窗,
    //    会把面板连同结果一起销毁(表现为"点了取字,啥也没发生")。
    emit ocrFinished(text, reason);
}

void EditorWindow::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    // 空格按住 = 抓手平移(放大看细节的配套;原先画布 NoDrag,放大后移不动)
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        if (auto* canvas = qobject_cast<EditorCanvas*>(m_view)) canvas->setSpaceHeld(true);
        return;
    }
    switch (e->key()) {
        case Qt::Key_Escape:
            // ⚠️ 文字编辑态下 Esc 只**确认当前文字**,不关整个编辑器 ——
            //    原先无条件 cancel(),用户写一半按 Esc 会把字和编辑器一起丢掉。
            if (m_engine->editingText()) {
                bakeTextItem();
                return;
            }
            cancel();
            return;
        case Qt::Key_S:
            if (ctrl) {
                saveToFile();
                return;
            }
            break;
        case Qt::Key_Z:
            if (ctrl) {
                (e->modifiers() & Qt::ShiftModifier) ? m_engine->undoStack()->redo()
                                                     : m_engine->undoStack()->undo();
                return;
            }
            break;
        case Qt::Key_Y:
            if (ctrl) {
                m_engine->undoStack()->redo();
                return;
            }
            break;
        // 缩放入口(原先只能滚轮缩、且缩了回不去,见 zoomToFit 注释)
        case Qt::Key_0:
            if (ctrl) {
                zoomToFit();
                return;
            }
            break;
        case Qt::Key_1:
            if (ctrl) {
                zoomToOriginal();
                return;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // 文字工具编辑态(m_textItem 非空)时 Enter 留给**换行** ——
            // 此刻图元持有焦点,事件仍会先经过本窗口,不拦截否则标注无法换行。
            if (m_engine->editingText()) break;
            finish();   // Enter 直接完成(Ctrl+Enter 保留为等价别名)
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(e);
}

} // namespace nb

#include "editor_window.moc"