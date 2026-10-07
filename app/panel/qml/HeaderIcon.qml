// 主面板头部按钮的矢量图标集(纯 QML 绘制,零图片资源)。
//
// 用法:`ThemedButton { iconName: "translate" }` —— 颜色由按钮按自身文字色注入,
// 因此 primary(强调底)与 ghost/secondary 三种状态下对比度都成立。
//
// 设计约束(与 docs/theme.md §38 一致):统一 Outline 线性风、1.5px 描边、
// 16px 视口;只用几何图形,不用字体字形/emoji(跨字体渲染不可控)。
import QtQuick
import Nextboard.panel

Item {
    id: root

    /// 图标种类:"translate" | "settings" | "pin"
    property string kind: ""
    /// 描边与填充色(由 ThemedButton 注入)
    property color color: Theme.colors.textPrimary
    /// 线宽基准
    readonly property real stroke: 1.5

    implicitWidth: 16
    implicitHeight: 16

    // 与按钮同节奏换色(变体切换时不会突变)
    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

    // ---- 翻译:地球(语言 / 翻译的通用隐喻) ----
    Item {
        anchors.fill: parent
        visible: root.kind === "translate"

        // 外圈
        Rectangle {
            anchors.centerIn: parent
            width: 14; height: 14; radius: 7
            color: "transparent"
            border.width: root.stroke
            border.color: root.color
        }
        // 赤道
        Rectangle {
            anchors.centerIn: parent
            width: 14; height: root.stroke; radius: root.stroke / 2
            color: root.color
        }
        // 经线(用胶囊形近似经线圈)
        Rectangle {
            anchors.centerIn: parent
            width: 7; height: 14; radius: 3.5
            color: "transparent"
            border.width: root.stroke
            border.color: root.color
        }
    }

    // ---- 设置:齿轮(八齿 + 中空圆环) ----
    Item {
        anchors.fill: parent
        visible: root.kind === "settings"

        // 八个齿:委托自身填满图标区并绕中心旋转,齿固定在顶端 → 绕中心均匀分布
        Repeater {
            model: 8
            delegate: Item {
                anchors.fill: parent
                rotation: index * 45
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 1
                    width: 2.5; height: 4; radius: 1.25
                    color: root.color
                }
            }
        }
        // 齿轮主体(圆环,中心留孔)
        Rectangle {
            anchors.centerIn: parent
            width: 11; height: 11; radius: 5.5
            color: "transparent"
            border.width: root.stroke
            border.color: root.color
        }
    }

    // ---- 固定:图钉(帽 + 身 + 针尖) ----
    Item {
        anchors.fill: parent
        visible: root.kind === "pin"

        Rectangle {   // 钉帽
            anchors.horizontalCenter: parent.horizontalCenter
            y: 2
            width: 10; height: 3; radius: 1.5
            color: root.color
        }
        Rectangle {   // 钉身
            anchors.horizontalCenter: parent.horizontalCenter
            y: 5
            width: 5; height: 4.5; radius: 1
            color: root.color
        }
        Rectangle {   // 针尖
            anchors.horizontalCenter: parent.horizontalCenter
            y: 9.5
            width: 1.8; height: 4.5; radius: 0.9
            color: root.color
        }
    }
}
