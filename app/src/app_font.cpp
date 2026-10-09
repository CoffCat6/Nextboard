#include "app_font.hpp"

#include <QApplication>
#include <QFont>
#include <QStringList>

namespace nb {
namespace {

/// UI 正文:雅黑 UI 优先。
/// 留 "Microsoft YaHei"(经典雅黑)给没有 UI 变体的老系统;
/// 留 "Noto Sans SC" 给雅黑整体缺失的极简环境。
/// ⚠️ 族名都经 QFontDatabase::families() 实测核对过(见头文件注 2)。
QStringList makeUiFamilies() {
    return {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
            QStringLiteral("Noto Sans SC")};
}

/// 等宽:Cascadia Mono 是 Windows Terminal 默认字体(Win11 自带),
/// Consolas 全 Windows 版本都有 —— 老机器上没有 Cascadia 时自动落到 Consolas。
/// 链尾补一条雅黑:即便替换表没装上,中文也不至于掉到宋体。
QStringList makeMonoFamilies() {
    return {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
            QStringLiteral("Microsoft YaHei UI")};
}

} // namespace

const QStringList& uiFontFamilies() {
    static const QStringList kFamilies = makeUiFamilies();
    return kFamilies;
}

const QStringList& monoFontFamilies() {
    static const QStringList kFamilies = makeMonoFamilies();
    return kFamilies;
}

void installFontSubstitutions() {
    // 每条语义:"请求族 X 缺字时改用 Y"。三条互相独立,顺序无关。
    //
    // Segoe UI → 雅黑:兜底。QML 的 Theme.fontFamily 已改雅黑,但工程里
    //   仍有路径会请求 Segoe(QSS、旧代码、系统默认控件),不钉住就会掉宋体。
    //   这是本项目"界面里两种中文字体"的根因修复点。
    //
    // Cascadia Mono / Consolas → 雅黑:等宽字体不含中文字形。技术字段里
    //   出现中文(中文数据目录、JSON 里的中文)时,不钉住会掉到宋体,
    //   与同一输入框里的拉丁等宽字体明显不同族。
    QFont::insertSubstitution(QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"));
    QFont::insertSubstitution(QStringLiteral("Cascadia Mono"),
                              QStringLiteral("Microsoft YaHei UI"));
    QFont::insertSubstitution(QStringLiteral("Consolas"), QStringLiteral("Microsoft YaHei UI"));
}

void installApplicationFont() {
    // 替换表先行:必须在任何 QFont 解析(含下面这个 QApplication::font()
    // 拷贝、以及后续 QQuickStyle 的控件默认字体)之前装好。
    installFontSubstitutions();

    QFont appFont = QApplication::font();
    appFont.setFamilies(uiFontFamilies());
    appFont.setPixelSize(kUiFontPixelSize);
    appFont.setStyleStrategy(QFont::PreferAntialias);
    QApplication::setFont(appFont);
}

QFont uiFont(int pixelSize) {
    QFont font;
    font.setFamilies(uiFontFamilies());
    font.setPixelSize(pixelSize);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

QFont monoFont(int pixelSize) {
    QFont font;
    font.setFamilies(monoFontFamilies());
    font.setPixelSize(pixelSize);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

} // namespace nb
