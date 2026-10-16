#include "editor_window.hpp"

#include "annotation_engine.hpp"

#include "host/memory_probe.hpp"
#include "host/core_bridge.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
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
#include <QGuiApplication>
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
#include <QStandardPaths>
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

    /// 适配显示:重置为指定缩放,并清除"用户已手动缩放"标记。
    void resetZoom(qreal zoom) {
        m_userZoomed = false;
        m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
        resetTransform();
        scale(m_zoom, m_zoom);
    }

    bool userZoomed() const { return m_userZoomed; }

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
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
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
};

// ---------- EditorWindow ----------

EditorWindow::EditorWindow(QImage base, QWidget* parent) : QWidget(parent), m_base(std::move(base)) {
    setWindowTitle(QStringLiteral("Nextboard 截图编辑"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(960, 720);

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
    for (QToolBar* bar : m_toolBars) layout->addWidget(bar);

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

void EditorWindow::buildToolBar() {
    const auto addTool = [this](QToolBar* bar, const QString& text, bool checked,
                                const std::function<void()>& onTrigger) {
        QAction* action = bar->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::triggered, this, [onTrigger] { onTrigger(); });
        return action;
    };

    // ---- 第一行:工具 + 颜色 ----
    auto* tools = new QToolBar(this);
    tools->setMovable(false);
    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);
    // 顺序:绘制类 → 擦除类(马赛克/模糊) → 标注类(序号/文字) → 取色
    const EditorTool kTools[] = {EditorTool::Pen,  EditorTool::Arrow,  EditorTool::Line,
                                 EditorTool::Rect, EditorTool::Ellipse, EditorTool::Mosaic,
                                 EditorTool::Blur, EditorTool::Number,  EditorTool::Text,
                                 EditorTool::Picker};
    for (EditorTool tool : kTools) {
        QAction* action = addTool(tools, toolLabel(tool), tool == m_engine->tool(),
                                  [this, tool] { setTool(tool); });
        action->setActionGroup(m_toolGroup);
        // 存枚举值:程序化切换工具(取色器采样后回退)时据此同步选中态
        action->setData(int(tool));
    }
    tools->addSeparator();

    // 颜色:第一代 PALETTE 七色
    m_colorGroup = new QActionGroup(this);
    m_colorGroup->setExclusive(true);
    const QColor kPalette[] = {QColor(0xE0, 0x31, 0x31), QColor(0xF7, 0x67, 0x07),
                               QColor(0xFA, 0xB0, 0x05), QColor(0x40, 0xC0, 0x57),
                               QColor(0x1C, 0x7E, 0xD6), QColor(0x21, 0x25, 0x29),
                               QColor(0xFF, 0xFF, 0xFF)};
    for (const QColor& color : kPalette) {
        QAction* action = tools->addAction(swatchIcon(color), color.name());
        action->setCheckable(true);
        action->setActionGroup(m_colorGroup);
        action->setChecked(color == m_engine->color());
        // 存色值:取色器采到调色板外的颜色时,需要据此把全部色块取消选中
        action->setData(color);
        connect(action, &QAction::triggered, this, [this, color] { m_engine->setColor(color); });
    }
    m_toolBars.append(tools);

    // ---- 第二行:粗细 / 字号 / 撤销重做 / 另存 / 完成 ----
    auto* actions = new QToolBar(this);
    actions->setMovable(false);

    auto* sizeGroup = new QActionGroup(this);
    sizeGroup->setExclusive(true);
    const int kSizes[] = {2, 4, 8};
    for (int size : kSizes) {
        QAction* action =
            addTool(actions, QStringLiteral("粗细 %1").arg(size), size == m_engine->strokeWidth(),
                    [this, size] { m_engine->setStrokeWidth(size); });
        action->setActionGroup(sizeGroup);
    }
    actions->addSeparator();

    auto* fontGroup = new QActionGroup(this);
    fontGroup->setExclusive(true);
    const int kFontSizes[] = {24, 36, 48};
    for (int size : kFontSizes) {
        QAction* action =
            addTool(actions, QStringLiteral("字号 %1").arg(size), size == m_engine->fontSize(),
                    [this, size] { m_engine->setFontSize(size); });
        action->setActionGroup(fontGroup);
    }
    actions->addSeparator();

    connect(actions->addAction(QStringLiteral("撤销")), &QAction::triggered, this, [this] { m_engine->undoStack()->undo(); });
    connect(actions->addAction(QStringLiteral("重做")), &QAction::triggered, this, [this] { m_engine->undoStack()->redo(); });
    connect(actions->addAction(QStringLiteral("另存为 (Ctrl+S)")), &QAction::triggered, this,
            &EditorWindow::saveToFile);
    connect(actions->addAction(QStringLiteral("贴图")), &QAction::triggered, this,
            &EditorWindow::pinToScreen);
    connect(actions->addAction(QStringLiteral("OCR取字")), &QAction::triggered, this,
            &EditorWindow::ocrTakeText);
    actions->addSeparator();
    connect(actions->addAction(QStringLiteral("完成 (Enter)")), &QAction::triggered, this,
            &EditorWindow::finish);
    connect(actions->addAction(QStringLiteral("取消 (Esc)")), &QAction::triggered, this,
            &EditorWindow::cancel);
    m_toolBars.append(actions);
}

void EditorWindow::syncToolAction() {
    if (!m_toolGroup) return;
    for (QAction* action : m_toolGroup->actions()) {
        if (action->data().toInt() == int(m_engine->tool())) {
            action->setChecked(true);   // 互斥组:选中一个会自动取消其余
            return;
        }
    }
}

void EditorWindow::syncColorSwatch() {
    if (!m_colorGroup) return;
    // 互斥组无法直接取消选中:临时关掉互斥再逐项设置,最后恢复
    const bool exclusive = m_colorGroup->isExclusive();
    m_colorGroup->setExclusive(false);
    for (QAction* action : m_colorGroup->actions())
        action->setChecked(action->data().value<QColor>() == m_engine->color());
    m_colorGroup->setExclusive(exclusive);
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

void EditorWindow::setTool(EditorTool tool) {
    m_engine->setTool(tool);
    syncColorSwatch();
}

void EditorWindow::onCanvasPressed(const QPointF& scenePos) {
    m_engine->onCanvasPressed(scenePos);
    if (m_engine->editingText()) {
        // 文字编辑态:焦点交给图元 + 视图 passthrough(引擎无视图)
        if (auto* canvas = qobject_cast<EditorCanvas*>(m_view)) canvas->setPassthrough(true);
        m_view->setFocus();
    }
}

void EditorWindow::onCanvasMoved(const QPointF& scenePos) {
    m_engine->onCanvasMoved(scenePos);
}

void EditorWindow::onCanvasReleased(const QPointF& scenePos) {
    m_engine->onCanvasReleased(scenePos);
}

void EditorWindow::bakeTextItem() {
    m_engine->bakeTextItem();
}

QImage EditorWindow::compose() const {
    return m_engine->compose();
}

void EditorWindow::saveToFile() {
    bakeTextItem();
    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath(QStringLiteral("Nextboard-%1.png")
                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("另存为"), suggested, QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty()) return;
    if (!compose().save(path, "PNG"))
        QMessageBox::warning(this, QStringLiteral("另存为"),
                             QStringLiteral("写入失败:%1").arg(path));
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
        showOcrResult(watcher->result(), {});
    });
    connect(watcher, &QFutureWatcher<QString>::canceled, watcher, &QObject::deleteLater);
    watcher->setFuture(QtConcurrent::run([image = composed]() -> QString {
        return CoreBridge::instance().ocrImage(image);
    }));
    // 弹层先以"识别中"占位,完成后由 showOcrResult 填充(全屏 4~5s 不能没反馈)
    showOcrResult(QString(), QStringLiteral("识别中…"));
}

