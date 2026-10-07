// 主题化按钮(theme.md §24/§25):primary / secondary / ghost / danger 四种变体,
// 统一 hover 过渡(120ms)与按下反馈(scale 0.98,无弹跳)。
//
// 为什么不全用 Basic 的 Button:Basic 控件的配色走 QPalette,深色主题下
// 文字/底板对比度不足(项目已在设置页踩过坑),且各页面会出现"按钮样式漂移"。
// 项目内所有命令按钮统一走本组件,样式只有一个来源。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.panel

Button {
    id: control

    /// primary(强调动作) / secondary(默认) / ghost(无底色,用于工具栏) / danger(危险动作)
    property string variant: "secondary"
    /// sm(28) / normal(32) / lg(36)
    property string size: "normal"
    /// 图标/纯符号按钮:正方形(宽 = 高)
    property bool square: false
    /// 可选图标(项目图标词汇,见 HeaderIcon.qml):"" / "translate" / "settings" / "pin"
    /// ⚠️ 不能叫 `icon` —— Basic 的 Button(AbstractButton)已有 `icon`(QQuickIcon),
    /// 且被声明为 FINAL,QML 里重新声明会直接让整个组件加载失败
    /// ("Cannot override FINAL property"),连带所有使用方一起失效(曾导致主面板空白)。
    property string iconName: ""
    readonly property bool hasIcon: iconName !== ""

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

    // 指针反馈:命令按钮统一手型光标(Basic 默认是箭头,点按钮没有"可点"的暗示)
    HoverHandler { cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }

    contentItem: RowLayout {
        // 无图标时 HeaderIcon 不可见,布局会自动忽略它(也不占 spacing)
        spacing: control.hasIcon ? 6 : 0

        HeaderIcon {
            Layout.alignment: Qt.AlignVCenter
            visible: control.hasIcon
            kind: control.iconName
            color: control._textColor
        }
        Text {
            Layout.alignment: Qt.AlignVCenter
            // 按钮被压窄时由 Text 承担压缩并省略号截断(正常宽度下没有多余空间,不起作用)
            Layout.fillWidth: true
            text: control.text
            font: control.font
            color: control._textColor
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
        }
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
