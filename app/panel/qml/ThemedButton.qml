// 主题化按钮(theme.md §24/§25):primary / secondary / ghost / danger 四种变体,
// 统一 hover 过渡(120ms)与按下反馈(scale 0.98,无弹跳)。
//
// 为什么不全用 Basic 的 Button:Basic 控件的配色走 QPalette,深色主题下
// 文字/底板对比度不足(项目已在设置页踩过坑),且各页面会出现"按钮样式漂移"。
// 项目内所有命令按钮统一走本组件,样式只有一个来源。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Button {
    id: control

    /// primary(强调动作) / secondary(默认) / ghost(无底色,用于工具栏) / danger(危险动作)
    property string variant: "secondary"
    /// sm(28) / normal(32) / lg(36)
    property string size: "normal"
    /// 图标/纯符号按钮:正方形(宽 = 高)
    property bool square: false

    readonly property bool _dense: size === "sm"
    readonly property bool _large: size === "lg"

    implicitHeight: _dense ? Theme.metrics.controlHeightSm
                  : _large ? Theme.metrics.controlHeightLg
                  : Theme.metrics.controlHeight
    // 正方形(图标按钮)模式宽度跟随高度;其余沿用 Control 的默认推算。
    // ⚠️ 不能写 `square ? implicitHeight : undefined` —— 给 implicitWidth 赋 undefined
    // 会得到 "Unable to assign [undefined] to double",宽度失效为 0,
    // 按钮在布局里直接**消失**(实测:主面板「设置 / 翻译」按钮不可见 → 设置页进不去)。
    implicitWidth: square
                   ? implicitHeight
                   : Math.max(implicitBackgroundWidth + leftInset + rightInset,
                              implicitContentWidth + leftPadding + rightPadding)

    leftPadding: _dense ? 10 : _large ? 16 : 12
    rightPadding: leftPadding

    font.family: Theme.fontFamily
    font.pixelSize: _dense ? Theme.metrics.fontSecondary : Theme.metrics.fontBody
    font.weight: variant === "primary" ? Font.DemiBold : Font.Normal

    readonly property color _baseColor: {
        if (!control.enabled) return Theme.colors.surfaceSecondary
        switch (control.variant) {
            case "primary":
                return control.down ? Theme.colors.accentPressed
                     : control.hovered ? Theme.colors.accentHover
                     : Theme.colors.accent
            case "danger":
                return control.down ? Qt.darker(Theme.colors.error, 1.2)
                     : control.hovered ? Qt.lighter(Theme.colors.error, 1.12)
                     : Theme.colors.error
            case "ghost":
                return control.hovered ? Theme.colors.surfaceHover : "transparent"
            default:
                return control.hovered ? Theme.colors.surfaceHover : Theme.colors.surface
        }
    }
    readonly property color _textColor: {
        if (!control.enabled) return Theme.colors.textDisabled
        if (control.variant === "primary" || control.variant === "danger")
            return Theme.colors.accentText
        return Theme.colors.textPrimary
    }
    readonly property color _borderColor: {
        if (!control.enabled) return Theme.colors.border
        if (control.variant === "primary" || control.variant === "danger") return "transparent"
        if (control.variant === "ghost")
            return control.hovered ? Theme.colors.border : "transparent"
        return control.hovered ? Theme.colors.borderStrong : Theme.colors.border
    }

    // §25:按下轻微缩放,不做弹跳
    scale: control.down && control.enabled ? 0.98 : 1.0
    Behavior on scale {
        NumberAnimation { duration: Theme.animation.fast; easing.type: Easing.OutCubic }
    }

    contentItem: Text {
        text: control.text
        font: control.font
        color: control._textColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
    }

    background: Rectangle {
        radius: Theme.metrics.radiusButton
        color: control._baseColor
        border.width: control._borderColor === "transparent" ? 0 : Theme.metrics.borderWidth
        border.color: control._borderColor
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
        Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
    }
}
