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

    /// 图片卡片高度上限:截图多为 16:9,不封顶会把列表撑成"一屏一条"。
    readonly property int maxImageCardHeight: 300

    radius: 8
    color: ma.containsMouse || menu.opened ? Theme.surface2 : Theme.surface
    border.color: ma.containsMouse || menu.opened ? Theme.accent : Theme.border
    border.width: 1
    implicitHeight: cardLayout.implicitHeight + 20

    // 条目操作菜单(顺序与第一代 App.tsx ctx-menu 对齐)
    Menu {
        id: menu
        MenuItem {
            text: "贴到屏幕"
            onTriggered: Core.pinEntry(root.entryId)
        }
        MenuItem {
            text: "编辑"
            visible: root.kind === "image"
            onTriggered: {
                panelWindow.hidePanel()   // 编辑器会遮住面板,先收起(第一代同语义)
                Core.editEntry(root.entryId)
            }
        }
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
                color: Theme.pinText; background: Rectangle { color: Theme.pin; radius: 4 }
            }
            Label {
                visible: root.source !== "" && root.source !== "clipboard"
                text: root.source; font.pixelSize: 10
                padding: 3; leftPadding: 6; rightPadding: 6
                color: root.source === "capture" ? Theme.sourceCapture
                     : root.source === "translate" ? Theme.sourceTranslate : Theme.muted
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
            id: imageWrap
            width: parent ? parent.width : 0

            // 按图片真实宽高比铺满卡片宽度得到应占高度。
            // 注意用 implicitWidth/implicitHeight(加载后的实际尺寸),而不是
            // sourceSize——后者是"请求加载的尺寸",拿它算高度会得到错误结果。
            readonly property real fittedHeight:
                (image.implicitWidth > 0 && image.implicitHeight > 0)
                ? width * (image.implicitHeight / image.implicitWidth)
                : 0

            // 卡片高度 = 图片显示高度(未就绪时占位 60,加载完自动撑开)。
            // 原先固定 110 而 Image 自身按隐式尺寸画到 220,图片会**溢出卡片**
            // 盖住下方条目;这里让两者同源,并把 Image 显式约束到本项尺寸。
            implicitHeight: image.status === Image.Ready
                            ? Math.min(Math.max(fittedHeight, 40), root.maxImageCardHeight)
                            : 60

            Image {
                id: image
                // 显式宽高:确保图片永远不超出卡片(PreserveAspectFit 负责留白居中)
                width: imageWrap.width
                height: imageWrap.implicitHeight
                source: root.imageUrl
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: true
                // 缩略图最长边 320(入库时预生成),解码成本已受控,无需再设 sourceSize
            }
        }
    }
}
