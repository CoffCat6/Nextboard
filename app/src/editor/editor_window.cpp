#include "editor_window.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGuiApplication>
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
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace nb {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// 马赛克像素块边长(第一代 §3.4:底图预生成 8px 像素化版本)。
constexpr int kMosaicBlock = 8;

QString toolLabel(EditorTool tool) {
    switch (tool) {
        case EditorTool::Pen: return QStringLiteral("画笔");
        case EditorTool::Arrow: return QStringLiteral("箭头");
        case EditorTool::Rect: return QStringLiteral("矩形");
        case EditorTool::Mosaic: return QStringLiteral("马赛克");
        case EditorTool::Text: return QStringLiteral("文字");
    }
    return QStringLiteral("标注");
}

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

/// 撤销命令:一次标注 = 一个图元的增删。
///
/// 图元**不入场景**就交给命令,push() 会调用 redo() 完成首次添加;undo() 移出场景
/// (removeItem 不接管所有权),由命令持有并在析构时释放。
class AddItemCommand : public QUndoCommand {
public:
    AddItemCommand(QGraphicsScene* scene, QGraphicsItem* item, const QString& label)
        : m_scene(scene), m_item(item) {
        setText(label);
    }
    ~AddItemCommand() override { delete m_item; }

    void undo() override { m_scene->removeItem(m_item); }
    void redo() override {
        if (m_item->scene() != m_scene) m_scene->addItem(m_item);
    }

private:
    QGraphicsScene* m_scene;
    QGraphicsItem* m_item;
};

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

signals:
    void canvasPressed(const QPointF& scenePos);
    void canvasMoved(const QPointF& scenePos);
    void canvasReleased(const QPointF& scenePos);

protected:
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
    bool m_passthrough = false;
};

// ---------- EditorWindow ----------

EditorWindow::EditorWindow(QImage base, QWidget* parent) : QWidget(parent), m_base(std::move(base)) {
    setWindowTitle(QStringLiteral("Nextboard 截图编辑"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(960, 720);

    m_scene = new QGraphicsScene(this);
    // 场景坐标 = 底图物理像素(第一代 §3.4:画布按底图自然分辨率渲染)
    m_scene->setSceneRect(0, 0, m_base.width(), m_base.height());

    auto* background = new QGraphicsPixmapItem(QPixmap::fromImage(m_base));
    background->setPos(0, 0);
    m_scene->addItem(background);

    // 马赛克底图:先缩到 1/kMosaicBlock 再放大回来 = 像素化(FastTransformation
    // 保证取整不插值),作为马赛克图元的纹理刷子按路径"揭示"。
    {
        const int smallW = qMax(1, m_base.width() / kMosaicBlock);
        const int smallH = qMax(1, m_base.height() / kMosaicBlock);
        const QImage pixelated = m_base.scaled(smallW, smallH, Qt::IgnoreAspectRatio,
                                               Qt::FastTransformation)
                                     .scaled(m_base.size(), Qt::IgnoreAspectRatio,
                                             Qt::FastTransformation);
        m_mosaicSource = QPixmap::fromImage(pixelated);
    }

    m_undo = new QUndoStack(this);

    auto* canvas = new EditorCanvas(m_scene, this);
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

    buildToolBar();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (QToolBar* bar : m_toolBars) layout->addWidget(bar);
    layout->addWidget(m_view, 1);

    // 初始落在光标所在屏中央(与贴图窗口同一落点语义)
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        move(avail.center() - rect().center());
    }
}

EditorWindow::~EditorWindow() = default;

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
    const EditorTool kTools[] = {EditorTool::Pen, EditorTool::Arrow, EditorTool::Rect,
                                 EditorTool::Mosaic, EditorTool::Text};
    for (EditorTool tool : kTools) {
        QAction* action = addTool(tools, toolLabel(tool), tool == m_tool,
                                  [this, tool] { setTool(tool); });
        action->setActionGroup(m_toolGroup);
    }
    tools->addSeparator();

    // 颜色:第一代 PALETTE 七色
    auto* colorGroup = new QActionGroup(this);
    colorGroup->setExclusive(true);
    const QColor kPalette[] = {QColor(0xE0, 0x31, 0x31), QColor(0xF7, 0x67, 0x07),
                               QColor(0xFA, 0xB0, 0x05), QColor(0x40, 0xC0, 0x57),
                               QColor(0x1C, 0x7E, 0xD6), QColor(0x21, 0x25, 0x29),
                               QColor(0xFF, 0xFF, 0xFF)};
    for (const QColor& color : kPalette) {
        QAction* action = tools->addAction(swatchIcon(color), color.name());
        action->setCheckable(true);
        action->setActionGroup(colorGroup);
        action->setChecked(color == m_color);
        connect(action, &QAction::triggered, this, [this, color] { m_color = color; });
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
            addTool(actions, QStringLiteral("粗细 %1").arg(size), size == m_size,
                    [this, size] { m_size = size; });
        action->setActionGroup(sizeGroup);
    }
    actions->addSeparator();

    auto* fontGroup = new QActionGroup(this);
    fontGroup->setExclusive(true);
    const int kFontSizes[] = {24, 36, 48};
    for (int size : kFontSizes) {
        QAction* action =
            addTool(actions, QStringLiteral("字号 %1").arg(size), size == m_fontSize,
                    [this, size] { m_fontSize = size; });
        action->setActionGroup(fontGroup);
    }
    actions->addSeparator();

    connect(actions->addAction(QStringLiteral("撤销")), &QAction::triggered, m_undo,
            &QUndoStack::undo);
    connect(actions->addAction(QStringLiteral("重做")), &QAction::triggered, m_undo,
            &QUndoStack::redo);
    connect(actions->addAction(QStringLiteral("另存为 (Ctrl+S)")), &QAction::triggered, this,
            &EditorWindow::saveToFile);
    actions->addSeparator();
    connect(actions->addAction(QStringLiteral("完成 (Ctrl+Enter)")), &QAction::triggered, this,
            &EditorWindow::finish);
    connect(actions->addAction(QStringLiteral("取消 (Esc)")), &QAction::triggered, this,
            &EditorWindow::cancel);
    m_toolBars.append(actions);
}

