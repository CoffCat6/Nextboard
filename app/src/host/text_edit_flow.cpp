// 文本条目编辑验收(--text-edit-flow,3.1):原地改写正文这条链路的端到端回归。
//
// 为什么值得单开一条:文本编辑的失败模式**全都是静默**的 ——
//   ① 正文改了但 preview / byte_size 没跟:列表卡片显示旧摘要、容量统计错;
//   ② 正文改了但 FTS 没跟:搜新内容搜不到,搜旧内容却命中(最容易被当成"索引坏了");
//   ③ 编辑把标签/固定/来源一起清了:用户改一个错字,条目就"变了个样";
//   ④ 空正文被接受:列表里出现一张空卡片、行高塌陷;
//   ⑤ 非文本条目被误改:图片条目的正文变成一段陌生字符串。
// 这五种都不会报错,只能靠断言钉住。前四种走 FFI 断言,第五种顺带覆盖。
//
// 日志前缀 TEXT-EDIT。会写库(播种 + 改写),故必须配 --data-dir
// (main.cpp 的门禁统一处理),结束前删掉播种的条目。

#include "host/acceptance.hpp"

#include "panel/core_facade.hpp"

#include <QApplication>
#include <QDebug>
#include <QTimer>
#include <QVariantMap>

namespace nb {
namespace {

/// 调 FFI 并返回 data 段(失败时返回空 map,由调用方断言 ok)。
///
/// ⚠️ 用 CoreFacade::call(QVariantMap) 而不是 CoreBridge::call(QJsonObject):
///    QVariantMap 可以直接写 QStringList/JSON 数组,构造请求时不用手工拼 QJsonArray。
QVariantMap callOk(const QString& method, const QVariantMap& req, bool* ok) {
    const QVariantMap resp = CoreFacade::instance().call(method, req);
    const bool good = resp.value("ok").toBool();
    if (ok) *ok = good;
    return resp.value("data").toMap();
}

/// 播种一条文本条目,返回 id;失败返回空串。
QString seedText(const QString& text, const QString& source = QStringLiteral("clipboard")) {
    bool ok = false;
    const QVariantMap data = callOk("save_text_entry",
                                    {{"text", text}, {"source", source}}, &ok);
    if (!ok) return {};
    // save_text_entry 返回 { entry: {...} };entry.id 是新条目 id
    return data.value("entry").toMap().value("id").toString();
}

/// 按 id 读回条目。
QVariantMap getEntry(const QString& id, bool* ok) {
    return callOk("get_entry", {{"id", id}}, ok);
}

} // namespace

void runTextEditFlow() {
    int failures = 0;
    const auto check = [&failures](bool ok, const QString& what) {
        if (ok) {
            qInfo().noquote() << "TEXT-EDIT   ok:" << what;
        } else {
            ++failures;
            qWarning().noquote() << "TEXT-EDIT ⚠️ 失败:" << what;
        }
    };

    // ---- ① 正常改写:正文/preview/byte_size/时间跟新,元数据保留 ----
    const QString id = seedText(QStringLiteral("original alpha body"));
    check(!id.isEmpty(), "播种文本条目成功");
    if (id.isEmpty()) {
        qWarning() << "TEXT-EDIT DONE failures=" << failures;
        QApplication::quit();
        return;
    }
    // 先打标签 + 固定,验证编辑不会把它们清掉
    CoreFacade::instance().call("set_entry_tags",
                                {{"id", id}, {"tags", QStringList{QStringLiteral("keep")}}});
    CoreFacade::instance().call("set_pinned", {{"id", id}, {"pinned", true}});

    const QVariantMap before = getEntry(id, nullptr);
    const qint64 beforeUsed = before.value("lastUsedAt").toLongLong();

    bool ok = false;
    callOk("update_entry_text", {{"id", id}, {"text", QStringLiteral("  revised bravo body  ")}},
           &ok);
    check(ok, "update_entry_text 调用成功");

    const QVariantMap after = getEntry(id, nullptr);
    check(after.value("textContent").toString() == QStringLiteral("revised bravo body"),
          "正文已改写(首尾空白被裁掉)");
    check(after.value("preview").toString() == QStringLiteral("revised bravo body"),
          "preview 跟随正文(列表卡片摘要不显示旧内容)");
    check(after.value("byteSize").toLongLong()
              == QStringLiteral("revised bravo body").toUtf8().size(),
          "byteSize 跟随正文(容量统计不错)");
    check(after.value("lastUsedAt").toLongLong() >= beforeUsed,
          "lastUsedAt 已刷新(编辑算一次使用)");
    check(after.value("pinned").toBool(), "**固定态保留**");
    check(after.value("source").toString() == QStringLiteral("clipboard"), "**来源保留**");
    // tags 在 IPC 上是 JSON 数组字符串(不是 QStringList),见 ClipEntry 的 serde 定义
    check(after.value("tags").toString().contains(QStringLiteral("keep")), "**标签保留**");

    // ---- ② FTS 跟随:新词可搜、旧词不再命中 ----
    // ⚠️ list_entries 的返回键是 `items`(不是 entries),见 ffi.rs 的分发实现。
    bool listOk = false;
    const QVariantList hitNew =
        callOk("list_entries", {{"query", QStringLiteral("bravo")}}, &listOk)
            .value("items").toList();
    bool foundNew = false;
    for (const QVariant& v : hitNew) {
        if (v.toMap().value("id").toString() == id) foundNew = true;
    }
    check(foundNew, "改后的新词能被搜到(FTS 索引已跟随)");

    const QVariantList hitOld =
        callOk("list_entries", {{"query", QStringLiteral("alpha")}}, &listOk)
            .value("items").toList();
    bool foundOld = false;
    for (const QVariant& v : hitOld) {
        if (v.toMap().value("id").toString() == id) foundOld = true;
    }
    check(!foundOld, "旧词不再命中该条目(索引没留下陈旧行)");

    // ---- ③ 空正文必须被拒,且原内容不变 ----
    ok = true;
    callOk("update_entry_text", {{"id", id}, {"text", QStringLiteral("   ")}}, &ok);
    check(!ok, "纯空白正文被拒绝");
    check(getEntry(id, nullptr).value("textContent").toString()
              == QStringLiteral("revised bravo body"),
          "被拒后原正文保持不变");

    // ---- ④ 非文本条目不可改写 ----
    // 用不存在的 id 覆盖"条目不存在"分支(图片条目需要真实 blob,这里不造;
    // store 层的类型保护由 update_text_rejects_non_text_kinds 单测覆盖)。
    ok = true;
    callOk("update_entry_text", {{"id", QStringLiteral("no-such-entry")}, {"text", "x"}}, &ok);
    check(!ok, "不存在的条目被拒绝");

    // 清理播种数据
    CoreFacade::instance().call("delete_entry", {{"id", id}});

    qInfo() << "TEXT-EDIT DONE failures=" << failures;
    QApplication::quit();
}

} // namespace nb