void EditorWindow::showOcrResult(const QString& text, const QString& reason) {
    // OCR 结果弹层(docs/14 §15.6-2):可复制预留(后期),本期只展示。
    if (m_ocrPopup) { m_ocrPopup->deleteLater(); m_ocrPopup = nullptr; }
    auto* popup = new QLabel(this, Qt::Popup | Qt::FramelessWindowHint);
    popup->setObjectName("ocrResultPopup");
    popup->setWordWrap(true);
    popup->setTextFormat(Qt::PlainText);
    popup->setStyleSheet(QStringLiteral(
        "QLabel{background:#26272E;color:#DFE1E8;padding:12px;border-radius:8px;"
        "font-size:14px;}"));
    popup->setMaximumWidth(560);
    if (text.isEmpty() && !reason.isEmpty()) {
        popup->setText(reason);                    // "识别中…" 或错误原因
    } else if (text.isEmpty()) {
        popup->setText(QStringLiteral("未识别到文字(图片可能不含可识别文本)"));
    } else {
        popup->setText(text);
    }
    popup->adjustSize();
    // 弹层贴工具栏上方右侧,不遮画布中心
    const QPoint global = mapToGlobal(QPoint(width() - popup->width() - 24,
                                             height() - popup->height() - 72));
    popup->move(global);
    popup->show();
    m_ocrPopup = popup;
    // 决策:OCR 完成后关闭编辑器(与贴图同语义)。识别期间编辑器仍在,
    // 弹层随编辑器销毁;文本稍后由用户在弹层里自行处理(v1 只展示)。
    if (!text.isEmpty() || (reason.isEmpty())) {
        QTimer::singleShot(0, this, [this] {
            emit ocrFinished(QString(), {});
            close();
        });
    }
}

void EditorWindow::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    switch (e->key()) {
        case Qt::Key_Escape:
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
