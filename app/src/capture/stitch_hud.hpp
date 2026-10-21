// 滚动长截图的采集期浮层(docs/19 v2 §3.1/§3.2)。
//
// ## 为什么不再"在遮罩上挖洞"
//
// 旧实现让遮罩铺满整屏,再用 SetWindowRgn 把选区从窗口区域里挖掉,好让滚轮直达
// 下层页面。两个死结:
//  - 窗口区域是**设备像素**,而选区是 Qt **逻辑像素**;150% 屏上洞按 1:1 挖出来,
//    选区实际被遮罩盖住一部分 → 每帧采到的都是遮罩上的静止画面 → 核心判 duplicate
//    → 长图只有一屏高(真机现象);
//  - 选区 = 整屏时"整窗减选区"是空区域 → 遮罩整块消失,面板一起消失。
//
// 另外"挖洞"与"在选区上画 UI"本身互斥:洞里画不出东西、也点不到。
//
// ## v2 形态
//
// 采集期**整个遮罩隐藏**(页面完全可见、可交互、可滚动),由本类的两个窗口承担 UI:
//
//  1. StitchFrame: 贴合选区的 2px 描边。窗口 = 选区**外扩** thickness,
//     描边画在外扩出来的那一圈里 → 像素**天然在抓取区域之外**(几何保证,
//     不依赖"抓图会跳过我们的窗口"这种后端行为)。
//     鼠标穿透(WS_EX_TRANSPARENT),不抢焦点。
//
//  2. StitchPanel:「结束」「取消」+ 进度文案。位置由 overlay_geometry 的
//     stitchPanelCandidates 给出(优先选区外,与选区零相交);实在无处可放时
//     允许浮在选区内,此时靠 WDA_EXCLUDEFROMCAPTURE 与分层窗口两道保险
//     (验收用哨兵色断言它确实没进画面)。
//
// ## Esc
//
// 采集期页面持有焦点,遮罩已隐藏,本进程收不到 Esc。故用 RegisterHotKey 临时
// 注册 Esc(全局),采集结束立刻注销。注册失败**不静默**:面板上直接写
// "Esc 不可用,请点「结束」"。
#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QWidget>

namespace nb {

/// 采集期浮层的**唯一**入口:建两个窗口、摆位置、更新文案、收尾释放。
class StitchHud : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    /// @param screen        采集所属屏幕(用于把设备像素选区换算成全局逻辑坐标)。
    /// @param regionDevice  选区在**该屏内**的设备像素矩形。
    /// @param dpr           换算用的缩放比(必须与采集链路用的是同一个值)。
    StitchHud(QScreen* screen, const QRect& regionDevice, qreal dpr, QObject* parent = nullptr);
    ~StitchHud() override;

    /// 显示(采集开始时调)。
    void showHud();
    /// 隐藏并等待窗口真正消失(合成器需要一轮事件循环,见 .cpp)。
    void hideHud();

    /// 进度文案(「已拼接 1420 px」/「滚动过快,有内容漏掉」…)。
    void setStatus(const QString& text);
    /// 是否已提示过 Esc 不可用(验收与日志用)。
    bool escUnavailable() const { return m_escUnavailable; }
    /// 面板是否落在选区之外(验收断言用;false = 依赖"浮窗不进画面")。
    bool panelOutsideSelection() const { return m_panelOutside; }
    /// 面板的全局逻辑矩形(验收断言哨兵色用)。
    QRect panelRectGlobal() const { return m_panelRectGlobal; }

signals:
    /// 用户点了「结束」。
    void finishRequested();
    /// 用户点了「取消」。
    void abortRequested();

protected:
    /// 采集期的全局 Esc(WM_HOTKEY)→ finishRequested。
    ///
    /// 为什么需要 native 过滤而不是 keyPressEvent:采集期遮罩已隐藏、页面持有焦点,
    /// 本进程根本没有接收键盘事件的窗口。RegisterHotKey 是唯一能拿到 Esc 的手段。
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override;

private:
    void layoutWindows();
    void registerEsc();
    void unregisterEsc();

    QScreen* m_screen = nullptr;
    QRect m_regionDevice;
    qreal m_dpr = 1.0;
    QRect m_regionLogical;        // 全局逻辑坐标的选区(换算结果)
    QRect m_panelRectGlobal;      // 面板全局逻辑矩形
    bool m_panelOutside = true;
    bool m_escUnavailable = false;
    bool m_escRegistered = false;
    QWidget* m_frame = nullptr;
    QWidget* m_panel = nullptr;
};

} // namespace nb
