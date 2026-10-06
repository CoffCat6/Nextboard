// 单条历史卡片:文本 / 图片缩略图 / 文件路径三种形态 + 元信息行 + 右键菜单。
// 角色名与 ClipEntryModel::roleNames 一一对应,由 ListView 按名注入。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Rectangle {
    id: root
    required property string entryId
    required property string kind
    required property string text
    required property string preview
    required property string imageUrl
    required property string timeText
    required property string sizeText
    required property bool pinned
    required property string source

    radius: 8
    color: ma.containsMouse || menu.opened ? Theme.surface2 : Theme.surface
    border.color: ma.containsMouse || menu.opened ? Theme.accent : Theme.border
    border.width: 1
    implicitHeight: cardLayout.implicitHeight + 20

    Menu {
        id: menu
        MenuItem {
            text: root.pinned ? "取消固定" : "固定"
            onTriggered: Entries.togglePin(root.entryId, !root.pinned)
        }
        MenuItem {
            text: "OCR 识别文字"
            visible: root.kind === "image"
            onTriggered: Core.ocrEntry(root.entryId)
        }
        MenuItem {
            text: "删除"
            onTriggered: {
                const resp = Entries.deleteEntry(root.entryId)
                if (resp.ok) Core.showToast("已删除")
                else Core.showToast("删除失败:" + (resp.error ? resp.error.message : ""))
            }
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                menu.popup()
            } else {
                const resp = Entries.copyEntry(root.entryId)
                Core.showToast(resp.ok ? "已复制到剪贴板"
                               : "复制失败:" + (resp.error ? resp.error.message : ""))
            }
        }
    }

    ColumnLayout {
        id: cardLayout
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6

        Loader {
            id: bodyLoader
            Layout.fillWidth: true
            sourceComponent: root.kind === "image" && root.imageUrl !== "" ? imageBody : textBody
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Label { text: root.timeText; color: Theme.muted; font.pixelSize: 11 }
            Label { text: root.sizeText; color: Theme.muted; font.pixelSize: 11 }
            Label {
                visible: root.pinned
                text: "固定"; font.pixelSize: 10
                padding: 3; leftPadding: 6; rightPadding: 6
                color: "#222"; background: Rectangle { color: Theme.pin; radius: 4 }
            }
            Label {
                visible: root.source !== "" && root.source !== "clipboard"
                text: root.source; font.pixelSize: 10
                padding: 3; leftPadding: 6; rightPadding: 6
                color: root.source === "capture" ? "#7ec2ff"
                     : root.source === "translate" ? "#c9a7ff" : Theme.muted
                background: Rectangle { color: Theme.surface2; radius: 4 }
            }
            Label {
                text: root.kind; font.pixelSize: 10
                padding: 3; leftPadding: 6; rightPadding: 6
                color: Theme.muted; background: Rectangle { color: Theme.surface2; radius: 4 }
            }
            Item { Layout.fillWidth: true }
        }
    }

    Component {
        id: textBody
        Text {
            width: parent ? parent.width : 0
            text: root.text || root.preview || ""
            color: Theme.text
            font.pixelSize: 13
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 4
            elide: Text.ElideRight
        }
    }

    Component {
        id: imageBody
        Item {
            width: parent ? parent.width : 0
            implicitHeight: image.status === Image.Ready
                            ? Math.min(image.sourceSize.height, 110) : 60
            Image {
                id: image
                anchors.verticalCenter: parent.verticalCenter
                source: root.imageUrl
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: true
                sourceSize.height: 220
            }
        }
    }
}
