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
#include <QGraphicsView>
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
    /// 换底图(构图操作用)。尺寸可能变化,必须先 prepareGeometryChange,
    /// 否则场景按旧包围盒做索引,会出现"图换了但区域没跟上"的残影。
    void setImage(const QImage& image) {
        prepareGeometryChange();
        m_image = image;
        update();
    }

private:
    QImage m_image;
};

/// 裁剪交互的手柄序号(四角 0-3 → 四边中点 4-7;与 cropHandleAt/缩放逻辑共用)。
constexpr int kHandleTL = 0, kHandleTR = 1, kHandleBR = 2, kHandleBL = 3;
constexpr int kHandleT = 4, kHandleR = 5, kHandleB = 6, kHandleL = 7;
constexpr int kCropMove = 8;    // 框内拖动 = 整体移动
constexpr int kCropNew = 9;     // 框外/无框按下 = 拖一个新框

/// compose 期间隐藏裁剪覆盖层:覆盖层是交互提示,绝不能被合成进输出图
/// (用户裁到一半按「完成/贴图/OCR」是真实场景)。析构恢复,两个重载共用。
class CropOverlayHide {
public:
    explicit CropOverlayHide(QGraphicsItem* item) : m_item(item) {
        if (m_item) {
            m_wasVisible = m_item->isVisible();
            m_item->setVisible(false);
        }
    }
    ~CropOverlayHide() {
        if (m_item) m_item->setVisible(m_wasVisible);
    }
    CropOverlayHide(const CropOverlayHide&) = delete;
    CropOverlayHide& operator=(const CropOverlayHide&) = delete;

private:
    QGraphicsItem* m_item = nullptr;
    bool m_wasVisible = true;
};

/// 90° 旋转的像素置换(无损:不做插值,反复旋转不糊化)。
/// 与 PinWindow::rotate90cw 同一手法 —— 引擎不引贴图模块的头,两处各自实现,
/// 改动时互看一眼。统一按 32 位 ARGB 处理,源图其他格式先转换。
QImage rotateImage90(const QImage& src, bool clockwise) {
    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    const int w = in.width();
    const int h = in.height();
    QImage out(h, w, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            if (clockwise) {
                // 顺时针:源 (x, y) → 目标 (h-1-y, x)
                reinterpret_cast<QRgb*>(out.scanLine(x))[h - 1 - y] = line[x];
            } else {
                // 逆时针:源 (x, y) → 目标 (y, w-1-x)
                reinterpret_cast<QRgb*>(out.scanLine(w - 1 - x))[y] = line[x];
            }
        }
    }
    return out;
}

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
        case EditorTool::Crop: return QStringLiteral("裁剪");
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

// 工具栏元数据:定义只此一份,遮罩浮条与编辑窗口都引用这里(见头文件注释)。
// 顺序:绘制类 → 擦除类(马赛克/模糊) → 标注类(序号/文字) → 取色 → 构图类(裁剪)
// 裁剪放最末:与"选工具画东西"的语义距离最远,且紧贴调色板分隔符自然分组。
// (浮条宿主会在 AnnotationToolbar 里跳过 Crop,见其 rebuildActions。)
const QList<EditorTool>& toolbarTools() {
    static const QList<EditorTool> kTools = {
        EditorTool::Pen,     EditorTool::Arrow, EditorTool::Line, EditorTool::Rect,
        EditorTool::Ellipse, EditorTool::Mosaic, EditorTool::Blur, EditorTool::Number,
        EditorTool::Text,    EditorTool::Picker, EditorTool::Crop,
    };
    return kTools;
}

const QList<QColor>& annotationPalette() {
    // 第一代 PALETTE 七色(与 docs/phase5.md §3.4 一致)
    static const QList<QColor> kPalette = {
        QColor(0xE0, 0x31, 0x31), QColor(0xF7, 0x67, 0x07), QColor(0xFA, 0xB0, 0x05),
        QColor(0x40, 0xC0, 0x57), QColor(0x1C, 0x7E, 0xD6), QColor(0x21, 0x25, 0x29),
        QColor(0xFF, 0xFF, 0xFF),
    };
    return kPalette;
}

