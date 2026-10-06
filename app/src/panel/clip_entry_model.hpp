// 剪贴板历史列表模型:驱动主面板 ListView 的 QAbstractListModel。
//
// 职责:
// - 从 nbcore list_entries 拉取(分类 kind / 固定 pinned / 搜索 query 过滤);
// - 分页:首次 limit,底部触底 loadMore(overlay 的分页语义由 QML 侧触发);
// - 通知驱动刷新:nbcore 的 clip_changed → refresh()(内部 200ms 去抖,
//   取代 Sciter 版 JS 的 1s 轮询,事件延迟降到消息级);
// - 搜索防抖:setQuery 写入 pending,200ms 后真正拉取;
// - 图片条目经 role 提供 file:// URL(QQuickImageProvider 之外的轻量路径,
//   直接复用磁盘缩略图缓存,行为对齐原 Sciter 版 imageUrl)。
#pragma once

#include <QAbstractListModel>
#include <QVariantMap>
#include <QStringList>
#include <QTimer>

namespace nb {

class ClipEntryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int total READ total NOTIFY totalChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        KindRole,
        TextRole,        // 文本内容 / 文件路径列表换行拼接 / 空
        ImageUrlRole,    // 图片条目的缩略图 file:// URL,其余空串
        PreviewRole,
        TimeRole,        // "2026-10-06 10:37"
        SizeRole,        // "41 B" / "1.2 KB"
        ByteSizeRole,
        PinnedRole,
        SourceRole,
        FilePathsRole,   // QStringList
        BlobPathRole,
    };
    Q_ENUM(Roles)

    explicit ClipEntryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    int total() const { return m_total; }
    bool loading() const { return m_loading; }
    bool hasMore() const { return m_rows.size() < m_total; }
    const QString& errorText() const { return m_errorText; }

    // ---- QML 接口 ----
    Q_INVOKABLE void setView(const QString& view);      // all|text|image|file|pinned
    Q_INVOKABLE void setQuery(const QString& query);    // 搜索(防抖 200ms)
    Q_INVOKABLE void refresh();                         // 通知驱动(去抖 200ms)
    Q_INVOKABLE void refreshNow();                      // 立即重拉(打开面板/翻页后)
    Q_INVOKABLE void fetchMoreRequest();                // 触底预取(内部防重入)
    Q_INVOKABLE QVariantMap copyEntry(const QString& id);
    Q_INVOKABLE QVariantMap deleteEntry(const QString& id);
    Q_INVOKABLE void togglePin(const QString& id, bool pinned);

signals:
    void totalChanged();
    void countChanged();
    void loadingChanged();
    void hasMoreChanged();
    void errorTextChanged();
    void toast(const QString& message);

private:
    void reload(int limit);            // 全量重拉(视图/搜索/刷新)
    void applyPage(const QVariantMap& resp, bool append);
    QString imageUrlFor(const QString& id, bool thumb) const;
    static QString formatTime(qint64 ms);
    static QString formatSize(qint64 bytes);

    struct Row {
        QString id;
        QString kind;
        QString text;
        QString preview;
        QString blobPath;
        QStringList filePaths;
        QString source;
        qint64 lastUsedAt = 0;
        qint64 byteSize = 0;
        bool pinned = false;
    };
    QVector<Row> m_rows;
    QString m_view = "all";
    QString m_query;
    QString m_pendingQuery;
    QString m_errorText;
    int m_total = 0;
    int m_limit = 50;
    bool m_loading = false;
    QTimer m_debounce;   // 刷新/搜索共用:200ms 去抖
};

} // namespace nb
