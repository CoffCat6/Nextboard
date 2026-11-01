// 快捷指令分发器(docs/24):QuickRail 的每颗按钮 = 一条用户配置的指令,
// 点击后按 type 分发到执行层。
//
// 三层分离(docs/24 §7.1):
//   配置层  settings.quick_rail_commands(nbcore 只做语法校验);
//   分发层  本文件:type → handler 注册表(QHash,开放注册 —— 新增类型 =
//           注册一个 handler,为插件入口留的接缝之一);
//   执行层  ShellExecute(app/system)/ CoreFacade+PanelWindow(internal)。
//
// 安全边界(docs/24 §7.4):
//   - nbcore 永不执行进程,所有启动动作在宿主;
//   - app 类型不做 shell 包装:args 原样进 ShellExecute 参数槽,
//     不经 cmd,&& / | / > 全部无效 —— 想跑脚本就建 .bat 再指向它;
//   - system 类型只认 kSystemEntries 目录键 + ms-settings: 前缀。
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QHash>

namespace nb {

class PanelWindow;
class CoreBridge;

class QuickCommands : public QObject {
    Q_OBJECT
public:
    /// 单条 internal 动作的注册项。available 为插件时代预留(v1 恒 true):
    /// 某动作在当前环境不可用时目录里标记 false,UI 置灰。
    struct ActionEntry {
        QString label;
        QString iconKind;
        bool available = true;
        std::function<bool()> run;   // true = 已触发(执行失败的 toast 由 handler 自理)
    };

    QuickCommands(PanelWindow& panel, CoreBridge& core, QObject* parent = nullptr);

    /// internal 动作目录(QML 渲染 + 设置页挑选 + 宿主语义校验)。
    /// [{id, label, iconKind, available}];顺序即设置页挑选列表的展示序。
    Q_INVOKABLE QVariantList quickActionCatalog() const;

    /// system 精选目录(设置页下拉):[{key, label}]。
    Q_INVOKABLE QVariantList quickSystemCatalog() const;

    /// system 键 → 执行串(验收断言用,不真执行;未知键返回空串)。
    Q_INVOKABLE QString quickSystemResolve(const QString& key) const;

    /// 执行一条指令(QuickRail 点击的统一入口)。cmd 是 settings 里的单条
    /// 命令对象(id/type/label/target/args/workdir)。未知 type 静默忽略
    /// (前向兼容契约:高版本配置在低版本宿主上不炸)。
    Q_INVOKABLE void runCommand(const QVariantMap& cmd);

    /// 执行 internal 动作(设置页"试运行"与验收用)。未知 id → toast。
    /// 返回是否找到 handler。
    Q_INVOKABLE bool runQuickAction(const QString& actionId);

signals:
    /// internal 动作的动态状态可能变了(如 monitor.toggle 的 label 随监听状态),
    /// rail 与设置页据此刷新目录相关 UI。
    void quickCatalogChanged();

private:
    void registerActions();
    /// app/system/internal 三个执行层。返回 false = 启动失败(已 toast)。
    bool runApp(const QVariantMap& cmd);
    bool runSystem(const QVariantMap& cmd);
    bool runInternal(const QVariantMap& cmd);
    /// ShellExecuteW 统一封装。返回是否成功(>32 为 Win32 成功约定)。
    static bool shellOpen(const QString& target, const QString& args,
                          const QString& workdir);

    PanelWindow& m_panel;
    CoreBridge& m_core;
    QHash<QString, ActionEntry> m_actions;
};

} // namespace nb
