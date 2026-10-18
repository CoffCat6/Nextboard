#include "image_save.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <QWidget>

namespace nb::image_save {
namespace {

/// JPEG 质量:发聊天窗/贴文档场景的常用档,再高体积涨得快、肉眼几乎无差。
constexpr int kJpegQuality = 90;

/// WebP 有损质量:截图这类大面积纯色 + 少量文字的内容上体积约为 PNG 的一半,
/// 文字边缘仍清晰;要无损可改成 100(体积接近 PNG)。
constexpr int kWebpQuality = 85;

/// 该格式是否吃质量参数。**PNG 不吃** —— 它的第三参数是压缩等级(映射自 0~100),
/// 传高值只会让文件变大(实测同一张图 5448B -> 6270B)。
bool formatTakesQuality(const QString& format) {
    return format == QLatin1String("JPEG") || format == QLatin1String("WEBP");
}

int qualityFor(const QString& format) {
    if (format == QLatin1String("JPEG")) return kJpegQuality;
    if (format == QLatin1String("WEBP")) return kWebpQuality;
    return -1;
}

/// 把带 alpha 的图铺成不透明白底。
///
/// 用 DestinationOver(画在已有内容**下面**)而不是 SourceOver:后者会在
/// 半透明笔画上再叠一层白,把标注冲淡。
QImage flattenOntoWhite(const QImage& image) {
    QImage out(image.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.drawImage(0, 0, image);
        p.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        p.fillRect(out.rect(), Qt::white);
    }
    return out.convertToFormat(QImage::Format_RGB32);
}

} // namespace

QString dialogFilter() {
    // 顺序即默认选中项:PNG 放第一(无损、最安全)。
    // 后缀集合必须与 formatForPath 的判定一致,否则会出现选了 JPEG 却存成 PNG。
    return QStringLiteral(
        "PNG 图片 (*.png);;JPEG 图片 (*.jpg *.jpeg);;WebP 图片 (*.webp)");
}

QString formatForPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
        return QStringLiteral("JPEG");
    if (suffix == QLatin1String("webp")) return QStringLiteral("WEBP");
    // 空后缀 / 未知后缀一律按 PNG:宁可存成无损,也不要落到 Qt 的猜格式上
    // (猜不出来时 save() 直接返回 false,用户只看到写入失败却说不出为什么)。
    return QStringLiteral("PNG");
}

bool formatSupportsAlpha(const QString& format) {
    // JPEG 标准里没有 alpha 通道;Qt 的 JPEG 写入会把透明区域按黑处理。
    return format != QLatin1String("JPEG");
}

bool saveImageToFile(const QImage& image, const QString& path, QString* outError) {
    if (outError) outError->clear();
    if (image.isNull()) {
        if (outError) *outError = QStringLiteral("图像为空");
        return false;
    }
    const QString format = formatForPath(path);
    const QByteArray fmt = format.toLatin1();
    // 目标不支持 alpha 时先铺白底;支持且原图已无 alpha 时直接存,不做多余转换。
    const QImage source = formatSupportsAlpha(format) ? image : flattenOntoWhite(image);
    const int quality = qualityFor(format);
    const bool ok = formatTakesQuality(format)
        ? source.save(path, fmt.constData(), quality)
        : source.save(path, fmt.constData());
    if (!ok && outError) *outError = QStringLiteral("写入失败:%1").arg(path);
    return ok;
}

QString saveImageWithDialog(QWidget* parent, const QImage& image,
                            const QString& suggestedBaseName, QString* outError) {
    if (outError) outError->clear();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested = QDir(dir).filePath(suggestedBaseName + QStringLiteral(".png"));
    const QString path = QFileDialog::getSaveFileName(
        parent, QStringLiteral("另存为"), suggested, dialogFilter());
    if (path.isEmpty()) return QString();   // 用户取消:不是错误

    // 用户可能没写后缀(有些平台的对话框不会自动补):补 .png 保证一定能存。
    const QString finalPath =
        QFileInfo(path).suffix().isEmpty() ? path + QStringLiteral(".png") : path;
    if (!saveImageToFile(image, finalPath, outError)) return QString();
    return finalPath;
}

} // namespace nb::image_save