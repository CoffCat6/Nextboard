// 主面板:header(标题/搜索/翻译/设置) + 分类标签 + 历史列表 + 抽屉 + toast。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Rectangle {
    id: root
    color: Theme.bg
    focus: true

    function showToast(message) { toast.showMsg(message) }

    // 自动化验收入口:C++ applySearchText 直调(等价用户输入,走同一防抖链路)
    function setSearch(text) {
        searchInput.text = text
        Entries.setQuery(text)
    }

    Keys.onEscapePressed: {
        if (settingsDrawer.opened || translateDrawer.opened) {
            settingsDrawer.close()
            translateDrawer.close()
        } else {
            panelWindow.hidePanel()
        }
    }
    // 焦点在列表/卡片时按 / 直接开搜:默认焦点链路已让首键进搜索框
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier)) {
            searchInput.forceActiveFocus()
            event.accepted = true
            return
        }
        // Ctrl+1..9:复制当前视图第 N 条(编号 = 过滤后列表自上而下,第一代 F2);
        // 成功后隐藏面板,便于直接粘贴到目标应用。抽屉打开时不生效(第一代同语义)。
        if ((event.modifiers & Qt.ControlModifier)
                && !settingsDrawer.opened && !translateDrawer.opened
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

        // ---- header ----
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 10

            Label {
                text: "Nextboard"
                font.pixelSize: 15
                font.bold: true
                color: Theme.text
                Layout.alignment: Qt.AlignVCenter
            }

            Rectangle {
                id: searchBox
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                radius: 6
                color: Theme.surface
                border.color: searchInput.activeFocus ? Theme.accent : Theme.border
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 6
                    spacing: 4
                    Text { text: "🔍"; font.pixelSize: 12; color: Theme.muted }
                    TextInput {
                        id: searchInput
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        color: Theme.text
                        font.pixelSize: 13
                        clip: true
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        Text {
                            visible: searchInput.text === "" && !searchInput.activeFocus
                            anchors.verticalCenter: parent.verticalCenter
                            text: "搜索历史(文本 / OCR / 文件名)…"
                            color: Theme.muted
                            font.pixelSize: 13
                        }
                        onTextEdited: Entries.setQuery(text)
                    }
                    Text {
                        visible: searchInput.text !== ""
                        text: "✕"
                        color: Theme.muted
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { searchInput.clear(); Entries.setQuery(""); Entries.refreshNow() }
                        }
                    }
                }
            }

            Button { text: "翻译"; onClicked: translateDrawer.openAndFocus() }
            Button { text: "设置"; onClicked: settingsDrawer.openAndLoad() }
        }

        // ---- 分类标签 ----
        Row {
            id: tabs
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.bottomMargin: 6
            spacing: 4

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
                    // 计数来自 Core.counts(单一数据源 + countsChanged 通知):
                    // 条目增删后徽标自动更新,且不再每个标签各发一次同步查询。
                    readonly property var count: Core.counts[viewId]
                    radius: 6
                    height: 26
                    width: tabLabel.implicitWidth + 20
                    color: tabs.current === tabDelegate.viewId ? Theme.accent
                         : tabMa.containsMouse ? Theme.surface : "transparent"
                    Text {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: tabDelegate.count !== undefined
                              ? tabDelegate.label + " " + tabDelegate.count
                              : tabDelegate.label
                        color: tabs.current === tabDelegate.viewId ? Theme.accentText : Theme.muted
                        font.pixelSize: 12
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

        // ---- 列表 ----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            clip: true
            spacing: 6
            model: Entries

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: 6
                    radius: 3
                    color: Theme.muted
                    opacity: 0.4
                }
            }

            delegate: EntryCard {
                required property int index
                width: list.width
                onTranslateRequested: function(text) { translateDrawer.openWith(text) }
            }

            // 触底自动加载(fetchMore 由 ListView 自动调用,这里提前 200px 预取)
            onContentYChanged: {
                if (contentHeight > height && contentY >= contentHeight - height - 200)
                    Entries.fetchMoreRequest()
            }

            footer: Item {
                width: list.width
                height: visible ? 24 : 0
                visible: list.count > 0 && Entries.hasMore
                Text {
                    anchors.centerIn: parent
                    text: Entries.loading ? "加载中…" : "↓ 上滑加载更多"
                    color: Theme.muted
                    font.pixelSize: 11
                }
            }
        }

        // ---- 空状态(覆盖层,不随列表滚动) ----
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: list.count === 0
            Column {
                anchors.centerIn: parent
                spacing: 8
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: {
                        if (Entries.errorText !== "") return Entries.errorText
                        if (searchInput.text !== "") return "没有匹配的历史记录"
                        if (tabs.current === "pinned") return "暂无固定条目"
                        return "暂无历史记录,复制点什么试试"
                    }
                    color: Entries.errorText !== "" ? Theme.danger : Theme.muted
                    font.pixelSize: 13
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: Entries.errorText === "" && searchInput.text === ""
                    text: "Ctrl+Shift+V 呼出 / 隐藏面板"
                    color: Theme.accent
                    font.pixelSize: 12
                }
            }
        }

        // ---- footer ----
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.topMargin: 2
            Layout.bottomMargin: 6
            text: "Nextboard · Qt Quick"
            color: Theme.muted
            font.pixelSize: 10
        }
    }

    // ---- 抽屉 ----
    SettingsDrawer { id: settingsDrawer }
    TranslateDrawer { id: translateDrawer }

    // ---- Core 信号订阅 ----
    // toastMessage 是全局提示通道(C++ 与各抽屉都经 Core.showToast 发出);
    // 面板内只在此处订阅,避免多处弹窗重复。
    Connections {
        target: Core
        function onToastMessage(message) { toast.showMsg(message) }
        // 翻译链路(热键 Ctrl+Shift+T 或截图翻译 Ctrl+Shift+E 完成后回填):
        // 面板可能刚被截图流程收起,这里把它带回前台再打开抽屉。
        function onTranslatePrefill(text) {
            panelWindow.showPanel()
            translateDrawer.openWith(text)
        }
    }

    // ---- toast ----
    Rectangle {
        id: toast
        function showMsg(message) {
            toastText.text = message
            toast.opacity = 1
            hideTimer.restart()
        }
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 34
        anchors.horizontalCenter: parent.horizontalCenter
        radius: 6
        color: Theme.accent
        width: Math.max(toastText.implicitWidth + 32, 80)
        height: 28
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Text { id: toastText; anchors.centerIn: parent; color: Theme.accentText; font.pixelSize: 12 }
        Timer {
            id: hideTimer
            interval: 1400
            onTriggered: toast.opacity = 0
        }
    }
}
