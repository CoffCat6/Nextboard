// 贴图窗口:置顶无边框,拖动/滚轮缩放/透明度/双击关闭(第一代 pin 交互语义)。
// DPI:窗口逻辑尺寸 = 原图 ÷ 当前屏 DPR × 用户缩放,物理尺寸恒为「原图 × 缩放」,
//      跨不同缩放比的屏幕拖动、或改系统缩放,都不会忽大忽小。
// 阴影:窗口四周各留一圈留白并自绘柔和阴影(透明无边框窗口系统不会画),
//      使贴图与背景易于分辨。
// 交互补齐(对齐第一代 F13):ESC 关闭 + **右键**唤出工具条
//      (缩放值 / 复制 / 另存 / 关闭 / 不透明度)。
#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace nb {

/// 贴图窗口的初始行为,来自设置 `pinDefaultOpacity` / `pinDoubleclickClose`。
struct PinOptions {
    int opacityPercent = 100;        // 30–100
    bool doubleClickClose = true;
};

/// 文本贴图的渲染样式,来自设置 `pinTextBg` / `pinTextColor` /
/// `pinTextFontSize` / `pinTextMaxWidth` / `pinTextBgOpacity`。
struct PinTextStyle {
    QColor background = QColor(0x26, 0x27, 0x2E, 235);
    QColor textColor = QColor(0xDF, 0xE1, 0xE8);
    int fontSize = 15;
    int maxWidth = 520;
};

/// 贴图内容:位图 + 可选原始文本。
///
/// 文本贴图时 `text` 非空 —— 工具条的「复制 / 另存」按**文本**处理
/// (复制原文、存 .txt),而不是把渲染出的位图当图片存(第一代 F18 语义)。
struct PinContent {
    QImage image;      ///< 实际显示的位图(图片条目 = 原图;文本条目 = 渲染结果)
    QString text;      ///< 非空 = 文本贴图
    QString idPrefix;  ///< 另存默认文件名用的短标识(条目 id 前 8 位,可空)
};

class PinWindow : public QWidget {
    Q_OBJECT
public:
    /// content:要贴出的内容;options:初始透明度与双击关闭行为。
    PinWindow(PinContent content, const PinOptions& options = {}, QWidget* parent = nullptr);

    /// 文本条目贴图:把文本按 style 渲染成位图,复用同一套窗口与交互。
    static QImage renderText(const QString& text, const PinTextStyle& style);

    /// 在贴图底部显示一条瞬时提示(约 1.6s)。自包含在贴图窗口内 ——
    /// 主面板隐藏时应用级 toast 不可见,故贴图相关反馈走这里。
    void showNotice(const QString& text);

    /// 当前显示位图占用的字节数。
    /// ⚠️ 这是**图片本身**的字节数,不含窗口开销 —— 淘汰预算请用 memoryBytes()。
    qint64 imageBytes() const { return m_image.sizeInBytes(); }

    /// 这张贴图的**真实内存占用**:图片 + 软件渲染下置顶窗口的 backing store
    /// (≈ 窗口物理尺寸 ×4)。淘汰预算据此计算 —— 只算图片会低估近一半
    /// (实测 1755×1097 图 7.7MB/张,进程私有增量 +18.3MB/张)。
    qint64 memoryBytes() const;

    /// 用新的位图替换贴图内容(编辑结果回写):保持当前缩放,并让画面中心不动。
    void setImage(const QImage& image);

signals:
    /// 工具条「编辑」:请求对当前贴图内容做标注编辑。调用方负责打开编辑器,
    /// 并在「完成」后调 setImage() 回写,否则贴图会停留在编辑前的画面。
    void editRequested(const QImage& image);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    /// ESC 关闭(第一代语义:关闭走工具条 ✕ 或 ESC);工具条已展开时先收起工具条。
    void keyPressEvent(QKeyEvent* e) override;
    /// 右键唤出 / 收起工具条(取代原"悬停显示")。
    void contextMenuEvent(QContextMenuEvent* e) override;
    void showEvent(QShowEvent* e) override;
    /// 工具条(顶层弹层)关闭后把焦点收回贴图,ESC 才能继续生效。
    bool eventFilter(QObject* obj, QEvent* e) override;
    /// 窗口被拖到不同缩放比的屏幕(或系统缩放变化)时重算尺寸。
    bool event(QEvent* e) override;

private:
    void buildToolbar();
    /// 在指定**屏幕坐标**处展开工具条,并夹在屏幕可用区内。
    void showToolbarAt(const QPoint& globalPos);
    /// 收起工具条。
    void hideToolbar();
    void updateToolbar();
    void copyToClipboard();
    void saveAs();
    /// 顺时针旋转 90°(像素级置换,无损;仅图片贴图)。
    void rotateImage();
    /// 请求编辑当前贴图(发 editRequested)。
    void requestEdit();

    void applyScale();
    /// 窗口**当前所在屏**的 DPR;窗口未 show 时回退窗口自身/主屏值。
    qreal currentDpr() const;
    /// 目标逻辑尺寸 = 原图 × 用户缩放 ÷ DPR。
    QSize logicalSizeFor(qreal dpr) const;

    QImage m_image;
    QString m_text;        ///< 非空 = 文本贴图(复制/另存按文本处理)
    QString m_idPrefix;    ///< 另存默认文件名标识
    /// 用户缩放(0.1 ~ 4.0);1.0 = 1 图像像素对 1 物理像素。与 DPI 换算解耦。
    qreal m_zoom = 1.0;
    qreal m_opacity = 1.0;    // 0.3 ~ 1.0(滚轮 + Alt 调节,或工具条滑杆)
    bool m_dragging = false;
    QPoint m_dragOffset;
    bool m_doubleClickClose = true;

    // ---- 右键工具条(顶层弹层:不被贴图窗口裁剪,小贴图也能完整显示) ----
    QWidget* m_toolbar = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QSlider* m_opacitySlider = nullptr;
    QPushButton* m_rotateButton = nullptr;   ///< 仅图片贴图可见
    QPushButton* m_editButton = nullptr;     ///< 仅图片贴图可见
    QLabel* m_hintLabel = nullptr;      ///< 底部瞬时提示(已复制 / 已另存)
    QTimer* m_hintTimer = nullptr;
};

} // namespace nb
