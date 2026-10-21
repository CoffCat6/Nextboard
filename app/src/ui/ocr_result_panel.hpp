// OCR 识别结果的**驻留面板**(浮条「OCR取字」与编辑器「OCR 取字」共用)。
//
// ## 为什么是一个共享控件
//
// 同一个 OCR 能力此前有两套完全不同的落点:
//   - 编辑器:驻留的 QFrame 面板,文本可鼠标选取、自己关;
//   - 截图浮条:QToolTip 提示 4 秒 + 立刻结束会话。
// 结果用户"在浮条上取字"基本看不清、也拿不走片段 —— 同类能力两套体验,
// 改一处忘一处是必然。现在两边用同一个类(与 AnnotationToolbar 的收口动机相同)。
//
// ## 为什么是**顶层窗口**,不是遮罩的子控件
//
// 浮条取字后会话立即结束(emit cancelled → closeAllOverlays → 遮罩 close()),
// 遮罩一旦关闭,它的子控件会跟着销毁 —— 面板会"闪一下就不见了",
// 这正是编辑器早期踩过的坑(识别完 singleShot(0) close,面板一帧都看不到)。
// 所以面板自持一个顶层无边框窗口,生命周期与截图会话**解耦**。
//
// ## 为"图中取字"预留的位置
//
// 后续要做"在图上框选/点选文字":core 侧目前只返回纯文本(rapid.rs 的
// lines.join("\n")),没有逐行框。等 core 能返回每行的 bbox 后,本类只需
// 多接一组框数据并在 sourceImage() 上渲染高亮 —— 接口已留(见 setSource)。
#pragma once

#include <QFrame>
#include <QImage>
#include <QString>

class QLabel;
class QPushButton;
class QScreen;
class QScrollBar;
class QTextEdit;

namespace nb {

class OcrResultPanel : public QFrame {
    Q_OBJECT
public:
    /// 弹出识别结果面板(顶层窗口,不随宿主销毁)。
    ///
    /// @param text     识别出的文本;空串表示没识别到(此时 reason 是原因)。
    /// @param reason   状态行文案(如"已识别 42 个字符并复制到剪贴板")。
    /// @param source   被识别的原图。**当前只保存不渲染**,留给"图中取字"。
    /// @param anchorRectGlobal **定位基准矩形**(桌面全局逻辑坐标)。
    ///        截图链路必须传**选区**矩形,不能传遮罩窗口:遮罩是整屏,
    ///        传它会算出"屏幕右下角"——面板就跑到了屏幕边角(用户报的缺陷)。
    /// @param popup    true = Qt::Popup(点外部即关,编辑器旧行为);
    ///                 false = Qt::Tool(驻留,不受点击影响;浮条链路用)。
    /// @return 面板指针(自持生命周期;关闭后自动销毁,调用方用 QPointer 持有)。
    static OcrResultPanel* showResult(const QString& text, const QString& reason,
                                      const QImage& source, const QRect& anchorRectGlobal,
                                      bool popup);

    /// 便捷重载:基准 = 该控件的 frameGeometry()(编辑器用)。
    /// ⚠️ **不要**拿整屏窗口(遮罩)当 anchor —— 见上面 anchorRectGlobal 的说明。
    static OcrResultPanel* showResult(const QString& text, const QString& reason,
                                      const QImage& source, QWidget* anchor, bool popup);

    /// 被识别的原图(为"图中取字"预留)。
    const QImage& sourceImage() const { return m_source; }

    /// 正文当前是否需要滚动(验收用)。
    ///
    /// ⚠️ 单靠它**测不出"内容被裁掉"**:QTextEdit 在内容超长时无论多矮,
    /// 滚动条 maximum 都 > 0。见 bodyFullyReachable。
    bool bodyScrollable() const;

    /// 滚到底之后,最后一行是否**可达**(验收用)。
    ///
    /// 这才是"不被裁掉"的判据:把垂直滚动条拉到 maximum,文档末行应落在可视区内。
    /// 若正文高度被硬裁而文档排版没跟上,末行会永远留在视口之外。
    bool bodyLastLineReachable() const;

