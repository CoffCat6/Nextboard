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

    /// 文本条目是否展开显示全文(长文本默认折叠 4 行)。
    property bool textExpanded: false

    /// 请求翻译本条内容(翻译抽屉位于 Main.qml 作用域,跨组件经信号上抛)。
    signal translateRequested(string text)

    /// 卡片元信息行的操作按钮:小胶囊 + 悬停换底(§34,100~140ms)。
    component CardAction: Rectangle {
        id: action
        property string label: ""
        signal triggered()
        implicitWidth: actionLabel.implicitWidth + 14
        radius: Theme.metrics.radiusBadge
        implicitHeight: 22
        color: actionHover.containsMouse ? Theme.colors.accentSoft : Theme.colors.surfaceSecondary
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
        Text {
            id: actionLabel
            anchors.centerIn: parent
            text: action.label
            font.pixelSize: Theme.metrics.fontCaption
            color: actionHover.containsMouse ? Theme.colors.accent : Theme.colors.textSecondary
        }
        MouseArea {
            id: actionHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.triggered()
        }
    }

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

    // §18/28:Surface + 1px Border,hover 换 SurfaceHover(强调色不铺满列表);
    // 菜单打开时用 AccentSoft + Accent 边框表"当前操作对象"(§18 Selected)。
    radius: Theme.metrics.radiusCard
    color: menu.opened ? Theme.colors.surfaceSelected
         : ma.containsMouse ? Theme.colors.surfaceHover
         : Theme.colors.surface
    border.width: Theme.metrics.borderWidth
    border.color: menu.opened ? Theme.colors.accent
                : ma.containsMouse ? Theme.colors.borderStrong
                : Theme.colors.border
    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
    Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
    implicitHeight: cardLayout.implicitHeight + 24

    // ---- 公共操作(右键菜单 / 快捷按钮 / 点击手势共用) ----

    /// 复制条目内容;成功即收起面板,便于直接粘贴到目标应用(第一代 use-entries.copy 语义)。
    function copyEntryAndHide() {
        const resp = Entries.copyEntry(root.entryId)
        if (resp.ok) panelWindow.hidePanel()
        else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
    }

    /// 文件条目:显式复制路径文本(与「复制文件」CF_HDROP 区分开)。
    function copyPathAndHide() {
        const resp = Core.call("copy_text_to_clipboard", { text: root.text })
        if (resp.ok) panelWindow.hidePanel()
        else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
    }

    /// 图片条目:编辑器会遮住面板,先收起再打开(第一代同语义)。
    function editEntryAfterHide() {
        panelWindow.hidePanel()
        Core.editEntry(root.entryId)
    }

    function deleteEntryWithToast() {
        const resp = Entries.deleteEntry(root.entryId)
        if (resp.ok) Core.showToast("已删除")
        else Core.showToast("删除失败:" + (resp.error ? resp.error.message : ""))
    }

    // 条目操作菜单:按类型(text / image / file)分组构建,顺序为
    // 「类型专属操作 | 通用操作(复制/贴图) | 固定 | 删除(危险项置底)」。
    // 动态构建而非 visible 开关:Menu 底层是 ListView,visible:false 的
    // 委托仍占高度,会在菜单里留出空白间隙。
    ThemedMenu {
        id: menu

        Component { id: menuItemComp; ThemedMenuItem {} }
        Component { id: menuSepComp; ThemedMenuSeparator {} }

        function rebuild() {
            while (count > 0) takeItem(0).destroy()

            function addAction(label, fn, danger) {
                const it = menuItemComp.createObject(menu, { text: label, danger: danger === true })
                it.triggered.connect(fn)
                addItem(it)
            }
            function addSep() { addItem(menuSepComp.createObject(menu)) }

            // 组 1:类型专属操作
            if (root.kind === "file") {
                addAction("打开", function() { Core.openEntry(root.entryId) })
                addAction("定位文件", function() { Core.revealEntry(root.entryId) })
            } else if (root.kind === "image") {
                addAction("编辑", root.editEntryAfterHide)
                addAction("识别文字", function() { Core.ocrEntry(root.entryId) })
                addAction("翻译", root.requestTranslate)
            } else {
                addAction("翻译", root.requestTranslate)
            }
            addSep()

            // 组 2:通用操作(复制 / 贴图)
            addAction("复制", root.copyEntryAndHide)
            if (root.kind === "file") addAction("复制路径", root.copyPathAndHide)
            else addAction("贴到屏幕", function() { Core.pinEntry(root.entryId) })
            addSep()

            // 组 3:固定;组 4:删除(危险项置底)
            addAction(root.pinned ? "取消固定" : "固定",
                      function() { Entries.togglePin(root.entryId, !root.pinned) })
            addSep()
            addAction("删除", root.deleteEntryWithToast, true)
        }

        // 每次弹出前重建,保证「固定/取消固定」文案与当前状态一致
        onAboutToShow: rebuild()
    }

    // 文件条目单击复制的延迟器:双击的第一击同样会触发 onClicked,若不延迟一拍,
    // 双击会先执行一次复制(还隐藏面板)。300ms 内来了第二击则取消复制、改为打开。
    Timer {
        id: fileCopyTimer
        interval: 300
        onTriggered: root.copyEntryAndHide()
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
        onDoubleClicked: function(mouse) {
            // 双击文件 = 用系统默认程序打开;取消挂起的单击复制
            if (mouse.button === Qt.LeftButton && root.kind === "file") {
                fileCopyTimer.stop()
                Core.openEntry(root.entryId)
            }
        }
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                menu.popup()
            } else if (root.kind === "file") {
                // 单击 = 复制(经 fileCopyTimer 延迟一拍);双击 = 打开文件
                fileCopyTimer.restart()
            } else {
                root.copyEntryAndHide()
            }
        }
    }

    ColumnLayout {
        id: cardLayout
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Loader {
            id: bodyLoader
            Layout.fillWidth: true
            sourceComponent: root.kind === "image" && root.imageUrl !== "" ? imageBody : textBody
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            // 元数据弱化(§40:12px / TextSecondary)
            Label { text: root.timeText; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            Label { text: root.sizeText; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            Label {
                visible: root.pinned
                text: "固定"; font.pixelSize: Theme.metrics.fontMicro
                padding: 3; leftPadding: 6; rightPadding: 6
                color: Theme.colors.onWarning
                background: Rectangle { color: Theme.colors.warning; radius: Theme.metrics.radiusBadge }
            }
            Label {
                visible: root.source !== "" && root.source !== "clipboard"
                text: root.source; font.pixelSize: Theme.metrics.fontMicro
                padding: 3; leftPadding: 6; rightPadding: 6
                color: root.source === "capture" ? Theme.colors.sourceCapture
                     : root.source === "translate" ? Theme.colors.sourceTranslate
                     : Theme.colors.textSecondary
                background: Rectangle { color: Theme.colors.surfaceSecondary; radius: Theme.metrics.radiusBadge }
            }
            Label {
                text: root.kind; font.pixelSize: Theme.metrics.fontMicro
                padding: 3; leftPadding: 6; rightPadding: 6
                color: Theme.colors.textTertiary
                background: Rectangle { color: Theme.colors.surfaceSecondary; radius: Theme.metrics.radiusBadge }
            }
            Item { Layout.fillWidth: true }

            // 展开/收起:仅文本被 4 行截断时显示(截断与否由 textBody 内部判定)
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind === "text"
                         && bodyLoader.item !== null
                         && bodyLoader.item.truncated === true
                label: root.textExpanded ? "收起" : "展开"
                onTriggered: root.textExpanded = !root.textExpanded
            }

            // ---- 右下角快捷按钮 ----
            // 贴图:图片直贴;文本渲染成位图后贴(文件条目无可贴内容,故隐藏)
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind !== "file"
                label: "贴图"
                onTriggered: Core.pinEntry(root.entryId)
            }
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind === "image"
                label: "编辑"
                onTriggered: root.editEntryAfterHide()
            }
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                label: "删除"
                onTriggered: root.deleteEntryWithToast()
            }
        }
    }

    Component {
        id: textBody
        Column {
            id: textWrap
            width: parent ? parent.width : 0

            /// 折叠态是否被省略号截断(决定"展开"按钮是否显示;
            /// Text 的布局在不可见时也会计算,truncated 不会失效)
            readonly property bool truncated: collapsedText.truncated

            // 折叠态:最多 4 行,超出省略号
            Text {
                id: collapsedText
                visible: !root.textExpanded
                width: parent.width
                text: root.text || root.preview || ""
                color: Theme.colors.textPrimary
                font.pixelSize: Theme.metrics.fontBody
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 4
                elide: Text.ElideRight
            }

            // 展开态:完整文本;超过 320px 高度改为内部滚动。
            // 用只读 TextEdit 而非 Text:支持选中部分文字手动复制
            // (TextEdit 是交互项,点击它不会落到卡片的复制 MouseArea 上)
            Flickable {
                visible: root.textExpanded
                width: parent.width
                height: Math.min(fullEdit.contentHeight + 12, 320)
                clip: true
                contentWidth: width
                contentHeight: fullEdit.contentHeight + 12
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                TextEdit {
                    id: fullEdit
                    x: 6
                    width: textWrap.width - 12
                    text: root.text || ""
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.colors.textPrimary
                    font.pixelSize: Theme.metrics.fontBody
                    selectByMouse: true
                    selectionColor: Theme.colors.accentSoft
                    selectedTextColor: Theme.colors.textPrimary
                }
            }
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
