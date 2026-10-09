// 主题化弹出菜单:Menu 是独立窗口,不继承应用调色板;Basic 默认背景取
// palette.light/midlight(本项目 QPalette 未注入这些角色),深浅主题下都不可控。
// 这里背景/边框/圆角全部走 Theme 令牌,菜单项一律用 ThemedMenuItem,
// 分隔线一律用 ThemedMenuSeparator。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Menu {
    id: control

    /// 菜单最小宽度(像素)。
    /// 背景的 implicitWidth 是菜单宽度的**下限** —— 不放开这个口子,任何
    /// 比它窄的菜单(如文本编辑右键菜单那种 2 字标签的紧凑菜单)都会被强行
    /// 撑到这个值。默认 200 保持条目右键菜单的既有观感;紧凑菜单传 0,
    /// 让宽度跟随最宽条目。
    property int minWidth: 200

    padding: 4

    background: Rectangle {
        implicitWidth: control.minWidth
        implicitHeight: 40
        color: Theme.colors.surface
        border.color: Theme.colors.borderStrong
        border.width: Theme.metrics.borderWidth
        radius: Theme.metrics.radiusPopup
    }
}