const QList<int>& strokeSizes() {
    static const QList<int> kSizes = {2, 4, 8};
    return kSizes;
}

const QList<int>& textFontSizes() {
    static const QList<int> kSizes = {24, 36, 48};
    return kSizes;
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
    // 场景矩形**钉死**为底图尺寸:QGraphicsScene 不设则由图元包围盒隐式推导,
    // 粗笔画贴边画出图外一点点,sceneRect 就比底图大一圈,compose()(以 sceneRect
    // 为源)会把整图按那一圈挤压变形;fitCanvas 也会跟着算错。构图操作换底图时
    // 同步重钉(见 swapBase)。
    m_scene->setSceneRect(QRectF(QPointF(0, 0), QSizeF(m_base.size())));
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
    // 记住"上一个绘图工具",供取色器/裁剪完成后回退(两者自身不作回退目标)
    if (previous != EditorTool::Picker && previous != EditorTool::Crop) m_prevTool = previous;
    // 离开裁剪工具 = 放弃未应用的裁剪框(覆盖层拆掉;框只是交互中间态,不入撤销栈)
    if (previous == EditorTool::Crop && tool != EditorTool::Crop) {
        m_cropRect = QRectF();
        m_cropDragHandle = -1;
        m_cropPressButton = -1;
        m_cropHoverButton = -1;
        clearCropOverlay();
    }
    // 进入裁剪工具:已有框保留(cropReady 场景下重进),通常从空框开始
    if (tool == EditorTool::Crop) rebuildCropOverlay();
    ensureFilterTextures();
}

