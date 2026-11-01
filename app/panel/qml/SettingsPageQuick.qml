// 设置页 · 快捷功能分类:快捷栏显示开关 + 指令管理(docs/24 §6.2)。
//
// 管理区 = 指令列表(名称/类型徽标/启用/上移/下移/删除)+「添加指令」编辑卡片。
// 编辑卡片是三类型共表单:type 下拉切换可见字段(app: 路径/参数/工作目录;
// system: 目录下拉或 ms-settings 手填;internal: 动作目录下拉)。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    // ---- 编辑卡片状态(编辑中指令的浅拷贝;null = 卡片关) ----
    property var editingCmd: null
    /// 编辑中的图标值("" = 类型默认 / "@X" = 首字 / HeaderIcon kind)。
    property string editingIcon: ""
    /// 编辑的是新增(false = 修改已有):新增保存时生成 id。
    property bool editingNew: false
    /// system 类型:下拉挑选(true)或 ms-settings 手填(false)。
    property bool systemFromList: true
    /// internal 动作目录 / system 目录(Core 转发 QuickCommands,启动后可用)。
    readonly property var actionCatalog: Core.quickActionCatalog()
    readonly property var systemCatalog: Core.quickSystemCatalog()

    function load(s) {
        if (!s) return
        quickRailCheck.checked = s.quickRail !== false
        // settings.quickRailCommands:Vec<QuickCommand> 经 FFI 序列化成
        // QVariantList(QVariantMap);当只读快照用,变更经 railCommands 落 collect。
        railCommands = s.quickRailCommands || []
    }

    /// 工作副本:列表操作(排序/启停/删除)直接改它并 scheduleSave;
    /// load 时重建,collect 时写回。⚠️ 与 settings 快照分离,避免半编辑状态落盘。
    property var railCommands: []

    function collect(s) {
        s.quickRail = quickRailCheck.checked
        s.quickRailCommands = railCommands
    }

    // ---- 列表操作 ----
    function moveRow(index, delta) {
        const to = index + delta
        if (to < 0 || to >= railCommands.length) return
        const arr = railCommands.slice()
        const tmp = arr[index]; arr[index] = arr[to]; arr[to] = tmp
        railCommands = arr
        view.scheduleSave()
    }
    function toggleRow(index) {
        const arr = railCommands.slice()
        const it = Object.assign({}, arr[index])
        it.enabled = !(it.enabled !== false)
        arr[index] = it
        railCommands = arr
        view.scheduleSave()
    }
    function deleteRow(index) {
        const arr = railCommands.slice()
        arr.splice(index, 1)
        railCommands = arr
        view.scheduleSave()
    }
    function openEditor(cmd, isNew) {
        editingCmd = cmd ? Object.assign({}, cmd, {
            target: cmd.target || "", args: cmd.args || "", workdir: cmd.workdir || ""
        }) : null
        editingIcon = cmd && cmd.icon ? cmd.icon : ""
        editingNew = isNew
        if (cmd && cmd.type === "system")
            systemFromList = page.systemCatalog.some(function(e) { return e.key === cmd.target })
        else
            systemFromList = true
        editorCard.visible = true
    }
    function commitEditor() {
        const label = editorLabel.text.trim()
        if (label.length === 0 || label.length > 24) {
            editorError.text = "名称须为 1 ~ 24 个字符"
            return
        }
        const type = editorType.currentValue
        let target = ""
        if (type === "app") {
            target = editorAppPath.text.trim()
            if (target === "") { editorError.text = "请填写应用路径"; return }
        } else if (type === "system") {
            target = systemFromList ? editorSystemCombo.currentValue
                                    : editorSystemUri.text.trim()
            if (target === "") { editorError.text = "请选择或填写系统目标"; return }
            if (!systemFromList && target.indexOf("ms-settings:") !== 0) {
                editorError.text = "手填目标仅支持 ms-settings: 开头的地址"
                return
            }
        } else {
            target = editorActionCombo.currentValue
            if (target === undefined || target === "") { editorError.text = "请选择动作"; return }
        }
        const cmd = {
            id: editingCmd && editingCmd.id ? editingCmd.id : genId(),
            type: type,
            label: label,
            // icon 编码约定:"" = 类型默认;"@" = 首字(随 label);其它 = HeaderIcon kind
            icon: editingIcon,
            target: target,
            args: type === "app" ? editorAppArgs.text : "",
            workdir: type === "app" ? editorAppWorkdir.text : "",
            enabled: editingCmd ? editingCmd.enabled !== false : true
        }
        const arr = railCommands.slice()
        if (editingNew) {
            if (arr.length >= 12) { editorError.text = "最多 12 条指令"; return }
            arr.push(cmd)
        } else {
            for (let i = 0; i < arr.length; ++i)
                if (arr[i].id === cmd.id) { arr[i] = cmd; break }
        }
        railCommands = arr
        editorCard.visible = false
        view.scheduleSave()
    }
    function genId() {
        const hex = Math.floor(Math.random() * 0xffff).toString(16)
        return "qc-" + Date.now() + "-" + ("0000" + hex).slice(-4)
    }
    /// 内置图标词汇(docs/24 §6.3 扩展):精选自 HeaderIcon,类型无关 ——
    /// 用户按用途挑,同类型也可以不同图标。
    readonly property var iconChoices: [
        { kind: "app",       tip: "应用窗口" },
        { kind: "settings",  tip: "齿轮/设置" },
        { kind: "capture",   tip: "截图取景框" },
        { kind: "pin",       tip: "图钉/贴图" },
        { kind: "todo",      tip: "复选框/任务" },
        { kind: "text",      tip: "文本行" },
        { kind: "image",     tip: "图片" },
        { kind: "file",      tip: "文档" },
        { kind: "translate", tip: "地球/翻译" },
        { kind: "network",   tip: "网络节点" },
        { kind: "pause",     tip: "暂停" },
        { kind: "theme",     tip: "明暗切换" },
        { kind: "pencil",    tip: "编辑" },
        { kind: "trash",     tip: "删除" },
        { kind: "keyboard",  tip: "键盘/快捷键" },
        { kind: "plus",      tip: "添加" },
    ]
    /// 首字图标:label 首字符(非 ASCII 也能取)渲染成字母块,天然逐条可辨。
    function firstCharOf(label) {
        const s = (label || "").trim()
        return s.length > 0 ? s.charAt(0) : "?"
    }

    function typeLabel(t) {
        return t === "app" ? "应用" : t === "system" ? "系统" : t === "internal" ? "内部" : t
    }
    function typeBadgeColor(t) {
        return t === "app" ? Theme.colors.sourceCapture
             : t === "system" ? Theme.colors.sourceOcr
             : Theme.colors.sourceTranslate
    }

    /// 图标选择格:kind(HeaderIcon)或 isChar(首字字母块)二选一渲染。
    component IconPickCell: Rectangle {
        id: cell
        property string kind: ""
        property bool isChar: false
        property string charText: ""
        property string tip: ""
        property bool selected: false
        signal picked()

        width: 34; height: 34
        radius: Theme.metrics.radiusInput
        color: selected ? Theme.colors.accentSoft
             : cellMa.containsMouse ? Theme.colors.surfaceHover
             : Theme.colors.surfaceSecondary
        border.width: selected ? 2 : Theme.metrics.borderWidth
        border.color: selected ? Theme.colors.accent : Theme.colors.border
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

        HeaderIcon {
            anchors.centerIn: parent
            visible: !cell.isChar
            kind: cell.kind
            size: 18
            color: cell.selected ? Theme.colors.accent : Theme.colors.textSecondary
        }
        Text {
            anchors.centerIn: parent
            visible: cell.isChar
            text: cell.charText
            font.family: Theme.fontFamily
            font.pixelSize: Theme.metrics.fontBody
            font.weight: Font.DemiBold
            color: cell.selected ? Theme.colors.accent : Theme.colors.textSecondary
        }
        MouseArea {
            id: cellMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: cell.picked()
        }
        ToolTip.visible: cellMa.containsMouse
        ToolTip.delay: 400
        ToolTip.text: cell.tip
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

            // 显示开关:除了落库,还直写 panelWindow 让列表页**即时**生效。
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
                text: "呼出即用的一排功能入口:打开应用、直达系统设置页、触发软件内部动作。"
            }

            SectionDivider {}
            SectionTitle { text: "栏内按钮管理" }

            // ---- 指令列表 ----
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingXs

                Repeater {
                    model: page.railCommands
                    delegate: Rectangle {
                        id: cmdRow
                        required property var modelData
                        required property int index
                        objectName: "quickCmdRow_" + modelData.id
                        Layout.fillWidth: true
                        implicitHeight: 36
                        radius: Theme.metrics.radiusInput
                        color: Theme.colors.surfaceSecondary
                        border.width: Theme.metrics.borderWidth
                        border.color: Theme.colors.border
                        opacity: modelData.enabled !== false ? 1.0 : 0.45

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.metrics.spacingSm
                            anchors.rightMargin: Theme.metrics.spacingSm
                            spacing: Theme.metrics.spacingSm

                            Text {
                                text: cmdRow.modelData.label
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.metrics.fontSecondary
                                font.weight: Font.DemiBold
                                color: Theme.colors.textPrimary
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            // 类型徽标(来源徽标同语言:彩色小字 + 透明底)
                            Rectangle {
                                implicitWidth: typeText.implicitWidth + 10
                                implicitHeight: 18
                                radius: Theme.metrics.radiusBadge
                                color: Qt.rgba(page.typeBadgeColor(cmdRow.modelData.type).r,
                                               page.typeBadgeColor(cmdRow.modelData.type).g,
                                               page.typeBadgeColor(cmdRow.modelData.type).b, 0.15)
                                Text {
                                    id: typeText
                                    anchors.centerIn: parent
                                    text: page.typeLabel(cmdRow.modelData.type)
                                    font.pixelSize: Theme.metrics.fontMicro
                                    color: page.typeBadgeColor(cmdRow.modelData.type)
                                }
                            }
                            // 启用开关(停用的行整行置灰,配置宁可灰不可丢)
                            CheckBox {
                                objectName: "quickCmdToggle_" + cmdRow.modelData.id
                                checked: cmdRow.modelData.enabled !== false
                                onToggled: page.toggleRow(cmdRow.index)
                                ToolTip.visible: hovered
                                ToolTip.text: checked ? "已启用(点击停用)" : "已停用(点击启用)"
                            }
                            // ↑↓ 排序(数组顺序 = 栏内显示顺序;不做拖拽,docs/24 §6.2)
                            ThemedButton {
                                objectName: "quickCmdUp_" + cmdRow.modelData.id
                                variant: "ghost"; square: true; size: "sm"
                                text: "↑"
                                enabled: cmdRow.index > 0
                                onClicked: page.moveRow(cmdRow.index, -1)
                            }
                            ThemedButton {
                                objectName: "quickCmdDown_" + cmdRow.modelData.id
                                variant: "ghost"; square: true; size: "sm"
                                text: "↓"
                                enabled: cmdRow.index < page.railCommands.length - 1
                                onClicked: page.moveRow(cmdRow.index, 1)
                            }
                            ThemedButton {
                                objectName: "quickCmdEdit_" + cmdRow.modelData.id
                                variant: "ghost"; square: true; size: "sm"
                                iconName: "pencil"
                                onClicked: page.openEditor(cmdRow.modelData, false)
                            }
                            ThemedButton {
                                objectName: "quickCmdDelete_" + cmdRow.modelData.id
                                variant: "ghost"; square: true; size: "sm"
                                iconName: "trash"
                                onClicked: page.deleteRow(cmdRow.index)
                            }
                        }
                    }
                }

                ThemedButton {
                    objectName: "quickCmdAdd"
                    variant: "secondary"; size: "sm"
                    iconName: "plus"
                    text: "添加指令"
                    onClicked: page.openEditor(null, true)
                }
                HintLabel {
                    visible: page.railCommands.length === 0
                    text: "还没有指令。添加后在主面板右侧的快捷栏点击即可执行;"
                          + "最多 12 条,顺序即栏内从上到下。"
                }
            }
        }
    }

    // ---- 编辑卡片(屏障 + 居中卡片,复用面板卡片模式) ----
    MouseArea {
        id: editorBarrier
        anchors.fill: parent
        z: 950
        visible: editorCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: editorCard.visible = false
    }
    Rectangle {
        id: editorCard
        objectName: "quickCmdEditor"
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(420, page.width - 48)
        height: editorLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: editorLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: page.editingNew ? "添加快捷指令" : "编辑快捷指令"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
            }

            // 类型(编辑已有指令时锁定:改类型等于换语义,删了重建更清晰)
            FieldLabel { text: "类型" }
            ReadableComboBox {
                id: editorType
                objectName: "quickCmdType"
                Layout.fillWidth: true
                model: [
                    { value: "app", label: "应用(打开程序或文件夹)" },
                    { value: "system", label: "系统(直达系统设置页/工具)" },
                    { value: "internal", label: "内部(软件功能动作)" }
                ]
                textRole: "label"
                valueRole: "value"
                enabled: page.editingNew
            }

            // 名称
            FieldLabel { text: "名称(显示在按钮提示里)" }
            ReadableTextField {
                id: editorLabel
                objectName: "quickCmdLabel"
                Layout.fillWidth: true
                maximumLength: 24
                placeholderText: "例如:打开服务"
            }

            // 图标:内置词汇网格 + 首字。空串 = 按类型给默认(docs/24 §6.3)。
            FieldLabel { text: "图标" }
            Flow {
                id: iconFlow
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingXs
                // 首字选项(始终第一颗):"@首字" 编码,渲染层画字母块
                IconPickCell {
                    objectName: "quickCmdIconFirst"
                    isChar: true
                    charText: page.firstCharOf(editorLabel.text)
                    tip: "用名称首字"
                    selected: page.editingIcon === ""
                              || page.editingIcon.indexOf("@") === 0
                    onPicked: page.editingIcon = "@" + page.firstCharOf(editorLabel.text)
                }
                Repeater {
                    model: page.iconChoices
                    delegate: IconPickCell {
                        required property var modelData
                        kind: modelData.kind
                        tip: modelData.tip
                        selected: page.editingIcon === modelData.kind
                        onPicked: page.editingIcon = modelData.kind
                    }
                }
            }
            HintLabel {
                text: "「首字」随名称变化;不选则按指令类型自动分配。"
            }

            // ---- app 表单 ----
            ColumnLayout {
                visible: editorType.currentValue === "app"
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingXs
                FieldLabel { text: "应用路径(.exe / .lnk / 文件夹)" }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ReadableTextField {
                        id: editorAppPath
                        objectName: "quickCmdAppPath"
                        Layout.fillWidth: true
                        placeholderText: "C:\\Program Files\\...\\app.exe"
                    }
                    ThemedButton {
                        objectName: "quickCmdBrowseApp"
                        variant: "secondary"; size: "sm"
                        text: "浏览…"
                        onClicked: {
                            // 选文件(exe/lnk);用户取消(空串)时不覆盖已填内容
                            const f = Core.openFileDialog(
                                "选择应用程序",
                                "应用程序 (*.exe *.lnk *.bat *.cmd);;所有文件 (*.*)")
                            if (f !== "") editorAppPath.text = f
                        }
                    }
                    ThemedButton {
                        // 文件夹目标走目录选择器(只列目录,比文件对话框直接)
                        objectName: "quickCmdBrowseDir"
                        variant: "secondary"; size: "sm"
                        text: "文件夹…"
                        onClicked: {
                            const d = Core.openDirectoryDialog("选择文件夹(作为指令目标)")
                            if (d !== "") editorAppPath.text = d
                        }
                    }
                }
                FieldLabel { text: "启动参数(可选,原样传递)" }
                ReadableTextField {
                    id: editorAppArgs
                    objectName: "quickCmdAppArgs"
                    Layout.fillWidth: true
                    placeholderText: "例如:--profile work"
                }
                FieldLabel { text: "工作目录(可选,默认取应用所在目录)" }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ReadableTextField {
                        id: editorAppWorkdir
                        objectName: "quickCmdAppWorkdir"
                        Layout.fillWidth: true
                    }
                    ThemedButton {
                        objectName: "quickCmdBrowseWorkdir"
                        variant: "secondary"; size: "sm"
                        text: "浏览…"
                        onClicked: {
                            const d = Core.openDirectoryDialog("选择工作目录")
                            if (d !== "") editorAppWorkdir.text = d
                        }
                    }
                }
            }

            // ---- system 表单 ----
            ColumnLayout {
                visible: editorType.currentValue === "system"
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingXs
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ReadableComboBox {
                        id: editorSystemCombo
                        objectName: "quickCmdSystemCombo"
                        Layout.fillWidth: true
                        model: page.systemCatalog
                        textRole: "label"
                        valueRole: "key"
                        enabled: page.systemFromList
                    }
                    ReadableCheckBox {
                        id: systemUriToggle
                        objectName: "quickCmdSystemUriToggle"
                        text: "ms-settings 手填"
                        checked: !page.systemFromList
                        onClicked: {
                            page.systemFromList = !page.systemFromList
                            systemUriToggle.checked = !page.systemFromList
                        }
                    }
                }
                ReadableTextField {
                    id: editorSystemUri
                    objectName: "quickCmdSystemUri"
                    Layout.fillWidth: true
                    visible: !page.systemFromList
                    placeholderText: "ms-settings:display 等(仅支持此格式)"
                }
            }

            // ---- internal 表单 ----
            ColumnLayout {
                visible: editorType.currentValue === "internal"
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingXs
                FieldLabel { text: "动作" }
                ReadableComboBox {
                    id: editorActionCombo
                    objectName: "quickCmdActionCombo"
                    Layout.fillWidth: true
                    model: page.actionCatalog
                    textRole: "label"
                    valueRole: "id"
                }
                HintLabel { text: "内部动作随软件版本增加;不可用的动作执行时会提示。" }
            }

            Text {
                id: editorError
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.error
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"; size: "sm"
                    text: "取消"
                    onClicked: editorCard.visible = false
                }
                ThemedButton {
                    objectName: "quickCmdSave"
                    variant: "primary"; size: "sm"
                    text: "保存"
                    onClicked: page.commitEditor()
                }
            }
        }
        // 打开时把 editingCmd 预填进各字段(集中在 onVisibleChanged:
        // 字段 id 都在本作用域,openEditor 不必引用每个控件)
        onVisibleChanged: {
            editorError.text = ""
            if (!visible) return
            const c = page.editingCmd
            editorLabel.text = c ? c.label : ""
            editorAppPath.text = c ? c.target : ""
            editorAppArgs.text = c ? c.args : ""
            editorAppWorkdir.text = c ? c.workdir : ""
            editorSystemUri.text = (c && c.type === "system" && !page.systemFromList)
                                   ? c.target : ""
            if (c) {
                for (let i = 0; i < editorType.count; ++i)
                    if (editorType.valueAt(i) === c.type) { editorType.currentIndex = i; break }
                for (let j = 0; j < editorSystemCombo.count; ++j)
                    if (editorSystemCombo.valueAt(j) === c.target)
                        editorSystemCombo.currentIndex = j
                for (let k = 0; k < editorActionCombo.count; ++k)
                    if (editorActionCombo.valueAt(k) === c.target)
                        editorActionCombo.currentIndex = k
            } else {
                editorType.currentIndex = 0
                editorSystemCombo.currentIndex = 0
                editorActionCombo.currentIndex = 0
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
