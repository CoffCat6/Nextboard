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
    /// 全部标签及条目数([{tag, count}]),驱动面板的标签筛选条(S5.2)。
    /// ⚠️ 属性名不能叫 `tags`:模型另有一个**角色**名为 tags(条目自身标签),
    /// 属性与角色同名会让委托里的 `tags` 取到统计值而不是该条目的标签。
    Q_PROPERTY(QVariantList tagStats READ tagStats NOTIFY tagStatsChanged)
    /// 各来源及条目数([{source, count}]),驱动面板的来源筛选条(S5.1)。
    /// 与 tagStats 同一口径:由数据决定有哪些来源,计数为 0 的不出现。
    Q_PROPERTY(QVariantList sourceStats READ sourceStats NOTIFY sourceStatsChanged)
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
        TagsRole,        // QStringList(条目自身的标签)
        TodoDueRole,     // 待办:截止时刻毫秒;非 todo / 无截止 = 0
        TodoRemindRole,  // 待办:提醒时刻毫秒(编辑预填/推迟用);无 = 0
        TodoDoneRole,    // 待办:是否已完成(划线归档)
        TodoOverdueRole, // 待办:未完成且已过截止
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
    const QVariantList& tagStats() const { return m_tags; }
    const QVariantList& sourceStats() const { return m_sources; }

    // ---- QML 接口 ----
    Q_INVOKABLE void setView(const QString& view);      // all|text|image|file|pinned|todo
    Q_INVOKABLE void setQuery(const QString& query);    // 搜索(防抖 200ms)
    Q_INVOKABLE void refresh();                         // 通知驱动(去抖 200ms)
    Q_INVOKABLE void refreshNow();                      // 立即重拉(打开面板/翻页后)
    Q_INVOKABLE void fetchMoreRequest();                // 触底预取(内部防重入)
    Q_INVOKABLE QVariantMap copyEntry(const QString& id);
    /// 复制当前视图(含分页/过滤)第 index 条(Ctrl+1..9 序号复制,第一代 F2 语义)。
    /// 越界返回 ok=false;调用方据 ok 决定是否隐藏面板。
    Q_INVOKABLE QVariantMap copyByIndex(int index);
    Q_INVOKABLE QVariantMap deleteEntry(const QString& id);
    Q_INVOKABLE void togglePin(const QString& id, bool pinned);
    /// 标签筛选(S5.2):空串 = 不筛选;与分类/搜索可叠加。
    Q_INVOKABLE void setTag(const QString& tag);
    /// 来源筛选(S5.1):clipboard/capture/translate/ocr/todo;空串 = 不筛选。
    /// 与卡片上的来源徽标同口径;待办视图忽略它(待办来源恒为 todo)。
    Q_INVOKABLE void setSource(const QString& source);
    /// 覆盖设置条目的标签(S5.2);成功后重拉列表与标签统计。
    Q_INVOKABLE void saveTags(const QString& id, const QStringList& tags);
    /// 重命名标签(S5.2):所有带该标签的条目就地替换,单事务完成。
    /// to 为空 = 删除该标签;返回 { ok, changed };成功后重拉并刷新标签统计。
    Q_INVOKABLE QVariantMap renameTag(const QString& from, const QString& to);
    /// 批量给多条条目**追加**标签(S5.2):是"加"不是"覆盖"。
    /// 返回 { ok, changed };成功后重拉列表与标签统计。
    Q_INVOKABLE QVariantMap addTagsBulk(const QStringList& ids, const QStringList& tags);
    /// 批量删除(条目多选):单事务(含 todo_meta 级联);失效 id 静默跳过。
    /// 返回 { ok, deleted: [id...] };成功后重拉。
    Q_INVOKABLE QVariantMap deleteBulk(const QStringList& ids);
    /// 批量固定/取消固定(条目多选)。返回 { ok, changed };成功后重拉。
    Q_INVOKABLE QVariantMap setPinnedBulk(const QStringList& ids, bool pinned);
    /// 批量完成/取消待办(条目多选):只对 todo 条目生效,返回 { ok, changed }。
    /// 成功后重拉(待办计数/分组随之刷新)。
    Q_INVOKABLE QVariantMap todoSetDoneBulk(const QStringList& ids, bool done);
    /// 待办完成态切换(docs/05-todo.md §5):成功后重拉列表并刷新计数。
    Q_INVOKABLE void todoSetDone(const QString& id, bool done);
    /// 只更新待办时刻(times 键:dueAt/remindAt/notifyAt,毫秒;null = 清空该时刻)。
    /// 推迟/顺延用 —— 不动正文与标签。成功后重拉列表。
    Q_INVOKABLE void todoUpdateTimes(const QString& id, const QVariantMap& times);
    /// 新建/编辑待办:id 为空 = 新建,否则编辑;成功后重拉并返回 {ok, id?}。
    /// ⚠️ 编辑时 tags **总是**下发(空数组 = 清空标签),否则"删光标签保存"会静默无效。
    Q_INVOKABLE QVariantMap todoSave(const QString& id, const QString& text,
                                     const QVariantMap& times, const QStringList& tags);
    /// 待办编辑预填:{text, dueAt, remindAt, notifyAt}(毫秒,无则 0);
    /// 非 todo / 不存在返回空 map(QML 侧判空)。
    Q_INVOKABLE QVariantMap todoMeta(const QString& id);
    /// 条目 id → 当前行号(托盘「今日待办」点击定位用);不在当前行集返回 -1。
    /// ⚠️ 只查**已加载**的行(首屏 limit 50):托盘子菜单展示的条目取自同一
    ///    排序的前 N 条,必在首屏内;超出的场景由 QML 侧退化处理(不定位)。
    Q_INVOKABLE int indexOfId(const QString& id) const;
    /// 行号 → 条目 id(多选范围选择用);越界返回空串。
    Q_INVOKABLE QString idAt(int row) const;
    /// 条目 id → 类型(多选工具条"选中集里是否有待办"判定);不存在返回空串。
    Q_INVOKABLE QString kindOfId(const QString& id) const;
    /// 条目 id → 是否已固定(多选工具条「固定/取消固定」文案切换)。
    /// ⚠️ 只查**已加载**的行:多选集合本就来自已加载行,查不到视为未固定。
    Q_INVOKABLE bool isPinned(const QString& id) const;
    /// 第 row 行的**待办分组键**(QML 智能分组用):overdue/today/future/none/done;
    /// 非待办行或越界返回空串。
    /// ⚠️ 分组判定放 C++ 而不是 QML:标题是否显示 = "本行键 != 上一行键",
    ///    QML 逐行算会与委托复用状态纠缠(复用会跨行串味),这里按行号现算,
    ///    与 core §5 的排序口径同源(逾期>今日>未来>无期限,归档垫底)。
    Q_INVOKABLE QString rowGroupKey(int row) const;
    /// 清空已完成的待办(归档清理)。
    Q_INVOKABLE void todoClearDone();
    /// 面板**隐藏**时调用:清空全部行 —— QML 侧委托随之销毁,图片纹理一并释放;
    /// 期间拒绝一切刷新,避免隐藏时又被剪贴板通知填满(内存收口 S6.1/S6.3)。
    Q_INVOKABLE void release();
    /// 面板**显示**时调用:解除拒绝并立即重拉(列表在显示前填好,首帧不留空白)。
    /// 若此前处于 freeze() 且冻结期无变化,则**只解冻、不重拉**(避免销毁重建委托)。
    Q_INVOKABLE void acquire();
    /// 冻结:保留已加载的行(隐藏态不再销毁 QML 委托),仅拦刷新。
    /// 与 release() 的区别:不动 m_rows —— 隐藏→显示无委托重建 churn。
    /// 实测依据:显示态 122MB < 隐藏态 137MB,且首个"隐藏→显示"循环留下 +42MB
    /// 不可回落的堆增长(销毁 38 张复杂 Card 的 churn 撑大私有堆)。
    /// ⚠️ 冻结期收到刷新请求会置脏,acquire() 时补一次重载(保证"呼出即最新")。
    Q_INVOKABLE void freeze();

