#include "note_registry.hpp"

#include "note_window.hpp"

#include "../host/memory_probe.hpp"
#include "../panel/core_facade.hpp"

#include <QDebug>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

namespace nb {

namespace {

/// 便签数量上限(docs/20 §11 Q4 拍板):超限**拒绝新建**,绝不淘汰 ——
/// 静默关掉一张便签 = 用户桌面布局被销毁。
constexpr int kNoteLimit = 20;

} // namespace

NoteRegistry& NoteRegistry::instance() {
    static NoteRegistry registry;
    return registry;
}

bool NoteRegistry::open(const QString& todoId) {
    if (todoId.isEmpty()) return false;
    // 已开着:展开并抬到最前,不新建(1:1,docs/20 §11 Q6)
    if (NoteWindow* existing = m_windows.value(todoId).data()) {
        existing->expand();
        existing->show();
        existing->raise();
        return true;
    }

    CoreFacade& facade = CoreFacade::instance();
    // 先问待办本体(校验存在 + 取正文/时刻)
    const QVariantMap got = facade.call(QStringLiteral("todo_get"),
                                        {{QStringLiteral("id"), todoId}});
    if (!got.value(QStringLiteral("ok")).toBool()
        || got.value(QStringLiteral("data")).toMap().isEmpty()) {
        facade.showToast(QStringLiteral("这条待办已不存在"));
        return false;
    }
    const QVariantMap todoData = got.value(QStringLiteral("data")).toMap();

    // 已有便签行(visible=0 的"几何记忆")→ 用它回原位;没有则用默认摆放。
    QVariantMap note;
    const QVariantMap list = facade.call(QStringLiteral("todo_note_list"));
    if (list.value(QStringLiteral("ok")).toBool()) {
        for (const QVariant& raw : list.value(QStringLiteral("data")).toMap()
                                       .value(QStringLiteral("items"))
                                       .toList()) {
            const QVariantMap item = raw.toMap();
            if (item.value(QStringLiteral("id")).toString() == todoId) {
                note = item.value(QStringLiteral("note")).toMap();
                break;
            }
        }
    }
    if (note.isEmpty()) {
        // 默认落点:光标所在屏右上角内缩 24px(便签是"贴在桌面上"的东西,
        // 靠边更符合直觉;后续由用户拖动/吸附)。
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
        note.insert(QStringLiteral("x"), avail.right() - kNoteDefaultW - 24);
        note.insert(QStringLiteral("y"), avail.top() + 96);
        note.insert(QStringLiteral("w"), kNoteDefaultW);
        note.insert(QStringLiteral("h"), kNoteDefaultH);
        note.insert(QStringLiteral("screen"), screen ? screen->name() : QString());
        note.insert(QStringLiteral("visible"), true);
    } else {
        note.insert(QStringLiteral("visible"), true);
    }

    // 上限检查放在"已知这是新窗"之后:超额直接拒绝并说清原因。
    if (m_windows.size() >= kNoteLimit) {
        facade.showToast(QStringLiteral("便签已达上限 %1 张,请先关闭一些").arg(kNoteLimit));
        return false;
    }

    QVariantMap item = todoData;
    item.insert(QStringLiteral("note"), note);
    if (!createWindow(item, /*restoreCollapsed=*/false)) return false;

    // 首次建窗要把行写下去(含几何),否则重启时无处可恢复
    QVariantMap save = note;
    save.insert(QStringLiteral("id"), todoId);
    save.insert(QStringLiteral("visible"), true);
    save.remove(QStringLiteral("updatedAt"));
    facade.call(QStringLiteral("todo_note_save"), save);
    return true;
}

bool NoteRegistry::createWindow(const QVariantMap& item, bool restoreCollapsed) {
    const QString id = item.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) return false;
    if (m_windows.contains(id)) return true;

