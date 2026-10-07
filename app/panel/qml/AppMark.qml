// Nextboard 品牌标记(应用图标)。
//
// 设计意象:一块圆角"板"(board)+ 三条长短递减的记录横线 —— 对应产品定位
// "一体化效率板";配色只用主题的 accent / accentText,不引入第三种颜色,
// 与 theme.md 的克制用色一致。
//
// 纯 QML 矢量绘制,不依赖任何图片资源(项目对 windeployqt 漏部署资源有前科,
// 少一个资源文件就少一类运行期事故);尺寸由外部 implicitWidth/Height 驱动,
// 内部按 22px 基准等比缩放,在 16~32px 下都保持清晰。
import QtQuick
import Nextboard.panel

Item {
    id: root
    implicitWidth: 22
    implicitHeight: 22

    /// 以 22px 为设计基准的缩放系数
    readonly property real unit: Math.min(width, height) / 22.0
    /// 圆角矩形板
    readonly property color plateColor: Theme.colors.accent
    /// 板上的"记录"横线
    readonly property color lineColor: Theme.colors.accentText

    Rectangle {
        anchors.fill: parent
        radius: 6 * root.unit
        color: root.plateColor
    }

    Column {
        anchors.centerIn: parent
        spacing: 2.5 * root.unit

        Rectangle {
            width: 10 * root.unit
            height: Math.max(1, 2 * root.unit)
            radius: height / 2
            color: root.lineColor
            opacity: 0.9
        }
        Rectangle {
            width: 12 * root.unit
            height: Math.max(1, 2 * root.unit)
            radius: height / 2
            color: root.lineColor
        }
        Rectangle {
            width: 7 * root.unit
            height: Math.max(1, 2 * root.unit)
            radius: height / 2
            color: root.lineColor
            opacity: 0.75
        }
    }
}
