// 主题化菜单项:悬停底色 = accentSoft(内缩 4px + 圆角,与卡片悬停同族),
// 文字 hover 转 accent;danger 项(删除)恒用 error 色,不随 hover 变化。
// implicitWidth 180 保底,让各行等宽(否则悬停底色只铺到文字宽度、右边留空)。
// ⚠️ 不要声明 text/icon/highlighted 等基类 FINAL 属性(会整体加载失败);
//    danger 是自定义名,安全。文字缩进走控件自身 leftPadding(Text 无 padding)。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

MenuItem {
    id: control

    /// 危险操作(删除):文字恒为 error 色。
    property bool danger: false

    /// 紧凑模式:用于文本编辑右键菜单这类"小而密"的菜单。
    ///   · 高度 30 → 24
    ///   · 字号降一档:Secondary(13) → Caption(12),与界面 Caption 档、
    ///     托盘菜单(12px)取同一基准
    ///   · 左右内边距收紧:10/18 → 12/12
    ///   · 放开 180px 的最小宽度 —— 否则紧凑菜单仍被撑宽(见 ThemedMenu.minWidth)
    /// ⚠️ compact 项必须与 ThemedMenu 的 minWidth 配合:2 字中文标签
    ///    (剪切/复制/全选)自身只有 ~48px,不给菜单一个确定宽度会显得过窄。
    property bool compact: false

    leftPadding: compact ? 12 : 10
    rightPadding: compact ? 12 : 18
    topPadding: 0
    bottomPadding: 0

    implicitWidth: Math.max(compact ? 0 : 180,
                            itemText.implicitWidth + control.leftPadding + control.rightPadding)
    implicitHeight: compact ? 24 : 30

    background: Rectangle {
        x: 4
        y: 2
        width: control.width - 8
        height: control.height - 4
        radius: Theme.metrics.radiusButton
        color: control.highlighted ? Theme.colors.accentSoft : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
    }

    contentItem: Text {
        id: itemText
        text: control.text
        font.family: Theme.fontFamily
        font.pixelSize: compact ? Theme.metrics.fontCaption : Theme.metrics.fontSecondary
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Theme.colors.textDisabled
             : control.danger ? Theme.colors.error
             : control.highlighted ? Theme.colors.accent
             : Theme.colors.textPrimary
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
    }
}
