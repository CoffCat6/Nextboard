#include "annotation_engine.hpp"

#include <QAbstractGraphicsShapeItem>
#include <QFont>
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QUndoCommand>
#include <QUndoStack>

#include <cmath>

namespace nb {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// 马赛克像素块边长(第一代 §3.4:底图预生成 8px 像素化版本)。
constexpr int kMosaicBlock = 8;

/// 模糊纹理的降采样倍数:缩到 1/k 再平滑放大 = 一次近似高斯模糊。
constexpr int kBlurBlock = 8;


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

/// 序号标注:实心圆 + 居中数字(圆点半径随粗细档位)。
/// 自绘成**一个**图元,保证「一次标注 = 一条撤销记录」。
class NumberBadgeItem : public QGraphicsItem {
public:
    NumberBadgeItem(int number, const QColor& color, qreal radius)
        : m_number(number), m_color(color), m_radius(radius) {}

    QRectF boundingRect() const override {
        return QRectF(-m_radius, -m_radius, m_radius * 2.0, m_radius * 2.0);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(m_color);
        painter->drawEllipse(QPointF(0, 0), m_radius, m_radius);
        // 数字色随底色明暗取反,避免"白底白字"看不清
        QFont font;
        font.setPixelSize(qMax(10, qRound(m_radius * 1.25)));
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(m_color.lightness() > 160 ? QColor(0x21, 0x25, 0x29) : QColor(Qt::white));
        painter->drawText(boundingRect(), Qt::AlignCenter, QString::number(m_number));
    }

private:
    int m_number;
    QColor m_color;
    qreal m_radius;
};

/// 底图背景项:直接持 QImage 自绘,免去 QPixmap::fromImage 的**整图拷贝**。
/// QImage 与 m_base 隐式共享,存引用不产生拷贝。
class ImageBackgroundItem : public QGraphicsItem {
public:
    explicit ImageBackgroundItem(const QImage& image) : m_image(image) {}
    QRectF boundingRect() const override {
        return QRectF(QPointF(0, 0), QSizeF(m_image.size()));
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->drawImage(QPointF(0, 0), m_image);
    }

private:
    QImage m_image;
};

} // namespace

QString toolLabel(EditorTool tool) {
    switch (tool) {
        case EditorTool::Pen: return QStringLiteral("画笔");
        case EditorTool::Arrow: return QStringLiteral("箭头");
        case EditorTool::Line: return QStringLiteral("直线");
        case EditorTool::Rect: return QStringLiteral("矩形");
        case EditorTool::Ellipse: return QStringLiteral("椭圆");
        case EditorTool::Mosaic: return QStringLiteral("马赛克");
        case EditorTool::Blur: return QStringLiteral("模糊");
        case EditorTool::Number: return QStringLiteral("序号");
        case EditorTool::Text: return QStringLiteral("文字");
        case EditorTool::Picker: return QStringLiteral("取色");
    }
    return QStringLiteral("标注");
}


Qt::CursorShape toolCursor(EditorTool tool) {
    switch (tool) {
        case EditorTool::Text: return Qt::IBeamCursor;
        case EditorTool::Picker: return Qt::PointingHandCursor;
        default: return Qt::CrossCursor;
    }
}

// ---------- AnnotationEngine ----------

AnnotationEngine::AnnotationEngine(QImage base, QObject* parent)
    : QObject(parent), m_base(std::move(base)) {
    m_scene = new QGraphicsScene(this);
    // 底图作为场景第一项(与 EditorWindow 同一语义:场景坐标 = 物理像素)
    auto* bg = new ImageBackgroundItem(m_base);
    bg->setZValue(-1);
    m_scene->addItem(bg);
    m_background = bg;
    m_undo = new QUndoStack(this);
}