void EditorWindow::setTool(EditorTool tool) {
    bakeTextItem();
    m_tool = tool;
    if (auto* canvas = qobject_cast<EditorCanvas*>(m_view)) canvas->setPassthrough(false);
    setCursor(tool == EditorTool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
}

void EditorWindow::fitCanvas() {
    // resizeEvent 可能在构造早期(视图/场景尚未建好)就被触发,必须判空
    if (!m_scene || !m_view || !m_view->viewport()) return;
    const QRectF rect = m_scene->sceneRect();
    if (rect.isEmpty()) return;
    m_view->resetTransform();
    const QSizeF viewport = m_view->viewport()->size();
    const qreal scale = qMin(viewport.width() / rect.width(), viewport.height() / rect.height());
    // 只在图比窗口大时缩小;小图保持 1:1,不放大以免虚化
    if (scale < 1.0) m_view->scale(scale, scale);
}

void EditorWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    fitCanvas();
}

void EditorWindow::pushItem(QGraphicsItem* item, const QString& label) {
    item->setFlag(QGraphicsItem::ItemIsSelectable, false);
    m_undo->push(new AddItemCommand(m_scene, item, label));   // redo() 完成首次入场景
}

QPainterPath EditorWindow::strokePath() const {
    QPainterPath path;
    if (m_stroke.isEmpty()) return path;
    if (m_stroke.size() == 1) {
        path.addEllipse(m_stroke.first(), 0.5, 0.5);
        return path;
    }
    // 相邻点取中点做二次贝塞尔,折线更顺滑(锯齿在缩放屏上很显眼)
    path.moveTo(m_stroke.first());
    for (int i = 1; i + 1 < m_stroke.size(); ++i) {
        const QPointF mid = (m_stroke.at(i) + m_stroke.at(i + 1)) / 2.0;
        path.quadTo(m_stroke.at(i), mid);
    }
    path.lineTo(m_stroke.last());
    return path;
}

