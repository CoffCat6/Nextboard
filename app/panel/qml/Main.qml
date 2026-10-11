// 主面板(theme.md 布局):Header(标题/搜索/翻译/设置) + 导航标签 + 历史列表
// + 底部状态栏(快捷键提示) + 设置抽屉 + 右下角 Toast。
//
// 面板有**两种形态**(见 PanelWindow::Mode),同一窗口内互斥显示:
//  - 列表页:呼出即用、用完即走的剪贴板历史(本文件 142 行起的 ColumnLayout);
//  - 翻译页:需要停留的翻译工作区(TranslateView.qml,由 Loader 延迟构建)。
// 合并成整窗模式切换(而不是独立窗口)是为了省掉第二个 QQmlEngine 与它自己的
// 场景图/渲染栈 —— 详见 docs/progress.md 的内存收口记录。
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

    // 自动化验收入口:C++ openSettingsPanel 直调(等价点「设置」按钮)。
    // 设置抽屉已改 Loader 懒构建,这条链路同时验证"首次构建 + 尺寸对齐面板根"。
    function openSettings() { settingsDrawerLoader.openAndLoad() }

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

    /// 翻译页是否已构建过:首次进入才实例化,之后常驻 ——
    /// 这样"切走再回来"原文/译文还在,且从不翻译的用户完全不付这份开销。
    property bool translatePageBuilt: false

    /// 收掉列表页的全部浮层:设置抽屉 / 右键菜单 / 标签编辑。
    ///
    /// ⚠️ 设置抽屉(`AppDrawer` → `Drawer`)继承自 **Popup**,渲染在窗口的 overlay 层 ——
    /// 它永远盖在所有普通 Item 之上,与"谁先声明"无关。所以切到翻译页时若不清掉,
    /// 它会**浮在翻译页上面**,而且此时焦点已交给翻译页,抽屉自己的 Esc 也收不到。
    function dismissListOverlays() {
        // 设置抽屉已改 Loader 懒构建:item 为空 = 从未打开过,自然没有浮层要收
        if (settingsDrawerLoader.item && settingsDrawerLoader.item.opened)
            settingsDrawerLoader.item.close()
        if (root.menuTarget !== null) root.closeEntryMenu()
        if (root.tagEditId !== "") root.tagEditId = ""
    }

    /// C++ 侧(PanelWindow::showTranslate)调用:切到翻译页并预填原文。
    /// 页的构建是同步的(非异步 Loader),故置位后 item 立即可用。
    function openTranslate(text) {
        dismissListOverlays()   // 先把列表页的浮层收干净,再进翻译页
        translatePageBuilt = true
        if (translatePage.item) translatePage.item.openWith(text)
    }

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
        // 分层处理,一次 Esc 只消费一层(自上而下):
        //   设置抽屉 → 翻译页自己的浮层 → 收起面板
        // 翻译页的 Esc 语义与 ✕ 一致:**直接收起**,不再顺带切回列表页。
        if (settingsDrawerLoader.item && settingsDrawerLoader.item.opened) {
            settingsDrawerLoader.item.close()
            return
        }
        if (panelWindow.mode === "translate") {
            // 翻译页存在时先问它有没有浮层要关(如历史抽屉),没有就收起面板
            if (!translatePage.item || !translatePage.item.dismissTopLayer())
                panelWindow.hidePanel()
            return
        }
        panelWindow.hidePanel()
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
                && !(settingsDrawerLoader.item && settingsDrawerLoader.item.opened)
                && panelWindow.mode !== "translate"
                && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            const resp = Entries.copyByIndex(event.key - Qt.Key_1)
            if (resp.ok) panelWindow.hidePanel()
            else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            event.accepted = true
        }
    }

    // ---- 页 1:剪贴板历史(列表) ----
    // 与翻译页互斥显示:不可见时不产生场景图节点,切到翻译页时这块的渲染开销归零。
    ColumnLayout {
        anchors.fill: parent
        visible: panelWindow.mode === "list"
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

            // ⚠️ 拖动**只挂在下方的"品牌+标题"展示区**,不要挂整个头部:
            // 曾经把整个头部做成拖动区,结果在搜索框里按住拖选文字、指针一旦移出
            // 搜索框,DragHandler 就被激活 → 窗口跟着跑,体验很差。
            // 挂在"不参与任何交互的展示区"上,就与搜索框/按钮各不相干。
            // 这里是窗口移动的**唯一**入口(原先底部状态栏的把手已移除)。

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingMd
                spacing: Theme.metrics.spacingSm

                // 品牌标记 + 标题:纯展示,不参与任何鼠标交互 ——
                // 正因为"不参与交互",它才是头部唯一安全的窗口拖动把手。
                RowLayout {
                    id: headerDragArea
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillHeight: true   // 命中区撑满头部高度,好按
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

                    HoverHandler { cursorShape: Qt.SizeAllCursor }
                    DragHandler {
                        // ⚠️ `target: null` 不能省:PointerHandler 的 target **默认是
                        // 声明它的父项**,否则拖动会去改这个 RowLayout 的 x/y(布局下次
                        // 重排才弹回,拖动过程中标题就被挪走了)。置 null 即只留 active 信号。
                        target: null
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
                    onClicked: settingsDrawerLoader.openAndLoad()
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
                    /// 图标种类与 viewId 同源;"pinned" 复用图钉图标(与头部「固定」按钮同符号)。
                    readonly property string iconKind:
                        tabDelegate.viewId === "pinned" ? "pin" : tabDelegate.viewId
                    radius: Theme.metrics.radiusInput
                    height: Theme.metrics.navItemHeight
                    // 图标 + 间距 + 文字(与 ThemedButton 的 hasIcon 同口径)
                    // + 左右内边距 + 左侧指示条占位。总内边距刻意与改造前一致(22px),
                    // 免得 5 个标签页一起变宽把头部挤变形(Main.qml 的 PANEL_MIN_W 有下限)。
                    width: tabInner.implicitWidth + 6 + 2 * Theme.metrics.spacingSm
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

                    Row {
                        id: tabInner
                        anchors.centerIn: parent
                        spacing: 6
                        HeaderIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            kind: tabDelegate.iconKind
                            // 图标跟随文字色:选中态 textPrimary,未选中 textSecondary
                            color: tabDelegate.selected ? Theme.colors.textPrimary
                                                      : Theme.colors.textSecondary
                            size: 14
                        }
                        Text {
                            id: tabLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: tabDelegate.count !== undefined
                                  ? tabDelegate.label + " " + tabDelegate.count
                                  : tabDelegate.label
                            color: tabDelegate.selected ? Theme.colors.textPrimary
                                 : Theme.colors.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.metrics.fontSecondary
                        }
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
                        /// 标签自己的派生色:**与卡片上的同名标签同色**(同一纯函数、
                        /// 无状态,筛选条与条目两端不可能取到不同值)。
                        readonly property color tagFg: Theme.tagColor(modelData.tag)
                        height: 22
                        width: tagChipText.implicitWidth + 16
                        radius: Theme.metrics.radiusBadge
                        // 活跃态:文字用标签自己的色,底色**保持中性**(surfaceSecondary,
                        // 与非活跃同底),靠"同色边框 + 色点实心"表达选中。
                        //
                        // ⚠️ 为什么不用"标签色半透明叠底"(更直觉的做法):浅色调色板是按
                        // "在 surfaceSecondary 上 ≥ 4.7:1"定出来的,余量只有 0.2 —— 压一层
                        // 同色相叠底会立刻跌破 4.5:1(实测最差 4.02)。要两者兼得只能把浅色
                        // 调色板整体压暗,那又会让标签在卡片上显得沉重。故活跃态改由
                        // 边框/色点承担(实测边框与底的 ΔE ≥ 58,足够醒目)。
                        color: tagChipMa.containsMouse ? Theme.colors.surfaceHover
                             : Theme.colors.surfaceSecondary
                        border.width: Theme.metrics.borderWidth
                        border.color: tagChip.active ? tagChip.tagFg : Theme.colors.border
                        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
                        Row {
                            id: tagChipText
                            anchors.centerIn: parent
                            spacing: 4
                            // 色点:标签色在筛选条上的"锚",不依赖文字颜色即可对号入座。
                            // 活跃态实心、非活跃态空心 —— 这样"选中"在灰度屏/色弱下也读得出
                            // (只靠颜色深浅区分对色觉障碍不友好)。
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 6; height: 6; radius: 3
                                color: tagChip.active ? tagChip.tagFg : "transparent"
                                border.width: tagChip.active ? 0 : 1
                                border.color: tagChip.tagFg
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: tagChip.modelData.tag + " " + tagChip.modelData.count
                                font.pixelSize: Theme.metrics.fontCaption
                                // 活跃态用标签色,非活跃态中性灰:
                                // 全部着色会盖住分类标签页的层级(§47 配色比例)
                                color: tagChip.active ? tagChip.tagFg : Theme.colors.textSecondary
                            }
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

            // ---- 内存收口:委托回收复用 + 限制预建缓冲 ----
            // ① reuseItems:委托滚出视野后进复用池,而不是销毁重建。原先每次滚动都
            //    重新实例化 EntryCard(含 Loader/CardAction 等子对象)并重解码图片,
            //    是滚动期内存/CPU 反复抖动的来源。复用后这层开销只在首屏付一次。
            //    代价:委托内部状态会跨行保留 —— 已在 EntryCard 的 ListView.onReused
            //    里复位 textExpanded 与双击计时器(见该文件)。
            // ② cacheBuffer:显式压到 240px。默认值是"平台相关且通常大于零",会预建
            //    较多视野外委托(图片条目下每张都带解码图,内存代价高)。240px 约等于
            //    上下各一块卡片高度,足够平滑预取,又不去缓存整屏之外的委托。
            reuseItems: true
            cacheBuffer: 240

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

                // 窗口拖动已上移到头部「品牌 + 标题」区(见 Header 的 headerDragArea),
                // 状态栏这里不再需要专用把手。

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

    // ---- 页 2:翻译 ----
    // 与列表**同一窗口、同一 QQuickWidget、同一 QQmlEngine** —— 这是本轮内存收口的关键:
    // 原先翻译是独立顶层窗口,自带第二个 QML 引擎与它自己的场景图/渲染栈(见 docs/progress.md)。
    // 延迟构建 + 之后常驻:从不翻译的用户不付这份开销,常用的人切走再回来内容还在。
    Loader {
        id: translatePage
        active: root.translatePageBuilt
        anchors.fill: parent
        visible: panelWindow.mode === "translate"
        sourceComponent: Component { TranslateView { } }
        // 页切换时把焦点交接清楚:进翻译页→页内拿焦点(否则 Ctrl+Enter 不生效);
        // 回列表页→面板根拿焦点(否则 Esc/Ctrl+F 这些根级按键会留在已隐藏的页里)。
        onVisibleChanged: {
            // 进入:把焦点交给本页(否则 Ctrl+Enter 之类不生效)。
            // 离开时的清理由 TranslateView 自己处理(它最清楚自己有哪些浮层)。
            if (visible) { if (item) item.forceActiveFocus() }
            else root.forceActiveFocus()
        }
    }

    // ---- 抽屉(设置仍留在面板内;翻译已并入面板为"整窗模式切换") ----
    // 懒构建:SettingsDrawer 是一个 1200+ 行的重表单(6 组设置 + 多个自绘控件),
    // 原先作为 Main.qml 的直接子项**启动即整棵实例化**,哪怕从不打开 —— 它单独
    // 吃掉数 MB 的常驻(含委托/控件对象)。这里改为首次点「设置」才构建,之后常驻
    // (再次打开省去重建,且保留上次的滚动位置)。
    //
    // item 为空 = 从未打开:调用方据此判空(opened 之类属性经 item 访问)。
    // openAndLoad() 供 Header 的设置按钮调用:先建后开,再拉取设置回填。
    Loader {
        id: settingsDrawerLoader
        active: false
        // ⚠️ 尺寸必须对齐面板根:AppDrawer 用 `parent.width * 0.52` / `parent.height`
        // 给自己定尺寸,而经 Loader 构建后它的 `parent` 就是**本 Loader** ——
        // 不设尺寸会让抽屉拿不到宽高(0×0),打开时不可见。对齐后行为与原先
        // `SettingsDrawer` 直接挂在 root 下完全一致。
        width: root.width
        height: root.height
        sourceComponent: Component { SettingsDrawer { } }
        function openAndLoad() {
            active = true                 // 构建是同步的,置位后 item 立即可用
            item.openAndLoad()
        }
    }

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