AnnotationEngine::~AnnotationEngine() {
    // 与 EditorWindow 析构同因:先拆撤销栈(图元所有权在命令手里),
    // 场景由 QObject 父子关系随后析构 —— 顺序错会双重释放。
    delete m_undo;
    m_undo = nullptr;
}
void AnnotationEngine::setTool(EditorTool tool) {
    bakeTextItem();
    const EditorTool previous = m_tool;
    m_tool = tool;
    // 记住"上一个绘图工具",供取色器采样后回退(取色器自身不作回退目标)
    if (previous != EditorTool::Picker) m_prevTool = previous;
    // 马赛克纹理惰性生成:不用马赛克工具的图不必付这份全尺寸拷贝
    // (构造时就建 = 每个编辑器会话都白付,且带一次 2× 整图的构造期临时峰值)。
    // 与下方模糊纹理同一模式。
    if (tool == EditorTool::Mosaic && m_mosaicSource.isNull()) {
        // 先缩到 1/kMosaicBlock 再放大回来 = 像素化(FastTransformation 保证
        // 取整不插值),作为马赛克图元的纹理刷子按路径"揭示"。
        const int smallW = qMax(1, m_base.width() / kMosaicBlock);
        const int smallH = qMax(1, m_base.height() / kMosaicBlock);
        const QImage pixelated = m_base.scaled(smallW, smallH, Qt::IgnoreAspectRatio,
                                               Qt::FastTransformation)
                                     .scaled(m_base.size(), Qt::IgnoreAspectRatio,
                                             Qt::FastTransformation);
        m_mosaicSource = QPixmap::fromImage(pixelated);
    }
    // 模糊纹理惰性生成:不用模糊工具的图不必付这份全尺寸拷贝
    if (tool == EditorTool::Blur && m_blurSource.isNull()) {
        const int smallW = qMax(1, m_base.width() / kBlurBlock);
        const int smallH = qMax(1, m_base.height() / kBlurBlock);
        const QImage blurred = m_base.scaled(smallW, smallH, Qt::IgnoreAspectRatio,
                                             Qt::SmoothTransformation)
                                   .scaled(m_base.size(), Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
        m_blurSource = QPixmap::fromImage(blurred);
    }
}



qreal AnnotationEngine::badgeRadius() const {
    // 随粗细档位变化:细 14 / 中 18 / 粗 26
    return qBound(14.0, 10.0 + m_size * 2.0, 26.0);
}



void AnnotationEngine::pushItem(QGraphicsItem* item, const QString& label) {
    item->setFlag(QGraphicsItem::ItemIsSelectable, false);
    m_undo->push(new AddItemCommand(m_scene, item, label));   // redo() 完成首次入场景
}

QPainterPath AnnotationEngine::strokePath() const {
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

QGraphicsItem* AnnotationEngine::buildDragItem() const {
    const QRectF rect = QRectF(m_start, m_current).normalized();
    if (m_tool == EditorTool::Line) {
        auto* item = new QGraphicsLineItem(QLineF(m_start, m_current));
        item->setPen(QPen(m_color, m_size, Qt::SolidLine, Qt::RoundCap));
        return item;
    }
    if (m_tool == EditorTool::Rect || m_tool == EditorTool::Ellipse) {
        // 两者同为 QAbstractGraphicsShapeItem 子类,统一设置画笔/画刷
        auto* item = m_tool == EditorTool::Rect
                         ? static_cast<QAbstractGraphicsShapeItem*>(new QGraphicsRectItem(rect))
                         : static_cast<QAbstractGraphicsShapeItem*>(new QGraphicsEllipseItem(rect));
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

QGraphicsItem* AnnotationEngine::buildPreviewItem() const {
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic ||
        m_tool == EditorTool::Blur) {
        auto* item = new QGraphicsPathItem(strokePath());
        // 马赛克/模糊预览只画示意描边:真正的纹理在松手时按轮廓合成,
        // 避免每一帧都重算 QPainterPathStroker
        const bool eraser = (m_tool == EditorTool::Mosaic || m_tool == EditorTool::Blur);
        item->setPen(eraser
                         ? QPen(QColor(255, 255, 255, 170), qMax(m_size * 6, 18),
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                         : QPen(m_color, m_size, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        item->setBrush(Qt::NoBrush);
        return item;
    }
    return buildDragItem();
}

QGraphicsItem* AnnotationEngine::buildFinalItem() const {
    if (m_tool == EditorTool::Pen) {
        auto* item = new QGraphicsPathItem(strokePath());
        item->setPen(QPen(m_color, m_size, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        item->setBrush(Qt::NoBrush);
        return item;
    }
    if (m_tool == EditorTool::Mosaic || m_tool == EditorTool::Blur) {
        // 以笔画为 mask:先把折线描成有宽度的轮廓,再用像素化/模糊纹理填充该轮廓
        QPainterPathStroker stroker;
        stroker.setWidth(qMax(m_size * 6, 18));
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        auto* item = new QGraphicsPathItem(stroker.createStroke(strokePath()));
        item->setPen(Qt::NoPen);
        item->setBrush(QBrush(m_tool == EditorTool::Mosaic ? m_mosaicSource : m_blurSource));
        return item;
    }
    return buildDragItem();
}

void AnnotationEngine::clearPreview() {
    if (!m_preview) return;
    m_scene->removeItem(m_preview);
    delete m_preview;
    m_preview = nullptr;
}

void AnnotationEngine::onCanvasPressed(const QPointF& scenePos) {
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
        item->setFocus();
        return;
    }

    if (m_tool == EditorTool::Number) {
        // 序号:点击即落一个圆点徽标,编号会话内自增(不参与拖拽)
        auto* item = new NumberBadgeItem(m_nextNumber, m_color, badgeRadius());
        item->setPos(scenePos);
        pushItem(item, toolLabel(m_tool));
        ++m_nextNumber;
        return;
    }

    if (m_tool == EditorTool::Picker) {
        // 取色:从**底图**取样(不应被已画的标注影响);采完切回上一个绘图工具
        const QPoint p = scenePos.toPoint();
        if (QRect(0, 0, m_base.width(), m_base.height()).contains(p)) {
            m_color = m_base.pixelColor(p);
        }
        setTool(m_prevTool);
        return;
    }

    m_dragging = true;
    m_start = scenePos;
    m_current = scenePos;
    m_stroke.clear();
    m_stroke.append(scenePos);
}

void AnnotationEngine::onCanvasMoved(const QPointF& scenePos) {
    if (!m_dragging) return;
    m_current = scenePos;
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic ||
        m_tool == EditorTool::Blur)
        m_stroke.append(scenePos);
    clearPreview();
    m_preview = buildPreviewItem();
    if (m_preview) m_scene->addItem(m_preview);
}

void AnnotationEngine::onCanvasReleased(const QPointF& scenePos) {
    if (!m_dragging) return;
    m_dragging = false;
    m_current = scenePos;
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic ||
        m_tool == EditorTool::Blur)
        m_stroke.append(scenePos);
    clearPreview();

    // 滤掉"点一下没动"的误触:描边类要有轨迹,拖拽类要有实际范围
    const bool isStroke = (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic ||
                           m_tool == EditorTool::Blur);
    if (isStroke ? m_stroke.size() < 2 : QLineF(m_start, m_current).length() < 3.0) return;

    if (QGraphicsItem* item = buildFinalItem()) pushItem(item, toolLabel(m_tool));
}

void AnnotationEngine::addText(const QString& text, const QPointF& topLeft,
                               const QColor& color, int pixelSize) {
    if (text.trimmed().isEmpty()) return;
    auto* item = new QGraphicsTextItem;
    item->setPlainText(text);
    item->setDefaultTextColor(color);
    QFont font = item->font();
    font.setPixelSize(qMax(8, pixelSize));
    item->setFont(font);
    // 已烘焙:不再进入编辑态(浮条没有视图,进编辑态没人能输入)
    item->setTextInteractionFlags(Qt::NoTextInteraction);
    item->setPos(topLeft);
    pushItem(item, toolLabel(EditorTool::Text));
}

void AnnotationEngine::bakeTextItem() {
    if (!m_textItem) return;
    m_textItem->setTextInteractionFlags(Qt::NoTextInteraction);
    // 空文字不入栈:撤销掉刚 push 的那条命令,避免留一个看不见的空图元
    if (m_textItem->toPlainText().trimmed().isEmpty()) m_undo->undo();
    m_textItem = nullptr;
}

QImage AnnotationEngine::compose() const {
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

QImage AnnotationEngine::compose(const QRect& physicalRect) const {
    if (!physicalRect.isValid() || physicalRect.isEmpty()) return compose();
    QImage out(physicalRect.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // 只渲染选区对应的场景区域(源 = 物理像素 = 场景坐标)
    m_scene->render(&painter, QRectF(QPointF(0, 0), QSizeF(physicalRect.size())),
                    QRectF(physicalRect));
    painter.end();
    return out;
}

QColor AnnotationEngine::pickColor(const QPointF& scenePos) {
    const QPoint p = scenePos.toPoint();
    if (QRect(0, 0, m_base.width(), m_base.height()).contains(p)) {
        m_color = m_base.pixelColor(p);
        emit colorPicked(m_color);
    }
    return m_color;
}
} // namespace nb