void AnnotationEngine::ensureFilterTextures() {
    // 马赛克纹理惰性生成:不用马赛克工具的图不必付这份全尺寸拷贝
    // (构造时就建 = 每个编辑器会话都白付,且带一次 2× 整图的构造期临时峰值)。
    // 与下方模糊纹理同一模式。换底图后缓存失效,swapBase 会清空再调到这里 ——
    // 若此刻正用马赛克/模糊,必须立即按新底图重建,否则空纹理画笔画出无效内容。
    if (m_tool == EditorTool::Mosaic && m_mosaicSource.isNull()) {
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
    if (m_tool == EditorTool::Blur && m_blurSource.isNull()) {
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

    if (m_tool == EditorTool::Crop) {
        // 裁剪:按钮最优先(画在最上层,命中判定同序);然后手柄缩放、框内移动,
        // 框外/无框 = 拖新框。按在按钮上时不进入拖拽 —— 松手仍在按钮上才触发
        // (与系统按钮同语义:按下后移出去松手 = 放弃)。
        m_cropPressButton = cropButtonAt(scenePos);
        if (m_cropPressButton >= 0) return;
        m_cropDragHandle = cropHandleAt(scenePos);
        if (m_cropDragHandle < 0 && m_cropRect.contains(scenePos)) m_cropDragHandle = kCropMove;
        if (m_cropDragHandle < 0) m_cropDragHandle = kCropNew;
        m_cropAnchor = scenePos;
        m_cropAnchorRect = m_cropRect;
        if (m_cropDragHandle == kCropNew) m_cropRect = QRectF(scenePos, scenePos);
        m_dragging = true;
        rebuildCropOverlay();
        return;
    }

    m_dragging = true;
    m_start = scenePos;
    m_current = scenePos;
    m_stroke.clear();
    m_stroke.append(scenePos);
}

void AnnotationEngine::onCanvasMoved(const QPointF& scenePos) {
    if (!m_dragging) {
        // 裁剪按钮悬停高亮:鼠标进/出按钮要有可见反馈,不然"能不能点"全靠猜
        if (m_tool == EditorTool::Crop) updateCropHover(scenePos);
        return;
    }

    if (m_tool == EditorTool::Crop) {
        if (m_cropDragHandle == kCropNew) {
            m_cropRect = QRectF(m_cropAnchor, scenePos).normalized();
        } else if (m_cropDragHandle == kCropMove) {
            m_cropRect = m_cropAnchorRect.translated(scenePos - m_cropAnchor);
        } else {
            // 手柄缩放:对角/对边锚定在按下时的原框上,拖动侧跟随鼠标
            const QRectF a = m_cropAnchorRect;
            switch (m_cropDragHandle) {
                case kHandleTL: m_cropRect = QRectF(scenePos, a.bottomRight()); break;
                case kHandleTR: m_cropRect = QRectF(QPointF(scenePos.x(), a.top()), a.bottomLeft()); break;
                case kHandleBR: m_cropRect = QRectF(a.topLeft(), scenePos); break;
                case kHandleBL: m_cropRect = QRectF(a.topRight(), QPointF(scenePos.x(), a.bottom())); break;
                case kHandleT:  m_cropRect = QRectF(QPointF(a.left(), scenePos.y()), a.bottomRight()); break;
                case kHandleR:  m_cropRect = QRectF(a.topLeft(), QPointF(scenePos.x(), a.bottom())); break;
                case kHandleB:  m_cropRect = QRectF(a.topLeft(), QPointF(a.right(), scenePos.y())); break;
                case kHandleL:  m_cropRect = QRectF(QPointF(scenePos.x(), a.top()), a.bottomRight()); break;
            }
            m_cropRect = m_cropRect.normalized();
        }
        clampCropRect();
        rebuildCropOverlay();
        return;
    }

    m_current = scenePos;
    if (m_tool == EditorTool::Pen || m_tool == EditorTool::Mosaic ||
        m_tool == EditorTool::Blur)
        m_stroke.append(scenePos);
    clearPreview();
    m_preview = buildPreviewItem();
    if (m_preview) m_scene->addItem(m_preview);
}

void AnnotationEngine::onCanvasReleased(const QPointF& scenePos) {
    // 裁剪按钮:按下与松手都在按钮上才触发(移出松手 = 放弃,系统按钮同语义)。
    // 放在 !m_dragging 早退**之前** —— 按下按钮时不进入拖拽态。
    if (m_tool == EditorTool::Crop && m_cropPressButton >= 0) {
        const int pressed = m_cropPressButton;
        m_cropPressButton = -1;
        if (cropButtonAt(scenePos) == pressed) {
            if (pressed == 0) applyCrop();
            else cancelCrop();
        }
        return;
    }
    if (!m_dragging) return;
    m_dragging = false;

    if (m_tool == EditorTool::Crop) {
        if (m_cropDragHandle == kCropNew) {
            m_cropRect = QRectF(m_cropAnchor, scenePos).normalized();
            clampCropRect();
            // 拖新框松手时太小 = 误触:当作没画框(移动/缩放不在此列,尺寸有保底)
            if (m_cropRect.width() < 2.0 || m_cropRect.height() < 2.0) m_cropRect = QRectF();
        }
        m_cropDragHandle = -1;
        rebuildCropOverlay();
        return;
    }

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

bool AnnotationEngine::editingTextContains(const QPointF& scenePos) const {
    if (!m_textItem) return false;
    const QRectF box = m_textItem->sceneBoundingRect();
    if (box.isEmpty()) {
        // 刚落下、还没输入内容:包围盒为空 → 用"落点附近一小块"当命中区,
        // 否则刚点下去的那次点击会被判成"点在文字外"而立刻又落一个新图元。
        const QPointF at = m_textItem->pos();
        const qreal pad = qMax(12.0, m_fontSize * 0.6);
        return QRectF(at.x() - pad, at.y() - pad, pad * 2, pad * 2).contains(scenePos);
    }
    // 容差:文字光标本身有宽度,框边上几像素的点击应仍算"在文字内"
    return box.adjusted(-4, -4, 4, 4).contains(scenePos);
}

void AnnotationEngine::bakeTextItem() {
    if (!m_textItem) return;
    m_textItem->setTextInteractionFlags(Qt::NoTextInteraction);
    // 空文字不入栈:撤销掉刚 push 的那条命令,避免留一个看不见的空图元
    if (m_textItem->toPlainText().trimmed().isEmpty()) m_undo->undo();
    m_textItem = nullptr;
}

// ---------- 构图类操作:裁剪/旋转(docs/18 §3.13)----------

/// 裁剪覆盖层:框外遮罩 + 白虚线边框 + 三分线 + 8 手柄 + 尺寸标签。
/// 纯交互提示,**不入撤销栈**;compose() 渲染前隐藏(见 CropOverlayHide)。
class AnnotationEngine::CropOverlayItem : public QGraphicsItem {
public:
    /// 手柄位置(序号与 kHandleTL..kHandleL 一一对应;命中判定与绘制共用一份)。
    static QList<QPointF> handlePoints(const QRectF& r) {
        return {
            r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(),
            QPointF((r.left() + r.right()) / 2.0, r.top()),
            QPointF(r.right(), (r.top() + r.bottom()) / 2.0),
            QPointF((r.left() + r.right()) / 2.0, r.bottom()),
            QPointF(r.left(), (r.top() + r.bottom()) / 2.0),
        };
    }

    /// 确认(0)/取消(1)按钮的场景矩形。**绘制与命中判定共用这一份**,
    /// 两条路径(viewScale / painter 变换)取同一倍率即严格一致。
    /// 布局:右对齐在裁剪框下方;贴住底图下缘时翻进框内(按钮永远可点);
    /// 水平方向整体夹回底图内。尺寸随缩放取反 = 屏幕常量。
    static QList<QRectF> buttonRects(const QRectF& crop, const QSizeF& base, qreal scale) {
        const qreal size = kButtonPx / scale;
        const qreal gap = kButtonGapPx / scale;
        const qreal margin = kButtonMarginPx / scale;
        const qreal totalW = size * 2.0 + gap;
        qreal y = crop.bottom() + margin;
        if (y + size > base.height()) y = crop.bottom() - size - margin;   // 贴底 → 翻进框内
        qreal x = crop.right() - totalW;
        x = qBound(margin, x, qMax(margin, base.width() - totalW - margin));
        return { QRectF(x, y, size, size), QRectF(x + size + gap, y, size, size) };
    }

    void setBaseSize(const QSize& size) {
        if (m_baseSize == size) return;
        prepareGeometryChange();
        m_baseSize = size;
        update();
    }
    void setRect(const QRectF& rect) {
        prepareGeometryChange();
        m_rect = rect.normalized();
        update();
    }
    /// 悬停高亮(-1 = 无;仅视觉状态,不改几何)。
    void setHover(int button) {
        if (m_hover == button) return;
        m_hover = button;
        update();
    }

    QRectF boundingRect() const override {
        // 罩住整张底图(四块遮罩)+ 手柄/标签/按钮在极限缩放下朝外的外扩。
        // 余量按 0.2× 下限放大:按钮组在框下方最多伸出 (10+32)/0.2 ≈ 210 场景像素。
        return QRectF(QPointF(0, 0), QSizeF(m_baseSize)).adjusted(-kPad, -kPad, kPad, kPad);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const QRectF full(QPointF(0, 0), QSizeF(m_baseSize));
        const QRectF r = m_rect;
        if (!full.isValid() || r.isEmpty()) return;

        // 屏幕常量元素(手柄/字号)按视图缩放取反:缩到 0.3× 时按场景尺寸画的
        // 手柄只剩 3 屏幕像素,根本点不中。painter 变换已含视图缩放,m11 即倍率;
        // 倍率下限 0.2 与 boundingRect 的外扩余量配套(见 kPad)。
        const qreal scale = qBound(0.2, painter->transform().m11(), 8.0);
        const qreal inv = 1.0 / scale;
        const qreal handle = 5.0 * inv;   // 手柄半边长 ≈ 10 屏幕像素见方

        // ① 框外遮罩(四块半透明黑)
        const QColor shade(0, 0, 0, 120);
        painter->fillRect(QRectF(full.left(), full.top(), full.width(), r.top() - full.top()), shade);
        painter->fillRect(QRectF(full.left(), r.bottom(), full.width(), full.bottom() - r.bottom()), shade);
        painter->fillRect(QRectF(full.left(), r.top(), r.left() - full.left(), r.height()), shade);
        painter->fillRect(QRectF(r.right(), r.top(), full.right() - r.right(), r.height()), shade);

        // ② 边框:深色实线衬底 + 白色虚线。线宽 0 = cosmetic 笔,
        //    任意缩放下都是 1 屏幕像素,不会随缩放变糊或变粗。
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(0, 0, 0, 170), 0));
        painter->drawRect(r);
        painter->setPen(QPen(Qt::white, 0, Qt::DashLine));
        painter->drawRect(r);

        // ③ 三分构图线(取景辅助,低存在感)
        painter->setPen(QPen(QColor(255, 255, 255, 90), 0));
        for (int i = 1; i <= 2; ++i) {
            const qreal x = r.left() + r.width() * i / 3.0;
            const qreal y = r.top() + r.height() * i / 3.0;
            painter->drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            painter->drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }

        // ④ 8 个手柄:白色方块 + 深色描边
        painter->setPen(QPen(QColor(0x21, 0x25, 0x29), 0));
        for (const QPointF& pt : handlePoints(r)) {
            const QRectF h(pt.x() - handle, pt.y() - handle, handle * 2.0, handle * 2.0);
            painter->fillRect(h, Qt::white);
            painter->drawRect(h);
        }

        // ⑤ 尺寸标签:框内右下角(框太矮贴不住时上移),深色底白字
        const QString label = QStringLiteral("%1 × %2")
                                  .arg(qRound(r.width())).arg(qRound(r.height()));
        QFont font = painter->font();
        font.setPixelSize(qMax(9, qRound(12 * inv)));
        painter->setFont(font);
        const QFontMetricsF fm(font);
        const QSizeF box = fm.size(0, label) + QSizeF(10 * inv, 6 * inv);
        QPointF at(r.right() - box.width(), r.bottom() - box.height());
        if (at.y() < r.top()) at.ry() = r.top();   // 框比标签还矮:顶对齐,不全遮
        const QRectF labelRect(at, box);
        painter->fillRect(labelRect, QColor(0x21, 0x25, 0x29, 200));
        painter->setPen(QPen(Qt::white, 0));
        painter->drawText(labelRect, Qt::AlignCenter, label);

        // ⑥ 确认/取消按钮:绿 ✓ / 红 ✕(几何字形,与 tool_icons 同一原则 ——
        //    不用字体字形/emoji,跨字体渲染不可控)。悬停提亮,暗示可点。
        //    没有键盘的用户也能完成裁剪(Enter/Esc 之外的完整指针路径)。
        const QList<QRectF> buttons = buttonRects(r, QSizeF(m_baseSize), scale);
        const QColor kConfirm(0x40, 0xC0, 0x57);   // 调色板绿(annotationPalette 同款)
        const QColor kCancel(0xE0, 0x31, 0x31);    // 默认标注红
        for (int i = 0; i < buttons.size(); ++i) {
            const QRectF& b = buttons.at(i);
            const QColor fill = i == 0 ? kConfirm : kCancel;
            painter->setPen(QPen(QColor(0, 0, 0, 90), 0));   // 细轮廓,任何底色上有边界
            painter->setBrush(m_hover == i ? fill.lighter(118) : fill);
            painter->drawRoundedRect(b, 8 * inv, 8 * inv);
            // 字形笔画按屏幕像素定宽(2.2 屏幕像素,圆头),缩放下粗细稳定
            painter->setBrush(Qt::NoBrush);
            QPen glyph(Qt::white, qMax(1.5, 2.2 * inv));
            glyph.setCapStyle(Qt::RoundCap);
            glyph.setJoinStyle(Qt::RoundJoin);
            painter->setPen(glyph);
            const QPointF c = b.center();
            const qreal s = b.width() * 0.24;
            if (i == 0) {
                // ✓:短下撇 + 长上挑
                painter->drawLine(QPointF(c.x() - s, c.y() - s * 0.1),
                                  QPointF(c.x() - s * 0.25, c.y() + s * 0.6));
                painter->drawLine(QPointF(c.x() - s * 0.25, c.y() + s * 0.6),
                                  QPointF(c.x() + s, c.y() - s * 0.6));
            } else {
                // ✕:两条对角线
                painter->drawLine(QPointF(c.x() - s, c.y() - s), QPointF(c.x() + s, c.y() + s));
                painter->drawLine(QPointF(c.x() + s, c.y() - s), QPointF(c.x() - s, c.y() + s));
            }
        }
    }

private:
    /// boundingRect 的外扩余量(场景像素):覆盖极限缩放(0.2×)下手柄、
    /// 标签与按钮组的出界(按钮组下方最多 (10+32)/0.2 = 210)。
    static constexpr qreal kPad = 240.0;
    /// 按钮几何(屏幕像素):见 buttonRects。
    static constexpr qreal kButtonPx = 32.0;
    static constexpr qreal kButtonGapPx = 8.0;
    static constexpr qreal kButtonMarginPx = 10.0;

    QSize m_baseSize;
    QRectF m_rect;
    int m_hover = -1;
};

/// 构图变换的撤销命令。undo/redo 都是「绝对还原」—— 换回那张底图、逐图元恢复
/// pos/transform —— 而不是相对变换:QUndoStack::push() 会立刻调一次 redo(),
/// 相对变换会把刚应用过的矩阵再乘一遍。
///
/// 图元指针的所有权仍在各自的 AddItemCommand 手里,这里只存裸指针:
/// 撤销栈截断(新命令 push 清空 redo 分支)总是先销毁更晚的命令,TransformCommand
/// 只会与它引用的图元"同生或后死",不会悬空。
class AnnotationEngine::TransformCommand : public QUndoCommand {
public:
    struct ItemState {
        QGraphicsItem* item;
        QPointF pos;
        QTransform xform;
    };

    TransformCommand(AnnotationEngine* engine, const QString& label, QImage baseBefore,
                     QImage baseAfter, const QVector<ItemState>& before,
                     const QVector<ItemState>& after)
        : m_engine(engine), m_baseBefore(std::move(baseBefore)),
          m_baseAfter(std::move(baseAfter)), m_before(before), m_after(after) {
        setText(label);
    }

    void undo() override {
        m_engine->swapBase(m_baseBefore);
        apply(m_before);
        emit m_engine->baseChanged();
    }
    void redo() override {
        m_engine->swapBase(m_baseAfter);
        apply(m_after);
        emit m_engine->baseChanged();
    }

private:
    static void apply(const QVector<ItemState>& states) {
        // 对捕获的全量图元施变 —— 含已随撤销暂离场景的图元:它们回场景时
        // 坐标必须已经对上当时的底图(LIFO 一致性)。
        for (const ItemState& s : states) {
            s.item->setPos(s.pos);
            s.item->setTransform(s.xform);
        }
    }

    AnnotationEngine* m_engine;
    QImage m_baseBefore;
    QImage m_baseAfter;
    QVector<ItemState> m_before;
    QVector<ItemState> m_after;
};

bool AnnotationEngine::cropReady() const {
    return m_tool == EditorTool::Crop && m_cropRect.isValid()
           && m_cropRect.width() >= 2.0 && m_cropRect.height() >= 2.0;
}

void AnnotationEngine::rotate90(bool clockwise) {
    bakeTextItem();
    // 旋转让裁剪框几何失去意义(方向/范围都变了):先弃框(swapBase 兜底再清一次)
    m_cropRect = QRectF();
    clearCropOverlay();
    // 场景坐标的仿射:顺时针 (x,y)→(H−y,x);逆时针 (x,y)→(y,W−x)。
    // QTransform(a,b,c,d,e,f):x' = a·x + c·y + e;y' = b·x + d·y + f。
    const QTransform T = clockwise
                             ? QTransform(0, 1, -1, 0, qreal(m_base.height()), 0.0)
                             : QTransform(0, -1, 1, 0, 0.0, qreal(m_base.width()));
    applyTransform(T, rotateImage90(m_base, clockwise),
                   clockwise ? QStringLiteral("旋转 90°(顺时针)")
                             : QStringLiteral("旋转 90°(逆时针)"));
    // 只有正处裁剪工具时才回退到上一绘图工具;其它工具下旋转是顺手动作,不该抢工具
    if (m_tool == EditorTool::Crop) setTool(m_prevTool);
}

void AnnotationEngine::applyCrop() {
    if (!cropReady()) return;
    bakeTextItem();
    const QRect rect = m_cropRect.toAlignedRect().intersected(m_base.rect());
    if (rect.width() < 2 || rect.height() < 2) return;
    // 图元保持可撤销:平移进新坐标系,被裁掉的部分由钉死的 sceneRect 裁掉;
    // 撤销时 TransformCommand 把它们原样放回 —— 不做"裁掉即删",那是另一层语义。
    applyTransform(QTransform::fromTranslate(-qreal(rect.x()), -qreal(rect.y())),
                   m_base.copy(rect), QStringLiteral("裁剪"));
    setTool(m_prevTool);   // 裁剪完成回到上一个绘图工具(与取色器回退同一先例)
}

void AnnotationEngine::applyTransform(const QTransform& T, const QImage& newBase,
                                      const QString& label) {
    clearPreview();
    // 此刻 m_textItem 已由调用方烘焙、m_preview 已清:场景里除底图与裁剪覆盖层外
    // 全是"已定稿"的标注图元,正是要跟着变换的全部对象。
    const auto capture = [this]() {
        QVector<TransformCommand::ItemState> states;
        const auto items = m_scene->items();
        for (QGraphicsItem* item : items) {
            if (item == m_background || item == m_cropOverlay) continue;
            states.append({item, item->pos(), item->transform()});
        }
        return states;
    };

    const QImage baseBefore = m_base;
    const QVector<TransformCommand::ItemState> before = capture();
    for (const TransformCommand::ItemState& s : before) {
        // 变换规则(实测修正过一次,勿"顺手简化"):
        //   QGraphicsItem 场景映射 = translate(pos) ∘ transform,即 scene(q) = M(q) + p。
        //   外面再套 T:scene'(q) = T(M(q) + p) = (T∘M)(q) + A·p(A = T 的线性部分)。
        //   ① pos 用**线性部分**作用:A·p = T(p) − T(0) —— 直接 T.map(pos) 会把
        //      T 的平移分量重复计入一次(旋转后标注整体飞出图外,验收 A2 抓到过);
        //   ② transform 用 Qt 行向量约定:M * T = 先 M 后 T(写 T * M 顺序反了)。
        const QPointF shift = T.map(QPointF(0, 0));
        s.item->setPos(T.map(s.pos) - shift);
        s.item->setTransform(s.xform * T);
    }
    swapBase(newBase);
    const QVector<TransformCommand::ItemState> after = capture();
    // push() 会立刻调 redo():它是绝对还原(幂等),再跑一遍无害
    m_undo->push(new TransformCommand(this, label, baseBefore, newBase, before, after));
}

void AnnotationEngine::swapBase(const QImage& next) {
    m_base = next;
    static_cast<ImageBackgroundItem*>(m_background)->setImage(m_base);
    m_scene->setSceneRect(QRectF(QPointF(0, 0), QSizeF(m_base.size())));
    // 裁剪框随底图失效:undo/redo 走到这里同样成立(旧框对新底图没有意义)
    m_cropRect = QRectF();
    clearCropOverlay();
    // 马赛克/模糊纹理是旧底图的降采样,对不上新底图:清空;正用则立即按新底图重建
    m_mosaicSource = QPixmap();
    m_blurSource = QPixmap();
    ensureFilterTextures();
}

void AnnotationEngine::rebuildCropOverlay() {
    if (m_tool != EditorTool::Crop || !m_cropRect.isValid()
        || m_cropRect.width() < 1.0 || m_cropRect.height() < 1.0) {
        clearCropOverlay();
        return;
    }
    if (!m_cropOverlay) {
        m_cropOverlay = new CropOverlayItem;
        m_cropOverlay->setZValue(1000);   // 盖在一切标注之上
        m_scene->addItem(m_cropOverlay);
    }
    m_cropOverlay->setBaseSize(m_base.size());
    m_cropOverlay->setRect(m_cropRect);
}

void AnnotationEngine::clearCropOverlay() {
    if (!m_cropOverlay) return;
    m_scene->removeItem(m_cropOverlay);
    delete m_cropOverlay;
    m_cropOverlay = nullptr;
}

int AnnotationEngine::cropHandleAt(const QPointF& scenePos) const {
    if (!m_cropRect.isValid()) return -1;
    // 手柄按屏幕尺寸画,命中判定同口径(容差随视图缩放取反)
    const qreal tol = qMax(6.0, 8.0 / viewScale());
    int index = 0;
    for (const QPointF& pt : CropOverlayItem::handlePoints(m_cropRect)) {
        if (qAbs(scenePos.x() - pt.x()) <= tol && qAbs(scenePos.y() - pt.y()) <= tol)
            return index;
        ++index;
    }
    return -1;
}

qreal AnnotationEngine::viewScale() const {
    // 无视图时(离屏合成/验收直调)按 1.0;下限 0.2 与覆盖层的绘制口径一致
    if (m_scene->views().isEmpty()) return 1.0;
    return qMax(m_scene->views().constFirst()->transform().m11(), 0.2);
}

int AnnotationEngine::cropButtonAt(const QPointF& scenePos) const {
    if (!m_cropRect.isValid()) return -1;
    const QList<QRectF> buttons =
        CropOverlayItem::buttonRects(m_cropRect, QSizeF(m_base.size()), viewScale());
    for (int i = 0; i < buttons.size(); ++i) {
        // 少量外扩容差:贴着按钮边缘的点击也算命中(按钮本身已是屏幕常量大小)
        if (buttons.at(i).adjusted(-3.0, -3.0, 3.0, 3.0).contains(scenePos)) return i;
    }
    return -1;
}

QRectF AnnotationEngine::cropButtonRect(int index) const {
    if (!m_cropRect.isValid()) return QRectF();
    const QList<QRectF> buttons =
        CropOverlayItem::buttonRects(m_cropRect, QSizeF(m_base.size()), viewScale());
    return (index >= 0 && index < buttons.size()) ? buttons.at(index) : QRectF();
}

void AnnotationEngine::cancelCrop() {
    // 取消 = 丢弃裁剪框并回到上一个绘图工具(与 Esc 退出裁剪、取消按钮共用语义)
    if (m_tool != EditorTool::Crop) return;
    setTool(m_prevTool);
}

void AnnotationEngine::updateCropHover(const QPointF& scenePos) {
    // 悬停态只影响视觉(提亮),不改几何;在覆盖层类定义之后实现,
    // 这里才能用到完整类型(setHover)。
    const int hover = cropButtonAt(scenePos);
    if (hover == m_cropHoverButton) return;
    m_cropHoverButton = hover;
    if (m_cropOverlay) m_cropOverlay->setHover(hover);
}

void AnnotationEngine::clampCropRect() {
    if (!m_cropRect.isValid()) return;
    m_cropRect = m_cropRect.intersected(QRectF(QPointF(0, 0), QSizeF(m_base.size())));
}

QImage AnnotationEngine::compose() const {
    const QSize size = m_base.size();
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    // 裁剪覆盖层不进输出(用户裁到一半按「完成」是真实场景)
    const CropOverlayHide cropHidden(m_cropOverlay);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // 源矩形**显式**取底图尺寸,不吃 sceneRect 的隐式推导:
    // 标注画出图外时隐式 sceneRect 会比底图大一圈,整图被按圈挤压变形。
    const QRectF target(QPointF(0, 0), QSizeF(size));
    m_scene->render(&painter, target, target);
    painter.end();
    return out;
}

QImage AnnotationEngine::compose(const QRect& physicalRect) const {
    if (!physicalRect.isValid() || physicalRect.isEmpty()) return compose();
    QImage out(physicalRect.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const CropOverlayHide cropHidden(m_cropOverlay);
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