    NoteData data;
    data.id = id;
    data.text = item.value(QStringLiteral("textContent")).toString();
    // 备注在顶层 noteText(todo_get / todo_list / todo_note_list 同一口径)
    data.noteText = item.value(QStringLiteral("noteText")).toString();
    const QVariantMap todo = item.value(QStringLiteral("todo")).toMap();
    data.done = todo.contains(QStringLiteral("done"))
                    ? todo.value(QStringLiteral("done")).toBool()
                    : item.contains(QStringLiteral("doneAt"));
    data.dueAt = todo.value(QStringLiteral("dueAt")).toLongLong();
    data.overdue = todo.value(QStringLiteral("overdue")).toBool();
    data.note = item.value(QStringLiteral("note")).toMap();
    if (restoreCollapsed) {
        data.note.insert(QStringLiteral("collapsed"),
                         data.note.value(QStringLiteral("collapsed"), false));
    }

    auto* window = new NoteWindow(data);
    window->setAttribute(Qt::WA_DeleteOnClose);
    m_windows.insert(id, window);
    wireWindow(window);
    applyLimit();
    window->show();
    logMemory("note-add");
    emit changed();
    return true;
}

void NoteRegistry::wireWindow(NoteWindow* window) {
    const QString id = window->noteId();
    connect(window, &NoteWindow::closeRequested, this, [this](const QString& closedId) {
        close(closedId);
    });
    connect(window, &NoteWindow::editRequested, this, [](const QString& editId) {
        // 复用面板的待办编辑卡片(截止/标签的复杂控件只在那边实现一份)
        emit CoreFacade::instance().todoOpenRequested(editId);
    });
    connect(window, &NoteWindow::stateChanged, this, [this, id](const QString&) {
        Q_UNUSED(id);
        emit changed();
    });
    connect(window, &QObject::destroyed, this, [this, id] {
        // 窗口被任何人销毁(close/清理)都要从注册表摘掉,否则 describe() 会拿到野指针
        forget(id);
    });
}

void NoteRegistry::forget(const QString& todoId) {
    m_windows.remove(todoId);
    m_hiddenForCapture.remove(todoId);
    emit changed();
}

void NoteRegistry::applyLimit() {
    // 上限只用于**拒绝新建**(见 open),这里不淘汰。函数留作将来的可观测点:
    // 数量接近上限时打一条日志,便于排查"为什么点贴屏没反应"。
    if (m_windows.size() >= kNoteLimit) {
        qWarning() << "便签数量已达上限:" << m_windows.size();
    }
}

void NoteRegistry::close(const QString& todoId) {
    // ⚠️ 用 value() 而非 operator[](2026-11-03 修)。原写法 `QPointer<NoteWindow>& ref =
    //    m_windows[todoId]` 有三个隐患:
    //    ① 键不存在时**隐式插入**一个空 QPointer(查询带上了修改语义,与
    //       open()/describe() 用 value()/contains() 的口径不一致);
    //    ② ref 是哈希槽的**引用**,而下面的 window->close() 会同步触发
    //       destroyed → forget() → m_windows.remove(id) —— 哈希可能重排,ref 悬空;
    //    ③ 今天 ref 在 close() 之后没被再用,所以崩不了。这种"恰好没用到"的
    //       约定迟早被后人踩穿。
    const QPointer<NoteWindow> window = m_windows.value(todoId);
    if (!window) {
        m_windows.remove(todoId);
        return;
    }
    window->flushBody();
    // visible=0:关窗 ≠ 忘位置(几何保留,下次重贴回原位,docs/20 §3.1)
    CoreFacade::instance().call(
        QStringLiteral("todo_note_save"),
        {{QStringLiteral("id"), todoId}, {QStringLiteral("visible"), false}});
    // 直接 close() → WA_DeleteOnClose → destroyed → forget()
    window->close();
}

