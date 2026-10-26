#include "clip_entry_model.hpp"

#include "core_facade.hpp"
#include "host/core_bridge.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QDebug>

namespace nb {

ClipEntryModel::ClipEntryModel(QObject* parent) : QAbstractListModel(parent) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(200);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        m_query = m_pendingQuery;
        reload(m_limit);
    });
}

int ClipEntryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant ClipEntryModel::data(const QModelIndex& index, int role) const {
    if (!hasIndex(index.row(), index.column(), QModelIndex())) return {};
    const Row& r = m_rows[index.row()];
    switch (role) {
        case IdRole: return r.id;
        case KindRole: return r.kind;
        case TextRole: return r.text;
        case ImageUrlRole: return r.kind == QLatin1String("image")
                                 ? imageUrlForBlob(r.blobPath, true) : QString();
        case PreviewRole: return r.preview;
        case TimeRole: return formatTime(r.lastUsedAt);
        case SizeRole: return formatSize(r.byteSize);
        case ByteSizeRole: return r.byteSize;
        case PinnedRole: return r.pinned;
        case SourceRole: return r.source;
        case FilePathsRole: return r.filePaths;
        case BlobPathRole: return r.blobPath;
        case TagsRole: return r.tags;
        case TodoDueRole: return r.todoDueAt;
        case TodoRemindRole: return r.todoRemindAt;
        case TodoDoneRole: return r.todoDone;
        case TodoOverdueRole: return r.todoOverdue;
        case TodoNoteRole: return r.todoNote;
    }
    return {};
}

QHash<int, QByteArray> ClipEntryModel::roleNames() const {
    return {
        {IdRole, "entryId"},   {KindRole, "kind"},     {TextRole, "text"},
        {ImageUrlRole, "imageUrl"}, {PreviewRole, "preview"},
        {TimeRole, "timeText"}, {SizeRole, "sizeText"}, {ByteSizeRole, "byteSize"},
        {PinnedRole, "pinned"}, {SourceRole, "source"}, {FilePathsRole, "filePaths"},
        {BlobPathRole, "blobPath"}, {TagsRole, "tags"},
        {TodoDueRole, "todoDueAt"}, {TodoRemindRole, "todoRemindAt"},
        {TodoDoneRole, "todoDone"}, {TodoOverdueRole, "todoOverdue"},
        {TodoNoteRole, "todoNote"},
    };
}

QVariantMap ClipEntryModel::buildListRequest(int limit, int offset) const {
    QVariantMap req{{"limit", limit}, {"offset", offset}};
    // 待办视图走独立端点(todo_list):它的 kind 是隐含的,且排序口径不同
    // (逾期>今日>未来>无期限,归档垫底,docs/05-todo.md §5)。
    if (m_view == QLatin1String("todo")) {
        // ⚠️ 取 all:完成的待办是归档不是删除,从待办页消失会像"数据丢了"。
        req.insert("done", "all");
    } else {
        const QString kind = (m_view == QLatin1String("pinned")
                              || m_view == QLatin1String("all"))
                                 ? QString() : m_view;
        if (!kind.isEmpty()) req.insert("kind", kind);
        if (m_view == QLatin1String("pinned")) req.insert("pinned", true);
    }
    // 搜索/标签/来源筛选与主列表同口径叠加(两个端点都认这几个键;
    // todo_list 不吃 source —— 待办来源恒为 'todo',无需筛选)
    if (!m_query.trimmed().isEmpty()) req.insert("query", m_query);
    if (!m_tag.isEmpty()) req.insert("tag", m_tag);
    if (!m_source.isEmpty() && m_view != QLatin1String("todo"))
        req.insert("source", m_source);
    return req;
}

/// 列表端点:待办视图用 todo_list,其余用 list_entries。
static const char* listMethodFor(const QString& view) {
    return view == QLatin1String("todo") ? "todo_list" : "list_entries";
}

bool ClipEntryModel::canFetchMore(const QModelIndex& parent) const {
    return !m_released && !parent.isValid() && m_rows.size() < m_total;
}

void ClipEntryModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || m_loading || m_released) return;
    m_loading = true;
    emit loadingChanged();
    // 追加下一页:请求口径与首屏完全一致(buildListRequest 单一来源)
    const QVariantMap resp =
        CoreFacade::instance().call(listMethodFor(m_view),
                                    buildListRequest(m_limit, m_rows.size()));
    m_loading = false;
    emit loadingChanged();
    applyPage(resp, true);
}

