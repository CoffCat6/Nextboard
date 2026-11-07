// 单实例门禁与跨进程唤起。
// Windows:命名互斥量 + 命名共享节(HWND 定点投递)+ WM_APP_ACTIVATE;
// Linux(M0b):QLockFile(数据目录内)+ QLocalServer/QLocalSocket(唤起通道),
// 唤起经 activateRequested() 信号交付(main.cpp 连接到 PanelWindow::showAtCursor)。
#pragma once

#include <QObject>

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    /// 尝试成为唯一实例;false 表示已有实例在运行。
    /// lastError:Windows = CreateMutexW 的原始 GetLastError(诊断用);
    /// Linux = 0 成功 / 1 锁被占用(诊断口径,不参与逻辑)。
    bool ensure(const QString& key, unsigned long* lastError = nullptr);
    /// (二次启动时)向已有实例发送唤起消息。
    /// Windows:实例 HWND 经命名共享节定点投递(广播对 Qt::Tool 窗口无效);
    /// Linux:连接实例的 QLocalServer 通道。
    void activateExisting();

    /// 发布本实例的唤起通道。
    /// Windows:主面板 HWND 经命名共享节;Linux:开启 QLocalServer 监听
    /// (参数 hwnd 在 Linux 无意义,保留签名一致)。
    /// 必须在主面板窗口真正创建之后调用(winId() 有效)。
    void publishWindow(void* hwnd);

signals:
    /// (Linux)第二个实例经 QLocalServer 送来唤起请求。
    /// Windows 不经过本信号 —— 走 PanelWindow::nativeEvent 的 WM_APP_ACTIVATE 分支。
    void activateRequested();

private:
    void* m_mutex = nullptr; // Windows: HANDLE;Linux: QLockFile*(避免头文件引平台头)
};
