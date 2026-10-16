#include "editor/tool_icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

#include <cmath>

namespace nb {

// MSVC 的 <cmath> 默认不暴露圆周率常量(需 _USE_MATH_DEFINES),自己定一个,
// 免得这个文件对编译宏产生隐式依赖。
constexpr qreal kPi = 3.14159265358979323846;

namespace {

/// 16px 视口下的基准线宽(与 HeaderIcon.qml 的 stroke 一致)
constexpr qreal kBaseStroke = 1.5;
constexpr qreal kViewport = 16.0;

/// 造一张透明画布并把画笔按 16px 视口归一化:
/// 之后绘制代码里的数字都写"16px 基准值",与 QML 侧同一手法。
/// dpr 参与 —— 高分屏上图标不发虚(与"截图画布"同一类要注意的点)。
QPixmap makeCanvas(int size, qreal dpr) {
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    return pm;
}

void setupPainter(QPainter& p, int size, const QColor& ink) {
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(size / kViewport, size / kViewport);   // → 进入 16px 基准坐标系
    p.setBrush(Qt::NoBrush);
    QPen pen(ink, kBaseStroke);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
}

/// 画一条圆头线段(16px 基准坐标)。
void line(QPainter& p, qreal x1, qreal y1, qreal x2, qreal y2) {
    p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

/// 箭头头部(指向 (x,y),按 angle 方向)。
void arrowHead(QPainter& p, qreal x, qreal y, qreal angle, const QColor& ink,
               qreal head = 4.0) {
    constexpr qreal kSpread = 0.42;   // ≈24°,与引擎画箭头同口径
    QPolygonF tri;
    for (qreal sign : {-1.0, 1.0}) {
        const qreal a = angle + kPi + sign * kSpread;
        tri << QPointF(x + std::cos(a) * head, y + std::sin(a) * head);
    }
    p.setBrush(ink);
    p.drawPolygon(tri);
    p.setBrush(Qt::NoBrush);
}

} // namespace

QColor defaultIconInk(bool dark) {
    // 与 Theme.qml 的 textPrimary 对齐(暗 #F1F4F7 / 亮 #17202A)
    return dark ? QColor(0xF1, 0xF4, 0xF7) : QColor(0x17, 0x20, 0x2A);
}

QColor iconInkFor(const QPalette& palette) {
    // 取 ButtonText:工具条按钮上的图标就该跟按钮文字同色 ——
    // 这样 applyTheme 注入 QPalette 后,图标自动跟随深浅主题,无需各处判断。
    return palette.color(QPalette::ButtonText);
}

QIcon toolIcon(EditorTool tool, int size, const QColor& ink) {
    QPixmap pm = makeCanvas(size, 2.0);
    QPainter p(&pm);
    setupPainter(p, size, ink);

    switch (tool) {
        case EditorTool::Pen: {
            // 画笔:斜杆 + 笔尖
            line(p, 3.5, 12.5, 11.0, 5.0);
            line(p, 4.5, 13.0, 3.0, 13.0);
            p.drawPolygon(QPolygonF({QPointF(3.0, 13.0), QPointF(3.6, 11.0),
                                     QPointF(5.0, 12.4)}));
            break;
        }
        case EditorTool::Arrow: {
            // 箭头:斜向主干 + 箭头头部
            line(p, 3.0, 13.0, 12.0, 4.0);
            arrowHead(p, 12.0, 4.0, std::atan2(-9.0, 9.0), ink);
            break;
        }
        case EditorTool::Line:
            line(p, 3.0, 13.0, 13.0, 3.0);
            break;
        case EditorTool::Rect:
            p.drawRect(QRectF(2.5, 3.5, 11.0, 9.0));
            break;
        case EditorTool::Ellipse:
            p.drawEllipse(QRectF(2.5, 3.5, 11.0, 9.0));
            break;
        case EditorTool::Mosaic: {
            // 马赛克:3×3 方格,部分实心
            const qreal s = 3.4, x0 = 2.9, y0 = 2.9;
            p.setPen(Qt::NoPen);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    if ((r + c) % 2 == 0) {
                        p.setBrush(ink);
                        p.drawRect(QRectF(x0 + c * s, y0 + r * s, s - 0.7, s - 0.7));
                    } else {
                        p.setPen(QPen(ink, 1.0));
                        p.setBrush(Qt::NoBrush);
                        p.drawRect(QRectF(x0 + c * s, y0 + r * s, s - 0.7, s - 0.7));
                        p.setPen(Qt::NoPen);
                    }
                }
            p.setPen(QPen(ink, kBaseStroke));
            break;
        }
        case EditorTool::Blur: {
            // 模糊:同心水滴(简化为两条弧线 + 中心点)
            p.drawArc(QRectF(3.0, 3.0, 10.0, 10.0), 30 * 16, 300 * 16);
            p.setBrush(ink);
            p.drawEllipse(QPointF(8.0, 8.0), 1.6, 1.6);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case EditorTool::Number: {
            // 序号标注:**圆底 + 数字"1"字形**。
            //
            // 为什么不画抽象符号:此前的"圆环"与粗细档位/椭圆工具撞形,用户
            // "必须看到提示词才知道是序号"。序号工具的真实产物是一枚带数字的
            // 徽章(点击落 1、再点落 2…),把数字字形直接画进图标才是零歧义表达 ——
            // 所见即所得,与引擎 NumberBadgeItem 的渲染同一意象。
            // ⚠️ 只画**subtract 之后的路径**,不要先 drawEllipse 再 drawPath ——
            //    那样挖空区透出的是第一次画的实心圆,等于没挖(实测踩过)。
            QPainterPath badge;
            badge.addEllipse(QPointF(8.0, 8.0), 6.2, 6.2);   // 徽章圆底
            QPainterPath one;
            // "1" 字形(16px 基准):斜起笔 + 竖干 + 底横(无衬线)
            const qreal w = 0.8;
            one.addRoundedRect(QRectF(8.0 - w / 2, 4.6, w, 6.8), w / 2, w / 2);   // 竖干
            QPolygonF flag;                                                        // 斜起笔
            flag << QPointF(6.6, 6.2) << QPointF(8.0, 4.6) << QPointF(8.0, 6.4);
            one.addPolygon(flag);
            one.addRoundedRect(QRectF(6.4, 11.4, 3.2, w), w / 2, w / 2);          // 底横
            badge = badge.subtracted(one);
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawPath(badge);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case EditorTool::Text: {
            // 文字:T 字骨架(横 + 竖)
            line(p, 3.0, 4.0, 13.0, 4.0);
            line(p, 8.0, 4.0, 8.0, 13.0);
            break;
        }
        case EditorTool::Picker: {
            // 取色:吸管(斜杆 + 顶端小方)
            line(p, 4.0, 12.5, 10.5, 6.0);
            p.drawPolygon(QPolygonF({QPointF(3.0, 13.5), QPointF(3.6, 11.4),
                                     QPointF(5.2, 13.0)}));
            p.drawRect(QRectF(9.4, 3.2, 3.4, 3.4));
            break;
        }
    }
    p.end();
    return QIcon(pm);
}

QIcon barIcon(BarIcon icon, int size, const QColor& ink) {
    QPixmap pm = makeCanvas(size, 2.0);
    QPainter p(&pm);
    setupPainter(p, size, ink);

    switch (icon) {
        case BarIcon::Undo:
            // 左向弧线 + 箭头
            p.drawArc(QRectF(3.0, 4.5, 10.0, 8.0), 30 * 16, 160 * 16);
            arrowHead(p, 5.0, 6.4, std::atan2(-1.0, -1.0), ink, 3.6);
            break;
        case BarIcon::Redo:
            p.drawArc(QRectF(3.0, 4.5, 10.0, 8.0), -10 * 16, -160 * 16);
            arrowHead(p, 11.0, 6.4, std::atan2(-1.0, 1.0), ink, 3.6);
            break;
        case BarIcon::SaveAs:
            // 软盘:外框 + 上沿缺口 + 底部标签
            p.drawRect(QRectF(2.5, 2.5, 11.0, 11.0));
            p.drawRect(QRectF(5.5, 2.5, 5.0, 3.6));
            line(p, 5.0, 9.0, 11.0, 9.0);
            line(p, 5.0, 12.0, 11.0, 12.0);
            break;
        case BarIcon::Pin: {
            line(p, 8.0, 3.0, 8.0, 9.5);
            p.setBrush(ink);
            p.drawEllipse(QPointF(8.0, 3.2), 2.0, 2.0);
            p.setBrush(Qt::NoBrush);
            line(p, 8.0, 9.5, 8.0, 13.2);
            break;
        }
        case BarIcon::Ocr: {
            // 取字:四角括号 + 中间横线(扫描框)
            const qreal m = 2.5, L = 3.0, r = 13.5, b = 13.5;
            line(p, m, m + L, m, m); line(p, m, m, m + L, m);
            line(p, r - L, m, r, m); line(p, r, m, r, m + L);
            line(p, m, b - L, m, b); line(p, m, b, m + L, b);
            line(p, r - L, b, r, b); line(p, r, b, r, b - L);
            line(p, 5.5, 8.0, 10.5, 8.0);
            break;
        }
        case BarIcon::Done:
            line(p, 3.0, 8.4, 6.6, 12.0);
            line(p, 6.6, 12.0, 13.0, 4.4);
            break;
        case BarIcon::Cancel:
            line(p, 4.0, 4.0, 12.0, 12.0);
            line(p, 12.0, 4.0, 4.0, 12.0);
            break;
        case BarIcon::Opacity: {
            // 透明度:半实半虚的圆
            p.setBrush(ink);
            p.drawChord(QRectF(2.5, 2.5, 11.0, 11.0), 90 * 16, 180 * 16);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(2.5, 2.5, 11.0, 11.0));
            break;
        }
        case BarIcon::ZoomFit: {
            // 适应窗口:四角向内 + 中间方框
            p.drawRect(QRectF(5.0, 5.0, 6.0, 6.0));
            line(p, 2.5, 2.5, 4.6, 4.6); line(p, 13.5, 2.5, 11.4, 4.6);
            line(p, 2.5, 13.5, 4.6, 11.4); line(p, 13.5, 13.5, 11.4, 11.4);
            break;
        }
        case BarIcon::ZoomOriginal: {
            // 100%:1:1 提示(两条等长竖线 + 中间点)
            line(p, 4.0, 5.0, 4.0, 11.0);
            line(p, 12.0, 5.0, 12.0, 11.0);
            p.setBrush(ink);
            p.drawEllipse(QPointF(8.0, 8.0), 1.3, 1.3);
            p.setBrush(Qt::NoBrush);
            break;
        }
        case BarIcon::Pan: {
            // 手型(拖动画面)。度量按 16px 视口:手掌 7.6×5.4,指节宽 1.4。
            // 用"手掌 + 三指"而不是实心手:Loutline 线性风与其余图标一致,
            // 实心手在小尺寸下会糊成一团。
            p.setPen(QPen(ink, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(4.4, 7.6, 7.2, 5.2), 1.6, 1.6);   // 手掌
            for (int i = 0; i < 3; ++i) {                               // 三指
                const qreal x = 5.6 + i * 2.2;
                line(p, x, 7.6, x, 4.4 - (i == 1 ? 0.9 : 0.0));
            }
            line(p, 4.4, 9.2, 2.8, 10.6);                               // 拇指
            break;
        }
    }
    p.end();
    return QIcon(pm);
}

QCursor crosshairCursor(bool dark, qreal dpr) {
    // 逻辑尺寸 24:比 Qt 内置十字(约 16)略大,容错更好;
    // 四条臂在中心留 4px 空隙,不遮住落点本身。
    constexpr int kLogical = 24;
    const qreal k = dpr > 0.1 ? dpr : 1.0;
    const int px = qRound(kLogical * k);
    QPixmap pm(px, px);
    pm.setDevicePixelRatio(k);   // 物理像素画布 + 声明 DPR
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // ⚠️ **不要**再手动 scale(k,k):设过 setDevicePixelRatio 之后,painter 已经
    //    按逻辑坐标工作了,再 scale 一次就是**双重缩放** —— 图形被推到画布外,
    //    只剩左上角一小块(实测 bbox 从 4,4 40x40 变成 8,8 40x40 且内容错位,
    //    渲染出来是个方块而不是准星)。下面的数字一律按 24×24 逻辑坐标写。

    // 几何:中心 c,四条臂各长 arm,中心留 gap 不遮落点。
    // ⚠️ arm 必须满足 c - gap - arm >= 1:否则臂会顶到画布边缘被**截断**
    //    (实测 arm=9、c=12 时画布被撑满,臂端贴边看着像"没画完")。
    const qreal c = kLogical / 2.0;   // 中心 = 12
    const qreal gap = 2.0;            // 中心空隙(不遮落点)
    const qreal arm = c - gap - 2.5;  // 臂长 → 端点离边 2.5,给外晕留余量(否则描边贴边)
    const qreal w = 1.4;              // 线宽(逻辑像素)

    // 先画一层"对比底色描边":深色主题用近黑打底、浅色主题用近白打底,
    // 这样在任何画布内容上准星都看得见(纯白画布 / 纯黑截图两头都有救)。
    const QColor halo = dark ? QColor(0, 0, 0, 140) : QColor(255, 255, 255, 160);
    const QColor ink = dark ? QColor(0xF1, 0xF4, 0xF7) : QColor(0x17, 0x20, 0x2A);

    const auto drawArms = [&](const QColor& color, qreal width) {
        QPen pen(color, width);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(c, c - gap - arm), QPointF(c, c - gap));   // 上
        p.drawLine(QPointF(c, c + gap), QPointF(c, c + gap + arm));   // 下
        p.drawLine(QPointF(c - gap - arm, c), QPointF(c - gap, c));   // 左
        p.drawLine(QPointF(c + gap, c), QPointF(c + gap + arm, c));   // 右
    };

    drawArms(halo, w + 1.6);   // 底:略粗的对比描边
    drawArms(ink, w);          // 面:细的准星本体

    // 中心小点:精确落点提示(比纯十字更容易对准)
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawEllipse(QPointF(c, c), 0.9, 0.9);

    p.end();
    // 热点 = 正中心(与 drawArms 的对称中心一致)
    return QCursor(pm, kLogical / 2, kLogical / 2);
}

QIcon strokeIcon(int width, int size, const QColor& ink) {
    QPixmap pm = makeCanvas(size, 2.0);
    QPainter p(&pm);
    setupPainter(p, size, ink);
    // 半径随档位递增(2/4/8 → 1.8/3.0/4.6),用实心圆点直观测
    const qreal r = 1.0 + width * 0.45;
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawEllipse(QPointF(8.0, 8.0), r, r);
    p.end();
    return QIcon(pm);
}

QIcon fontSizeIcon(int pixelSize, int size, const QColor& ink) {
    QPixmap pm = makeCanvas(size, 2.0);
    QPainter p(&pm);
    setupPainter(p, size, ink);
    // 方框高度示意字号(24/36/48 → 5/7/9),配一条基线
    const qreal h = 3.0 + (pixelSize / 24.0) * 2.4;
    p.drawRect(QRectF(4.0, 8.0 - h / 2, 8.0, h));
    line(p, 4.0, 11.6, 12.0, 11.6);
    p.end();
    return QIcon(pm);
}

} // namespace nb
