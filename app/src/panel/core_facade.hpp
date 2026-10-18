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
    /// 分类计数(all/text/image/file/pinned)。QML 直接绑定 `Core.counts[viewId]`,
    /// 依赖 countsChanged 通知刷新;缺失键返回 undefined。
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    /// 当前是否为暗色主题。QML 的 Theme 单例绑定它,切换后全部引用点自动刷新。
    Q_PROPERTY(bool darkTheme READ darkTheme NOTIFY themeChanged)
    /// 应用版本(由 CMake 的 project(... VERSION) 注入,单一来源)。设置页「关于」显示。
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    /// nbcore 版本(形如 "0.1.0";核心未加载时为空串)。
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    /// 更新检查地址(CMake 的 `NB_UPDATE_CHECK_URL`,docs/07-auto-update.md §5)。
    /// **空串 = 功能禁用**:设置页据此把「检查更新」置灰并说明原因。
    /// ⚠️ CONSTANT 属性 —— QML 绑定时读一次就定型,所以验收的 `--update-url` 覆盖
    ///    必须在 QML 装配**之前**落到 setUpdateCheckUrlOverride(见 main.cpp)。
    Q_PROPERTY(QString updateCheckUrl READ updateCheckUrl CONSTANT)
public:
    static CoreFacade& instance();

    CoreFacade(const CoreFacade&) = delete;
    CoreFacade& operator=(const CoreFacade&) = delete;

    /// JSON 门面透传:返回 { ok, data|error }(嵌套结构转 QVariantMap/QVariantList)。
    Q_INVOKABLE QVariantMap call(const QString& method, const QVariantMap& req = {});

    /// 分类计数(entry_counts 的展平:all/text/image/file/pinned,-1 表示取不到)。
    Q_INVOKABLE int entryCount(const QString& kind);

    /// 重新拉取分类计数;有变化才发 countsChanged(避免无谓的绑定重算)。
    /// 条目增删/固定/清空、以及剪贴板变化后调用。
    void refreshCounts();

    const QVariantMap& counts() const { return m_counts; }

    // ---- 主题 ----
    /// 应用主题:mode = "system" | "light" | "dark"。
    /// system 由 C++ 侧解析系统配色(QStyleHints::colorScheme),QML 侧不感知。
    /// 同时刷新 QPalette(Basic 风格控件)与 darkTheme 属性(Theme 单例)。
    Q_INVOKABLE void applyTheme(const QString& mode);

    /// 启动时按 settings.theme 应用一次。
    void refreshThemeFromSettings();

    /// 系统配色变化时重新解析(仅 theme=system 会真正改变)。
    void reapplyTheme() { applyTheme(m_themeMode); }

    bool darkTheme() const { return m_darkTheme; }

    /// 应用版本 / nbcore 版本(设置页「关于」)。
    QString appVersion() const;
    QString coreVersion() const;

    /// 更新检查地址(未配置时为空串;空 = 功能禁用)。
    QString updateCheckUrl() const;

    /// 验收注入的更新检查地址(覆盖编译期常量)。调用时机见属性注释。
    static void setUpdateCheckUrlOverride(const QString& url);
    /// 验收模式:openExternalUrl 只记日志、不真起浏览器 —— 否则验收会弹出一个抢焦点的
    /// 浏览器窗口,既干扰桌面也没法反复跑。真实打开由发布前人工冒烟覆盖。
    static void setBrowserLaunchDisabledForAcceptance(bool disabled);

    /// UI 反馈(toast 由 QML 侧显示;C++ 各处也能发)。
    Q_INVOKABLE void showToast(const QString& message);

    /// 翻译(网络,慢):线程池执行,完成发 translateFinished。
    Q_INVOKABLE void translateText(const QString& text, const QString& from, const QString& to);

    /// 测试翻译服务(网络,慢):线程池执行,完成发 translateProbeFinished。
    /// 必须异步 —— 探测会真发一次请求,同步调用会把界面冻住整个超时时长。
    Q_INVOKABLE void probeTranslate(const QString& provider);

    /// 测试代理链路(网络,异步:线程池执行,完成发 proxyProbeFinished)。
    Q_INVOKABLE void probeProxy();

    // ---- 更新检查(S6.4 v1「检查更新 + 打开下载页」,docs/07-auto-update.md)----
    /// 检查更新(网络,异步:线程池执行,完成发 updateFinished)。
    /// 地址取 `updateCheckUrl()`:为空时 core 侧短路返回 `{configured:false}`,连请求都不发
    /// —— UI 置灰与这条短路是同一语义的两道保险。
    Q_INVOKABLE void checkUpdate();
    /// 用系统默认浏览器打开外部链接(「打开下载页」)。非 http(s) 一律拒绝。
    Q_INVOKABLE void openExternalUrl(const QString& url);

    // ---- 数据备份与导出(慢 I/O:线程池执行,完成发 backupFinished/exportFinished) ----
    /// 创建备份 zip:path 由 QFileDialog 提供,nbcore 内 checkpoint 后打包。
    Q_INVOKABLE void backupCreate(const QString& path);
    /// 导出全部条目为 Markdown 单文件(图片 base64 内嵌)。
    Q_INVOKABLE void exportEntries(const QString& path);
    /// 恢复备份(慢 I/O:线程池执行,完成发 restoreFinished):**替换**当前数据;
    /// nbcore 侧先自动给当前数据打险备份 zip(恢复源同目录)再热替换。
    Q_INVOKABLE void restoreBackup(const QString& path);
    /// 导入备份(慢 I/O:线程池执行,完成发 importFinished):**并入**当前数据;
    /// 按内容 hash 去重(重复跳过),id 冲突重铸,blob 随条目搬迁。
    Q_INVOKABLE void importBackup(const QString& path);
    /// 保存文件对话框(原生;caption/filter/defaultName)。返回选中路径,取消为空串。
    /// QML 不引 QtQuick.Dialogs(windeployqt 不部署该模块,见 SettingsDrawer 内注释),
    /// 故由 C++ 提供该入口。
    Q_INVOKABLE QString saveFileDialog(const QString& caption, const QString& filter,
                                       const QString& defaultName);
    /// 打开文件对话框(原生;选备份 zip 等)。返回选中路径,取消为空串。
    Q_INVOKABLE QString openFileDialog(const QString& caption, const QString& filter);

    /// 把当前剪贴板内容**自动粘贴**到前台窗口(仅当设置 copyAutoPaste 开启)。
    /// 调用时机:复制条目成功、面板已 hide() 之后(见 Main.qml 的 copyEntryAndAutoPaste)。
    /// 关闭该设置时本函数立即返回 —— 调用方不必自己判断设置,漏判只会少粘一次。
    Q_INVOKABLE void autoPasteIfEnabled();

    /// 开机自启当前状态(注册表 Run 键)。
    Q_INVOKABLE bool autoStartEnabled();
    /// 启用/停用开机自启。返回是否成功;失败时 toast 提示。
    Q_INVOKABLE bool setAutoStart(bool enable);

    /// OCR(慢):线程池执行,完成发 ocrFinished。等价于 processCapture(id, "ocr")。
    Q_INVOKABLE void ocrEntry(const QString& id);

    /// 截图后处理:对刚入库的截图条目做 OCR(慢,线程池)。
    ///  - mode = "ocr"       → 识别文本写入剪贴板(截图取字)
    ///  - mode = "translate" → 发 translatePrefill,QML 打开翻译抽屉预填并自动翻译
    Q_INVOKABLE void processCapture(const QString& id, const QString& mode);

    /// 截图取字入口(翻译窗口按钮):调用方先收起自身窗口 → 框选 → OCR → 文本进剪贴板。
    Q_INVOKABLE void captureOcr();

    // ---- 条目操作(主面板右键菜单) ----
    /// 一键贴图(热键):取可贴条目并贴出。口径由设置 `pinPreferImage` 决定——
    /// 默认"最近复制优先"(文本/图片按最近使用时间竞争,谁最新贴谁);开启则"图片优先、无图回退文本"。
    Q_INVOKABLE void pinLatest();
    /// 把指定条目贴到屏幕(图片直贴;文本渲染成位图)。
    Q_INVOKABLE void pinEntry(const QString& id);
    /// 编辑图片条目:用编辑器打开,「完成」后入库 + 写剪贴板(对齐第一代 edit_entry)。
    Q_INVOKABLE void editEntry(const QString& id);
    /// 定位文件:在资源管理器中打开所在文件夹并选中(仅文件条目有实际语义)。
    Q_INVOKABLE void revealEntry(const QString& id);
    /// 用系统默认程序打开文件条目(左键点击文件的默认行为,对齐第一代 open)。
    Q_INVOKABLE void openEntry(const QString& id);

    /// 请求进入面板的翻译页(空文本 = 手动输入)。订阅方(PanelWindow::showTranslate)
    /// 在 translatePrefill 里完成"切形态 + 调窗口尺寸 + 预填",因此它同时服务
    /// 「热键 / 面板按钮」与「截图翻译完成后回填」两条链路。
    Q_INVOKABLE void openTranslate() { emit translatePrefill(QString()); }

    /// 同上,但要求**居中于屏幕**打开 —— 供托盘菜单「打开翻译」用(此时鼠标常在
    /// 任务栏角落,跟随光标会把窗口顶到边缘)。其余链路(热键/面板按钮/截图翻译)
    /// 仍是跟随光标。
    Q_INVOKABLE void openTranslateCentered() { emit translatePrefillCentered(QString()); }

    /// 用指定原文进入翻译页(条目右键「翻译」入口)。
    Q_INVOKABLE void openTranslateWith(const QString& text) { emit translatePrefill(text); }

    /// 请求弹面板并切到"待办"视图(docs/05-todo.md §4.3:气泡点击链路)。
    Q_INVOKABLE void showCenteredOnTodos() { emit todosRequested(); }

    /// 请求弹面板、切"待办"视图并定位高亮指定条目
    /// (托盘「今日待办」子菜单点击某条,docs/05-todo.md §6.6)。
    Q_INVOKABLE void openTodoEntry(const QString& id) { emit todoOpenRequested(id); }

    /// 把一条待办贴到桌面(桌面便签,docs/20-todo-sticky-note.md)。
    /// 已开着则展开前置;达上限(20 张)时 toast 提示并拒绝 —— 绝不静默淘汰旧便签。
    Q_INVOKABLE void openTodoNote(const QString& id);

    /// 托盘「便签」子菜单 / 热键的兜底入口:全部展开 / 全部收起。
    Q_INVOKABLE void expandAllTodoNotes();
    Q_INVOKABLE void collapseAllTodoNotes();

    /// 请求重新注册全局快捷键(设置里改完热键后调用)。
    /// 本类不认识 Hotkeys 实例,故只发信号,由 main.cpp 接线。
    Q_INVOKABLE void reloadHotkeys() { emit hotkeysChanged(); }

