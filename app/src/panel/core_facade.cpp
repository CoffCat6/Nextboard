#include "core_facade.hpp"

#include "capture/capture_session.hpp"
#include "host/core_bridge.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QThreadPool>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <functional>
#include <QDebug>

namespace nb {

CoreFacade& CoreFacade::instance() {
    static CoreFacade facade;
    return facade;
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
        p.setColor(QPalette::Window, QColor(0x1E, 0x1F, 0x24));
        p.setColor(QPalette::WindowText, QColor(0xDF, 0xE1, 0xE8));
        p.setColor(QPalette::Base, QColor(0x26, 0x27, 0x2E));
        p.setColor(QPalette::AlternateBase, QColor(0x2D, 0x2E, 0x37));
        p.setColor(QPalette::Button, QColor(0x2D, 0x2E, 0x37));
        p.setColor(QPalette::ButtonText, QColor(0xDF, 0xE1, 0xE8));
        p.setColor(QPalette::Text, QColor(0xDF, 0xE1, 0xE8));
        p.setColor(QPalette::PlaceholderText, QColor(0x9A, 0xA0, 0xAB));
        p.setColor(QPalette::Highlight, QColor(0x4F, 0x8C, 0xFF));
        p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipBase, QColor(0x2D, 0x2E, 0x37));
        p.setColor(QPalette::ToolTipText, QColor(0xDF, 0xE1, 0xE8));
        p.setColor(QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::Link, QColor(0x4F, 0x8C, 0xFF));
    } else {
        p.setColor(QPalette::Window, QColor(0xF4, 0xF5, 0xF7));
        p.setColor(QPalette::WindowText, QColor(0x1F, 0x23, 0x29));
        p.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::AlternateBase, QColor(0xEA, 0xEC, 0xEF));
        p.setColor(QPalette::Button, QColor(0xEA, 0xEC, 0xEF));
        p.setColor(QPalette::ButtonText, QColor(0x1F, 0x23, 0x29));
        p.setColor(QPalette::Text, QColor(0x1F, 0x23, 0x29));
        p.setColor(QPalette::PlaceholderText, QColor(0x8B, 0x91, 0x99));
        p.setColor(QPalette::Highlight, QColor(0x2F, 0x6F, 0xE4));
        p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipBase, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::ToolTipText, QColor(0x1F, 0x23, 0x29));
        p.setColor(QPalette::BrightText, QColor(0x00, 0x00, 0x00));
        p.setColor(QPalette::Link, QColor(0x2F, 0x6F, 0xE4));
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
            // 交回 QML:打开翻译抽屉、预填原文并自动翻译(截图翻译 F25)
            emit translatePrefill(text);
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
    // 调用方(翻译抽屉)已先收起面板;这里再等窗口真正消失,避免面板被拍进画面。
    QTimer::singleShot(150, [] {
        startCaptureSession(CaptureIntent::Region, [](const QString& entryId) {
            if (!entryId.isEmpty())
                CoreFacade::instance().processCapture(entryId, QStringLiteral("ocr"));
        });
    });
}

} // namespace nb
