#include "core_facade.hpp"
#include "../host/quick_commands.hpp"

#include "../ui_theme.hpp"

#include "capture/capture_session.hpp"
#include "editor/editor_window.hpp"
#include "host/app_paths.hpp"
#include "host/auto_paste.hpp"
#include "host/auto_start.hpp"
#include "host/core_bridge.hpp"
#include "host/memory_probe.hpp"
#include "note/note_registry.hpp"
#include "pin/pin_window.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QGuiApplication>
#include <QFileDialog>
#include <QStandardPaths>
#include <QPalette>
#include <QStyleHints>
#include <QThreadPool>
#include <QRunnable>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <QDebug>

namespace nb {

CoreFacade& CoreFacade::instance() {
    static CoreFacade facade;
    return facade;
}

// 验收专用的两个进程级开关(只在本 TU 使用,故不挂成员):
//   g_updateUrlOverride —— --update-url 注入的更新检查地址,覆盖编译期常量;
//   g_noBrowserLaunch   —— 验收模式下 openExternalUrl 只记日志,不真起浏览器。
static QString g_updateUrlOverride;
static bool g_noBrowserLaunch = false;

// 版本号由 CMake 注入(见 app/CMakeLists.txt 的 NB_APP_VERSION);独立构建时兜底。
#ifndef NB_APP_VERSION
#define NB_APP_VERSION "0.0.0"
#endif

QString CoreFacade::appVersion() const {
    return QStringLiteral(NB_APP_VERSION);
}

QString CoreFacade::coreVersion() const {
    return CoreBridge::instance().coreVersion();
}

// 更新检查地址:唯一配置点是 CMake 的 NB_UPDATE_CHECK_URL(docs/07-auto-update.md §5)。
// 独立构建(不经该 CMake)时兜底为空串 —— 行为与"发布渠道未定"完全一致:功能禁用。
#ifndef NB_UPDATE_CHECK_URL
#define NB_UPDATE_CHECK_URL ""
#endif

QString CoreFacade::updateCheckUrl() const {
    return g_updateUrlOverride.isEmpty()
               ? QStringLiteral(NB_UPDATE_CHECK_URL)
               : g_updateUrlOverride;
}

void CoreFacade::setUpdateCheckUrlOverride(const QString& url) {
    g_updateUrlOverride = url;
}

void CoreFacade::setBrowserLaunchDisabledForAcceptance(bool disabled) {
    g_noBrowserLaunch = disabled;
}

int CoreFacade::entryCount(const QString& kind) {
    const QVariantMap resp = call("entry_counts");
    if (!resp.value("ok").toBool()) return -1;
    return resp.value("data").toMap().value(kind).toInt();
}

void CoreFacade::refreshCounts() {
    const QVariantMap resp = call("entry_counts");
    const QVariantMap data =
        resp.value("ok").toBool() ? resp.value("data").toMap() : QVariantMap{};
    if (data != m_counts) {
        m_counts = data;
        emit countsChanged();
    }
}

void CoreFacade::showToast(const QString& message) {
    emit toastMessage(message);
}

namespace {

/// 解析 settings.theme("system"/"light"/"dark")为"是否暗色"。
/// system 用 Qt 的配色提示(Qt 6.5+),避免自己读注册表。
bool resolveDark(const QString& mode) {
    if (mode == QLatin1String("dark")) return true;
    if (mode == QLatin1String("light")) return false;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

/// QPalette:Basic 风格控件(Button/ComboBox/SpinBox/TextArea…)跟随它取色。
/// 只改 QML 的 Theme 单例是不够的——这些控件走的是 QPalette。
///
/// ⚠️ 取值**不再在这里写一遍**:统一由 ui::paletteFor 从 ui_theme.hpp 的令牌构建
///    (那份与 Theme.qml 的 colors 逐项对齐)。此前这里另抄了一份十六进制,
///    改主题色要改三处(Theme.qml / 这里 / 截图浮条),漏一处就是"半套主题"。
QPalette buildPalette(bool dark) { return ui::paletteFor(dark); }

} // namespace

void CoreFacade::applyTheme(const QString& mode) {
    m_themeMode = mode.isEmpty() ? QStringLiteral("system") : mode;
    const bool dark = resolveDark(m_themeMode);
    QGuiApplication::setPalette(buildPalette(dark));
    if (dark != m_darkTheme) {
        m_darkTheme = dark;
        emit themeChanged();
    }
}

void CoreFacade::refreshThemeFromSettings() {
    const QVariantMap resp = call("get_settings");
    const QString mode = resp.value("ok").toBool()
                             ? resp.value("data").toMap().value("theme").toString()
                             : QStringLiteral("system");
    applyTheme(mode);
}

void CoreFacade::refreshThemeFromSettings(const QJsonObject& known) {
    applyTheme(known.value("theme").toString(QStringLiteral("system")));
}

namespace {

/// nbcore JSON 响应 → QVariant(嵌套 object/array 递归转换)。
QVariant jsonToVariant(const QJsonValue& v) {
    if (v.isObject()) {
        QVariantMap m;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            m.insert(it.key(), jsonToVariant(it.value()));
        return m;
    }
    if (v.isArray()) {
        QVariantList l;
        for (const QJsonValue& x : v.toArray()) l.append(jsonToVariant(x));
        return l;
    }
    return v.toVariant();
}

} // namespace

QVariantMap CoreFacade::call(const QString& method, const QVariantMap& req) {
    CoreBridge& core = CoreBridge::instance();
    if (!core.ok())
        return {{"ok", false},
                {"error", QVariantMap{{"code", "NoCore"},
                                      {"message", QStringLiteral("nbcore 未初始化(请查看启动日志)")}}}};
    QJsonObject jreq;
    for (auto it = req.begin(); it != req.end(); ++it)
        jreq.insert(it.key(), QJsonValue::fromVariant(it.value()));
    const QJsonObject resp = core.call(method, jreq);
    return jsonToVariant(resp).toMap();
}

namespace {

/// 线程池任务:在 worker 线程调 nbcore,结果回 UI 线程发信号。
/// nbcore 的 nb_call 是进程级单门面;CoreBridge 内无锁(调用方都同线程)。
/// worker 里专门加一把静态互斥,防止未来其它线程再进 nb_call 撞车。
struct AsyncCall : QRunnable {
    QString method;
    QJsonObject req;
    std::function<void(const QJsonObject&)> onDone;

