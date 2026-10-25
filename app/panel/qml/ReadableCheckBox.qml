// 主题化勾选框:Basic 控件深色可读性覆盖(截图确认:勾选框文字过暗)。
//
// Basic 的 CheckBox 文字在深色下取调色板混合值,对比度不足;contentItem/指示器
// 全部走 Theme 令牌。原先内联在 SettingsDrawer.qml,设置页拆分(docs/22)时文件化,
// 与 ReadableComboBox.qml 同一做法。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

CheckBox {
    id: control

    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.colors.textPrimary
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.indicator ? control.indicator.width + control.spacing : 0
    }
    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: control.leftPadding
        y: control.height / 2 - height / 2
        radius: Theme.metrics.radiusBadge
        color: control.checked ? Theme.colors.accent : Theme.colors.surfaceSecondary
        border.width: Theme.metrics.borderWidth
        border.color: control.checked ? Theme.colors.accent : Theme.colors.borderStrong
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
        Text {
            visible: control.checked
            anchors.centerIn: parent
            text: "✓"
            color: Theme.colors.accentText
            font.pixelSize: 12
            font.bold: true
        }
    }
}