void ClipEntryModel::setView(const QString& view) {
    if (m_view == view) return;
    m_view = view;
    reload(m_limit);
}

void ClipEntryModel::setQuery(const QString& query) {
    if (m_pendingQuery == query) return;
    m_pendingQuery = query;
    m_debounce.start();   // 与 refresh 共用 200ms 定时器:输入停顿才真正查询
}

void ClipEntryModel::refresh() {
    // 隐藏期间不填列表(否则释放/冻结白做);冻结期要**记账**,
    // 解冻时补一次重载,否则会漏掉隐藏期间的最新剪贴板内容。
    if (m_released || m_frozen) {
        m_dirtyWhileFrozen = true;
        return;
    }
    if (m_debounce.isActive()) return;   // 已有待执行的刷新/查询
    m_debounce.start();
}

void ClipEntryModel::refreshNow() {
    // 冻结/释放期间调用(常见于 acquire 之前的竞态):记账,解冻时补一次重载。
    if (m_frozen) { m_dirtyWhileFrozen = true; return; }
    if (m_released) return;
    m_debounce.stop();                    // 打断待执行的合并刷新,立即重拉
    m_query = m_pendingQuery;
    reload(m_limit);
}

void ClipEntryModel::release() {
    if (m_released) return;
    m_released = true;
    m_frozen = false;            // 两种"离线"状态互斥,避免状态残留
    m_dirtyWhileFrozen = false;
    m_loadedOnce = false;        // 行已清空 → 不再"有效",acquire() 必须重载
    m_debounce.stop();   // 打断待执行的防抖刷新,否则释放后又被补一次
    if (m_rows.isEmpty()) return;
    // 整表重置:QML 委托被销毁 → 图片纹理随之下线
    beginResetModel();
    m_rows.clear();
    m_total = 0;
    endResetModel();
    emit totalChanged();
    emit countChanged();
    emit hasMoreChanged();
}

void ClipEntryModel::acquire() {
    // 冻结期数据未变 → 行仍然有效:**只解冻、不重载**。
    // 重载会 beginResetModel + 清空 + 重填,即把全部 QML 委托销毁重建 ——
    // 实测那正是"首个显隐循环 +42MB 不可回落"的成因(堆 churn)。
    // ⚠️ 首次显示必须装载:此时 m_rows 空且两个标志都没置位,
    //    只看 released/frozen 会跳过装载 → 面板空白。
    const bool needReload = m_released || m_dirtyWhileFrozen || !m_loadedOnce;
    m_released = false;
    m_frozen = false;
    m_dirtyWhileFrozen = false;
    if (needReload) refreshNow();
}

void ClipEntryModel::freeze() {
    if (m_frozen || m_released) return;
    // 还没装载过就没什么可保留的:直接按"未装载"处理,acquire() 时自然会拉。
    m_frozen = true;
    m_debounce.stop();   // 同 release:打断待执行的防抖刷新
}

void ClipEntryModel::fetchMoreRequest() {
    if (canFetchMore({})) fetchMore({});
}

bool ClipEntryModel::updateRowById(const QString& id, const std::function<void(Row&)>& mutate) {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id != id) continue;
        mutate(m_rows[i]);
        // 不指定角色集:QML 按需重读该行各角色,成本远低于整表 reset
        emit dataChanged(index(i), index(i));
        return true;
    }
    return false;
}

void ClipEntryModel::refreshStats() {
    CoreFacade::instance().refreshCounts();
    refreshTags();
    refreshSources();
}

QVariantMap ClipEntryModel::copyEntry(const QString& id) {
    return CoreFacade::instance().call("copy_entry", {{"id", id}});
}

QVariantMap ClipEntryModel::copyByIndex(int index) {
    if (index < 0 || index >= m_rows.size())
        return {{"ok", false},
                {"error", QVariantMap{{"code", "OutOfRange"},
                                      {"message", QStringLiteral("序号越界")}}}};
    return copyEntry(m_rows[index].id);
}