void NoteRegistry::restoreAll() {
    const QVariantMap resp = CoreFacade::instance().call(QStringLiteral("todo_note_list"));
    if (!resp.value(QStringLiteral("ok")).toBool()) {
        qWarning() << "便签恢复失败:todo_note_list 调用异常";
        return;
    }
    const QVariantList items = resp.value(QStringLiteral("data")).toMap()
                                   .value(QStringLiteral("items")).toList();
    int restored = 0;
    for (const QVariant& raw : items) {
        const QVariantMap item = raw.toMap();
        const QVariantMap note = item.value(QStringLiteral("note")).toMap();
        // visible=0 的行只是"几何记忆",不建窗(用户上次主动关掉了)
        if (!note.value(QStringLiteral("visible"), true).toBool()) continue;
        if (createWindow(item, /*restoreCollapsed=*/true)) ++restored;
    }
    if (restored > 0) qInfo() << "便签恢复:" << restored << "张";
}

void NoteRegistry::syncFromCore() {
    if (m_windows.isEmpty()) return;
    const QVariantMap resp = CoreFacade::instance().call(QStringLiteral("todo_note_list"));
    if (!resp.value(QStringLiteral("ok")).toBool()) return;
    const QVariantList items = resp.value(QStringLiteral("data")).toMap()
                                   .value(QStringLiteral("items")).toList();

    QHash<QString, QVariantMap> byId;
    for (const QVariant& raw : items) {
        const QVariantMap item = raw.toMap();
        byId.insert(item.value(QStringLiteral("id")).toString(), item);
    }

    // 1) 存活检查:条目被删/被清空/被导入替换 → 关窗(不再写 visible,行已经没了)
    const QList<QString> ids = m_windows.keys();
    for (const QString& id : ids) {
        if (byId.contains(id)) continue;
        if (NoteWindow* window = m_windows.value(id).data()) {
            // 关窗是**用户可见的删除**,不该再由 destroyed 回调去 forget(会重复 emit);
            // 这里先断开,再 close,最后手动从表里摘掉(docs/20 §7.3 的存活检查)。
            window->close();
        }
        m_windows.remove(id);
        m_hiddenForCapture.remove(id);
    }

    // 2) 内容同步(正文/完成态/截止):只在真的不一致时回灌,避免光标跳动
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        if (NoteWindow* window = m_windows.value(it.key()).data()) {
            const QVariantMap todo = it.value().value(QStringLiteral("todo")).toMap();
            window->applyExternalState(
                it.value().value(QStringLiteral("textContent")).toString(),
                it.value().value(QStringLiteral("noteText")).toString(),
                todo.value(QStringLiteral("done")).toBool(),
                todo.value(QStringLiteral("dueAt")).toLongLong(),
                todo.value(QStringLiteral("overdue")).toBool());
        }
    }
    emit changed();
}

void NoteRegistry::hideAllForCapture() {
    m_hiddenForCapture.clear();
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        NoteWindow* window = it.value();
        if (!window || !window->isVisible()) continue;
        // 收起态也要记:它们是"本来就在屏幕上(半隐藏)"的,恢复时要回到收起态
        m_hiddenForCapture.insert(it.key(), window->isCollapsed());
        window->flushBody();
        window->hide();
    }
}

void NoteRegistry::restoreAfterCapture() {
    const QHash<QString, bool> pending = m_hiddenForCapture;
    m_hiddenForCapture.clear();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        NoteWindow* window = m_windows.value(it.key());
        if (!window) continue;
        window->show();
        if (it.value()) window->collapse(/*animate=*/false);
    }
}

void NoteRegistry::reclampAll() {
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (NoteWindow* window = it.value()) {
            // 屏幕变了:drop 掉记录的屏名,让窗口按当前几何重新定位
            window->reclampToCurrentScreen();
        }
    }
    emit changed();
}

QVariantList NoteRegistry::describe() const {
    QVariantList out;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        NoteWindow* window = it.value();
        if (!window) continue;
        out.append(QVariantMap{
            {QStringLiteral("id"), it.key()},
            {QStringLiteral("collapsed"), window->isCollapsed()},
        });
    }
    return out;
}

void NoteRegistry::expandAll() {
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (NoteWindow* window = it.value()) window->expand();
    }
}

void NoteRegistry::collapseAll() {
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it) {
        if (NoteWindow* window = it.value()) window->collapse(/*animate=*/true, /*force=*/true);
    }
}

} // namespace nb
