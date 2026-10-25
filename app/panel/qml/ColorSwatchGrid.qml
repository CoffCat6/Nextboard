// 内联色板(36 色常用色)。
//
// ⚠️ 两点刻意的取舍,都是踩过的坑:
// 1. **不用 `QtQuick.Dialogs.ColorDialog`** —— windeployqt 不会部署该模块(QML 已
//    编译进 qrc,它扫描不到 import),运行时 `import QtQuick.Dialogs` 直接报
//    "module is not installed" → 整个设置页加载失败 → **主面板空白**。
// 2. **也不用 `Popup`** —— 设置抽屉时代外层本身是 Popup,嵌套 Popup 的定位与
//    事件派发不可靠(实测点色块没反应)。内联网格没有这个问题,而且设置页本来
//    就可滚动,就地展开比浮层更自然。
import QtQuick
import QtQuick.Layouts
import Nextboard.panel

GridLayout {
    id: grid

    /// 当前值,用于高亮已选中的色块。
    property string current: "#000000"
    signal picked(string color)

    readonly property var swatches: [
        "#000000", "#3b3b3b", "#6b6b6b", "#9a9a9a", "#c8c8c8", "#ffffff",
        "#e03131", "#f76707", "#fab009", "#40c057", "#1c7ed6", "#7048e8",
        "#ff8787", "#ffa94d", "#ffe066", "#8ce99a", "#74c0fc", "#b197fc",
        "#a61e1e", "#b35309", "#a37b00", "#2b8a3e", "#1552a3", "#4c2f9e",
        "#1e1f24", "#26272e", "#2d2e37", "#33353d", "#dfe1e8", "#9aa0ab",
        "#7ec2ff", "#c9a7ff", "#e8b339", "#e5534b", "#4f8cff", "#2f6fe4"
    ]

    columns: 6
    rowSpacing: Theme.metrics.spacingXs
    columnSpacing: Theme.metrics.spacingXs

    Repeater {
        model: grid.swatches
        delegate: Rectangle {
            readonly property bool isCurrent:
                String(modelData).toLowerCase() === grid.current.toLowerCase()
            implicitWidth: 24
            implicitHeight: 24
            radius: Theme.metrics.radiusBadge
            color: modelData
            border.width: isCurrent ? 2 : Theme.metrics.borderWidth
            border.color: isCurrent ? Theme.colors.accent : Theme.colors.borderStrong
            Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: grid.picked(modelData)
            }
        }
    }
}
