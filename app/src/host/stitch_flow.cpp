// 滚动长截图验收(--stitch-flow):用**构造的帧序列**跑通 append → finish 全链路。
//
// 为什么不用真屏:本流程要断言"位移算对没有、缺口记没记、会话隔离没隔离",
// 这些只与**像素内容**有关。真屏采集一次就要真的滚窗口、还要等渲染,不可重复、
// 不可并行,而且失败时说不清是算法错还是采集错。构造帧把变量收到一处。
//
// 真机采集链路由 S4 的真机验收覆盖(docs/19 §8 A4),不在这里假装。
//
// 日志前缀 STITCH STEP*。不写库、不联网、不碰注册表,只是内存里的图像运算。

#include "host/acceptance.hpp"

#include "host/core_bridge.hpp"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>

namespace nb {
namespace {

/// 造一页"每行都不同"的内容:行号编码进 R/G/B 三个通道。
///
/// ## 为什么是三个通道而不是两个
///
/// 核心的行指纹均值项是 `sum(R+G+B)`。若 B 恒为 0,两行只要 `lo+hi` 相同就撞上:
/// y=550(38+2=40)与 y=295(39+1=40)是一对实例。实测正是这个碰撞让"无重叠"
/// 用例被误判成"匹配成功 k=195"——**看起来像算法缺陷,其实是测试数据不唯一**。
/// 所以第三个通道取 `lo ^ hi`:(lo, hi, lo^hi) 对每个 y 唯一。
QImage makePage(int width, int height) {
    QImage img(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const uchar lo = static_cast<uchar>(y & 0xFF);
        const uchar hi = static_cast<uchar>((y >> 8) & 0xFF);
        const uchar tag = static_cast<uchar>(lo ^ hi);
        uchar* line = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            line[x * 4 + 0] = lo;
            line[x * 4 + 1] = hi;
            line[x * 4 + 2] = tag;
            line[x * 4 + 3] = 255;
        }
    }
    return img;
}

/// 从长页里裁出从 top 开始的 rows 行 —— 模拟"滚到某位置后的截图"。
///
/// ⚠️ 越界必须当作**测试缺陷**暴露:QImage::copy 越界时会与图像矩形求交,
///    静默返回半张图。那样"无重叠"用例会拿一张只有 50 行的图去比,
///    位移算出来是别的值,断言失败却指向算法。这里先断死。
QImage sliceRows(const QImage& page, int top, int rows) {
    if (top + rows > page.height()) {
        qWarning() << "STITCH ⚠️ 测试数据越界:请求" << top << "+" << rows
                   << "但长页只有" << page.height() << "行";
        return {};
    }
    return page.copy(0, top, page.width(), rows);
}

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        qWarning() << "STITCH ⚠️ 失败:" << what;
    } else {
        qInfo() << "STITCH   ok:" << what;
    }
}

/// QString 重载:带数值的断言要能拼进失败信息(否则失败时无从知道实得多少)。
void check(bool ok, const QString& what) { check(ok, what.toUtf8().constData()); }

/// begin 一个测试会话。
///
/// 传 nullptr 作 QScreen:本流程只验**拼接契约**(尺寸/上限/会话隔离),
/// 与"截的是哪块屏"无关;真屏采集链路由 --stitch-live-flow 覆盖。
/// 核心侧 begin 允许空显示器名(仅测试口径),生产路径一定带屏。
bool beginFor(CoreBridge& core, const QString& session, int w, int h, int maxHeight,
              qint64 maxBytes, int overlapRatio) {
    QString err;
    if (!core.stitchBegin(session, nullptr, QRect(0, 0, w, h), maxHeight, maxBytes,
                          overlapRatio, &err)) {
        qWarning() << "STITCH   begin 失败:" << err;
        return false;
    }
    return true;
}

} // namespace (匿名:本文件的构造帧工具)

