// 独立翻译窗口:翻译不再是主面板里的抽屉,而是有自己生命周期的顶层窗口。
//
// 与主面板的分工:
// - 主面板 = 剪贴板历史(呼出即用、失焦即隐);
// - 翻译窗口 = 需要停留、反复输入输出的工作区,因此**不随失焦隐藏**,
//   可独立移动(拖动标题栏)/最小化/关闭,与主面板互不干扰。
#pragma once

#include <QWidget>

class QQuickWidget;

namespace nb {

class TranslateWindow : public QWidget {
    Q_OBJECT
public:
    explicit TranslateWindow(QWidget* parent = nullptr);
    ~TranslateWindow() override;

    /// 显示窗口:首次显示定位到光标所在屏中央,之后保持用户拖动后的位置。
    void showWindow();

    /// 用给定原文打开并自动翻译(空文本 = 只聚焦输入框,供手动输入)。
    void openWith(const QString& text);

    /// QML 侧调用:拖动标题栏移动窗口(交给系统,避免自实现的跟手抖动)。
    Q_INVOKABLE void startSystemMove();

    /// QML 侧调用:关闭窗口(隐藏而非销毁,下次打开保留位置)。
    Q_INVOKABLE void hideWindow() { hide(); }

protected:
    void showEvent(QShowEvent* e) override;

private:
    void setupUi();

    QQuickWidget* m_quick = nullptr;
    /// 是否已定位过(只定位一次,不覆盖用户拖动后的位置)。
    bool m_placed = false;
};

} // namespace nb
