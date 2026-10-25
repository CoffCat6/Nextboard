// 分组标题(§10 分组标题,Section 档字号 + 半粗)。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.panel

Label {
    font.family: Theme.fontFamily
    font.pixelSize: Theme.metrics.fontSection
    font.weight: Font.DemiBold
    color: Theme.colors.textPrimary
    Layout.topMargin: Theme.metrics.spacingXs
}
