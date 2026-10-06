#include "core_facade.hpp"

#include "host/core_bridge.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QThreadPool>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
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

void CoreFacade::showToast(const QString& message) {
    emit toastMessage(message);
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
    auto* job = new AsyncCall;
    job->method = "ocr_entry";
    job->req = QJsonObject{{"id", id}};
    job->setAutoDelete(true);
    job->onDone = [this](const QJsonObject& resp) {
        emit ocrFinished(jsonToVariant(resp).toMap());
        const bool ok = resp.value("ok").toBool();
        emit toastMessage(ok ? QStringLiteral("OCR 完成,已写入条目")
                             : QStringLiteral("OCR 失败:%1").arg(
                                   resp.value("error").toObject().value("message").toString()));
    };
    QThreadPool::globalInstance()->start(job);
}

} // namespace nb
