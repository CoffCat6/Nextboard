// 主题化菜单分隔线:Basic 的 MenuSeparator 颜色取 palette.mid(未注入),
// 这里固定走 Theme.colors.border。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

MenuSeparator {
    id: control

    padding: 0
    verticalPadding: 4

    contentItem: Rectangle {
        implicitWidth: 160
        implicitHeight: 1
        color: Theme.colors.border
    }

    background: Item {}
}