signals:
    void totalChanged();
    void countChanged();
    void loadingChanged();
    void hasMoreChanged();
    void errorTextChanged();
    void tagStatsChanged();
    void sourceStatsChanged();
    void toast(const QString& message);

private:
    void reload(int limit);            // 全量重拉(视图/搜索/刷新)
    void applyPage(const QVariantMap& resp, bool append);
    /// 组装一次列表请求(query/tag/source/kind/pinned 与待办 done 口径都在这里统一)。
    /// ⚠️ reload 与 fetchMore 必须共用本函数:两处各写一份曾导致翻页与首屏口径漂移。
    QVariantMap buildListRequest(int limit, int offset) const;
    /// 重新拉取标签统计(与列表同源:applyPage 后刷新)。
    void refreshTags();
    /// 重新拉取来源统计(S5.1 来源筛选条,与列表同源刷新)。
    void refreshSources();
    /// 由 blob 路径拼出 file:// URL(thumb=true 时取同目录 .thumb<版本>.png 缩略图,
    /// 命名规则见 CoreBridge::thumbnailPathFor)。
    /// 静态且只依赖入参:列表重绘时**不得**再走同步 FFI 查询。
    static QString imageUrlForBlob(const QString& blobPath, bool thumb);
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
        QStringList tags;      // 条目自身标签(S5.2)
        qint64 lastUsedAt = 0;
        qint64 byteSize = 0;
        bool pinned = false;
        // 待办扩展(docs/05-todo.md §2.3):list_entries / todo_list 对 todo 条目下发;
        // 非 todo 条目恒为默认值。
        qint64 todoDueAt = 0;     // 0 = 无截止
        qint64 todoRemindAt = 0;  // 0 = 无提醒
        bool todoDone = false;
        bool todoOverdue = false;
    };
    QVector<Row> m_rows;
    QString m_view = "all";
    QString m_query;
    QString m_pendingQuery;
    QString m_tag;          // 标签筛选(空 = 不筛选)
    QString m_source;       // 来源筛选(空 = 不筛选,S5.1)
    QVariantList m_tags;    // [{tag, count}]
    QVariantList m_sources; // [{source, count}]
    QString m_errorText;
    int m_total = 0;
    int m_limit = 50;
    bool m_loading = false;
    /// 已释放(面板隐藏):行已清空且拒绝刷新,直到 acquire()。
    bool m_released = false;
    /// 已冻结(面板隐藏但保留行):拒绝刷新但**不清行**,直到 acquire()。
    bool m_frozen = false;
    /// 冻结期间收到过刷新请求(剪贴板/设置变化)→ acquire() 必须重载一次。
    bool m_dirtyWhileFrozen = false;
    /// 是否成功装载过一次列表。⚠️ acquire() 判"行是否仍然有效"时必须带上它:
    /// 首次显示时 m_rows 本就是空的(从未装载),只看 released/frozen 会误判为
    /// "冻结期无变化"而跳过装载 → 面板空白。
    bool m_loadedOnce = false;
    QTimer m_debounce;   // 刷新/搜索共用:200ms 去抖
};

} // namespace nb