QVariantMap ClipEntryModel::deleteEntry(const QString& id) {
    const QVariantMap resp = CoreFacade::instance().call("delete_entry", {{"id", id}});
    if (!resp.value("ok").toBool()) return resp;
    // 单行删除走 beginRemoveRows:整表 reset 会销毁全部委托、丢滚动位置与展开态。
    // (删除不影响其余行的排序口径 pinned/last_used,单行移除是安全的。)
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id != id) continue;
        beginRemoveRows({}, i, i);
        m_rows.remove(i);
        endRemoveRows();
        break;
    }
    m_total = qMax(0, m_total - 1);
    emit totalChanged();
    emit countChanged();
    emit hasMoreChanged();
    refreshStats();   // 计数徽标与标签/来源统计都随条目数变化
    return resp;
}

void ClipEntryModel::togglePin(const QString& id, bool pinned) {
    const QVariantMap resp =
        CoreFacade::instance().call("set_pinned", {{"id", id}, {"pinned", pinned}});
    if (!resp.value("ok").toBool()) {
        emit toast(QStringLiteral("固定失败:%1").arg(
            resp.value("error").toMap().value("message").toString()));
        return;
    }
    // 固定状态改变排序(固定置顶),重拉对齐第一代 refetch 语义
    reload(m_limit);
}

void ClipEntryModel::setTag(const QString& tag) {
    if (m_tag == tag) return;
    m_tag = tag;
    reload(m_limit);
}

void ClipEntryModel::setSource(const QString& source) {
    if (m_source == source) return;
    m_source = source;
    reload(m_limit);
}

void ClipEntryModel::saveTags(const QString& id, const QStringList& tags) {
    const QVariantMap resp =
        CoreFacade::instance().call("set_entry_tags", {{"id", id}, {"tags", tags}});
    if (!resp.value("ok").toBool()) {
        emit toast(QStringLiteral("保存标签失败:%1").arg(
            resp.value("error").toMap().value("message").toString()));
        return;
    }
    // 就地更新:标签变更不再整表 reset。仅当当前正按**被移除的**标签筛选时
    // 才需要重拉(该行应从列表消失);标签统计条照常刷新。
    const bool filteredOut = !m_tag.isEmpty() && !tags.contains(m_tag);
    if (!filteredOut && updateRowById(id, [&tags](Row& r) { r.tags = tags; })) {
        refreshTags();
        return;
    }
    reload(m_limit);   // applyPage 内会一并刷新标签统计
}

void ClipEntryModel::refreshTags() {
    const QVariantMap resp = CoreFacade::instance().call("list_tags");
    if (!resp.value("ok").toBool()) return;
    const QVariantList list = resp.value("data").toList();
    if (list == m_tags) return;   // 无变化不发信号,避免无谓的绑定重算
    m_tags = list;
    emit tagStatsChanged();
}

void ClipEntryModel::refreshSources() {
    const QVariantMap resp = CoreFacade::instance().call("list_sources");
    if (!resp.value("ok").toBool()) return;
    const QVariantList list = resp.value("data").toList();
    if (list == m_sources) return;   // 同上:无变化不发信号
    m_sources = list;
    emit sourceStatsChanged();
}

QVariantMap ClipEntryModel::renameTag(const QString& from, const QString& to) {
    QVariantMap req{{"from", from}, {"to", to}};
    const QVariantMap resp = CoreFacade::instance().call("rename_tag", req);
    if (!resp.value("ok").toBool()) {
        return {{"ok", false},
                {"message", resp.value("error").toMap().value("message").toString()}};
    }
    // 当前筛选的正是被改名的标签 → 跟随到新名(否则列表会突然空掉)
    if (!m_tag.isEmpty() && m_tag == from) {
        m_tag = to.trimmed();
        emit tagStatsChanged();   // 让 QML 的 activeTag 也能同步
    }
    reload(m_limit);
    return {{"ok", true}, {"changed", resp.value("data").toMap().value("changed").toInt()}};
}

QVariantMap ClipEntryModel::addTagsBulk(const QStringList& ids, const QStringList& tags) {
    if (ids.isEmpty() || tags.isEmpty()) return {{"ok", true}, {"changed", 0}};
    const QVariantMap resp = CoreFacade::instance().call(
        "add_tags_bulk", QVariantMap{{"ids", ids}, {"tags", tags}});
    if (!resp.value("ok").toBool()) {
        return {{"ok", false},
                {"message", resp.value("error").toMap().value("message").toString()}};
    }
    reload(m_limit);
    return {{"ok", true}, {"changed", resp.value("data").toMap().value("changed").toInt()}};
}

