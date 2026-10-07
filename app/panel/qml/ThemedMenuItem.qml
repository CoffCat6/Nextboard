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

    leftPadding: 10
    rightPadding: 18
    topPadding: 0
    bottomPadding: 0

    implicitWidth: Math.max(180, itemText.implicitWidth + control.leftPadding + control.rightPadding)
    implicitHeight: 30

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
        font.pixelSize: Theme.metrics.fontSecondary
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Theme.colors.textDisabled
             : control.danger ? Theme.colors.error
             : control.highlighted ? Theme.colors.accent
             : Theme.colors.textPrimary
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
    }
}
