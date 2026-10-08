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

    // ---- 右键菜单:全面板**唯一实例** ----
    // 原先每个 EntryCard 各自持有一个 Menu,存在两个问题:
    //   ① 委托被 ListView 回收/重建时,菜单与卡片的绑定可能失效;
    //   ② 点击菜单外部(尤其左键点其他卡片)时,这一次点击会**穿透**到下层卡片,
    //      触发"复制并收起面板",与菜单操作互相干扰、菜单也没能正常关闭。
    // 现改为面板级单例:卡片只上抛内容快照;菜单打开期间由**屏障**吃掉外部点击。
    /// 当前菜单的目标条目快照(entryId/kind/pinned/text/tags);null = 菜单未打开。
    property var menuTarget: null

    /// 标签筛选(S5.2):空串 = 不筛选。与分类标签/搜索叠加。
    property string activeTag: ""

    /// 标签编辑:目标条目 id 与内联卡片可见性。
    property string tagEditId: ""

    /// 打开标签编辑卡片(右键菜单「标签…」)。
    function openTagEditor(entry) {
        tagEditId = entry.entryId
        tagInput.text = entry.tags || ""
        tagEditorCard.visible = true
        tagInput.forceActiveFocus()
    }

    /// 提交标签:按逗号/空白切分,去空去重后交模型保存。
    function commitTagEditor() {
        const parts = tagInput.text.split(/[,，\s]+/)
        const tags = []
        for (let i = 0; i < parts.length; ++i) {
            const t = parts[i].trim()
            if (t.length > 0 && tags.indexOf(t) < 0) tags.push(t)
        }
        tagEditorCard.visible = false
        Entries.saveTags(tagEditId, tags)
    }

    /// EntryCard 右键时调用:记住目标条目并在光标处弹出菜单。
    function openEntryMenu(entry) {
        menuTarget = entry
        entryMenu.popup()
    }

    /// 收起菜单并清空目标(屏障点击、菜单自身关闭都会走到这里)。
    function closeEntryMenu() {
        menuTarget = null
        entryMenu.close()
    }

    // ---- 菜单动作 ----
    // 与卡片内的同名逻辑等价,但只作用于**快照里的 id** —— 不依赖委托实例是否还在。
    function copyEntryAndHide(id) {
        const resp = Entries.copyEntry(id)
        if (resp.ok) panelWindow.hidePanel()
        else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
    }
    function copyPathAndHide(text) {
        const resp = Core.call("copy_text_to_clipboard", { text: text })
        if (resp.ok) panelWindow.hidePanel()
        else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
    }
    function editEntryAfterHide(id) {
        panelWindow.hidePanel()
        Core.editEntry(id)
    }
    function deleteEntryWithToast(id) {
        const resp = Entries.deleteEntry(id)
        if (resp.ok) Core.showToast("已删除")
        else Core.showToast("删除失败:" + (resp.error ? resp.error.message : ""))
    }
    /// 翻译:文本条目取全文交翻译窗口;图片/文件明确提示而非静默失败。
    function translateEntry(entry) {
        if (entry.kind !== "text") {
            Core.showToast(entry.kind === "image" ? "图片请先「识别文字」再翻译"
                                                 : "文件条目不支持翻译")
            return
        }
        const resp = Core.call("get_entry", { id: entry.entryId })
        if (resp.ok && resp.data.textContent) Core.openTranslateWith(resp.data.textContent)
        else Core.showToast("读取条目内容失败")
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

            // ⚠️ 顶部**不挂任何拖动**:图标 / 标题 / 搜索框 / 按钮都不可拖动。
            // 曾经把整个头部做成拖动区,结果在搜索框里按住拖选文字时,指针一旦
            // 移出搜索框,DragHandler 就被激活 → 窗口跟着跑,体验很差。
            // 窗口移动改为状态栏左侧的专用把手(见底部 DragGrip)。

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingMd
                spacing: Theme.metrics.spacingSm

                // 品牌标记 + 标题:纯展示,不参与任何鼠标交互(钉死)
                RowLayout {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: Theme.metrics.spacingSm

                    AppMark {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 20
                        implicitHeight: 20
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
                    iconName: "translate"
                    text: "翻译"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: Core.openTranslate()
                }
                ThemedButton {
                    variant: "ghost"
                    iconName: "settings"
                    text: "设置"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: settingsDrawer.openAndLoad()
                }
                // 固定:开启后点其它地方不会自动隐藏(选中态用 accent 底反馈)
                ThemedButton {
                    variant: panelWindow.pinned ? "primary" : "ghost"
                    iconName: "pin"
                    text: "固定"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: panelWindow.setPinned(!panelWindow.pinned)
                }
                ThemedButton {
                    variant: "ghost"
                    square: true
                    text: "✕"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: panelWindow.hidePanel()
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

        // ---- 标签筛选条(S5.2):有标签时才出现;再点一次取消筛选 ----
        Flickable {
            id: tagBar
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            implicitHeight: visible ? 24 : 0
            visible: Entries.tagStats.length > 0
            contentWidth: tagRow.implicitWidth
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: tagRow
                spacing: 6
                Repeater {
                    model: Entries.tagStats
                    delegate: Rectangle {
                        id: tagChip
                        required property var modelData
                        readonly property bool active: root.activeTag === modelData.tag
                        height: 22
                        width: tagChipText.implicitWidth + 16
                        radius: Theme.metrics.radiusBadge
                        color: active ? Theme.colors.accentSoft
                             : tagChipMa.containsMouse ? Theme.colors.surfaceHover
                             : Theme.colors.surfaceSecondary
                        border.width: Theme.metrics.borderWidth
                        border.color: active ? Theme.colors.accent : Theme.colors.border
                        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                        Text {
                            id: tagChipText
                            anchors.centerIn: parent
                            text: modelData.tag + " " + modelData.count
                            font.pixelSize: Theme.metrics.fontCaption
                            color: active ? Theme.colors.accent : Theme.colors.textSecondary
                        }
                        MouseArea {
                            id: tagChipMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.activeTag = tagChip.active ? "" : tagChip.modelData.tag
                                Entries.setTag(root.activeTag)
                            }
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
                // 「当前操作对象」高亮:只有被右键的那张卡片亮起(菜单为面板级单例)
                menuOpen: root.menuTarget !== null && root.menuTarget.entryId === entryId
                // 防御:菜单打开期间,任何卡片都吞掉点击(即使屏障因平台差异没拦住)
                menuOpenElsewhere: root.menuTarget !== null
                onContextMenuRequested: function(entry) { root.openEntryMenu(entry) }
                onMenuDismissRequested: root.closeEntryMenu()
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

                // 窗口移动的**唯一**入口:状态栏左侧把手。
                // 顶部(图标/标题/搜索/按钮)全部钉死不可拖动,避免误触;
                // 需要挪动面板时认准这个把手即可。
                DragGrip {}
                Text {
                    text: "拖动移动"
                    color: Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                    Layout.alignment: Qt.AlignVCenter
                }
                Rectangle {
                    Layout.preferredWidth: Theme.metrics.borderWidth
                    Layout.preferredHeight: 12
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: Theme.metrics.spacingXs
                    Layout.rightMargin: Theme.metrics.spacingXs
                    color: Theme.colors.border
                }

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
                    text: "拖动窗口边缘可调整大小"
                    color: Theme.colors.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                    Layout.rightMargin: Theme.metrics.spacingSm
                }
                Text {
                    text: panelWindow.pinned ? "已固定 · 不自动隐藏"
                          : Entries.loading ? "加载中…" : "Nextboard"
                    color: panelWindow.pinned ? Theme.colors.accent : Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                }
            }
        }
    }

    // ---- 窗口拖动把手(状态栏专用) ----
    // 六个小圆点 = 通用"抓手"符号。只有它能移动窗口,顶部控件一律不可拖动。
    component DragGrip: Item {
        id: grip
        // 命中区比图形略大,方便点中(状态栏高 30px,20px 的手柄仍有余量)
        implicitWidth: 20
        implicitHeight: 20
        Layout.alignment: Qt.AlignVCenter
        readonly property color dotColor: gripHover.hovered ? Theme.colors.accent
                                                            : Theme.colors.textTertiary
        Grid {
            anchors.centerIn: parent
            columns: 2
            rowSpacing: 3
            columnSpacing: 3
            Repeater {
                model: 6
                delegate: Rectangle {
                    width: 3
                    height: 3
                    radius: 1.5
                    color: grip.dotColor
                }
            }
        }
        // 悬停给出"可拖动"的光标与配色反馈
        HoverHandler { id: gripHover; cursorShape: Qt.SizeAllCursor }
        DragHandler {
            onActiveChanged: if (active) panelWindow.startSystemMove()
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
        z: 1000   // 高于菜单外部点击屏障,通知不会被挡
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

    // ---- 右键菜单(面板级单例;内容按目标条目类型在弹出前重建) ----
    // 动态构建而非 visible 开关:Menu 底层是 ListView,visible:false 的委托仍占高度,
    // 会在菜单里留出空白间隙。
    ThemedMenu {
        id: entryMenu

        // 外部点击**只由屏障处理**,这里必须关掉 Menu 默认的 CloseOnPressOutside:
        // 否则按下时 Qt 会先关菜单 → onClosed 把 menuTarget 置空 → 屏障立刻隐藏,
        // 紧接着这次点击继续派发给下层卡片,于是"关菜单"的同时又触发了一次复制。
        closePolicy: Popup.CloseOnEscape

        Component { id: menuItemComp; ThemedMenuItem {} }
        Component { id: menuSepComp; ThemedMenuSeparator {} }

        /// 菜单关闭即清空目标:屏障随之撤掉(点菜单项、点外部、Esc 都走这里)
        onClosed: root.menuTarget = null

        function rebuild() {
            while (count > 0) takeItem(0).destroy()
            const entry = root.menuTarget
            if (!entry) return

            function addAction(label, fn, danger) {
                const it = menuItemComp.createObject(entryMenu, { text: label, danger: danger === true })
                it.triggered.connect(fn)
                addItem(it)
            }
            function addSep() { addItem(menuSepComp.createObject(entryMenu)) }

            // 组 1:类型专属操作
            if (entry.kind === "file") {
                addAction("打开", function() { Core.openEntry(entry.entryId) })
                addAction("定位文件", function() { Core.revealEntry(entry.entryId) })
            } else if (entry.kind === "image") {
                addAction("编辑", function() { root.editEntryAfterHide(entry.entryId) })
                addAction("识别文字", function() { Core.ocrEntry(entry.entryId) })
                addAction("翻译", function() { root.translateEntry(entry) })
            } else {
                addAction("翻译", function() { root.translateEntry(entry) })
            }
            addSep()

            // 组 2:通用操作(复制 / 贴图)
            addAction("复制", function() { root.copyEntryAndHide(entry.entryId) })
            if (entry.kind === "file")
                addAction("复制路径", function() { root.copyPathAndHide(entry.text) })
            else
                addAction("贴到屏幕", function() { Core.pinEntry(entry.entryId) })
            addSep()

            // 组 3:标签(S5.2)
            addAction("标签…", function() { root.openTagEditor(entry) })
            // 有标签时给出**一键清除**入口(不必进编辑器手动删空)
            if (entry.tags) addAction("清除标签", function() { Entries.saveTags(entry.entryId, []) })
            addSep()

            // 组 4:固定;组 5:删除(危险项置底)
            addAction(entry.pinned ? "取消固定" : "固定",
                      function() { Entries.togglePin(entry.entryId, !entry.pinned) })
            addSep()
            addAction("删除", function() { root.deleteEntryWithToast(entry.entryId) }, true)
        }

        // 每次弹出前重建,保证「固定/取消固定」文案与当前状态一致
        onAboutToShow: rebuild()
    }

    // 面板收起时收掉菜单:否则下次呼出会残留菜单与点击屏障
    Connections {
        target: panelWindow
        function onPanelHidden() { root.closeEntryMenu() }
    }

    // ---- 菜单外部点击屏障 ----
    // 菜单打开时铺满面板,**吃掉**这一次点击:否则左键会穿透到下层卡片触发
    // "复制并收起面板",与菜单操作互相干扰。z 高于所有内容、低于 Toast。
    // (菜单本身在 Popup 层,位于内容之上,不受本屏障影响。)
    MouseArea {
        id: menuDismissBarrier
        anchors.fill: parent
        z: 900
        visible: root.menuTarget !== null
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: root.closeEntryMenu()
    }

    // ---- 标签编辑卡片(S5.2) ----
    // 与右键菜单同样的"外部点击关闭"思路:先铺屏障,再放卡片(z 介于菜单与 Toast 之间)。
    MouseArea {
        id: tagEditorBarrier
        anchors.fill: parent
        z: 950
        visible: tagEditorCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: tagEditorCard.visible = false
    }

    Rectangle {
        id: tagEditorCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(320, root.width - 48)
        // 只锚上/左右:高度由内容隐式决定(anchors.fill 会与 implicitHeight 互相依赖)
        height: tagEditorLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: tagEditorLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: "编辑标签"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
            }
            TextField {
                id: tagInput
                Layout.fillWidth: true
                placeholderText: "用逗号分隔,例如:工作, 重要;留空保存即清除全部标签"
                placeholderTextColor: Theme.colors.textTertiary
                color: Theme.colors.textPrimary
                selectionColor: Theme.colors.accent
                selectedTextColor: Theme.colors.accentText
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: tagInput.activeFocus ? Theme.colors.accent : Theme.colors.border
                }
                Keys.onReturnPressed: root.commitTagEditor()
                Keys.onEnterPressed: root.commitTagEditor()
                Keys.onEscapePressed: tagEditorCard.visible = false
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"
                    size: "sm"
                    text: "取消"
                    onClicked: tagEditorCard.visible = false
                }
                ThemedButton {
                    variant: "primary"
                    size: "sm"
                    text: "保存"
                    onClicked: root.commitTagEditor()
                }
            }
        }
    }
}
