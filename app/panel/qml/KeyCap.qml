// 快捷键帽(theme.md §37):SurfaceSecondary 底 + 1px 边框 + 4px 圆角,
// 用于状态栏/空状态里可视化展示全局快捷键。
import QtQuick
import Nextboard.panel

Rectangle {
    id: root
    required property string label
    implicitWidth: capLabel.implicitWidth + 10
    implicitHeight: 18
    radius: Theme.metrics.radiusBadge
    color: Theme.colors.surfaceSecondary
    border.color: Theme.colors.border
    border.width: Theme.metrics.borderWidth

    Text {
        id: capLabel
        anchors.centerIn: parent
        text: root.label
        font.pixelSize: Theme.metrics.fontCaption
        color: Theme.colors.textSecondary
    }
}
