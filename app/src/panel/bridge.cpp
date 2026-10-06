#include "bridge.hpp"

#include "host/core_bridge.hpp"

#include <sciter-x.h>

#include <QByteArray>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QStringList>
#include <QDebug>

namespace nb {

using sciter::value;

// ---- native 通知队列(桥内部;pushNativeNotification 由 C++ 各处调用) ----
static QMutex& notifMutex() { static QMutex m; return m; }
static QStringList& notifQueue() { static QStringList q; return q; }

void pushNativeNotification(const QString& kind) {
    QMutexLocker lock(&notifMutex());
    notifQueue().append(kind);
}

namespace {

/// sciter::value → QString(统一入口,避免各处重复的 UTF-16 搬运)
QString vstr(const value& v) {
    if (v.is_undefined() || v.is_null()) return {};
    const sciter::string s = v.to_string();
    return QString::fromWCharArray(s.c_str(), int(s.length()));
}

/// QString → sciter::value
value qv(const QString& s) { return value(s.toStdWString().c_str()); }

/// sciter::value → JSON 对象
QJsonObject vobj(const value& v) {
    if (v.is_undefined() || v.is_null()) return {};
    const sciter::string s = v.to_string(CVT_JSON_LITERAL);
    const QByteArray utf8 = QString::fromWCharArray(s.c_str(), int(s.length())).toUtf8();
    return QJsonDocument::fromJson(utf8).object();
}

} // namespace

// ---- JSON ↔ value 互转(统一走 JSON 字符串,规避逐字段样板) ----

value NextboardAsset::jsonToValue(const QByteArray& jsonText) {
    // CVT_JSON_LITERAL:按 JSON 字面量解析 → value(map/array/…)
    const QString q = QString::fromUtf8(jsonText);
    return value::from_string(reinterpret_cast<const WCHAR*>(q.utf16()),
                              size_t(q.size()), CVT_JSON_LITERAL);
}

QByteArray NextboardAsset::valueToJson(const value& v) {
    const sciter::string s = v.to_string(CVT_JSON_LITERAL);
    return QString::fromWCharArray(s.c_str(), int(s.length())).toUtf8();
}

/// QJsonObject 响应 → sciter value(桥内统一出口)
value fromCoreResp(const QJsonObject& resp) {
    return NextboardAsset::jsonToValue(QJsonDocument(resp).toJson(QJsonDocument::Compact));
}

value NextboardAsset::errResp(const QString& code, const QString& message) {
    value err;
    err.set_item("code", qv(code));
    err.set_item("message", qv(message));
    value resp;
    resp.set_item("ok", value(false));
    resp.set_item("error", err);
    return resp;
}

NextboardAsset::NextboardAsset() = default;

// ---- 通用调用 ----

value NextboardAsset::call(const value& method, value req) {
    CoreBridge& core = CoreBridge::instance();
    if (!core.ok()) return errResp("NoCore", "nbcore 未初始化(请查看启动日志)");
    const QString m = vstr(method);
    if (m.isEmpty()) return errResp("BadRequest", "方法名为空");
    return fromCoreResp(core.call(m, vobj(req)));
}

// ---- 条目 ----

value NextboardAsset::listEntries(int limit, int offset, value query,
                                  value kind, value pinned) {
    QJsonObject req{{"limit", limit}, {"offset", offset}};
    const QString q = vstr(query);
    if (!q.trimmed().isEmpty()) req.insert("query", q);
    const QString k = vstr(kind);
    if (!k.isEmpty() && k != "all") req.insert("kind", k);
    if (!pinned.is_undefined() && !pinned.is_null()) req.insert("pinned", pinned.get<bool>());
    return fromCoreResp(CoreBridge::instance().call("list_entries", req));
}

value NextboardAsset::entryCounts() {
    return fromCoreResp(CoreBridge::instance().call("entry_counts"));
}

value NextboardAsset::copyEntry(const value& id) {
    const QString entryId = vstr(id);
    if (entryId.isEmpty()) return errResp("BadRequest", "缺少条目 id");
    // core 侧 copy_entry:文本直写 / 图片 PNG→CF_DIBV5 / 文件 CF_HDROP
    return fromCoreResp(CoreBridge::instance().call("copy_entry", {{"id", entryId}}));
}

value NextboardAsset::deleteEntry(const value& id) {
    const QString entryId = vstr(id);
    if (entryId.isEmpty()) return errResp("BadRequest", "缺少条目 id");
    return fromCoreResp(CoreBridge::instance().call("delete_entry", {{"id", entryId}}));
}

value NextboardAsset::setPinned(const value& id, bool pinned) {
    const QString entryId = vstr(id);
    if (entryId.isEmpty()) return errResp("BadRequest", "缺少条目 id");
    return fromCoreResp(
        CoreBridge::instance().call("set_pinned", {{"id", entryId}, {"pinned", pinned}}));
}

// ---- 设置 ----

value NextboardAsset::getSettings() {
    return fromCoreResp(CoreBridge::instance().call("get_settings"));
}

value NextboardAsset::saveSettings(value settings) {
    const QJsonObject obj = vobj(settings);
    if (obj.isEmpty()) return errResp("BadRequest", "设置对象为空");
    return fromCoreResp(CoreBridge::instance().call("save_settings", obj));
}

// ---- 翻译 ----

value NextboardAsset::translate(const value& text, value from, value to) {
    const QString t = vstr(text);
    if (t.trimmed().isEmpty()) return errResp("EmptyInput", "原文为空");
    QJsonObject req{{"text", t}};
    const QString f = vstr(from);
    if (!f.isEmpty() && f != "undefined") req.insert("from", f);
    const QString t2 = vstr(to);
    if (!t2.isEmpty() && t2 != "undefined") req.insert("to", t2);
    return fromCoreResp(CoreBridge::instance().call("translate_text", req));
}

value NextboardAsset::translateHistory() {
    return fromCoreResp(CoreBridge::instance().call("translate_history"));
}

value NextboardAsset::clearTranslateHistory() {
    return fromCoreResp(CoreBridge::instance().call("clear_translate_history"));
}

// ---- OCR ----

value NextboardAsset::ocrEntry(const value& id) {
    const QString entryId = vstr(id);
    if (entryId.isEmpty()) return errResp("BadRequest", "缺少条目 id");
    return fromCoreResp(CoreBridge::instance().call("ocr_entry", {{"id", entryId}}));
}

// ---- 图片路径 ----

value NextboardAsset::imageUrl(const value& id, bool thumb) {
    // 条目 blob 相对路径 → 绝对路径 → file:/// URL(Sciter 原生加载磁盘文件)
    const QString entryId = vstr(id);
    if (entryId.isEmpty()) return value();
    const QJsonObject resp = CoreBridge::instance().call("get_entry", {{"id", entryId}});
    const QString blob = resp.value("data").toObject().value("blobPath").toString();
    if (blob.isEmpty()) return value();
    QString abs = CoreBridge::blobsDirAbs() + "/" + blob;
    if (thumb) {
        // xxx.png → xxx.thumb.png(与 core 缩略图缓存命名一致)
        const int slash = abs.lastIndexOf('/');
        const int dot = abs.lastIndexOf('.');
        if (dot > slash) abs.insert(dot, ".thumb");
    }
    return qv(QUrl::fromLocalFile(abs).toString());
}

value NextboardAsset::blobsDir() {
    return qv(CoreBridge::blobsDirAbs());
}

// ---- native 通知管道 ----
// nbcore 的 WM_APP_* 经 C++ 侧 push 进队列;JS 轮询 takeNotifications 消费。
// (SciterEval 在 ProcX 位图实例上不可用,只能用拉模式;详见 docs/progress.md)

void pushNativeNotification(const char* kind) {
    QMutexLocker lock(&notifMutex());
    notifQueue().append(QString::fromLatin1(kind));
}

value NextboardAsset::takeNotifications() {
    QMutexLocker lock(&notifMutex());
    value arr;
    int i = 0;
    for (const QString& n : std::as_const(notifQueue())) {
        arr.set_item(i++, qv(n));
    }
    notifQueue().clear();
    return arr;
}

void installSciterBridge() {
    // 交给引擎托管引用计数;进程级一次
    static bool installed = false;
    if (installed) return;
    installed = true;
    ::SciterSetGlobalAsset(new NextboardAsset());
}

} // namespace nb
