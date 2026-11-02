// 日志轮转验收(--log-rotate-flow):把"跨阈值 → 滚动 → 单代不累积"这条链路钉住。
//
// 为什么值得单开一条:轮转错了的表现都很**安静** —— 要么永远不滚动(磁盘被写满,
// 用户几周后才发现)、要么滚动成多代(.log.1/.log.2/…无限累积,同样失控)、要么
// 滚动时把最后一条日志弄丢(崩溃前排障最关键的那条)。这三种都不会报错,
// 只能靠断言。
//
// 断言方式:直接在**临时目录**里造文件(不碰用户真实日志),用小阈值逼迫滚动,
// 逐条验证文件存在性、内容归属与单代语义。
//
// 日志前缀 LOGROTATE。不写库、不联网、不读真实日志,无需 --data-dir。

#include "host/acceptance.hpp"

#include "host/log_rotate.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace nb {
namespace {

/// 往文件里追加一段字节(模拟日志写入)。
bool appendBytes(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    return f.write(bytes) == bytes.size();
}

QByteArray fileBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

/// 文件字节数;不存在返回 -1(便于"文件不该存在"的断言与大小断言共用)。
qint64 sizeOf(const QString& path) {
    const QFileInfo info(path);
    return info.exists() ? info.size() : -1;
}

} // namespace

void runLogRotateFlow() {
    int failures = 0;
    const auto check = [&failures](bool ok, const QString& what) {
        if (ok) {
            qInfo().noquote() << "LOGROTATE   ok:" << what;
        } else {
            ++failures;
            qWarning().noquote() << "LOGROTATE ⚠️ 失败:" << what;
        }
    };

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "LOGROTATE ⚠️ 临时目录创建失败,跳过";
        QApplication::quit();
        return;
    }
    const QString dir = tmp.path();
    const QString logPath = QDir(dir).filePath(QStringLiteral("probe.log"));
    const QString archPath = logPath + QStringLiteral(".1");

    // ① 未达阈值:不滚动(阈值 1KB,先写 100 字节)
    check(appendBytes(logPath, QByteArray(100, 'a')), "写入 100 字节");
    check(!rotateLogIfNeeded(logPath, 100, 1024), "未达阈值不滚动");
    check(QFile::exists(logPath) && !QFile::exists(archPath), "原文件仍在、无归档");

    // ② 跨阈值:滚动成单代,内容原样进归档位,原文件消失
    check(appendBytes(logPath, QByteArray(1000, 'b')), "再写 1000 字节(累计 1100)");
    check(rotateLogIfNeeded(logPath, 1100, 1024), "跨阈值触发滚动");
    check(!QFile::exists(logPath), "原日志已移走");
    check(sizeOf(archPath) == 1100, "归档位大小 = 1100(内容完整搬运)");

    // ③ 二次滚动:覆盖旧归档,不产生 .log.2(单代语义)
    check(appendBytes(logPath, QByteArray(1200, 'c')), "新文件写 1200 字节");
    check(rotateLogIfNeeded(logPath, 1200, 1024), "再次跨阈值");
    check(sizeOf(archPath) == 1200, "归档位被新内容覆盖");
    check(!QFile::exists(logPath + QStringLiteral(".2")), "**不产生第二代**(.log.2 不存在)");
    check(fileBytes(archPath) == QByteArray(1200, 'c'), "归档内容是最新一批(旧 1100 已丢弃)");

    // ④ 打开前判断:文件已超限 → 先滚动再打开(应对"上次被强杀没滚动")
    const QString prePath = QDir(dir).filePath(QStringLiteral("pre.log"));
    check(appendBytes(prePath, QByteArray(2048, 'x')), "造一个已超限的日志");
    QFile opened;
    check(openLogFileAppend(opened, prePath, 1024), "打开成功");
    check(sizeOf(prePath) == 0, "打开前已滚动,新文件从 0 开始");
    check(QFile::exists(prePath + QStringLiteral(".1")), "超限内容进归档");
    opened.close();

    // ⑤ 打开后累计起点 = 已有大小:预置 900 字节,再写 200 应触发滚动(而不是从 0 计)
    const QString accPath = QDir(dir).filePath(QStringLiteral("acc.log"));
    check(appendBytes(accPath, QByteArray(900, 'y')), "预置 900 字节(未超 1024)");
    QFile accFile;
    check(openLogFileAppend(accFile, accPath, 1024), "打开(未超限,不滚动)");
    check(sizeOf(accPath) == 900, "未超限时直接追加,不清空");
    accFile.close();
    check(rotateLogIfNeeded(accPath, 900 + 200, 1024), "累计跨阈值(900+200)滚动");
    check(sizeOf(accPath + QStringLiteral(".1")) == 900, "归档含原有 900 字节");

    // ⑥ 文件不存在:不 panic、返回 false(日志目录没建好时也不能崩)
    check(!rotateLogIfNeeded(QDir(dir).filePath(QStringLiteral("absent.log")), 999999, 1024),
          "文件不存在时返回 false 而非崩溃");

    qInfo() << "LOGROTATE DONE failures=" << failures;
    QApplication::quit();
}

} // namespace nb


