// 全局主题:亮/暗两套配色,由 Core.darkTheme 驱动。
//
// settings.theme("system"/"light"/"dark")由 C++ 侧解析(含系统配色探测)并写入
// Core.darkTheme,切换后所有引用 Theme 的地方自动刷新——QML 侧不感知 system 细节。
// 注意:Basic 风格控件的配色走 QPalette,由 CoreFacade::applyTheme 一并注入,
// 只改这里是不够的。
pragma Singleton
import QtQuick
import Nextboard.Core 1.0

QtObject {
    readonly property bool dark: Core.darkTheme

    readonly property color bg:        dark ? "#1e1f24" : "#f4f5f7"
    readonly property color surface:   dark ? "#26272e" : "#ffffff"
    readonly property color surface2:  dark ? "#2d2e37" : "#eaecef"
    readonly property color border:    dark ? "#33353d" : "#d5d8de"
    readonly property color text:      dark ? "#dfe1e8" : "#1f2329"
    readonly property color muted:     dark ? "#9aa0ab" : "#6b7280"
    readonly property color accent:    dark ? "#4f8cff" : "#2f6fe4"
    readonly property color accentDim: dark ? "#3a6fd8" : "#2559b8"
    readonly property color danger:    dark ? "#e5534b" : "#d9382f"
    readonly property color pin:       dark ? "#e8b339" : "#e0a800"

    /// 强调色/固定色**之上**的文字色(标签选中态、toast、徽标)。
    /// 亮暗两套下强调色都是深蓝/深黄,白字或近黑字均可读,分开定义便于日后微调。
    readonly property color onAccent:  "#ffffff"
    readonly property color onPin:     "#222222"

    /// 来源徽标配色(capture / translate);亮色下需加深,否则浅底上看不清。
    readonly property color sourceCapture:   dark ? "#7ec2ff" : "#1a6fc4"
    readonly property color sourceTranslate: dark ? "#c9a7ff" : "#6f42c1"

    readonly property font baseFont: Qt.font({
        family: "Segoe UI, Microsoft YaHei",
        pixelSize: 13
    })
}
