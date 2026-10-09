#include "core_facade.hpp"

#include "capture/capture_session.hpp"
#include "editor/editor_window.hpp"
#include "host/auto_start.hpp"
#include "host/core_bridge.hpp"
#include "host/memory_probe.hpp"
#include "pin/pin_window.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QColor>
#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QThreadPool>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <QDebug>

namespace nb {

CoreFacade& CoreFacade::instance() {
    static CoreFacade facade;
    return facade;
}

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

/// 两套 QPalette:Basic 风格控件(Button/ComboBox/SpinBox/TextArea…)跟随它取色。
/// 只改 QML 的 Theme 单例是不够的——这些控件走的是 QPalette。
QPalette buildPalette(bool dark) {
    QPalette p;
    if (dark) {
        // 与 Theme.qml colors 对齐(theme.md §8)
        p.setColor(QPalette::Window, QColor(0x0E, 0x12, 0x17));
        p.setColor(QPalette::WindowText, QColor(0xF1, 0xF4, 0xF7));
        p.setColor(QPalette::Base, QColor(0x15, 0x1A, 0x21));
        p.setColor(QPalette::AlternateBase, QColor(0x1A, 0x20, 0x28));
        p.setColor(QPalette::Button, QColor(0x1A, 0x20, 0x28));
        p.setColor(QPalette::ButtonText, QColor(0xF1, 0xF4, 0xF7));
        p.setColor(QPalette::Text, QColor(0xF1, 0xF4, 0xF7));
        // 与 Theme.qml textTertiary 对齐(暗色提亮一档保可读)
        p.setColor(QPalette::PlaceholderText, QColor(0x8D, 0x99, 0xA6));
        p.setColor(QPalette::Highlight, QColor(0x4C, 0x9A, 0xFF));
        p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipBase, QColor(0x1A, 0x20, 0x28));
        p.setColor(QPalette::ToolTipText, QColor(0xF1, 0xF4, 0xF7));
        p.setColor(QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::Link, QColor(0x4C, 0x9A, 0xFF));
    } else {
        // 与 Theme.qml colors 对齐(theme.md §7)
        p.setColor(QPalette::Window, QColor(0xF4, 0xF6, 0xF8));
        p.setColor(QPalette::WindowText, QColor(0x17, 0x20, 0x2A));
        p.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::AlternateBase, QColor(0xF8, 0xFA, 0xFC));
        p.setColor(QPalette::Button, QColor(0xF8, 0xFA, 0xFC));
        p.setColor(QPalette::ButtonText, QColor(0x17, 0x20, 0x2A));
        p.setColor(QPalette::Text, QColor(0x17, 0x20, 0x2A));
        p.setColor(QPalette::PlaceholderText, QColor(0x89, 0x94, 0xA1));
        p.setColor(QPalette::Highlight, QColor(0x3B, 0x82, 0xF6));
        p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipBase, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipText, QColor(0x17, 0x20, 0x2A));
        p.setColor(QPalette::BrightText, QColor(0x00, 0x00, 0x00));
        p.setColor(QPalette::Link, QColor(0x3B, 0x82, 0xF6));
    }
    return p;
}

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
            // 交回订阅方(TranslateWindow):打开翻译窗口、预填原文并自动翻译(截图翻译 F25)
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
    // allowEditor=false:取字链路始终直通,不弹标注编辑器。
    QTimer::singleShot(150, [] {
        startCaptureSession(CaptureIntent::Region, [](const QString& entryId) {
            if (!entryId.isEmpty())
                CoreFacade::instance().processCapture(entryId, QStringLiteral("ocr"));
        }, /*allowEditor=*/false);
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
constexpr qint64 kPinByteBudget = 96LL * 1024 * 1024;

/// 按创建顺序记录存活的贴图窗口 —— 超限时据此淘汰"最旧"的一张。
/// 用 QPointer:窗口经 WA_DeleteOnClose 销毁后自动置空,无需手工反注册。
QList<QPointer<PinWindow>>& pinRegistry() {
    static QList<QPointer<PinWindow>> list;
    return list;
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

    // 先剔除已销毁/已关闭的窗口,再按"最旧"淘汰到上限之内
    QList<QPointer<PinWindow>>& pins = pinRegistry();
    pins.removeIf([](const QPointer<PinWindow>& p) { return p.isNull() || !p->isVisible(); });
    qint64 totalBytes = 0;
    for (const QPointer<PinWindow>& pin : pins) totalBytes += pin->imageBytes();
    const qint64 incoming = content.image.sizeInBytes();
    bool evicted = false;
    // 张数或字节任一超限就继续淘汰最旧的一张;至少保留 1 张(否则永远贴不出来)
    while (!pins.isEmpty() &&
           (pins.size() >= kPinSoftLimit || totalBytes + incoming > kPinByteBudget)) {
        QPointer<PinWindow> victim = pins.takeFirst();
        if (!victim) continue;
        totalBytes -= victim->imageBytes();
        victim->close();
        evicted = true;
    }

    auto* pin = new PinWindow(content, options);
    pin->setAttribute(Qt::WA_DeleteOnClose);
    logMemory(evicted ? "pin-add(evicted)" : "pin-add");
    // 工具条「编辑」:用贴图**当前**内容(含已做的旋转)开编辑器;「完成」后走与
    // 历史条目编辑同一条通道(入库 + 写剪贴板),并把合成结果回写贴图 ——
    // 否则贴图会停留在编辑前的画面。
    QObject::connect(pin, &PinWindow::editRequested, pin, [pin](const QImage& image) {
        auto* editor = new EditorWindow(image);
        QPointer<PinWindow> guard(pin);   // 贴图可能先被关闭
        QObject::connect(editor, &EditorWindow::committed, editor,
                         [guard](const QImage& composed) {
            CoreFacade& facade = CoreFacade::instance();
            if (ingestImageAndCopy(composed).isEmpty()) {
                facade.showToast(QStringLiteral("编辑结果未入库(可能超过图片大小上限)"));
                return;
            }
            facade.showToast(QStringLiteral("已保存编辑结果"));
            facade.notifyClipChanged();   // 让主面板列表刷新出新图
            if (guard) guard->setImage(composed);
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
    const PinContent content = contentForEntry(entry, pinTextStyleFromSettings());
    if (content.image.isNull()) {
        emit toastMessage(QStringLiteral("该条目无可贴内容"));
        return;
    }
    showPinWindow(content);
}

void CoreFacade::pinEntry(const QString& id) {
    const QVariantMap resp = call("get_entry", {{"id", id}});
    if (!resp.value("ok").toBool()) {
        emit toastMessage(QStringLiteral("读取条目失败"));
        return;
    }
    const PinContent content =
        contentForEntry(resp.value("data").toMap(), pinTextStyleFromSettings());
    if (content.image.isNull()) {
        emit toastMessage(QStringLiteral("该条目无可贴内容"));
        return;
    }
    showPinWindow(content);
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
        if (ingestImageAndCopy(composed).isEmpty()) {
            emit toastMessage(QStringLiteral("编辑结果未入库(可能超过图片大小上限)"));
            return;
        }
        emit toastMessage(QStringLiteral("已保存编辑结果"));
        emit clipChanged();   // 让主面板列表刷新出新图
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

} // namespace nb
