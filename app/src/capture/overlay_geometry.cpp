#include "capture/overlay_geometry.hpp"

#include <QtGlobal>

namespace nb::overlay {

namespace {
/// dpr 兜底:底图异常/未就绪时按 1 处理,避免除零与整条几何塌缩。
qreal safeDpr(qreal dpr) { return dpr > 0.01 ? dpr : 1.0; }

/// 浮条贴整屏底边时留的边距(逻辑像素)。
///
/// 为什么留 12 而不是 4:任务栏通常占 40~48px,浮条贴在窗口底边时若只留 4px,
/// 会与"贴边手势"挨得太近;12 既能让用户看出"这是浮在内容上的条",又不浪费
/// 垂直空间(浮条本身 44px)。
constexpr int kEdgeInset = 12;
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

    // ---- 选区占满整屏(贴住上下边)时:浮条**必然**压在内容上 ----
    //
    // 上下都没有空白可站,夹住之后浮条就压进了选区中部。此时:
    //   * 智能框选场景下这几乎必然发生("框住一个最大化窗口"是主线用法);
    //   * 美观上不如贴边,但**贴边会盖住窗口标题栏** —— 而标题栏恰好是用户
    //     最常想确认"框对了没有"的地方。
    //
    // 取舍:贴住**底边**。理由:截图内容的主体通常在上方(正文从顶部开始),
    // 而浮条在底部时用户仍能看到标题栏与首屏内容;反过来则一眼全是浮条。
    // 只改 y,不动 x(横向仍居中,与平常一致)。
    const bool noRoomAbove = (sel.top() - margin - h) < 4;
    const bool noRoomBelow = (sel.bottom() + margin + h) > (widgetSize.height() - 4);
    if (noRoomAbove && noRoomBelow) {
        y = qBound(4, widgetSize.height() - h - kEdgeInset, qMax(4, widgetSize.height() - h - 4));
    }
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

QList<QRect> stitchPanelCandidates(const QRect& selectionLogical, const QSize& widgetSize,
                                   const QSize& panelSize, int margin) {
    const QRect sel = selectionLogical.normalized();
    const int w = panelSize.width();
    const int h = panelSize.height();
    const int maxX = qMax(4, widgetSize.width() - w - 4);
    const int maxY = qMax(4, widgetSize.height() - h - 4);
    // 横向对齐:默认贴选区右边(鼠标滚到底时就在那一侧),但必须夹进窗口
    const auto alignRight = [&] { return qBound(4, sel.right() - w + 1, maxX); };
    const auto alignLeft = [&] { return qBound(4, sel.left(), maxX); };
    const auto center = [&] { return qBound(4, sel.center().x() - w / 2, maxX); };

    QList<QRect> out;
    const auto push = [&out, w, h](int x, int y) {
        const QRect r(x, y, w, h);
        if (!out.contains(r)) out.append(r);
    };

    // ① 选区下方(最不碍事:注意力在选区内容与滚动条上,下方通常是空白)
    push(alignRight(), sel.bottom() + margin);
    push(center(), sel.bottom() + margin);
    // ② 选区上方(下方放不下时)
    push(alignRight(), sel.top() - margin - h);
    push(center(), sel.top() - margin - h);
    // ③ 选区左右两侧(整屏选区时上下都没有空间,只剩左右)
    push(sel.right() + margin, qBound(4, sel.top(), maxY));
    push(sel.left() - margin - w, qBound(4, sel.top(), maxY));
    push(sel.right() + margin, qBound(4, sel.bottom() - h + 1, maxY));
    push(sel.left() - margin - w, qBound(4, sel.bottom() - h + 1, maxY));
    // ④ 四个角外侧
    push(widgetSize.width() - w - 4, 4);
    push(4, widgetSize.height() - h - 4);

    // 统一夹进窗口后**剔除与选区相交者** —— 这是本函数的全部意义所在
    QList<QRect> fits;
    for (const QRect& raw : out) {
        QRect r(raw);
        r.moveLeft(qBound(4, r.left(), maxX));
        r.moveTop(qBound(4, r.top(), maxY));
        if (stitchPanelFitsOutside(r, sel) && !fits.contains(r)) fits.append(r);
    }
    return fits;
}

bool stitchPanelFitsOutside(const QRect& panelRect, const QRect& selectionLogical) {
    return !panelRect.intersects(selectionLogical.normalized());
}

QRect stitchPanelRectFor(const QRect& selectionLogical, const QSize& widgetSize,
                         const QSize& panelSize, int margin) {
    const QRect sel = selectionLogical.normalized();
    const QList<QRect> fits = stitchPanelCandidates(sel, widgetSize, panelSize, margin);
    if (!fits.isEmpty()) return fits.first();
    // 无处可放:退到选区右下角外侧并夹进窗口。调用方必须用
    // stitchPanelFitsOutside() 判出这种局面并走"浮窗不进画面"的降级路径
    // (docs/19 v2 §3.2)。这里**不**再像旧实现那样压进选区内侧 ——
    // 那块区域画不出来也点不到,等于面板凭空消失。
    const int w = panelSize.width();
    const int h = panelSize.height();
    const int x = qBound(4, sel.right() - w + 1, qMax(4, widgetSize.width() - w - 4));
    const int y = qBound(4, sel.bottom() + margin, qMax(4, widgetSize.height() - h - 4));
    return QRect(x, y, w, h);
}

QList<QRect> stitchFrameRects(const QRect& selectionLogical, const QSize& widgetSize,
                              int thickness) {
    const QRect sel = selectionLogical.normalized();
    const int t = qMax(1, thickness);
    QList<QRect> out;
    const int maxX = widgetSize.width() - 1;
    const int maxY = widgetSize.height() - 1;
    // 上边:选区上方 t 像素,横向覆盖选区宽度
    const int topY = sel.top() - t;
    out.append(topY >= 0 ? QRect(sel.left(), topY, sel.width(), t) : QRect());
    // 下边
    const int bottomY = sel.bottom() + 1;
    out.append(bottomY + t <= maxY ? QRect(sel.left(), bottomY, sel.width(), t) : QRect());
    // 左边:纵向覆盖选区高度(含上下两条描边的角,避免出现缺口)
    const int leftX = sel.left() - t;
    out.append(leftX >= 0 ? QRect(leftX, sel.top() - t, t, sel.height() + 2 * t) : QRect());
    // 右边
    const int rightX = sel.right() + 1;
    out.append(rightX + t <= maxX ? QRect(rightX, sel.top() - t, t, sel.height() + 2 * t)
                                  : QRect());
    return out;
}

} // namespace nb::overlay