// ---- 条目多选批量操作 ----
// 统一形态:调 FFI → 失败带 message 返回、成功 reload。批量后**清空选择状态
// 是调用方(Main.qml)的职责** —— 模型不知道选择集合的存在(见方案:选择态归 QML)。

QVariantMap ClipEntryModel::deleteBulk(const QStringList& ids) {
    if (ids.isEmpty()) return {{"ok", true}, {"deleted", QStringList{}}};
    const QVariantMap resp = CoreFacade::instance().call(
        "delete_entries_bulk", QVariantMap{{"ids", ids}});
    if (!resp.value("ok").toBool()) {
        return {{"ok", false},
                {"message", resp.value("error").toMap().value("message").toString()}};
    }
    reload(m_limit);
    return {{"ok", true}, {"deleted", resp.value("data").toMap().value("deleted").toList()}};
}

QVariantMap ClipEntryModel::setPinnedBulk(const QStringList& ids, bool pinned) {
    if (ids.isEmpty()) return {{"ok", true}, {"changed", 0}};
    const QVariantMap resp = CoreFacade::instance().call(
        "set_pinned_bulk", QVariantMap{{"ids", ids}, {"pinned", pinned}});
    if (!resp.value("ok").toBool()) {
        return {{"ok", false},
                {"message", resp.value("error").toMap().value("message").toString()}};
    }
    reload(m_limit);
    return {{"ok", true}, {"changed", resp.value("data").toMap().value("changed").toInt()}};
}

QVariantMap ClipEntryModel::todoSetDoneBulk(const QStringList& ids, bool done) {
    if (ids.isEmpty()) return {{"ok", true}, {"changed", 0}};
    const QVariantMap resp = CoreFacade::instance().call(
        "todo_set_done_bulk", QVariantMap{{"ids", ids}, {"done", done}});
    if (!resp.value("ok").toBool()) {
        return {{"ok", false},
                {"message", resp.value("error").toMap().value("message").toString()}};
    }
    reload(m_limit);
    return {{"ok", true}, {"changed", resp.value("data").toMap().value("changed").toInt()}};
}

// ---- 待办(docs/05-todo.md §5/§6) ----

/// times 里的时刻 → FFI 三态参数:键缺失/无效 = 不传(null),null/0 = 明确清空。
static void applyTimeFields(QVariantMap& req, const QVariantMap& times, const QString& key) {
    const QVariant v = times.value(key);
    if (!v.isValid()) return;                        // 未提及:不动
    if (v.isNull()) {                                // 显式清空
        req.insert(key, QVariant());
        return;
    }
    bool ok = false;
    const qint64 ms = v.toLongLong(&ok);
    if (ok && ms > 0) {
        // ⚠️ 必须转成 double:QML 的 real 经 QVariant 过来是 double,直接塞
        //    qint64 会被 JSON 序列化成字符串(实测),core 侧 as_i64() 拿不到。
        req.insert(key, static_cast<double>(ms));
    } else {
        req.insert(key, QVariant());                 // 传 null = 清空该时刻
    }
}

void ClipEntryModel::todoSetDone(const QString& id, bool done) {
    const QVariantMap resp =
        CoreFacade::instance().call("todo_set_done", {{"id", id}, {"done", done}});
    if (!resp.value("ok").toBool()) {
        emit toast(QStringLiteral("更新待办失败:%1").arg(
            resp.value("error").toMap().value("message").toString()));
        return;
    }
    // 就地更新:勾选完成不再整表 reset(销毁全部委托、丢展开态与滚动位置)。
    // 待办视图的排序受完成态影响(归档垫底,见 core TODO_ORDER)→ 保留重拉;
    // all 视图排序只看 pinned/last_used,与完成态无关,行内更新即可。
    if (m_view != QLatin1String("todo")
        && updateRowById(id, [done](Row& r) { r.todoDone = done; })) {
        CoreFacade::instance().refreshCounts();   // 完成态影响"待办"徽标计数
        return;
    }
    reload(m_limit);
}