QGraphicsItem* EditorWindow::buildDragItem() const {
    const QRectF rect = QRectF(m_start, m_current).normalized();
    if (m_tool == EditorTool::Rect) {
        auto* item = new QGraphicsRectItem(rect);
        item->setPen(QPen(m_color, m_size));
        item->setBrush(Qt::NoBrush);
        return item;
    }
    // 箭头:主干 + 两条箭头边
    QPainterPath path;
    path.moveTo(m_start);
    path.lineTo(m_current);
    const QPointF delta = m_current - m_start;
    const qreal length = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
    if (length > 1.0) {
        const qreal head = qBound(10.0, length * 0.25, 12.0 + m_size * 4.0);
        const qreal angle = std::atan2(delta.y(), delta.x());
        constexpr qreal kSpread = 0.42;   // ≈24°
        for (qreal sign : {-1.0, 1.0}) {
            const qreal a = angle + kPi + sign * kSpread;
            path.moveTo(m_current);
            path.lineTo(m_current + QPointF(std::cos(a), std::sin(a)) * head);
        }
    }
    auto* item = new QGraphicsPathItem(path);
    item->setPen(QPen(m_color, m_size, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    item->setBrush(Qt::NoBrush);
    return item;
}

QGraphicsItem* EditorWindow::buildPreviewItem() const {
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic) {
        auto* item = new QGraphicsPathItem(strokePath());
        // 马赛克预览只画示意描边:真正的像素化纹理在松手时按轮廓合成,
        // 避免每一帧都重算 QPainterPathStroker
        item->setPen(m_tool == EditorTool::Mosaic
                         ? QPen(QColor(255, 255, 255, 170), qMax(m_size * 6, 18),
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                         : QPen(m_color, m_size, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        item->setBrush(Qt::NoBrush);
        return item;
    }
    return buildDragItem();
}

QGraphicsItem* EditorWindow::buildFinalItem() const {
    if (m_tool == EditorTool::Pen) {
        auto* item = new QGraphicsPathItem(strokePath());
        item->setPen(QPen(m_color, m_size, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        item->setBrush(Qt::NoBrush);
        return item;
    }
    if (m_tool == EditorTool::Mosaic) {
        // 以笔画为 mask:先把折线描成有宽度的轮廓,再用像素化纹理填充该轮廓
        QPainterPathStroker stroker;
        stroker.setWidth(qMax(m_size * 6, 18));
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        auto* item = new QGraphicsPathItem(stroker.createStroke(strokePath()));
        item->setPen(Qt::NoPen);
        item->setBrush(QBrush(m_mosaicSource));
        return item;
    }
    return buildDragItem();
}

void EditorWindow::clearPreview() {
    if (!m_preview) return;
    m_scene->removeItem(m_preview);
    delete m_preview;
    m_preview = nullptr;
}

void EditorWindow::onCanvasPressed(const QPointF& scenePos) {
    bakeTextItem();

    if (m_tool == EditorTool::Text) {
        // 文字:落点即建图元并进入编辑态;确认后烘焙(第一代 v1 取舍)
        auto* item = new QGraphicsTextItem;
        item->setDefaultTextColor(m_color);
        QFont font = item->font();
        font.setPixelSize(m_fontSize);
        item->setFont(font);
        item->setPos(scenePos);
        item->setTextInteractionFlags(Qt::TextEditorInteraction);
        pushItem(item, toolLabel(m_tool));
        m_textItem = item;
        if (auto* canvas = qobject_cast<EditorCanvas*>(m_view)) canvas->setPassthrough(true);
        m_view->setFocus();
        item->setFocus();
        return;
    }

    m_dragging = true;
    m_start = scenePos;
    m_current = scenePos;
    m_stroke.clear();
    m_stroke.append(scenePos);
}

void EditorWindow::onCanvasMoved(const QPointF& scenePos) {
    if (!m_dragging) return;
    m_current = scenePos;
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic) m_stroke.append(scenePos);
    clearPreview();
    m_preview = buildPreviewItem();
    if (m_preview) m_scene->addItem(m_preview);
}

void EditorWindow::onCanvasReleased(const QPointF& scenePos) {
    if (!m_dragging) return;
    m_dragging = false;
    m_current = scenePos;
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic) m_stroke.append(scenePos);
    clearPreview();

    // 滤掉"点一下没动"的误触:描边类要有轨迹,拖拽类要有实际范围
    const bool isStroke = (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic);
    if (isStroke ? m_stroke.size() < 2 : QLineF(m_start, m_current).length() < 3.0) return;

    if (QGraphicsItem* item = buildFinalItem()) pushItem(item, toolLabel(m_tool));
}

void EditorWindow::bakeTextItem() {
    if (!m_textItem) return;
    m_textItem->setTextInteractionFlags(Qt::NoTextInteraction);
    // 空文字不入栈:撤销掉刚 push 的那条命令,避免留一个看不见的空图元
    if (m_textItem->toPlainText().trimmed().isEmpty()) m_undo->undo();
    m_textItem = nullptr;
    if (auto* canvas = qobject_cast<EditorCanvas*>(m_view)) canvas->setPassthrough(false);
}

QImage EditorWindow::compose() const {
    const QSize size = m_base.size();
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    m_scene->render(&painter, QRectF(QPointF(0, 0), QSizeF(size)), m_scene->sceneRect());
    painter.end();
    return out;
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
                (e->modifiers() & Qt::ShiftModifier) ? m_undo->redo() : m_undo->undo();
                return;
            }
            break;
        case Qt::Key_Y:
            if (ctrl) {
                m_undo->redo();
                return;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (ctrl) {
                finish();
                return;
            }
            break;
        default:
            break;
    }
    QWidget::keyPressEvent(e);
}

} // namespace nb

#include "editor_window.moc"