/// 滚动长截图验收。声明在 acceptance.hpp 的 `nb` 命名空间里,故本函数
/// **不能**放进匿名命名空间(否则内部链接,链接期找不到)。
void runStitchFlow() {
    CoreBridge& core = CoreBridge::instance();
    if (!core.hasStitch()) {
        qWarning() << "STITCH ⚠️ 当前 nbcore.dll 没有长截图导出"
                      "(nb_stitch_begin/append/finish/abort)—— 核心版本过旧";
        QApplication::quit();
        return;
    }

    constexpr int W = 120;
    constexpr int H = 300;
    // 长页要够"滚很远":STEP5 故意跳到 550 行处,需要 550+300 都在页内
    const QImage page = makePage(W, 1000);

    // ---- ⓪ begin:固化选区(设备像素)+ 上限 ----
    const QString session = QStringLiteral("accept-stitch-1");
    check(beginFor(core, session, W, H, 30000, 128 * 1024 * 1024, 3),
          "begin 应成功(选区 120x300,无字节预算压力)");

    // ---- ① 首帧:整帧吸收 ----
    QJsonObject r1 = core.stitchAppend(session, sliceRows(page, 0, H));
    qInfo() << "STITCH STEP1" << QJsonDocument(r1).toJson(QJsonDocument::Compact);
    check(r1.value("matched").toBool(), "首帧应被吸收");
    check(r1.value("shiftPx").toInt() == H, "首帧位移 = 帧高");
    check(r1.value("totalHeight").toInt() == H, "首帧后总高 = 帧高");
    check(r1.value("reason").toString() == "ok", "首帧 reason=ok");

    // ---- ② 向下滚 100 行:只吸 100 行 ----
    QJsonObject r2 = core.stitchAppend(session, sliceRows(page, 100, H));
    qInfo() << "STITCH STEP2" << QJsonDocument(r2).toJson(QJsonDocument::Compact);
    check(r2.value("shiftPx").toInt() == 100, "位移应为 100");
    check(r2.value("totalHeight").toInt() == H + 100, "总高应为 400");

    // ---- ③ 同一帧再送一次:duplicate,高度不变 ----
    QJsonObject r3 = core.stitchAppend(session, sliceRows(page, 100, H));
    qInfo() << "STITCH STEP3" << QJsonDocument(r3).toJson(QJsonDocument::Compact);
    check(!r3.value("matched").toBool(), "重复帧不应吸收");
    check(r3.value("reason").toString() == "duplicate", "重复帧 reason=duplicate");
    check(r3.value("totalHeight").toInt() == H + 100, "重复帧不改变高度");
    check(r3.value("debug").toObject().value("k").toInt() == 0,
          "duplicate 应回传判定数值(k=0),供真机日志定位");

    // ---- ④ 跳远到无重叠处:no-overlap + 缺口记账 ----
    QJsonObject r4 = core.stitchAppend(session, sliceRows(page, 550, H));
    qInfo() << "STITCH STEP4" << QJsonDocument(r4).toJson(QJsonDocument::Compact);
    check(r4.value("reason").toString() == "no-overlap", "无重叠应明确报告");
    check(r4.value("gaps").toInt() >= 1, "无重叠要记缺口(供宿主提示漏行)");

    // ---- ⑤ 会话隔离:另一个 id 互不影响 ----
    const QString other = QStringLiteral("accept-stitch-2");
    check(beginFor(core, other, W, 80, 30000, 0, 3), "第二会话 begin 应成功");
    QJsonObject o1 = core.stitchAppend(other, sliceRows(page, 0, 80));
    qInfo() << "STITCH STEP5" << QJsonDocument(o1).toJson(QJsonDocument::Compact);
    check(o1.value("totalHeight").toInt() == 80, "另一会话应独立计数");

    // ---- ⑥ finish:取回 PNG 与元信息 ----
    QImage outPng;
    QJsonObject meta;
    const bool finished = core.stitchFinish(session, &outPng, &meta);
    qInfo() << "STITCH STEP6 ok:" << finished
            << "meta:" << QJsonDocument(meta).toJson(QJsonDocument::Compact)
            << "image:" << outPng.width() << "x" << outPng.height();
    check(finished, "finish 应成功");
    check(!outPng.isNull(), "finish 应给出可解码的 PNG");
    check(outPng.width() == W, "产物宽度 = 帧宽");
    // 首帧 300 + 第二帧 100 + 无重叠帧不吸收 + 警示条 = 400 + bands*4
    const int bands = meta.value("bands").toInt();
    check(meta.value("gaps").toInt() >= 1, "元信息应带缺口数");
    // 漏过内容后必须留痕(docs/19 §4.3:禁止静默缺行)。补条时机有两处:
    // 下一次成功吸收之前,或 finish 收尾时兜底 —— 本用例走的是后者。
    check(bands >= 1, "漏过内容后应插入警示条(docs/19 v2 §4.2)");
    check(outPng.height() == H + 100 + bands * 4,
          QStringLiteral("产物高度 = 已吸收行数 + 警示条(%1)。实得 %2")
              .arg(bands * 4)
              .arg(outPng.height())
              .toUtf8()
              .constData());

    // ---- ⑦ finish 后会话已销毁:再取一次必须失败 ----
    QImage again;
    QJsonObject meta2;
    const bool againOk = core.stitchFinish(session, &again, &meta2);
    qInfo() << "STITCH STEP7 second-finish(expect false):" << againOk;
    check(!againOk, "会话取完即弃:二次 finish 必须失败");

    // ---- ⑧ abort 幂等 ----
    core.stitchAbort(other);
    core.stitchAbort(other);
    core.stitchAbort(QStringLiteral("never-existed"));
    qInfo() << "STITCH STEP8 abort x3(应无异常)";

    // ---- ⑨ 尺寸不符必须失败(不能静默拼错) ----
    //
    // 这是 v2 的硬契约:帧宽**或帧高**与 begin 时约定的选区不符,一律报错。
    // 旧实现只查宽度,高度变化(取错屏幕/选区被改)会被"看似合理"的位移掩盖,
    // 拼出来的图缺行却毫无提示 —— 真机上就是"长图少一截但日志全绿"。
    const QString wSession = QStringLiteral("accept-stitch-w");
    check(beginFor(core, wSession, W, 50, 30000, 0, 3), "尺寸用例 begin 应成功");
    QJsonObject wOk = core.stitchAppend(wSession, makePage(W, 50));
    check(!wOk.isEmpty(), "同尺寸首帧应被接受");
    QJsonObject wBad = core.stitchAppend(wSession, makePage(W + 10, 50));
    qInfo() << "STITCH STEP9a width-change(expect empty):"
            << QJsonDocument(wBad).toJson(QJsonDocument::Compact);
    check(wBad.isEmpty(), "帧宽变化必须失败(空对象)");
    QJsonObject hBad = core.stitchAppend(wSession, makePage(W, 44));
    qInfo() << "STITCH STEP9b height-change(expect empty):"
            << QJsonDocument(hBad).toJson(QJsonDocument::Compact);
    check(hBad.isEmpty(), "帧高变化必须失败(空对象)");
    core.stitchAbort(wSession);

    // ---- ⑩ 重复 begin 必须失败(会话泄漏检测) ----
    const QString dupBegin = QStringLiteral("accept-stitch-dupbegin");
    check(beginFor(core, dupBegin, W, 40, 30000, 0, 3), "首次 begin 应成功");
    QString err;
    check(!core.stitchBegin(dupBegin, nullptr, QRect(0, 0, W, 40), 30000, 0, 3, &err),
          "重复 begin 必须失败(上次未 abort)");
    check(!err.isEmpty(), QStringLiteral("begin 失败应给可读原因: %1").arg(err).toUtf8().constData());
    core.stitchAbort(dupBegin);

    // ---- ⑪ 未 begin 就 append 必须失败 ----
    QJsonObject noBegin = core.stitchAppend(QStringLiteral("never-begun"),
                                            makePage(W, 30));
    qInfo() << "STITCH STEP11 append-without-begin(expect empty):"
            << QJsonDocument(noBegin).toJson(QJsonDocument::Compact);
    check(noBegin.isEmpty(), "没 begin 就 append 必须失败(不能隐式建会话)");

    qInfo() << "STITCH DONE failures=" << g_failures;
    QApplication::quit();
}

} // namespace nb
