// 主面板右侧快捷栏(列表页专属):固定窄条 + 纵向图标按钮。
//
// 定位:类比 VS Code 的 Activity Bar —— 常驻、窄(52)、只放图标,
// 给"一击直达"的高频功能一个固定落点,不与头部命令按钮(动作型)混用。
// 头部与状态栏通宽,本栏只占列表内容区右侧(见 Main.qml 的中部内容区)。
//
// 数据驱动:按钮来自 actions 数组(Repeater),本期只有「添加快捷功能」
// 一个占位钮;二期用户自定义时,这里变成"已启用动作 + 末尾固定添加钮",
// 增删按钮只改数组,布局与样式零改动。
//
// 交互约定:
//  - 悬停 surfaceHover + 图标提亮(与分类标签/筛选 chip 同一套中性语言);
//  - 占位钮(dim)整体弱化到 0.55,表达"这里可以放东西"而非可用命令,
//    点击给出 toast 而不是静默无响应;
//  - 悬停 ToolTip 显示功能名:纯图标按钮没有文字,必须有tooltip兜底。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Rectangle {
    id: root
    objectName: "quickRail"

    /// 栏宽(布局子项用 implicitWidth):40 钮 + 左右各 6 呼吸边。
    implicitWidth: 52
    color: Theme.colors.surfaceSecondary

    /// 按钮模型:{ id, iconKind, label, dim }。
    /// dim = 占位/未启用(整体弱化);二期真实动作不设 dim。
    readonly property var actions: [
        { id: "addQuick", iconKind: "plus", label: "添加快捷功能", dim: true }
    ]

    /// 点击上抛(带动作 id),行为由使用方(Main.qml)接线 ——
    /// 本组件只管"长什么样",不知道每个动作做什么。
    signal actionTriggered(string actionId)

    // 左侧分隔线:与状态栏同一"框架色"语言,把内容区与快捷栏分开
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.metrics.borderWidth
        color: Theme.colors.border
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: Theme.metrics.spacingSm
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.metrics.spacingXs

        Repeater {
            model: root.actions

            delegate: Item {
                id: railButton
                required property var modelData
                // 验收钩子:占位钮(首个流程后续接验收时真实点击用)
                objectName: railButton.modelData.id === "addQuick" ? "quickRailAdd" : ""
                width: 40
                height: 40
                opacity: railButton.modelData.dim ? 0.55 : 1.0
                Behavior on opacity { NumberAnimation { duration: Theme.animation.fast } }

                // 悬停底(无选中态:占位阶段没有"激活"语义,选中态随二期动作一起加)
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.metrics.radiusButton
                    color: railMa.containsMouse ? Theme.colors.surfaceHover : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                }

                HeaderIcon {
                    anchors.centerIn: parent
                    kind: railButton.modelData.iconKind
                    size: 20
                    color: railMa.containsMouse ? Theme.colors.textPrimary
                                                : Theme.colors.textSecondary
                }

                MouseArea {
                    id: railMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.actionTriggered(railButton.modelData.id)
                }

                ToolTip.visible: railMa.containsMouse
                ToolTip.delay: 500
                ToolTip.text: railButton.modelData.label
            }
        }
    }
}
