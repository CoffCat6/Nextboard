// 单实例:命名互斥量 + 自定义窗口消息唤起已有实例的主面板。
#pragma once

#include <QObject>

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    /// 尝试成为唯一实例;false 表示已有实例在运行。
    /// lastError:CreateMutexW 的原始 GetLastError(诊断用)。
    bool ensure(const QString& key, unsigned long* lastError = nullptr);
    /// (二次启动时)向已有实例发送唤起消息。
    void activateExisting();

private:
    void* m_mutex = nullptr; // HANDLE,避免在头文件引 windows.h
};