void ClipEntryModel::todoUpdateTimes(const QString& id, const QVariantMap& times) {
    QVariantMap req{{"id", id}};
    applyTimeFields(req, times, "dueAt");
    applyTimeFields(req, times, "remindAt");
    applyTimeFields(req, times, "notifyAt");
    const QVariantMap resp = CoreFacade::instance().call("todo_update", req);
    if (!resp.value("ok").toBool()) {
        emit toast(QStringLiteral("推迟失败:%1").arg(
            resp.value("error").toMap().value("message").toString()));
        return;
    }
    // 就地更新(取舍同 todoSetDone:待办视图排序受 due_at 影响,保留重拉)。
    // overdue 按 core 的同一定义重算:due_at 非空且 < now 且未完成(TODO_ORDER 的 CASE)。
    // req 里是三态化后的值:缺键 = 不提及(不动),null = 清空,double = 新时刻。
    if (m_view != QLatin1String("todo")) {
        const bool updated = updateRowById(id, [&req](Row& r) {
            if (req.contains("dueAt")) {
                const QVariant v = req.value("dueAt");
                r.todoDueAt = v.isNull() ? 0 : static_cast<qint64>(v.toDouble());
            }
            if (req.contains("remindAt")) {
                const QVariant v = req.value("remindAt");
                r.todoRemindAt = v.isNull() ? 0 : static_cast<qint64>(v.toDouble());
            }
            r.todoOverdue = r.todoDueAt > 0
                            && r.todoDueAt < QDateTime::currentMSecsSinceEpoch()
                            && !r.todoDone;
        });
        if (updated) {
            CoreFacade::instance().refreshCounts();
            return;
        }
    }
    reload(m_limit);
}

QVariantMap ClipEntryModel::todoSave(const QString& id, const QString& text,
                                     const QVariantMap& times, const QStringList& tags,
                                     const QString& noteText) {
    const bool editing = !id.isEmpty();
    QVariantMap req;
    req.insert("text", text);
    // 备注:编辑时**总是**下发(空串 = 明确清空);新建时只在非空时下发
    // —— 与 tags 同一取舍:core 对缺键视为"不改",不下发就清不掉旧备注。
    if (editing || !noteText.isEmpty()) req.insert("noteText", noteText);
    if (editing) req.insert("id", id);
    applyTimeFields(req, times, "dueAt");
    applyTimeFields(req, times, "remindAt");
    applyTimeFields(req, times, "notifyAt");
    // ⚠️ 编辑时必须**无条件下发 tags**(空数组 = 明确清空):core 的 tags_opt 对
    //    缺键视为"不改",不下发就删不掉标签(实测"编辑里删光标签保存"无效)。
    if (editing || !tags.isEmpty()) {
        req.insert("tags", tags);
    }
    const QVariantMap resp =
        CoreFacade::instance().call(editing ? "todo_update" : "todo_create", req);
    if (!resp.value("ok").toBool()) {
        const QString msg = resp.value("error").toMap().value("message").toString();
        emit toast(QStringLiteral("保存待办失败:%1").arg(msg));
        // 把 core 的原文一并回传,让卡片错误行能显示具体原因(如正文超长)
        return {{"ok", false}, {"message", msg}};
    }
    reload(m_limit);
    return {{"ok", true},
            {"id", resp.value("data").toMap().value("id").toString()}};
}

QVariantMap ClipEntryModel::todoMeta(const QString& id) {
    if (id.isEmpty()) return {};
    // 一次 todo_get 拿全:正文 + 三个时刻 + 标签(比在 m_rows 里翻更可靠 ——
    // 条目可能被分页/筛选挡在列表外,那时行内查不到就会丢时刻)。
    const QVariantMap resp = CoreFacade::instance().call("todo_get", {{"id", id}});
    if (!resp.value("ok").toBool()) return {};
    const QVariantMap data = resp.value("data").toMap();
    if (data.isEmpty()) return {};
    const QString kind = data.value("kind").toString();
    if (kind != QLatin1String("todo")) return {};
    QVariantMap out;
    out.insert("text", data.value("textContent").toString());
    // 备注(docs/21):空 = 无备注
    out.insert("noteText", data.value("noteText").toString());
    out.insert("dueAt", static_cast<qint64>(data.value("dueAt").toDouble()));
    out.insert("remindAt", static_cast<qint64>(data.value("remindAt").toDouble()));
    out.insert("notifyAt", static_cast<qint64>(data.value("notifyAt").toDouble()));
    // 标签以 JSON 数组字符串存库,这里解析成 QStringList 供输入框回填
    const QString tagsJson = data.value("tags").toString();
    QStringList tags;
    if (!tagsJson.isEmpty()) {
        const QJsonArray arr = QJsonDocument::fromJson(tagsJson.toUtf8()).array();
        for (const QJsonValue& t : arr) tags.append(t.toString());
    }
    out.insert("tags", tags);
    return out;
}

