#include "capture/overlay_geometry.hpp"

#include <QtGlobal>

namespace nb::overlay {

namespace {
/// dpr 兜底:底图异常/未就绪时按 1 处理,避免除零与整条几何塌缩。
qreal safeDpr(qreal dpr) { return dpr > 0.01 ? dpr : 1.0; }
} // namespace

QRect annotationSceneRect(const QRect& selectionLogical, qreal dpr) {
    const QRect sel = selectionLogical.normalized();
    const qreal k = safeDpr(dpr);
    return QRect(QPoint(0, 0),
                 QSize(qRound(sel.width() * k), qRound(sel.height() * k)));
}

QPointF toScene(const QPoint& widgetPos, const QRect& selectionLogical, qreal dpr) {
    const QRect sel = selectionLogical.normalized();
    const qreal k = safeDpr(dpr);
    return QPointF((widgetPos.x() - sel.x()) * k, (widgetPos.y() - sel.y()) * k);
}

QRect toolbarRectFor(const QRect& selectionLogical, const QSize& widgetSize,
                     const QSize& barSize, int margin) {
    const QRect sel = selectionLogical.normalized();
    const int w = barSize.width();
    const int h = barSize.height();
    int x = sel.center().x() - w / 2;
    int y = sel.bottom() + margin;
    if (y + h > widgetSize.height() - 4) {
        y = qMax(4, sel.top() - margin - h);   // 翻到选区上方;再夹住(见头文件注释)
    }
    x = qBound(4, x, qMax(4, widgetSize.width() - w - 4));
    return QRect(x, y, w, h);
}

QRect subbarRectFor(const QRect& toolbarRect, const QSize& barSize, int gap,
                    const QSize& widgetSize) {
    const int w = barSize.width();
    const int h = barSize.height();
    int x = toolbarRect.center().x() - w / 2;
    int y = toolbarRect.top() - gap - h;
    if (y < 4) y = toolbarRect.bottom() + gap;
    y = qBound(4, y, qMax(4, widgetSize.height() - h - 4));
    x = qBound(4, x, qMax(4, widgetSize.width() - w - 4));
    return QRect(x, y, w, h);
}

QRect stitchPanelRectFor(const QRect& selectionLogical, const QSize& widgetSize,
                         const QSize& panelSize, int margin) {
    const QRect sel = selectionLogical.normalized();
    const int w = panelSize.width();
    const int h = panelSize.height();
    // 首选:贴选区右下角(下方)
    int x = sel.right() - w + 1;
    int y = sel.bottom() + margin;
    if (y + h > widgetSize.height() - 4) y = sel.top() - margin - h;   // 翻到上方
    if (y < 4) y = sel.bottom() - h - margin;                          // 压进选区底部内侧
    y = qBound(4, y, qMax(4, widgetSize.height() - h - 4));
    x = qBound(4, x, qMax(4, widgetSize.width() - w - 4));
    return QRect(x, y, w, h);
}

QRect stitchProgressRectFor(const QRect& selectionLogical, const QSize& widgetSize,
                            int height) {
    const QRect sel = selectionLogical.normalized();
    const int w = qMin(sel.width(), qMax(0, widgetSize.width() - 8));
    const int h = qMax(0, height);
    int x = sel.center().x() - w / 2;
    int y = sel.bottom() - h + 1;
    y = qBound(4, y, qMax(4, widgetSize.height() - h - 4));
    x = qBound(4, x, qMax(4, widgetSize.width() - w - 4));
    return QRect(x, y, w, h);
}

} // namespace nb::overlay
