// 设置页 · 快捷功能分类:主面板右侧「快捷功能栏」(QuickRail)的显示开关。
//
// 本期只有界面壳:栏内目前是占位钮,真正的动作池(内置功能)与
// 用户自定义命令二期接入;到时本页长出"启用/排序/添加命令"的管理列表,
// 「显示快捷栏」开关继续承担"要不要这条栏"。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    function load(s) {
        if (!s) return
        // 容器级 default:旧配置无此键时 undefined,兜底 true 与 core 默认同口径
        quickRailCheck.checked = s.quickRail !== false
    }

    function collect(s) {
        s.quickRail = quickRailCheck.checked
    }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: form.implicitHeight + 2 * Theme.metrics.spacingMd
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: form
            width: Math.min(flick.width - 2 * Theme.metrics.spacingLg, 680)
            x: (flick.width - width) / 2
            spacing: Theme.metrics.spacingXs

            SectionTitle { text: "快捷功能栏" }

            // 显示开关:除了落库,还直写 panelWindow 让列表页**即时**生效 ——
            // 设置页与列表页在同一窗口的不同形态,改动后切回去立刻可见,不必重启。
            ReadableCheckBox {
                id: quickRailCheck
                objectName: "quickRailCheck"   // 验收钩子
                text: "在主面板右侧显示快捷功能栏"
                onClicked: {
                    panelWindow.showQuickRail = quickRailCheck.checked
                    view.scheduleSave()
                }
            }
            HintLabel {
                text: "呼出即用的一排固定功能入口:智能截屏、贴图、便签等"
                      + "(逐步增加),也可以添加你自己的命令。"
            }

            // ---- 二期预告区:占位说明,让用户知道这里将有什么 ----
            SectionDivider {}
            SectionTitle { text: "栏内按钮管理" }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSecondary
                color: Theme.colors.textSecondary
                text: "栏内按钮的启用与排序(内置动作)、自定义命令(打开文件夹 /"
                      + " 网页 / 脚本)将在后续版本提供;目前可以先用面板右侧的"
                      + "「添加快捷功能」占位钮了解入口位置。"
            }
        }
    }

    // ---- 验收辅助(同 SettingsPageGeneral) ----
    function scrollToBottom() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }
    readonly property real acceptanceScrollY: flick.contentY
    function scrollItemIntoView(it) {
        const pos = it.mapToItem(flick.contentItem, 0, 0)
        flick.contentY = Math.max(0, Math.min(pos.y - 8,
                                    flick.contentHeight - flick.height))
        return true
    }
}