bool ClipEntryModel::todoClearDone() {
    const QVariantMap resp = CoreFacade::instance().call("todo_clear_done", {});
    if (!resp.value("ok").toBool()) {
        emit toast(QStringLiteral("清空已完成失败:%1").arg(
            resp.value("error").toMap().value("message").toString()));
        return false;
    }
    reload(m_limit);
    return true;
}

int ClipEntryModel::indexOfId(const QString& id) const {
    if (id.isEmpty()) return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].id == id) return i;
    }
    return -1;
}

QString ClipEntryModel::rowGroupKey(int row) const {
    if (row < 0 || row >= m_rows.size()) return QString();
    const Row& r = m_rows[row];
    if (r.kind != QLatin1String("todo")) return QString();
    // ⚠️ 分组顺序必须与 core §5 的 ORDER BY 完全一致,否则标题会错位
    //    (core: 归档垫底;未完成内 逾期 > 今日 > 未来 > 无期限)。
    if (r.todoDone) return QStringLiteral("done");
    if (r.todoOverdue) return QStringLiteral("overdue");
    if (r.todoDueAt <= 0) return QStringLiteral("none");
    const QDate due = QDateTime::fromMSecsSinceEpoch(r.todoDueAt).date();
    return (due == QDate::currentDate()) ? QStringLiteral("today")
                                         : QStringLiteral("future");
}

QString ClipEntryModel::idAt(int row) const {
    return (row >= 0 && row < m_rows.size()) ? m_rows[row].id : QString();
}

QString ClipEntryModel::kindOfId(const QString& id) const {
    for (const Row& r : m_rows) {
        if (r.id == id) return r.kind;
    }
    return QString();
}

bool ClipEntryModel::isPinned(const QString& id) const {
    for (const Row& r : m_rows) {
        if (r.id == id) return r.pinned;
    }
    return false;
}

QString ClipEntryModel::imageUrlForBlob(const QString& blobPath, bool thumb) {
    if (blobPath.isEmpty()) return {};
    // blob 存的是相对 blobs 目录的路径;历史数据可能是绝对路径,两种都兼容。
    // 早期实现为每行发一次同步 get_entry 去取 blobPath —— 列表重绘时会放大成
    // N 次 DB 查询,是明显的性能陷阱;blobPath 已在 list_entries 响应里,直接用。
    const QString abs = QFileInfo(blobPath).isAbsolute()
                            ? blobPath
                            : CoreBridge::blobsDirAbs() + '/' + blobPath;
    QString url = abs;
    if (thumb) {
        // xxx.png → xxx.thumb<ver>.png(命名规则集中在 CoreBridge,与 core 侧对齐)
        url = CoreBridge::thumbnailPathFor(abs);
    }
    return QUrl::fromLocalFile(url).toString();
}

QString ClipEntryModel::formatTime(qint64 ms) {
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(ms);
    return d.date().toString(QStringLiteral("yyyy-MM-dd")) + ' ' +
           d.time().toString(QStringLiteral("HH:mm"));
}

