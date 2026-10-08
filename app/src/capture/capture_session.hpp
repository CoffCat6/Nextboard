// 截图会话:遮罩窗口(每屏一个)+ 冻结底图 + 框选;确认后裁剪 → 入库 → 写剪贴板。
//
// 坐标约定(关键):
//  - 冻结底图 = 屏幕**物理像素**(xcap/GDI 原样);
//  - 鼠标事件 / 窗口 rect = **逻辑像素**(Qt 6 高 DPI 下自动缩放);
//  - 交给核心 nb_capture_crop 的选区必须是**物理像素**且相对该屏左上角。
// 三者靠 devicePixelRatio 换算,漏掉换算会在缩放屏上取错区域/显示错内容。
#pragma once

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>

class QScreen;

namespace nb {

enum class CaptureIntent { Region, Fullscreen };

/// 裁剪入库完成后的回调,参数是刚入库条目的 id(未入库/失败时为空串)。
/// 截图翻译/取字用它接续 OCR → 翻译链路;不传则只做"入库 + 写剪贴板"。
using CaptureDone = std::function<void(const QString& entryId)>;

/// 启动会话(Fullscreen = 光标屏整屏直通;Region = 各屏遮罩框选)。
/// 忙碌重入检查沿用第一代 CAPTURE_BUSY 语义。
///
/// allowEditor:是否允许走标注编辑器(仅在 regionSkipEditor=false 时才有意义)。
/// 截图翻译 / 截图取字是"取内容"链路,应**始终直通**——中间弹一个标注编辑器
/// 既多余又会打断流程,故这两条链路传 false。
void startCaptureSession(CaptureIntent intent, CaptureDone onCaptured = {},
                         bool allowEditor = true);

/// 延时截图(F16):先弹一个置顶的倒计时数字(鼠标穿透),倒计时结束再启动会话。
/// 用途:按下入口后有时间去展开菜单 / 等悬浮提示出现。seconds<=0 时等同立即截图。
void startDelayedCapture(int seconds, CaptureIntent intent, CaptureDone onCaptured = {},
                         bool allowEditor = true);

/// 读取设置里的延时截图等待秒数(钳制到 1~10;读取失败按 3 秒)。
/// 托盘入口与快捷键共用,避免两处各写一份。
int captureDelaySeconds();

/// 入库一张图片并写入系统剪贴板(截图与编辑器「完成」共用同一通道)。
/// 返回条目 id;未入库(超大小上限)或失败时返回空串。
///
/// 顺序遵循第一代 docs/phase5.md §3.6:**先入库、后写剪贴板** —— 剪贴板监听线程
/// 读到同一张图时按 hash 命中去重,只 touch 不新建,不会产生重复条目。
QString ingestImageAndCopy(const QImage& image);

/// 遮罩窗口:铺满**指定**屏幕,绘制冻结底图 + 半透明遮罩 + 选区。
class OverlayWidget : public QWidget {
    Q_OBJECT
public:
    /// screen:本遮罩所属屏幕(frozen 即该屏的物理像素底图)。
    /// autoConfirm:对应设置 regionAutoConfirm —— true = 松开鼠标即确认(默认);
    ///              false = 选区保留,可用 8 向手柄微调,Enter/双击确认。
    /// rightClickCancel:对应设置 regionRightClickCancel —— 右键是否取消本次截图。
    OverlayWidget(QImage frozen, int screenIndex, QScreen* screen, bool autoConfirm,
                  bool rightClickCancel, QWidget* parent = nullptr);

signals:
    /// 选区确认:**本屏物理像素**坐标,相对该屏左上角(直接供 nb_capture_crop)。
    void regionConfirmed(const QRect& physicalRect);
    void cancelled();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    /// 8 向手柄 + 选区内部(整体拖动);仅微调模式使用。
    enum class Handle {
        None,
        TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left,
        Inside,
    };

    qreal dpr() const;
    /// 选区(局部逻辑坐标)按 DPR 换算为物理像素;越界由核心钳制。
    QRect selectionPhysical() const;
    /// 命中测试:pos 落在哪个手柄/内部(局部逻辑坐标)。
    Handle handleAt(const QPoint& pos) const;
    /// 8 个手柄中心点(局部逻辑坐标)。
    QList<QPoint> handlePoints(const QRect& rect) const;
    static Qt::CursorShape cursorForHandle(Handle handle);
    /// 按当前拖动的 handle 把选区调整到 pos(局部逻辑坐标)。
    void applyAdjustment(const QPoint& pos);
    /// 选区是否够大、可以确认。
    bool selectionUsable() const;

    QImage m_frozen;          // 该屏冻结底图(物理像素)
    int m_screenIndex;
    QScreen* m_screen = nullptr;
    bool m_autoConfirm = true;
    bool m_rightClickCancel = true;

    QPoint m_origin;          // 新选区的起点(局部逻辑坐标)
    QRect m_selection;        // 当前选区(局部逻辑坐标)
    bool m_selecting = false; // 正在拖出新选区
    bool m_adjusting = false; // 正在微调已有选区
    Handle m_activeHandle = Handle::None;
    QRect m_adjustStartRect;  // 微调开始时的选区
    QPoint m_adjustStartPos;  // 微调起始光标位置
};

} // namespace nb
