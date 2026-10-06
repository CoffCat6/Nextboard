#include "clip_entry_model.hpp"

#include "core_facade.hpp"
#include "host/core_bridge.hpp"

#include <QDateTime>
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
        case ImageUrlRole: return r.kind == QLatin1String("image") && !r.blobPath.isEmpty()
                                 ? imageUrlFor(r.id, true) : QString();
        case PreviewRole: return r.preview;
        case TimeRole: return formatTime(r.lastUsedAt);
        case SizeRole: return formatSize(r.byteSize);
        case ByteSizeRole: return r.byteSize;
        case PinnedRole: return r.pinned;
        case SourceRole: return r.source;
        case FilePathsRole: return r.filePaths;
        case BlobPathRole: return r.blobPath;
    }
    return {};
}

QHash<int, QByteArray> ClipEntryModel::roleNames() const {
    return {
        {IdRole, "entryId"},   {KindRole, "kind"},     {TextRole, "text"},
        {ImageUrlRole, "imageUrl"}, {PreviewRole, "preview"},
        {TimeRole, "timeText"}, {SizeRole, "sizeText"}, {ByteSizeRole, "byteSize"},
        {PinnedRole, "pinned"}, {SourceRole, "source"}, {FilePathsRole, "filePaths"},
        {BlobPathRole, "blobPath"},
    };
}

bool ClipEntryModel::canFetchMore(const QModelIndex& parent) const {
    return !parent.isValid() && m_rows.size() < m_total;
}

void ClipEntryModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || m_loading) return;
    m_loading = true;
    emit loadingChanged();
    // 追加下一页;query/kind/pinned 语义与首屏一致
    QVariantMap req{{"limit", m_limit}, {"offset", m_rows.size()}};
    const QString kind = (m_view == QLatin1String("pinned") || m_view == QLatin1String("all"))
                             ? QString() : m_view;
    if (!kind.isEmpty()) req.insert("kind", kind);
    if (m_view == QLatin1String("pinned")) req.insert("pinned", true);
    if (!m_query.trimmed().isEmpty()) req.insert("query", m_query);
    const QVariantMap resp = CoreFacade::instance().call("list_entries", req);
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
    if (m_debounce.isActive()) return;   // 已有待执行的刷新/查询
    m_debounce.start();
}

void ClipEntryModel::refreshNow() {
    m_debounce.stop();                    // 打断待执行的合并刷新,立即重拉
    m_query = m_pendingQuery;
    reload(m_limit);
}

void ClipEntryModel::fetchMoreRequest() {
    if (canFetchMore({})) fetchMore({});
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
    if (resp.value("ok").toBool()) reload(m_limit);
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

QString ClipEntryModel::imageUrlFor(const QString& id, bool thumb) const {
    const QVariantMap resp = CoreFacade::instance().call("get_entry", {{"id", id}});
    const QString blob = resp.value("data").toMap().value("blobPath").toString();
    if (blob.isEmpty()) return {};
    QString abs = CoreBridge::blobsDirAbs() + '/' + blob;
    if (thumb) {
        const int slash = abs.lastIndexOf('/');
        const int dot = abs.lastIndexOf('.');
        if (dot > slash) abs.insert(dot, ".thumb");
    }
    return QUrl::fromLocalFile(abs).toString();
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
    if (m_loading) { m_debounce.start(); return; }  // 进行中:稍后重试
    m_loading = true;
    emit loadingChanged();
    QVariantMap req{{"limit", m_limit}, {"offset", 0}};
    const QString kind =
        (m_view == QLatin1String("pinned") || m_view == QLatin1String("all")) ? QString() : m_view;
    if (!kind.isEmpty()) req.insert("kind", kind);
    if (m_view == QLatin1String("pinned")) req.insert("pinned", true);
    if (!m_query.trimmed().isEmpty()) req.insert("query", m_query);
    beginResetModel();
    m_rows.clear();
    m_total = 0;
    endResetModel();
    emit hasMoreChanged();
    const QVariantMap resp = CoreFacade::instance().call("list_entries", req);
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
    // 分类徽标与列表同源:列表重拉后同步刷新计数(无变化则不发信号)
    CoreFacade::instance().refreshCounts();
}

} // namespace nb