    /// 设置正文区高度上限(逻辑像素)。由 showResult 按**锚点所在屏**的可用高度算,
    /// 避免"内容一多就把面板撑出屏幕、按钮点不到"。
    ///
    /// ⚠️ 构造期窗口还没有几何,**不能**用 geometry() 反查屏幕(会拿到主屏);
    ///    屏幕由调用方显式传进来,顺便把上限一起定掉。
    void setMaxBodyHeight(int px);
    /// 显式指定用于计算上限的屏幕(可为 nullptr = 主屏)。
    void setAnchorScreen(QScreen* screen);

    /// 定位算法的**纯函数**入口(验收用)。
    ///
    /// 为什么暴露:面板摆位是"贴选区旁边"这条产品语义的全部实现。验收若自己
    /// 复算一遍算法,那是在测"两份实现是否一致",而不是测"面板摆得对不对";
    /// 真出现摆错,两边一起错,断言照样通过。
    static QPoint placeForTest(const QSize& panelSize, const QRect& anchorGlobal);

signals:
    /// 文本已重新复制(用户点「再次复制」)。
    void copiedToClipboard();

protected:
    /// ---- 拖动(用户要求:面板可以被拖走)----
    ///
    /// 为什么拖拽逻辑写在**面板**而不是标题行上:
    /// QWidget 的默认 mousePressEvent 是 e->ignore(),事件会**冒泡到父级**。
    /// 于是 QTextEdit(要划选文字)、QPushButton、滚动条这些**接受**了事件的子控件
    /// 天然不会被拖走,而标题行/状态行/边距这些不处理的区域会把事件送到这里。
    /// 一份实现同时满足"能拖"与"不干扰子控件",不必给每个子控件装事件过滤器
    /// (那种写法漏一个就会在某个区域拖不动)。
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    /// 拖动中按 Esc 取消(松手前反悔):恢复按下时的位置。
    void keyPressEvent(QKeyEvent* e) override;

    /// **底板由这里自绘**,不靠 stylesheet。
    ///
    /// 为什么:QFrame 默认不绘制 stylesheet 的 background(需要
    /// WA_StyledBackground,缺了就是全透明)。而顶层窗口要 WA_TranslucentBackground
    /// 才能有圆角 —— 两者叠加时"看起来什么都没画",整块面板透成一片(用户报的现象)。
    /// 自绘 paintEvent 不依赖这些属性细节,圆角/描边/阴影一处控制。
    void paintEvent(QPaintEvent* e) override;

private:
    explicit OcrResultPanel(const QString& text, const QString& reason,
                            const QImage& source, bool popup);
    /// 按主题把**子控件**重新着色(底板在 paintEvent 里画,不在这里)。
    void applyTheme();

    /// 正文区高度按内容收缩/展开(短内容不撑高,长内容封顶后滚动)。
    void fitBodyToContent();
    /// 当前屏可给出的正文高度上限。
    int effectiveMaxBodyHeight() const;

    QImage m_source;
    /// 标题行 = **拖动把手**。纯视觉(给出"这里能拖"的暗示) + 事件冒泡的落点。
    QWidget* m_header = nullptr;
    /// 拖动状态:按下点相对窗口左上角的偏移(用偏移而不是"按下点-移动点"累加,
    /// 避免每次 move 事件的取整误差累积成漂移)。
    bool m_dragging = false;
    QPoint m_dragOffset;
    QPoint m_dragStartPos;
    QTextEdit* m_body = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_closeBtn = nullptr;
    /// 正文宽度(固定:换行与"内容高度"的测量都依赖它,浮动宽度会让两者脱节)。
    int m_bodyWidth = 460;
    int m_maxBodyHeight = 420;
    /// 计算高度上限所依据的屏幕(见 setAnchorScreen)。
    QScreen* m_anchorScreen = nullptr;
};

} // namespace nb
