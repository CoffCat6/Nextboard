// Sciter ↔ nbcore 桥:以全局命名空间对象 Nextboard 暴露给面板 JS。
//
// JS 侧用法(主面板任意脚本):
//   const resp = await Nextboard.call("list_entries", { limit: 50, offset: 0 });
//   // resp = { ok: true, data: { items: [...], total: n } } 或 { ok:false, error:{code,message} }
//   Nextboard.copyEntry(id);          // 便捷方法
//   Nextboard.setSearch("关键词", cb) // 搜索防抖后的回调由 JS 控制,这里只透传
//
// 实现要点:
// - NextboardAsset 是 sciter::om::asset(引用计数),SciterSetGlobalAsset 交给引擎托管;
// - nbcore 的 JSON 响应 → sciter::value:统一走 JSON 字符串(CVT_JSON_LITERAL 解析),
//   避免逐字段手工组 value 的样板;
// - 图片:Nextboard.imageUrl(entryId, thumb) 返回 file:// URL,Sciter 原生加载磁盘文件。
#pragma once

#include <QString>
#include <QUrl>

#include "sciter-om.h"
#include "sciter-x-value.h"

namespace nb {

class CoreBridge;

class NextboardAsset : public sciter::om::asset<NextboardAsset> {
public:
    NextboardAsset();

    // ---- 通用调用 ----
    // call(method, reqObj) → 响应对象;reqObj 可省略
    sciter::value call(const sciter::value& method, sciter::value req);

    // ---- 便捷方法(JS 少写样板) ----
    sciter::value listEntries(int limit, int offset, sciter::value query,
                              sciter::value kind, sciter::value pinned);
    sciter::value entryCounts();
    sciter::value copyEntry(const sciter::value& id);
    sciter::value deleteEntry(const sciter::value& id);
    sciter::value setPinned(const sciter::value& id, bool pinned);
    sciter::value getSettings();
    sciter::value saveSettings(sciter::value settings);
    sciter::value translate(const sciter::value& text, sciter::value from, sciter::value to);
    sciter::value translateHistory();
    sciter::value clearTranslateHistory();
    sciter::value ocrEntry(const sciter::value& id);
    // 图片条目 → file:// URL(thumb = 列表缩略图,取 .thumb<版本>.png 缓存)
    sciter::value imageUrl(const sciter::value& id, bool thumb);
    sciter::value blobsDir();

    /// 取走并清空 native 侧积累的通知(剪贴板/设置变化)。
    /// 返回 ["clip_changed", "settings_changed", …];JS 每 1s 轮询一次。
    sciter::value takeNotifications();

    /// 引擎响应(JSON 字节)→ sciter::value;桥内所有出口共用。
    static sciter::value jsonToValue(const QByteArray& json);

    SOM_PASSPORT_BEGIN_EX(Nextboard, NextboardAsset)
        SOM_FUNCS(
            SOM_FUNC(call),
            SOM_FUNC(listEntries),
            SOM_FUNC(entryCounts),
            SOM_FUNC(copyEntry),
            SOM_FUNC(deleteEntry),
            SOM_FUNC(setPinned),
            SOM_FUNC(getSettings),
            SOM_FUNC(saveSettings),
            SOM_FUNC(translate),
            SOM_FUNC(translateHistory),
            SOM_FUNC(clearTranslateHistory),
            SOM_FUNC(ocrEntry),
            SOM_FUNC(imageUrl),
            SOM_FUNC(blobsDir),
            SOM_FUNC(takeNotifications),
        )
    SOM_PASSPORT_END

private:
    static QByteArray valueToJson(const sciter::value& v);
    sciter::value errResp(const QString& code, const QString& message);
};

/// 注册全局资产(进程生命周期一次;在引擎首窗口创建前调用)。
void installSciterBridge();

/// native 侧推送通知到桥队列(JS 轮询 takeNotifications 消费)。
void pushNativeNotification(const QString& kind);

} // namespace nb
