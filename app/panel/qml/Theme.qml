// 全局主题(暗色,对齐原 Sciter 版设计变量)。
pragma Singleton
import QtQuick
QtObject {
    readonly property color bg:        "#1e1f24"
    readonly property color surface:   "#26272e"
    readonly property color surface2:  "#2d2e37"
    readonly property color border:    "#33353d"
    readonly property color text:      "#dfe1e8"
    readonly property color muted:     "#9aa0ab"
    readonly property color accent:    "#4f8cff"
    readonly property color accentDim: "#3a6fd8"
    readonly property color danger:    "#e5534b"
    readonly property color pin:       "#e8b339"

    readonly property font baseFont: Qt.font({
        family: "Segoe UI, Microsoft YaHei",
        pixelSize: 13
    })
}
