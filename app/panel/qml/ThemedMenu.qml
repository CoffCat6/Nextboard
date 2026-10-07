// 主题化弹出菜单:Menu 是独立窗口,不继承应用调色板;Basic 默认背景取
// palette.light/midlight(本项目 QPalette 未注入这些角色),深浅主题下都不可控。
// 这里背景/边框/圆角全部走 Theme 令牌,菜单项一律用 ThemedMenuItem,
// 分隔线一律用 ThemedMenuSeparator。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Menu {
    id: control

    padding: 4

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 40
        color: Theme.colors.surface
        border.color: Theme.colors.borderStrong
        border.width: Theme.metrics.borderWidth
        radius: Theme.metrics.radiusPopup
    }
}
