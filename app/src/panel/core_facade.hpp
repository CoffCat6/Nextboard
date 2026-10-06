// QML ↔ nbcore 门面(替代 Sciter 桥的 JS 全局对象 Nextboard.*)。
//
// 设计:
// - 单例,QML 侧经 qmlRegisterSingletonInstance 以名字 `Core` 访问;
// - 同步 call():覆盖本地 SQLite 操作(毫秒级),与 Sciter 版桥语义一致;
// - 网络类操作(翻译/OCR)走线程池异步,完成后发信号——Sciter 版在 UI 线程
//   阻塞等待,这里改为不卡界面;
// - nbcore 的 WM_APP 通知由 PanelWindow::nativeEvent 转发进来,QML 用
//   Connections 直接订阅,取代原 JS 的 1s 轮询。
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>

namespace nb {

class CoreFacade : public QObject {
    Q_OBJECT
public:
    static CoreFacade& instance();

    CoreFacade(const CoreFacade&) = delete;
    CoreFacade& operator=(const CoreFacade&) = delete;

    /// JSON 门面透传:返回 { ok, data|error }(嵌套结构转 QVariantMap/QVariantList)。
    Q_INVOKABLE QVariantMap call(const QString& method, const QVariantMap& req = {});

    /// 分类计数(entry_counts 的展平:all/text/image/file/pinned,-1 表示取不到)。
    Q_INVOKABLE int entryCount(const QString& kind);

    /// UI 反馈(toast 由 QML 侧显示;C++ 各处也能发)。
    Q_INVOKABLE void showToast(const QString& message);

    /// 翻译(网络,慢):线程池执行,完成发 translateFinished。
    Q_INVOKABLE void translateText(const QString& text, const QString& from, const QString& to);

    /// OCR(慢):线程池执行,完成发 ocrFinished。
    Q_INVOKABLE void ocrEntry(const QString& id);

signals:
    /// nbcore 通知(剪贴板/设置变化)→ QML Connections 订阅。
    void clipChanged();
    void settingsChanged();
    void translateFinished(QVariantMap resp);
    void ocrFinished(QVariantMap resp);
    void toastMessage(const QString& message);

public:
    /// PanelWindow::nativeEvent 转发 nbcore WM_APP 通知(刷新去抖由模型侧负责)。
    void notifyClipChanged() { emit clipChanged(); }
    void notifySettingsChanged() { emit settingsChanged(); }

private:
    CoreFacade() = default;
    ~CoreFacade() = default;
};

} // namespace nb