signals:
    /// nbcore 通知(剪贴板/设置变化)→ QML Connections 订阅。
    void clipChanged();
    void translateFinished(QVariantMap resp);
    /// 翻译服务探测结果(provider/available/message)。
    void translateProbeFinished(QVariantMap resp);
    void proxyProbeFinished(QVariantMap resp);
    /// 更新检查完成。data:configured / hasUpdate / upToDate / latestVersion /
    /// downloadUrl / title / notes / publishedAt / checkedAtMs。
    void updateFinished(QVariantMap resp);
    /// 备份/导出完成(ok 与错误体都走这里,QML 侧统一提示)。
    void backupFinished(QVariantMap resp);
    void exportFinished(QVariantMap resp);
    /// 恢复/导入完成(同上;data 携带 entryCount/blobCount 或 imported/skipped)。
    void restoreFinished(QVariantMap resp);
    void importFinished(QVariantMap resp);
    void ocrFinished(QVariantMap resp);
    void toastMessage(const QString& message);
    void countsChanged();
    /// 快捷键设置已变更,请重新注册(见 reloadHotkeys)。
    void hotkeysChanged();
    /// 请求打开翻译窗口并预填原文(截图翻译链路 F25)。
    void translatePrefill(const QString& text);
    /// 同 translatePrefill,但要求居中显示(托盘链路)。
    void translatePrefillCentered(const QString& text);
    /// 请求弹面板并切到"待办"视图(气泡点击 / 托盘角标链路)。
    void todosRequested();
    /// 请求弹面板、切待办视图并定位高亮指定条目(托盘今日待办子菜单)。
    void todoOpenRequested(QString id);
    void themeChanged();

public:
    /// PanelWindow::nativeEvent 转发 nbcore WM_APP 通知(刷新去抖由模型侧负责)。
    void notifyClipChanged() { emit clipChanged(); }

private:
    CoreFacade() = default;
    ~CoreFacade() = default;

    QVariantMap m_counts;
    QString m_themeMode = QStringLiteral("system");
    bool m_darkTheme = true;
};

/// 把一张**已合成**的图片贴成置顶窗口(截图编辑器「贴图」用)。
/// 与条目贴图走同一套软上限/淘汰/设置读取,避免两套语义漂移。
/// 在 nb 命名空间公开 —— 编辑器(editor_window.cpp)与截图会话都可直接调。
void pinComposedImage(const QImage& image);

} // namespace nb