QString ClipEntryModel::formatSize(qint64 n) {
    if (n < 1024) return QStringLiteral("%1 B").arg(n);
    if (n < 1048576) return QStringLiteral("%1 KB").arg(n / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(n / 1048576.0, 0, 'f', 1);
}

void ClipEntryModel::reload(int limit) {
    m_limit = limit;
    if (m_released) return;                          // 已释放:等 acquire() 再拉(它会重载)
    if (m_frozen) {                                  // 已冻结:记账,解冻时补一次重载
        // 冻结期可能发生:剪贴板通知、切视图/筛选、搜索词生效 —— 这些都会走到这里。
        // 不记账的话,解冻时会被误判为"数据未变"而跳过重载 → 列表停留在旧数据。
        m_dirtyWhileFrozen = true;
        return;
    }
    if (m_loading) { m_debounce.start(); return; }  // 进行中:稍后重试
    m_loading = true;
    emit loadingChanged();
    // 不预清空:FFI 是同步调用,数据马就到 —— 预清空只会多出一轮
    // begin/endReset(一轮 reset 销毁全部委托含复用池),旧画面留到新数据
    // 一并替换,无闪空。
    // 端点与请求口径都由统一函数决定(待办视图走 todo_list,见 buildListRequest)
    const QVariantMap resp = CoreFacade::instance().call(listMethodFor(m_view),
                                                        buildListRequest(m_limit, 0));
    m_loading = false;
    emit loadingChanged();
    applyPage(resp, false);
}

void ClipEntryModel::applyPage(const QVariantMap& resp, bool append) {
    if (!resp.value("ok").toBool()) {
        const QString msg = resp.value("error").toMap().value("message").toString();
        if (!append) {   // 首屏失败:展示错误;追加失败静默(下次触底重试)
            beginResetModel();
            m_rows.clear();
            m_total = 0;
            endResetModel();
            emit hasMoreChanged();
            m_errorText = QStringLiteral("加载失败:%1").arg(msg);
            emit errorTextChanged();
        }
        return;
    }
    if (!m_errorText.isEmpty()) {
        m_errorText.clear();
        emit errorTextChanged();
    }
    m_loadedOnce = true;   // 成功装载过一次 → acquire() 才敢跳过重载(见 acquire 注释)
    const QVariantMap data = resp.value("data").toMap();
    const QVariantList items = data.value("items").toList();
    QVector<Row> rows;
    rows.reserve(items.size());
    for (const QVariant& v : items) {
        const QVariantMap e = v.toMap();
        Row r;
        r.id = e.value("id").toString();
        r.kind = e.value("kind").toString();
        r.source = e.value("source").toString();
        r.pinned = e.value("pinned").toBool();
        r.lastUsedAt = static_cast<qint64>(e.value("lastUsedAt").toDouble());
        r.byteSize = static_cast<qint64>(e.value("byteSize").toDouble());
        r.preview = e.value("preview").toString();
        r.blobPath = e.value("blobPath").toString();
        if (r.kind == QLatin1String("file")) {
            const QVariantList paths = e.value("filePaths").toList();
            for (const QVariant& p : paths) r.filePaths.append(p.toString());
            r.text = r.filePaths.join(QLatin1Char('\n'));
        } else {
            r.text = e.value("textContent").toString();
        }
        // 标签以 JSON 数组字符串下发;缺省字段(老条目)为空
        const QString tagsJson = e.value("tags").toString();
        if (!tagsJson.isEmpty()) {
            const QJsonArray arr = QJsonDocument::fromJson(tagsJson.toUtf8()).array();
            for (const QJsonValue& t : arr) r.tags.append(t.toString());
        }
        // 待办扩展(docs/05-todo.md §2.3):两个端点(list_entries / todo_list)都以
        // **扁平条目 + todo 徽标**下发,这里一套解析通吃。
        // ⚠️ 徽标键只在 todo 条目上出现;非 todo 条目保持默认零值。
        if (r.kind == QLatin1String("todo")) {
            const QVariantMap badge = e.value("todo").toMap();
            r.todoDueAt = static_cast<qint64>(badge.value("dueAt").toDouble());
            r.todoDone = badge.value("done").toBool();
            r.todoOverdue = badge.value("overdue").toBool();
            // 备注在**顶层** noteText(todo_get/todo_list/todo_note_list 同一口径),
            // 不在 todo 徽标里 —— 徽标只放卡片正面必需的最小集(docs/05 §2.3)。
            r.todoNote = e.value("noteText").toString();
            // 提醒时刻只有 todo_list / todo_get 下发(树标里没有):编辑预填要用
            r.todoRemindAt = static_cast<qint64>(e.value("remindAt").toDouble());
        }
        rows.append(std::move(r));
    }
    const int newTotal = data.value("total").toInt();
    // 以服务端 total 为准(首屏与翻页统一更新):翻页期间条目被增删也能收敛,
    // hasMore 不会因 total 陈旧而误判。
    m_total = newTotal;
    if (append) {
        const int first = m_rows.size();
        const int count = rows.size();
        if (count > 0) {
            beginInsertRows({}, first, first + count - 1);
            m_rows += rows;
            endInsertRows();
        }
    } else {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }
    emit totalChanged();
    emit countChanged();
    emit hasMoreChanged();
    if (append) return;   // 纯翻页不刷统计:计数/标签/来源不因多看一页而变化,
                          // 每页 3 次同步 FFI 是触底加载的固定税,去掉。
    // 分类徽标与列表同源:列表重拉后同步刷新计数(无变化则不发信号)
    CoreFacade::instance().refreshCounts();
    refreshTags();   // 标签筛选条同理
    refreshSources();   // 来源筛选条(S5.1)同理
}

} // namespace nb
