// 分组间的细分隔线(theme.md §10 Border Subtle)。设置页内分节用。
import QtQuick
import QtQuick.Layouts
import Nextboard.panel

Rectangle {
    Layout.fillWidth: true
    Layout.topMargin: Theme.metrics.spacingMd
    implicitHeight: Theme.metrics.borderWidth
    color: Theme.colors.border
}
