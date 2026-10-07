// 主面板(theme.md 布局):Header(标题/搜索/翻译/设置) + 导航标签 + 历史列表
// + 底部状态栏(快捷键提示) + 设置抽屉 + 右下角 Toast。
//
// 翻译已独立为单独窗口(TranslateWindow.qml),面板内不再有翻译抽屉 ——
// 面板专注"呼出即用、用完即走"的剪贴板历史。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Rectangle {
    id: root
    color: Theme.colors.background
    border.width: Theme.metrics.borderWidth
    border.color: Theme.colors.border
    focus: true

    function showToast(message) { toast.showMsg(message) }

    // 自动化验收入口:C++ applySearchText 直调(等价用户输入,走同一防抖链路)
    function setSearch(text) {
        searchInput.text = text
        Entries.setQuery(text)
    }

    Keys.onEscapePressed: {
        if (settingsDrawer.opened) {
            settingsDrawer.close()
        } else {
            panelWindow.hidePanel()
        }
    }
    // Ctrl+F / Ctrl+K 聚焦搜索(§27);Ctrl+1..9 复制当前视图第 N 条(第一代 F2)。
    Keys.onPressed: function(event) {
        if ((event.key === Qt.Key_F || event.key === Qt.Key_K)
                && (event.modifiers & Qt.ControlModifier)) {
            searchInput.forceActiveFocus()
            searchInput.selectAll()
            event.accepted = true
            return
        }
        if ((event.modifiers & Qt.ControlModifier)
                && !settingsDrawer.opened
                && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            const resp = Entries.copyByIndex(event.key - Qt.Key_1)
            if (resp.ok) panelWindow.hidePanel()
            else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Header(§16/§17:Surface 底 + 底边线,标题 + 搜索 + 动作) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.metrics.headerHeight
            color: Theme.colors.surface

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.metrics.borderWidth
                color: Theme.colors.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingMd
                spacing: Theme.metrics.spacingSm

                // 品牌标记 + 标题:同时作为窗口拖动区(无边框窗口的移动入口)
                Item {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: brandRow.implicitWidth
                    implicitHeight: brandRow.implicitHeight

                    RowLayout {
                        id: brandRow
                        spacing: Theme.metrics.spacingSm
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 8; implicitHeight: 8; radius: 4
                            color: Theme.colors.accent
                        }
                        Label {
                            text: "Nextboard"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.metrics.fontPageTitle
                            font.weight: Font.DemiBold
                            color: Theme.colors.textPrimary
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    DragHandler {
                        // 无边框窗口的移动:交系统处理,避免逐帧 setPosition 的抖动
                        onActiveChanged: if (active) panelWindow.startSystemMove()
                    }
                }

                // 搜索框(§27:34px,聚焦 Accent 边框;Focus 动画不改变布局)
                Rectangle {
                    id: searchBox
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.metrics.searchHeight
                    radius: Theme.metrics.radiusInput
                    color: Theme.colors.surfaceSecondary
                    border.width: Theme.metrics.borderWidth
                    border.color: searchInput.activeFocus ? Theme.colors.accent
                                : searchBoxMa.containsMouse ? Theme.colors.borderStrong
                                : Theme.colors.border
                    Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }

                    // 整框 hover 态与"点空白处聚焦"(放在 RowLayout 之后声明以覆盖空白区,
                    // 但 searchInput 与清空按钮在其上层,输入与清除不受影响)
                    MouseArea {
                        id: searchBoxMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.IBeamCursor
                        onClicked: searchInput.forceActiveFocus()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.metrics.spacingSm
                        anchors.rightMargin: 6
                        spacing: 6

                        // 放大镜图标(Outline:圆环 + 手柄,§38)
                        Item {
                            Layout.preferredWidth: 14
                            Layout.preferredHeight: 14
                            Layout.alignment: Qt.AlignVCenter
                            Rectangle {
                                anchors.centerIn: parent
                                anchors.verticalCenterOffset: -1
                                width: 9; height: 9; radius: 5
                                color: "transparent"
                                border.color: searchInput.activeFocus ? Theme.colors.accent
                                                                      : Theme.colors.textTertiary
                                border.width: 1.5
                                Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
                            }
                            Rectangle {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                width: 5; height: 1.5; radius: 1
                                rotation: 45
                                color: searchInput.activeFocus ? Theme.colors.accent
                                                               : Theme.colors.textTertiary
                                Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                            }
                        }

                        TextInput {
                            id: searchInput
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            color: Theme.colors.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.metrics.fontBody
                            clip: true
                            verticalAlignment: TextInput.AlignVCenter
                            selectByMouse: true
                            selectionColor: Theme.colors.accent
                            selectedTextColor: Theme.colors.accentText
                            Text {
                                visible: searchInput.text === "" && !searchInput.activeFocus
                                anchors.verticalCenter: parent.verticalCenter
                                text: "搜索历史(文本 / OCR / 文件名)…"
                                color: Theme.colors.textTertiary
                                font: searchInput.font
                            }
                            onTextEdited: Entries.setQuery(text)
                        }

                        Text {
                            visible: searchInput.text !== ""
                            text: "✕"
                            color: Theme.colors.textTertiary
                            font.pixelSize: Theme.metrics.fontSecondary
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: { searchInput.clear(); Entries.setQuery(""); Entries.refreshNow() }
                            }
                        }
                    }
                }

                ThemedButton {
                    variant: "ghost"
                    text: "翻译"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: Core.openTranslate()
                }
                ThemedButton {
                    variant: "ghost"
                    text: "设置"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: settingsDrawer.openAndLoad()
                }
            }
        }

        // ---- 分类标签(§15 选中态:AccentSoft 底 + Accent 字 + 左侧指示条) ----
        Row {
            id: tabs
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            Layout.topMargin: Theme.metrics.spacingSm
            Layout.bottomMargin: Theme.metrics.spacingXs + 2
            spacing: Theme.metrics.spacingXs

            property string current: "all"

            Repeater {
                model: ListModel {
                    ListElement { label: "全部"; viewId: "all" }
                    ListElement { label: "文本"; viewId: "text" }
                    ListElement { label: "图片"; viewId: "image" }
                    ListElement { label: "文件"; viewId: "file" }
                    ListElement { label: "固定"; viewId: "pinned" }
                }
                delegate: Rectangle {
                    id: tabDelegate
                    required property string label
                    required property string viewId
                    // 计数来自 Core.counts(单一数据源 + countsChanged 通知)。
                    readonly property var count: Core.counts[viewId]
                    readonly property bool selected: tabs.current === tabDelegate.viewId
                    radius: Theme.metrics.radiusInput
                    height: Theme.metrics.navItemHeight
                    width: tabLabel.implicitWidth + Theme.metrics.spacingLg + 6
                    color: selected ? Theme.colors.accentSoft
                         : tabMa.containsMouse ? Theme.colors.surfaceHover
                         : "transparent"
                    border.width: Theme.metrics.borderWidth
                    border.color: selected ? Theme.colors.accent : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

                    // 左侧 Accent 指示条(§15:2~3px)
                    Rectangle {
                        visible: tabDelegate.selected
                        anchors.left: parent.left
                        anchors.leftMargin: 3
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: parent.height - 12
                        radius: 1
                        color: Theme.colors.accent
                    }
                    Text {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: tabDelegate.count !== undefined
                              ? tabDelegate.label + " " + tabDelegate.count
                              : tabDelegate.label
                        color: tabDelegate.selected ? Theme.colors.textPrimary
                             : Theme.colors.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontSecondary
                    }
                    MouseArea {
                        id: tabMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            tabs.current = tabDelegate.viewId
                            Entries.setView(tabDelegate.viewId)
                        }
                    }
                }
            }
        }

        // ---- 列表(§19 新条目淡入,§43 ListView 回收) ----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            clip: true
            spacing: 6
            model: Entries

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: 6
                    radius: 3
                    color: Theme.colors.textTertiary
                    opacity: 0.4
                }
            }

            // 新条目加入(§19:Opacity 0→1 + Y -6→0,OutCubic,不用弹跳)
            add: Transition {
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animation.normal }
                    NumberAnimation { property: "y"; from: -6; to: 0; duration: Theme.animation.normal; easing.type: Easing.OutCubic }
                }
            }

            delegate: EntryCard {
                required property int index
                width: list.width
                // 翻译走独立窗口:打开即收起面板(C++ 侧也会兜底收起)
                onTranslateRequested: function(text) { Core.openTranslateWith(text) }
            }
        }

        // ---- 空状态(§41:小图标 + 标题 + 说明 + 快捷键帽) ----
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: list.count === 0
            Column {
                anchors.centerIn: parent
                spacing: Theme.metrics.spacingSm

                // 空剪贴板图标:Outline 剪贴板(§41 禁大插画)
                Item {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 36; height: 40
                    Rectangle {
                        radius: 4
                        anchors.fill: parent
                        anchors.topMargin: 6
                        color: "transparent"
                        border.color: Theme.colors.textTertiary
                        border.width: 1.5
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 14; height: 6; radius: 2
                        color: Theme.colors.background
                        border.color: Theme.colors.textTertiary
                        border.width: 1.5
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontSection
                    font.weight: Font.DemiBold
                    color: Theme.colors.textPrimary
                    text: {
                        if (Entries.errorText !== "") return "加载失败"
                        if (searchInput.text !== "") return "没有匹配的历史记录"
                        if (tabs.current === "pinned") return "暂无固定条目"
                        return "剪贴板是空的"
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Entries.errorText !== "" ? Theme.colors.error
                         : Theme.colors.textSecondary
                    text: {
                        if (Entries.errorText !== "") return Entries.errorText
                        if (searchInput.text !== "") return "换个关键词试试,或清除搜索条件"
                        if (tabs.current === "pinned") return "右键条目选择「固定」即可常驻这里"
                        return "复制任意内容,它会自动出现在这里"
                    }
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Theme.metrics.spacingXs
                    visible: Entries.errorText === "" && searchInput.text === ""
                    KeyCap { label: "Ctrl" }
                    KeyCap { label: "Shift" }
                    KeyCap { label: "V" }
                    Text {
                        text: "呼出 / 隐藏面板"
                        color: Theme.colors.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        // ---- 状态栏(§37:快捷键可视化) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: Theme.colors.surfaceSecondary
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: Theme.metrics.borderWidth
                color: Theme.colors.border
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingMd
                spacing: Theme.metrics.spacingXs
                KeyCap { label: "Ctrl" }
                KeyCap { label: "K" }
                Text {
                    text: "搜索"
                    color: Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                    Layout.rightMargin: Theme.metrics.spacingSm
                }
                KeyCap { label: "Ctrl" }
                KeyCap { label: "1-9" }
                Text {
                    text: "复制条目"
                    color: Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: Entries.loading ? "加载中…" : "Nextboard"
                    color: Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                }
            }
        }
    }

    // ---- 抽屉(设置仍留在面板内;翻译已独立为窗口) ----
    SettingsDrawer { id: settingsDrawer }

    // ---- Core 信号订阅 ----
    Connections {
        target: Core
        function onToastMessage(message) { toast.showMsg(message) }
    }

    // ---- Toast(§36:右下角通知卡,Surface+Border+柔和层级,非 accent 大色块) ----
    Rectangle {
        id: toast
        function showMsg(message) {
            toastText.text = message
            toast.x = -12   // 进入:X +12 → 0(配合 right anchor 用偏移)
            toast.opacity = 1
            hideTimer.restart()
        }
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 44
        radius: Theme.metrics.radiusPopup
        color: Theme.colors.surface
        border.color: Theme.colors.border
        border.width: Theme.metrics.borderWidth
        width: Math.max(toastText.implicitWidth + Theme.metrics.spacingLg + 16, 120)
        height: 32
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: Theme.animation.fast } }
        Behavior on x { NumberAnimation { duration: Theme.animation.normal; easing.type: Easing.OutCubic } }
        Text {
            id: toastText
            anchors.centerIn: parent
            color: Theme.colors.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.metrics.fontCaption
        }
        Timer {
            id: hideTimer
            interval: 1600
            onTriggered: toast.opacity = 0
        }
    }
}
