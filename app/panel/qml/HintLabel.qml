// 说明文字:较长,是"看不清"的重灾区 —— 暗色下用 Secondary(提亮档),自动换行。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.panel

Label {
    Layout.fillWidth: true
    font.family: Theme.fontFamily
    color: Theme.colors.textSecondary
    font.pixelSize: Theme.metrics.fontCaption
    wrapMode: Text.WrapAnywhere
}
