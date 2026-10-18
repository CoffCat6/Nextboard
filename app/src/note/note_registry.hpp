// 便签注册表:条目 id → NoteWindow,以及"全部隐藏/恢复/重夹"等批量动作。
//
// ⚠️ 与贴图的 pinRegistry() 刻意**不共用语义**(docs/20 §1.2/§7.1):
//    贴图是有界缓存,超限**静默淘汰最旧**;便签是用户的桌面布局,超限必须
//    **拒绝新建并提示**。两者的生命周期也不同 —— 便签关窗要落库 visible=0,
//    重启后仍保留几何(下次重贴回原位)。
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

namespace nb {

class NoteWindow;

class NoteRegistry : public QObject {
    Q_OBJECT
public:
    static NoteRegistry& instance();

    /// 打开(或前置)某条待办的便签。已开着则展开并抬到最前。
    /// 返回 false 表示失败(条目不是待办 / 核心调用失败 / 已达上限)。
    bool open(const QString& todoId);

    /// 关闭某条待办的便签窗口并落库 visible=0(几何保留)。
    void close(const QString& todoId);

    /// 启动恢复:读 todo_note_list,只为 visible=1 的行建窗(docs/20 §7.5)。
    void restoreAll();

    /// 面板侧变动后的同步:条目被删 → 关窗;内容/状态变了 → 回灌。
    /// 由 CoreFacade::clipChanged 驱动(去抖在主面板模型侧,这里只做轻量比对)。
    void syncFromCore();

    /// 截图会话期间隐藏全部便签(docs/20 §7.4)。
    void hideAllForCapture();
    void restoreAfterCapture();

    /// 屏幕增删/分辨率变化:全部重夹回各自可用区(拔副屏后不留虚空的窗口)。
    void reclampAll();

    int count() const { return m_windows.size(); }
    bool has(const QString& todoId) const { return m_windows.contains(todoId); }

    /// 当前所有便签的列表(托盘子菜单用:id + 首行摘要 + 是否收起)。
    QVariantList describe() const;

    /// 全部展开 / 全部收起(托盘兜底入口 —— 便签是 Qt::Tool,不占任务栏,
    /// 被别的置顶窗口盖住时只有托盘能找回它们)。
    void expandAll();
    void collapseAll();

signals:
    /// 便签集合或状态变化(托盘子菜单据此重建)。
    void changed();

private:
    NoteRegistry() = default;
    void forget(const QString& todoId);
    bool createWindow(const QVariantMap& item, bool restoreCollapsed);
    void wireWindow(NoteWindow* window);
    void applyLimit();

    QHash<QString, QPointer<NoteWindow>> m_windows;
    /// 截图期间隐藏的便签 id 集合:**只恢复我们真的隐藏过的那些**。
    /// 若恢复时照着"全部打开中的便签"去 show,会把用户本来就关着的窗口翻开。
    QHash<QString, bool> m_hiddenForCapture;
};

} // namespace nb