    void run() override {
        static QMutex coreMutex;
        QMutexLocker lock(&coreMutex);
        const QJsonObject resp = CoreBridge::instance().call(method, req);
        lock.unlock();
        QMetaObject::invokeMethod(&CoreFacade::instance(),  // 队列投递回 UI 线程
                                  [onDone = std::move(onDone), resp] { onDone(resp); },
                                  Qt::QueuedConnection);
    }
};

} // namespace

void CoreFacade::translateText(const QString& text, const QString& from, const QString& to) {
    QJsonObject req{{"text", text}};
    if (!from.isEmpty()) req.insert("from", from);
    if (!to.isEmpty()) req.insert("to", to);
    auto* job = new AsyncCall;
    job->method = "translate_text";
    job->req = std::move(req);
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit translateFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::ocrEntry(const QString& id) {
    processCapture(id, QStringLiteral("ocr"));
}

void CoreFacade::probeTranslate(const QString& provider) {
    auto* job = new AsyncCall;
    job->method = "translate_probe";
    job->req = QJsonObject{{"provider", provider}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit translateProbeFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::probeProxy() {
    auto* job = new AsyncCall;
    job->method = "proxy_probe";
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit proxyProbeFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

// ---- 更新检查(S6.4 v1;docs/07-auto-update.md)----

void CoreFacade::checkUpdate() {
    // 与 probeProxy 同一型:网络请求必须在池线程,UI 只等 updateFinished。
    // 超时/代理/解析与版本比较全在 nbcore 侧(复用 translate::http 那条已实测的路径)。
    auto* job = new AsyncCall;
    job->method = "check_update";
    job->req = QJsonObject{{"url", updateCheckUrl()}, {"currentVersion", appVersion()}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit updateFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::openExternalUrl(const QString& url) {
    const QUrl target(url.trimmed());
    // ⚠️ 两道白名单中的第二道:core 解析响应时已只留 http(s),但本方法对 QML 通用,
    //    而 openUrl 走 shell 语义 —— 给它 file: 之类会真的打开本地文件。
    //    (Qt 6.9 的 QUrl **没有** isHttp(),实测 qurl.h 只有 isRelative/isLocalFile,
    //     故这里自己比 scheme;空 scheme = 相对路径,同样拒绝。)
    const QString scheme = target.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        emit toastMessage(QStringLiteral("只允许打开 http(s) 链接,已拒绝:%1").arg(url));
        return;
    }
    if (g_noBrowserLaunch) {
        qInfo() << "ACCEPTANCE openExternalUrl(验收模式已降级,不真起浏览器):" << target.toString();
        return;
    }
    if (QDesktopServices::openUrl(target)) {
        emit toastMessage(QStringLiteral("已在浏览器中打开下载页"));
    } else {
        emit toastMessage(QStringLiteral("打开浏览器失败,可手动访问:%1").arg(target.toString()));
    }
}

// ---- 数据备份与导出(zip/Markdown 全量 I/O,秒级;必须离 UI 线程) ----

void CoreFacade::backupCreate(const QString& path) {
    auto* job = new AsyncCall;
    job->method = "backup_create";
    job->req = QJsonObject{{"path", path}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit backupFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::maybeAutoBackup() {
    // 节流:24h 内备过就不重复。空库(主库刚初始化,不足 8KB)不值得备。
    const QDir backupDir(nb::appDataDir() + QStringLiteral("/backups"));
    const QStringList stamps = backupDir.entryList({QStringLiteral("auto-*.zip")},
                                                   QDir::Files, QDir::Time);
    if (!stamps.isEmpty()) {
        const QFileInfo newest(backupDir.filePath(stamps.first()));
        if (newest.lastModified().msecsTo(QDateTime::currentDateTime())
            < 24LL * 3600 * 1000) return;
    }
    const QFileInfo dbFile(nb::appDataDir() + QStringLiteral("/clipboard.db"));
    if (!dbFile.exists() || dbFile.size() < 8 * 1024) return;

    // nbcore 的 backup_create 不建父目录,首次先补上。
    if (!backupDir.exists() && !QDir().mkpath(backupDir.absolutePath())) return;
    const QString target = backupDir.filePath(
        QStringLiteral("auto-%1.zip")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    auto* job = new AsyncCall;
    job->method = "backup_create";
    job->req = QJsonObject{{"path", target}};
    job->setAutoDelete(true);
    job->onDone = [target, backupDir](const QJsonObject& resp) {
        if (!resp.value("ok").toBool()) {
            qWarning() << "auto-backup 失败(不影响使用):"
                       << resp.value("error").toObject().value("message").toString();
            return;
        }
        // 只保留最近 3 份 auto-*(手动备份的 *.zip 不在清理范围)。
        const QStringList stamps = backupDir.entryList({QStringLiteral("auto-*.zip")},
                                                       QDir::Files, QDir::Time);
        for (int i = 3; i < stamps.size(); ++i)
            QFile::remove(backupDir.filePath(stamps.at(i)));
        qInfo().nospace() << "auto-backup 完成: " << target << " ("
                          << resp.value("data").toObject().value("zipBytes").toInt()
                          << " bytes)";
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::exportEntries(const QString& path, const QString& format) {
    auto* job = new AsyncCall;
    job->method = "export_entries";
    job->req = QJsonObject{{"path", path}, {"format", format}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit exportFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::restoreBackup(const QString& path) {
    auto* job = new AsyncCall;
    job->method = "restore_backup";
    job->req = QJsonObject{{"path", path}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit restoreFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::importBackup(const QString& path) {
    auto* job = new AsyncCall;
    job->method = "import_backup";
    job->req = QJsonObject{{"path", path}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit importFinished(jsonToVariant(resp).toMap());
    };
    QThreadPool::globalInstance()->start(job);
}

QString CoreFacade::saveFileDialog(const QString& caption, const QString& filter,
                                   const QString& defaultName) {
    // QFileDialog 放 UI 线程:它本身是原生对话框,不涉及 nbcore
    const QString selected = QFileDialog::getSaveFileName(
        nullptr, caption,
        defaultName.isEmpty() ? QString() : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/") + defaultName,
        filter);
    return selected;
}

QString CoreFacade::openFileDialog(const QString& caption, const QString& filter) {
    // 打开起点同样放 Documents(备份 zip 常见落点;上次目录由系统对话框自己记忆)
    return QFileDialog::getOpenFileName(
        nullptr, caption,
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), filter);
}

QString CoreFacade::openDirectoryDialog(const QString& caption) {
    return QFileDialog::getExistingDirectory(
        nullptr, caption,
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QFileDialog::ShowDirsOnly);
}

void CoreFacade::autoPasteIfEnabled() {
    // 同处 namespace nb,直接调(没有 auto_paste 命名空间,见头文件)
    pasteToForegroundIfEnabled();
}

bool CoreFacade::autoStartEnabled() {
    return auto_start::enabled();
}

bool CoreFacade::setAutoStart(bool enable) {
    if (!auto_start::setEnabled(enable)) {
        emit toastMessage(enable ? QStringLiteral("设置开机自启失败(注册表写入被拒绝)")
                                 : QStringLiteral("关闭开机自启失败(注册表删除被拒绝)"));
        return false;
    }
    return true;
}

void CoreFacade::processCapture(const QString& id, const QString& mode) {
    auto* job = new AsyncCall;
    job->method = "ocr_entry";
    job->req = QJsonObject{{"id", id}};
    job->setAutoDelete(true);
    job->onDone = [this, mode](const QJsonObject& resp) {
        emit ocrFinished(jsonToVariant(resp).toMap());
        if (!resp.value("ok").toBool()) {
            emit toastMessage(QStringLiteral("识别失败:%1").arg(
                resp.value("error").toObject().value("message").toString()));
            return;
        }
        const QString text = resp.value("data").toObject().value("text").toString();
        if (text.isEmpty()) {
            emit toastMessage(QStringLiteral("未识别到文字(图片可能不含可识别文本)"));
            return;
        }
        if (mode == QLatin1String("translate")) {
            // 交回订阅方(PanelWindow::showTranslate):切到面板的翻译页、预填原文
            // 并自动翻译(截图翻译 F25)
            emit translatePrefill(text);
            return;
        }
        if (mode == QLatin1String("entry")) {
            // 截图取字**入列表**:识别文本直接存为条目(source=ocr)。
            // 走与翻译「存为条目」同一条入库路径 —— 因此天然去重、入库即可被搜索。
            auto* saveJob = new AsyncCall;
            saveJob->method = "save_text_entry";
            saveJob->req = QJsonObject{{"text", text}, {"source", "ocr"}};
            saveJob->setAutoDelete(true);
            saveJob->onDone = [this, text](const QJsonObject& r) {
                emit toastMessage(r.value("ok").toBool()
                    ? QStringLiteral("已识别 %1 个字符并存入列表").arg(text.size())
                    : QStringLiteral("识别成功,但存入列表失败"));
            };
            QThreadPool::globalInstance()->start(saveJob);
            return;
        }
        // 取字:写入剪贴板供直接粘贴(对齐第一代 OCR 语义)。
        // 仍走线程池,与 nb_call 的串行化互斥保持一致。
        auto* copyJob = new AsyncCall;
        copyJob->method = "copy_text_to_clipboard";
        copyJob->req = QJsonObject{{"text", text}};
        copyJob->setAutoDelete(true);
        copyJob->onDone = [this, text](const QJsonObject& r) {
            emit toastMessage(r.value("ok").toBool()
                ? QStringLiteral("已取字 %1 个字符并复制到剪贴板").arg(text.size())
                : QStringLiteral("识别成功,但写入剪贴板失败"));
        };
        QThreadPool::globalInstance()->start(copyJob);
    };
    QThreadPool::globalInstance()->start(job);
}

void CoreFacade::captureOcr() {
    // 调用方(翻译窗口)已先收起自身;这里再等窗口真正消失,避免被拍进画面。
    QTimer::singleShot(150, [] {
        startCaptureSession(CaptureIntent::Region, [](const QString& entryId) {
            if (!entryId.isEmpty())
                CoreFacade::instance().processCapture(entryId, QStringLiteral("ocr"));
        });
    });
}

namespace {

/// 文本贴图样式取自设置(pinText*);颜色/数值非法时回退默认值。
PinTextStyle pinTextStyleFromSettings() {
    PinTextStyle style;
    const QVariantMap resp = CoreFacade::instance().call("get_settings");
    if (!resp.value("ok").toBool()) return style;
    const QVariantMap data = resp.value("data").toMap();

    const QColor background(data.value("pinTextBg").toString());
    if (background.isValid()) {
        QColor withAlpha = background;
        withAlpha.setAlphaF(qBound(0, data.value("pinTextBgOpacity").toInt(), 100) / 100.0);
        style.background = withAlpha;
    }
    const QColor textColor(data.value("pinTextColor").toString());
    if (textColor.isValid()) style.textColor = textColor;
    const int fontSize = data.value("pinTextFontSize").toInt();
    if (fontSize > 0) style.fontSize = fontSize;
    const int maxWidth = data.value("pinTextMaxWidth").toInt();
    if (maxWidth > 0) style.maxWidth = maxWidth;
    return style;
}

/// 由条目内容得到"可贴内容":图片读 blob;文本按样式渲染成位图,并保留原文
/// (工具条「复制/另存」按文本处理,而不是把渲染位图当图片存)。
PinContent contentForEntry(const QVariantMap& entry, const PinTextStyle& style) {
    PinContent content;
    content.idPrefix = entry.value("id").toString().left(8);
    const QString kind = entry.value("kind").toString();
    if (kind == QLatin1String("image")) {
        content.image = CoreBridge::loadEntryImage(entry.value("blobPath").toString());
    } else if (kind == QLatin1String("text")) {
        const QString text = entry.value("textContent").toString();
        if (!text.isEmpty()) {
            content.text = text;
            content.image = PinWindow::renderText(text, style);
        }
    }
    return content;
}

/// 贴图窗口软上限(架构 §3.1:贴图 3–10MB/张,数量软上限 10 张,超限淘汰最旧)。
constexpr int kPinSoftLimit = 10;

/// 贴图**字节**预算(架构 §3.1 的"3–10MB/张 × 10 张"上限)。
///
/// 为什么数量上限不够:贴图持有的是**全分辨率原图**,而预算按小图估的 ——
/// 一张 4K 截图 RGBA 就是 33MB,10 张 = 332MB,单这一项就能吃掉整个内存预算。
/// 故在数量之外再加字节上限,两者取先触发者。
///
/// 口径:**按"真实占用"计**(图片 + 窗口 backing store,见 PinWindow::memoryBytes),
/// 而不是只算图片 —— 软件渲染下后者只占约一半(实测 1755×1097 图:图片 7.7MB,
/// 进程增量 18.3MB/张),只算图片会让 10 张全分辨率贴图实际吃掉 ~189MB。
constexpr qint64 kPinByteBudget = 96LL * 1024 * 1024;

/// 按创建顺序记录存活的贴图窗口 —— 超限时据此淘汰"最旧"的一张。
/// 用 QPointer:窗口经 WA_DeleteOnClose 销毁后自动置空,无需手工反注册。
QList<QPointer<PinWindow>>& pinRegistry() {
    static QList<QPointer<PinWindow>> list;
    return list;
}

/// 正在被编辑器"暂隐让位"的贴图(方案 A):编辑期间窗口 hide() 但**不 close()**,
/// 编辑会话结束后按结果放回屏幕。
///
/// ⚠️ 必须与"已关闭"严格区分:淘汰前的可见性过滤会把隐藏窗口当已关闭踢出注册表,
///    那张贴图从此既不计数、也不再回表(张数上限与字节预算永久少算一张),
///    而它其实还会回到屏幕上。
QList<QPointer<PinWindow>>& parkedPinRegistry() {
    static QList<QPointer<PinWindow>> list;
    return list;
}

/// 该贴图是否处于"编辑中暂隐"状态(顺带清理已销毁的空指针)。
bool isParkedForEdit(const PinWindow* pin) {
    QList<QPointer<PinWindow>>& parked = parkedPinRegistry();
    parked.removeIf([](const QPointer<PinWindow>& p) { return p.isNull(); });
    if (!pin) return false;
    for (const QPointer<PinWindow>& p : parked)
        if (p.data() == pin) return true;
    return false;
}

/// 编辑会话开始:登记为"暂隐"并隐藏窗口。
/// 用 hide() 而不是 close():位置/缩放/不透明度/当前画面全部保留;close() 会因
/// WA_DeleteOnClose 直接销毁窗口 —— 那是"用户关闭"的语义,不是"让位"。
void parkPinForEdit(PinWindow* pin) {
    if (!pin) return;
    if (!isParkedForEdit(pin)) parkedPinRegistry().append(pin);
    pin->hide();
}

/// 编辑会话结束:把暂隐的贴图放回屏幕(方案 A 的**单一恢复点**)。
/// notice 非空时在贴图上打一条瞬时提示 —— 编辑入口下主面板往往是隐藏的,
/// 应用级 toast 看不见,贴图自身才是可靠的反馈载体。
void unparkPinAfterEdit(const QPointer<PinWindow>& pin, const QString& notice) {
    parkedPinRegistry().removeIf(
        [&pin](const QPointer<PinWindow>& p) { return p.isNull() || p.data() == pin.data(); });
    if (!pin) return;   // 贴图已被关闭/淘汰:内容仍已入库 + 写剪贴板,无需恢复
    if (!pin->isVisible()) pin->show();   // showEvent 自带取焦(ESC/双击/滚轮随后可用)
    if (!notice.isEmpty()) pin->showNotice(notice);
}

/// 把内容贴成置顶窗口;初始透明度/双击关闭取自设置。
/// 超出软上限(张数或字节)时关闭最旧的一张,并在新贴图上提示(架构 §3.1 内存预算)。
void showPinWindow(const PinContent& content) {
    if (content.image.isNull()) return;
    PinOptions options;
    const QVariantMap resp = CoreFacade::instance().call("get_settings");
    if (resp.value("ok").toBool()) {
        const QVariantMap data = resp.value("data").toMap();
        const int opacity = data.value("pinDefaultOpacity").toInt();
        if (opacity > 0) options.opacityPercent = opacity;
        options.doubleClickClose = data.value("pinDoubleclickClose").toBool();
    }

    // 先剔除已销毁/已关闭的窗口,再按"最旧"淘汰到上限之内。
    // 字节口径用 memoryBytes()(图片 + 窗口 backing store),只算图片会低估近一半。
    // ⚠️ "编辑中暂隐"的贴图不算已关闭(否则会被永久踢出注册表,见 parkedPinRegistry)。
    QList<QPointer<PinWindow>>& pins = pinRegistry();
    pins.removeIf([](const QPointer<PinWindow>& p) {
        return p.isNull() || (!p->isVisible() && !isParkedForEdit(p.data()));
    });
    qint64 totalBytes = 0;
    for (const QPointer<PinWindow>& pin : pins) totalBytes += pin->memoryBytes();
    // 待贴的窗口还没创建、拿不到真实窗口尺寸;贴图窗口尺寸 ≈ 图片尺寸 + 阴影留白
    // (见 PinWindow::logicalSizeFor),故按图片字节 ×2 估算"图片 + 后备"(与实测
    // 的 2× 关系一致)。淘汰是"最旧优先",估多估少只影响时机一张,不会误淘汰新图。
    const qint64 incoming = content.image.sizeInBytes() * 2;
    bool evicted = false;
    // 张数或字节任一超限就继续淘汰最旧的一张;至少保留 1 张(否则永远贴不出来)
    while (!pins.isEmpty() &&
           (pins.size() >= kPinSoftLimit || totalBytes + incoming > kPinByteBudget)) {
        // 挑最旧一张**非编辑中**的贴图:编辑中的贴图是用户当前的操作对象,
        // 淘汰掉它,编辑器完成时会静默不回写(guard 变空),用户只看到贴图消失。
        // ⚠️ 必须"先查表再删",不能 takeFirst 后 append 回尾 —— 全部都是编辑中时
        //    那种写法会一直转圈(死循环);查不到就直接放弃本次淘汰。
        int victimIndex = -1;
        for (int i = 0; i < pins.size(); ++i) {
            if (pins.at(i) && !isParkedForEdit(pins.at(i).data())) {
                victimIndex = i;
                break;
            }
        }
        if (victimIndex < 0) break;   // 只剩编辑中的贴图:允许暂时超出上限/预算
        QPointer<PinWindow> victim = pins.takeAt(victimIndex);
        totalBytes -= victim->memoryBytes();
        victim->close();
        evicted = true;
    }

    auto* pin = new PinWindow(content, options);
    pin->setAttribute(Qt::WA_DeleteOnClose);
    logMemory(evicted ? "pin-add(evicted)" : "pin-add");
    // 工具条「编辑」:用贴图**当前**内容(含已做的旋转)开编辑器;「完成」后走与
    // 历史条目编辑同一条通道(入库 + 写剪贴板),并把合成结果回写贴图 ——
    // 否则贴图会停留在编辑前的画面。
    //
    // 方案 A(2026-10-17 用户拍板):编辑期间贴图**暂隐让位**(hide,不 close)。
    //   为什么必须让位:①贴图是 WindowStaysOnTopHint|Tool、编辑器是普通窗口,
    //   同时存在时贴图永远压在编辑器之上且点不透,被盖住的画布区域没法编辑;
    //   ②同一内容两份可见呈现,用户分不清哪份最新;③贴图在编辑期间仍是活的可
    //   交互对象(滚轮缩放/拖动/双击关闭/ESC),误操作会直接毁掉正在编辑的宿主。
    QObject::connect(pin, &PinWindow::editRequested, pin, [pin](const QImage& image) {
        QPointer<PinWindow> guard(pin);   // 贴图可能先被关闭
        // 恢复时要打的提示(「已更新贴图」等)。结果分支与恢复不在同一时刻发生,
        // 故用共享字符串传过去(恢复只在编辑器析构时做,见下)。
        auto notice = std::make_shared<QString>();
        parkPinForEdit(pin);
        auto* editor = new EditorWindow(image);
        // 恢复用**单一恢复点**(编辑器 destroyed):完成/取消/✕/Alt+F4/「贴图」
        // 以及两个"入库失败"分支最后都会 close() → WA_DeleteOnClose 销毁编辑器,
        // 所以必然经过这里。逐出口补 show() 必漏(✕ 与失败分支原本就是漏点),
        // 而贴图是 Tool 窗口、没有任务栏入口 —— 漏一次用户就再也找不回来。
        // ⚠️ 这条依赖 EditorWindow 的 WA_DeleteOnClose(去掉它就不再触发)。
        QObject::connect(editor, &QObject::destroyed, &CoreFacade::instance(),
                         [guard, notice] { unparkPinAfterEdit(guard, *notice); });
        QObject::connect(editor, &EditorWindow::committed, editor,
                         [guard, notice](const QImage& composed) {
            CoreFacade& facade = CoreFacade::instance();
            if (ingestImageAndCopy(composed, nullptr, nullptr, QString()).isEmpty()) {
                facade.showToast(QStringLiteral("编辑结果未入库(可能超过图片大小上限)"));
                // 主面板隐藏时 toast 看不见,恢复时由贴图自己说一句
                *notice = QStringLiteral("保存失败");
                return;
            }
            facade.showToast(QStringLiteral("已保存编辑结果"));
            facade.notifyClipChanged();   // 让主面板列表刷新出新图
            if (guard) {
                guard->setImage(composed);
                *notice = QStringLiteral("已更新贴图");
            }
        });
        // ⚠️ 编辑器里的「贴图」按钮:此前**无人连接 pinRequested** —— 点了按钮
        //    编辑器 close(),贴图窗口却没建(信号飘在空中)。这个入口是框选链路
        //    下线后仅存的两个编辑器入口之一,必须接上。
        //    决策(2026-10-17,方案 A):本入口下宿主贴图只是**暂隐**,所以「贴图」
        //    = 回写原贴图 + 放回屏幕,**不再新建** —— 旧行为"新建 + 回写"会在屏幕上
        //    留下两张内容相同的贴图,而新建那张还会挤占预算、可能顺手淘汰掉别的贴图。
        QObject::connect(editor, &EditorWindow::pinRequested, editor,
                         [guard, notice](const QImage& composed) {
            CoreFacade& facade = CoreFacade::instance();
            if (ingestImageAndCopy(composed, nullptr, nullptr, QString()).isEmpty()) {
                facade.showToast(QStringLiteral("贴图失败(可能超过图片大小上限)"));
                *notice = QStringLiteral("贴图失败");
                return;
            }
            facade.notifyClipChanged();
            if (guard) {
                guard->setImage(composed);   // 原贴图同步为新画面(不再新建第二张)
                *notice = QStringLiteral("已更新贴图");
            }
        });
        editor->show();
        editor->raise();
        editor->activateWindow();
    });
    pins.append(pin);
    pin->show();
    if (evicted)
        pin->showNotice(QStringLiteral("贴图已达上限(最多 %1 张 / 约 %2 MB),最旧的一张已关闭")
                            .arg(kPinSoftLimit)
                            .arg(kPinByteBudget / (1024 * 1024)));
}

/// 贴图内容的重活放后台,完成回 UI 建窗。
///
/// ⚠️ 只有**图片条目**的磁盘解码(QImage::load,4K PNG 几十 ms)可以入池 ——
/// 文字条目的 renderText 走 uiFont()/QFontMetrics:**字体引擎不是线程安全的**
/// (QFont 解析/字体查找绑定 GUI 线程),工作线程里构造 QFont 必现闪退
/// (2026-10-26 用户实测)。文本排版本身毫秒级,留在 GUI 线程同步做。
void pinEntryAsync(const QVariantMap& entry, const PinTextStyle& style) {
    const QString kind = entry.value("kind").toString();
    if (kind != QLatin1String("image")) {
        const PinContent content = contentForEntry(entry, style);
        if (content.image.isNull()) {
            CoreFacade::instance().toastMessage(QStringLiteral("该条目无可贴内容"));
            return;
        }
        showPinWindow(content);
        return;
    }
    auto* watcher = new QFutureWatcher<QImage>(&CoreFacade::instance());
    QObject::connect(watcher, &QFutureWatcher<QImage>::finished,
                     &CoreFacade::instance(), [watcher, entry] {
        watcher->deleteLater();
        QImage image = watcher->result();
        if (image.isNull()) {
            CoreFacade::instance().toastMessage(QStringLiteral("该条目无可贴内容"));
            return;
        }
        PinContent content;
        content.idPrefix = entry.value("id").toString().left(8);
        content.image = std::move(image);
        showPinWindow(content);
    });
    const QString blobPath = entry.value("blobPath").toString();
    watcher->setFuture(QtConcurrent::run([blobPath]() -> QImage {
        return CoreBridge::loadEntryImage(blobPath);
    }));
}

} // namespace

void CoreFacade::pinLatest() {
    const QVariantMap resp = call("latest_pinnable");
    if (!resp.value("ok").toBool()) {
        emit toastMessage(QStringLiteral("取最近条目失败:%1")
                              .arg(resp.value("error").toMap().value("message").toString()));
        return;
    }
    const QVariantMap entry = resp.value("data").toMap();
    if (entry.isEmpty()) {
        emit toastMessage(QStringLiteral("没有可贴的条目"));
        return;
    }
    // 设置读取(毫秒级 FFI)留在 UI 线程;重活(解码/渲染)由 pinEntryAsync 入池。
    pinEntryAsync(entry, pinTextStyleFromSettings());
}

void CoreFacade::pinEntry(const QString& id) {
    const QVariantMap resp = call("get_entry", {{"id", id}});
    if (!resp.value("ok").toBool()) {
        emit toastMessage(QStringLiteral("读取条目失败"));
        return;
    }
    pinEntryAsync(resp.value("data").toMap(), pinTextStyleFromSettings());
}

void CoreFacade::editEntry(const QString& id) {
    const QVariantMap resp = call("get_entry", {{"id", id}});
    if (!resp.value("ok").toBool()) {
        emit toastMessage(QStringLiteral("读取条目失败"));
        return;
    }
    const QVariantMap entry = resp.value("data").toMap();
    if (entry.value("kind").toString() != QLatin1String("image")) {
        emit toastMessage(QStringLiteral("只有图片条目可以编辑"));
        return;
    }
    const QImage image = CoreBridge::loadEntryImage(entry.value("blobPath").toString());
    if (image.isNull()) {
        emit toastMessage(QStringLiteral("读取图片失败"));
        return;
    }
    auto* editor = new EditorWindow(image);
    // 与第一代一致:编辑产物走同一条入库通道(按 hash 去重),并写回剪贴板
    QObject::connect(editor, &EditorWindow::committed, editor, [this](const QImage& composed) {
        if (ingestImageAndCopy(composed, nullptr, nullptr, QString()).isEmpty()) {
            emit toastMessage(QStringLiteral("编辑结果未入库(可能超过图片大小上限)"));
            return;
        }
        emit toastMessage(QStringLiteral("已保存编辑结果"));
        emit clipChanged();   // 让主面板列表刷新出新图
    });
    // ⚠️ 「贴图」按钮:pinRequested 此前无人连接 —— 这是框选链路(唯一接过此信号的
    //    地方)下线后遗留的孤儿信号,点了按钮编辑器关闭、贴图却没出现。
    //    口径与 committed 一致:入库 + 写剪贴板,再开贴图窗口。
    QObject::connect(editor, &EditorWindow::pinRequested, editor, [this](const QImage& composed) {
        if (ingestImageAndCopy(composed, nullptr, nullptr, QString()).isEmpty()) {
            emit toastMessage(QStringLiteral("贴图失败(可能超过图片大小上限)"));
            return;
        }
        emit clipChanged();
        pinComposedImage(composed);
    });
    editor->show();
    editor->raise();
    editor->activateWindow();
}

void CoreFacade::revealEntry(const QString& id) {
    const QVariantMap resp = call("get_entry", {{"id", id}});
    if (!resp.value("ok").toBool()) {
        emit toastMessage(QStringLiteral("读取条目失败"));
        return;
    }
    const QVariantMap entry = resp.value("data").toMap();
    if (entry.value("kind").toString() != QLatin1String("file")) {
        emit toastMessage(QStringLiteral("只有文件条目可以定位文件"));
        return;
    }
    const QVariantList paths = entry.value("filePaths").toList();
    if (paths.isEmpty()) {
        emit toastMessage(QStringLiteral("该条目没有文件路径"));
        return;
    }
    // /select, 后必须接未加引号的路径(资源管理器的怪癖),含空格路径整体仍可工作
    const QString dir = QFileInfo(paths.first().toString()).absolutePath();
    const QString file = QDir::toNativeSeparators(paths.first().toString());
    if (QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,"), file})) {
        emit toastMessage(QStringLiteral("已在资源管理器中显示"));
    } else {
        // 兜底:仅打开所在文件夹
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QDir::toNativeSeparators(dir)});
    }
}

void CoreFacade::openEntry(const QString& id) {
    const QVariantMap resp = call("get_entry", {{"id", id}});
    if (!resp.value("ok").toBool()) {
        emit toastMessage(QStringLiteral("读取条目失败"));
        return;
    }
    const QVariantMap entry = resp.value("data").toMap();
    if (entry.value("kind").toString() != QLatin1String("file")) {
        emit toastMessage(QStringLiteral("只有文件条目可以打开"));
        return;
    }
    const QVariantList paths = entry.value("filePaths").toList();
    if (paths.isEmpty()) {
        emit toastMessage(QStringLiteral("该条目没有文件路径"));
        return;
    }
    // 打开全部路径(一次复制多个文件时逐个打开);单个打开失败仅提示该文件
    for (const QVariant& p : paths) {
        const QUrl url = QUrl::fromLocalFile(p.toString());
        if (!QDesktopServices::openUrl(url)) {
            emit toastMessage(QStringLiteral("打开失败:%1").arg(p.toString()));
        }
    }
    emit toastMessage(QStringLiteral("已用系统默认程序打开"));
}

// ---- 快捷指令(docs/24):转发到 QuickCommands(main.cpp 装配) ----
// 经 QMetaObject::invokeMethod 调用:本处只有 QObject*,不引 quick_commands.hpp
// (避免 panel→host 反向包含);方法都是 Q_INVOKABLE,签名在 hpp 声明。
QVariantList CoreFacade::quickActionCatalog() const {
    QVariantList out;
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "quickActionCatalog",
                                  Q_RETURN_ARG(QVariantList, out));
    return out;
}
QVariantList CoreFacade::quickSystemCatalog() const {
    QVariantList out;
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "quickSystemCatalog",
                                  Q_RETURN_ARG(QVariantList, out));
    return out;
}
QString CoreFacade::quickSystemResolve(const QString& key) const {
    QString out;
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "quickSystemResolve",
                                  Q_RETURN_ARG(QString, out),
                                  Q_ARG(QString, key));
    return out;
}
void CoreFacade::runQuickCommand(const QVariantMap& cmd) {
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "runCommand",
                                  Q_ARG(QVariantMap, cmd));
}
bool CoreFacade::runQuickAction(const QString& actionId) {
    bool ok = false;
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "runQuickAction",
                                  Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, actionId));
    return ok;
}

bool CoreFacade::quickAppTargetValid(const QString& target) {
    bool ok = false;
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "appTargetValid",
                                  Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, target));
    return ok;
}

QString CoreFacade::appIconUrl(const QString& target) {
    QString out;
    if (m_quickCommands)
        QMetaObject::invokeMethod(m_quickCommands.data(), "appIconUrl",
                                  Q_RETURN_ARG(QString, out),
                                  Q_ARG(QString, target));
    return out;
}


void CoreFacade::openTodoNote(const QString& id) {
    // 注册表负责"已开则前置 / 未开则建窗 / 上限拒绝"全部判断(避免两处各判一遍)
    NoteRegistry::instance().open(id);
}

void CoreFacade::expandAllTodoNotes() { NoteRegistry::instance().expandAll(); }
void CoreFacade::collapseAllTodoNotes() { NoteRegistry::instance().collapseAll(); }

void pinComposedImage(const QImage& image) {
    showPinWindow(PinContent{image, {}, {}});
}

} // namespace nb