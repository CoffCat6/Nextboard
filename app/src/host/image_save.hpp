// 「另存为」的格式选择与落盘:PNG / JPEG / WebP 三个入口共用一份实现。
//
// 为什么抽出来:截图浮条「保存」、编辑器「另存为」、贴图「💾」原本各自写一份
// getSaveFileName + save(path,"PNG")。一旦加格式支持,那份逻辑要变成"后缀 → 格式
// → 是否铺白底 → 是否传质量参数",三处各写一遍必然漂移(比如某处忘了给 JPEG 铺白底,
// 用户就得到一张黑底图)。
//
// ⚠️ 三条容易踩错的规则(改实现前先读):
//  1. **以用户实际输入的后缀为准,不以对话框过滤器为准**。用户经常手动把
//     Nextboard-xxx.png 改成 .jpg 再回车,此时过滤器仍是 PNG,但文件必须是 JPEG。
//  2. **JPEG 没有 alpha 通道,Qt 会把透明像素写成纯黑**(实测:全透明 → 0x000000)。
//     编辑器产物是 ARGB32_Premultiplied + fill(Qt::transparent),天生带透明边角;
//     不铺白底就会得到黑块。铺底用 CompositionMode_DestinationOver(铺在内容下面,
//     不覆盖笔画)。
//  3. **质量参数只对有损格式传**。save(path, "PNG", 90) 里的 90 是压缩等级
//     而非画质,实测同一张图 PNG 默认 5448B、传 90 变 6270B —— 反而变大。
#pragma once

#include <QString>

class QImage;
class QWidget;

namespace nb::image_save {

/// 另存为对话框的过滤器串(顺序即默认选中项:PNG 在前)。
QString dialogFilter();

/// 弹出「另存为」对话框并把图像写入用户选择的文件。
///
/// - suggestedBaseName:建议文件名(**不含目录与后缀**),如 "Nextboard-20261018-163000";
/// - 返回空串表示用户取消(调用方不应提示错误);
/// - 失败时返回空串并回填 outError(调用方各自决定弹窗还是内联提示)。
///
/// 内部按**实际后缀**决定格式:.jpg/.jpeg → JPEG,.webp → WebP,其余 → PNG;
/// 用户没写后缀时补 .png。无 alpha 能力的格式会先铺白底。
QString saveImageWithDialog(QWidget* parent, const QImage& image,
                            const QString& suggestedBaseName, QString* outError = nullptr);

/// 无对话框的落盘(验收/批量导出用)。按后缀决定格式,规则与上面完全一致。
bool saveImageToFile(const QImage& image, const QString& path, QString* outError = nullptr);

/// 由路径后缀推出 Qt 格式名("PNG"/"JPEG"/"WEBP");未知后缀按 PNG 处理。
QString formatForPath(const QString& path);

/// 目标格式是否支持 alpha 通道。JPEG 系列返回 false(需铺白底)。
bool formatSupportsAlpha(const QString& format);

} // namespace nb::image_save