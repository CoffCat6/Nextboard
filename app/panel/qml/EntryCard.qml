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

    /// 请求翻译本条内容(翻译抽屉位于 Main.qml 作用域,跨组件经信号上抛)。
    signal translateRequested(string text)

    /// 翻译:文本条目取全文交抽屉;图片/文件暂无取字链路,明确提示而非静默失败。
    function requestTranslate() {
        if (root.kind !== "text") {
            Core.showToast(root.kind === "image" ? "图片请先「识别文字」再翻译"
                                                 : "文件条目不支持翻译")
            return
        }
        const resp = Core.call("get_entry", { id: root.entryId })
        if (resp.ok && resp.data.textContent) root.translateRequested(resp.data.textContent)
        else Core.showToast("读取条目内容失败")
    }

    radius: 8
    color: ma.containsMouse || menu.opened ? Theme.surface2 : Theme.surface
    border.color: ma.containsMouse || menu.opened ? Theme.accent : Theme.border
    border.width: 1
    implicitHeight: cardLayout.implicitHeight + 20

    // 条目操作菜单(顺序与第一代 App.tsx ctx-menu 对齐)。
    // 「贴到屏幕」「编辑」待 S4(需按条目贴图 FFI 与 QGraphicsScene 编辑器)。
    Menu {
        id: menu
        MenuItem {
            text: "复制"
            onTriggered: {
                const resp = Entries.copyEntry(root.entryId)
                if (resp.ok) panelWindow.hidePanel()   // 复制即收起,便于粘贴
                else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            }
        }
        MenuItem {
            text: "翻译"
            visible: root.kind !== "file"
            onTriggered: root.requestTranslate()
        }
        MenuItem {
            text: "识别文字"
            visible: root.kind === "image"
            onTriggered: Core.ocrEntry(root.entryId)
        }
        MenuItem {
            text: root.pinned ? "取消固定" : "固定"
            onTriggered: Entries.togglePin(root.entryId, !root.pinned)
        }
        MenuSeparator {}
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
        // 滚动转发:卡片不拦截滚轮,显式转交父级 ListView 滚动。
        // (Sciter 时代需宿主手动转发滚轮进引擎;QML 化后由场景图直接派发,
        //  此处仅固定语义、防后续给卡片加 onWheel 时误吞滚动。)
        onWheel: function(wheel) { wheel.accepted = false }
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                menu.popup()
            } else {
                const resp = Entries.copyEntry(root.entryId)
                // 复制成功即收起面板,便于直接粘贴到目标应用(第一代 use-entries.copy 语义)
                if (resp.ok) panelWindow.hidePanel()
                else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
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
