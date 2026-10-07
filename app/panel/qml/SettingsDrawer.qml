// 设置抽屉:通用设置 + 翻译服务配置,读写 nbcore settings。
//
// 内容较多,故整体放在 Flickable 里可滚动;抽屉宽度只有主面板的 46%,不滚动会溢出。
// 翻译配置是必需项:core 侧会读 provider/apiKey 等,但此前没有任何界面入口,
// 只能手改 app_config.json —— 等于翻译功能不可用。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

AppDrawer {
    id: root

    /// 服务提供方,取值与 core 的 Provider::parse 一致
    readonly property var providers: ["openai", "microsoft", "custom"]
    readonly property string provider: providers[Math.max(0, providerCombo.currentIndex)]

    /// 代理是否处于手动配置模式(控制 manual 字段组的可见性)。
    readonly property bool isProxyManual: proxyModeCombo.currentIndex === 2

    /// 快捷键是否被改过 —— 只有改过才重新注册,避免每次自动保存都注销/重注册一遍。
    property bool hotkeysDirty: false

    /// 当前取色目标("" / "bg" / "color")——两个颜色字段共用一个色板弹层。
    property string paletteTarget: ""

    /// 数据目录是否被改过(改动需重启生效,保存后要显式提示,不能混入静默自动保存)。
    property bool dataDirDirty: false

    /// 改动去抖后自动落盘(不再有底部「保存」按钮)。
    /// 各控件只接**用户操作**信号(valueModified / clicked / textEdited / activated),
    /// 因此加载时回填值不会误触发保存,也就不需要 loading 守卫。
    Timer {
        id: autoSave
        interval: 400
        onTriggered: root.save()
    }

    function scheduleSave() {
        autoSave.restart()
    }

    /// 快捷键录制控件:点击进入录制态,按下组合键即捕获。
    /// 产出形如 "Ctrl+Shift+V" 的字符串;合法性(至少一个修饰键、各动作两两不重复、
    /// 不可为空)由 core 的 settings 校验负责 —— 这里不做客户端校验,直接展示后端错误。
    component HotkeyField: Item {
        id: hk
        property string value: ""
        property bool recording: false
        /// 录制完成、值真的变了才发(用于触发自动保存与热键重注册)。
        signal valueModified()
        implicitHeight: 30

        function keyName(key) {
            if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(key)
            if (key >= Qt.Key_0 && key <= Qt.Key_9) return String.fromCharCode(key)
            if (key >= Qt.Key_F1 && key <= Qt.Key_F24) return "F" + (key - Qt.Key_F1 + 1)
            return ""
        }

        Rectangle {
            anchors.fill: parent
            radius: 6
            color: Theme.surface2
            border.width: 1
            border.color: hk.recording ? Theme.accent : Theme.border
            Text {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                font.pixelSize: 12
                color: hk.recording ? Theme.accent
                     : hk.value === "" ? Theme.muted : Theme.text
                text: hk.recording ? "按下组合键…(Esc 取消)" : (hk.value || "未设置")
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                hk.recording = true
                hk.forceActiveFocus()
            }
        }

        Keys.onPressed: function(event) {
            if (!hk.recording) return
            if (event.key === Qt.Key_Escape) {
                hk.recording = false
                event.accepted = true
                return
            }
            // 只按修饰键时继续等待主键
            if (event.key === Qt.Key_Control || event.key === Qt.Key_Shift
                    || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta) {
                event.accepted = true
                return
            }
            const name = hk.keyName(event.key)
            if (name === "") {
                event.accepted = true
                return
            }
            const parts = []
            if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
            if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
            if (event.modifiers & Qt.AltModifier) parts.push("Alt")
            if (event.modifiers & Qt.MetaModifier) parts.push("Win")
            parts.push(name)
            const next = parts.join("+")
            const changed = next !== hk.value
            hk.value = next
            hk.recording = false
            if (changed) hk.valueModified()
            event.accepted = true
        }
    }

    /// 内联色板(36 色常用色)。
    ///
    /// ⚠️ 两点刻意的取舍,都是踩过的坑:
    /// 1. **不用 `QtQuick.Dialogs.ColorDialog`** —— windeployqt 不会部署该模块(QML 已
    ///    编译进 qrc,它扫描不到 import),运行时 `import QtQuick.Dialogs` 直接报
    ///    "module is not installed" → 整个 SettingsDrawer 加载失败 → **主面板空白**。
    /// 2. **也不用 `Popup`** —— 外层 SettingsDrawer 本身就是 Popup,嵌套 Popup 的定位与
    ///    事件派发不可靠(实测点色块没反应)。内联网格没有这个问题,而且设置页本来就可
    ///    滚动,就地展开比浮层更自然。
    component ColorSwatchGrid: GridLayout {
        id: grid
        /// 当前值,用于高亮已选中的色块。
        property string current: "#000000"
        signal picked(string color)

        readonly property var swatches: [
            "#000000", "#3b3b3b", "#6b6b6b", "#9a9a9a", "#c8c8c8", "#ffffff",
            "#e03131", "#f76707", "#fab009", "#40c057", "#1c7ed6", "#7048e8",
            "#ff8787", "#ffa94d", "#ffe066", "#8ce99a", "#74c0fc", "#b197fc",
            "#a61e1e", "#b35309", "#a37b00", "#2b8a3e", "#1552a3", "#4c2f9e",
            "#1e1f24", "#26272e", "#2d2e37", "#33353d", "#dfe1e8", "#9aa0ab",
            "#7ec2ff", "#c9a7ff", "#e8b339", "#e5534b", "#4f8cff", "#2f6fe4"
        ]

        columns: 6
        rowSpacing: 4
        columnSpacing: 4
        Repeater {
            model: grid.swatches
            delegate: Rectangle {
                readonly property bool isCurrent:
                    String(modelData).toLowerCase() === grid.current.toLowerCase()
                implicitWidth: 24
                implicitHeight: 24
                radius: 3
                color: modelData
                border.width: isCurrent ? 2 : 1
                border.color: isCurrent ? Theme.accent : Theme.border
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: grid.picked(modelData)
                }
            }
        }
    }

    function errText(resp) {
        return resp && resp.error ? resp.error.message : ""
    }

    /// 仅用于色块预览:非法值不喂给 color 属性(否则每敲一个字符就刷一条绑定警告)。
    function isHexColor(value) {
        return /^#[0-9a-fA-F]{6}$/.test(value)
    }

    function openAndLoad() {
        open()
        const resp = Core.call("get_settings")
        if (!resp.ok) {
            statusLabel.text = "读取失败:" + errText(resp)
            statusLabel.isError = true
            return
        }
        const s = resp.data
        historyLimit.value = s.historyLimit
        maxImage.value = s.maxImageMb
        monitorCheck.checked = s.monitorEnabled === true
        ocrCheck.checked = s.ocrEnabled === true
        skipEditorCheck.checked = s.regionSkipEditor === true
        autoConfirmCheck.checked = s.regionAutoConfirm === true
        rightClickCancelCheck.checked = s.regionRightClickCancel === true
        themeCombo.currentIndex = s.theme === "light" ? 1 : s.theme === "dark" ? 2 : 0
        dataDirInput.text = s.dataDir || ""
        root.dataDirDirty = false

        pinOpacity.value = s.pinDefaultOpacity > 0 ? s.pinDefaultOpacity : 100
        pinDoubleClick.checked = s.pinDoubleclickClose === true
        pinTextBg.text = s.pinTextBg || ""
        pinTextColor.text = s.pinTextColor || ""
        pinTextBgOpacity.value = s.pinTextBgOpacity >= 0 ? s.pinTextBgOpacity : 92
        pinTextFontSize.value = s.pinTextFontSize > 0 ? s.pinTextFontSize : 15
        pinTextMaxWidth.value = s.pinTextMaxWidth > 0 ? s.pinTextMaxWidth : 520

        const hk = s.hotkeys || {}
        hkToggleMain.value = hk.toggleMain || ""
        hkCaptureRegion.value = hk.captureRegion || ""
        hkCaptureFullscreen.value = hk.captureFullscreen || ""
        hkPinLatest.value = hk.pinLatest || ""
        hkOpenTranslate.value = hk.openTranslate || ""
        hkCaptureTranslate.value = hk.captureTranslate || ""

        const index = providers.indexOf(s.translateProvider)
        providerCombo.currentIndex = index < 0 ? 0 : index

        const oa = s.translateOpenai || {}
        oaBaseUrl.text = oa.baseUrl || ""
        oaApiKey.text = oa.apiKey || ""
        oaModel.text = oa.model || ""

        const ms = s.translateMicrosoft || {}
        msEndpoint.text = ms.endpoint || ""
        msApiKey.text = ms.apiKey || ""
        msRegion.text = ms.region || ""

        const cu = s.translateCustom || {}
        cuUrl.text = cu.url || ""
        cuMethod.currentIndex = (cu.method === "GET") ? 1 : 0
        cuApiKey.text = cu.apiKey || ""
        cuModel.text = cu.model || ""
        cuHeaders.text = cu.headers || ""
        cuBody.text = cu.bodyTemplate || ""
        cuPath.text = cu.responsePath || ""

        timeoutSpin.value = s.translateTimeoutSecs > 0 ? s.translateTimeoutSecs : 30
        const proxyMode = (s.proxy && s.proxy.mode) || "system"
        proxyModeCombo.currentIndex = proxyMode === "none" ? 1 : proxyMode === "manual" ? 2 : 0
        const pm = (s.proxy && s.proxy.manual) || {}
        proxyKindCombo.currentIndex = pm.kind === "socks5" ? 1 : 0
        proxyHost.text = pm.host || ""
        proxyPort.value = pm.port > 0 ? pm.port : 0
        proxyUser.text = pm.username || ""
        proxyPassword.text = pm.password || ""
        proxyBypass.text = pm.bypass || ""
        probeLabel.text = ""
        proxyProbeLabel.text = ""
        statusLabel.text = ""
    }

    function save() {
        const cur = Core.call("get_settings")
        if (!cur.ok) {
            statusLabel.text = "保存失败:" + errText(cur)
            statusLabel.isError = true
            return
        }
        const s = cur.data
        s.historyLimit = historyLimit.value
        s.maxImageMb = maxImage.value
        s.monitorEnabled = monitorCheck.checked
        s.ocrEnabled = ocrCheck.checked
        s.regionSkipEditor = skipEditorCheck.checked
        s.regionAutoConfirm = autoConfirmCheck.checked
        s.regionRightClickCancel = rightClickCancelCheck.checked
        s.theme = ["system", "light", "dark"][themeCombo.currentIndex]
        s.dataDir = dataDirInput.text.trim()

        s.pinDefaultOpacity = pinOpacity.value
        s.pinDoubleclickClose = pinDoubleClick.checked
        s.pinTextBg = pinTextBg.text
        s.pinTextColor = pinTextColor.text
        s.pinTextBgOpacity = pinTextBgOpacity.value
        s.pinTextFontSize = pinTextFontSize.value
        s.pinTextMaxWidth = pinTextMaxWidth.value

        const hk = s.hotkeys || {}
        hk.toggleMain = hkToggleMain.value
        hk.captureRegion = hkCaptureRegion.value
        hk.captureFullscreen = hkCaptureFullscreen.value
        hk.pinLatest = hkPinLatest.value
        hk.openTranslate = hkOpenTranslate.value
        hk.captureTranslate = hkCaptureTranslate.value
        s.hotkeys = hk

        s.translateProvider = provider
        const oa = s.translateOpenai || {}
        oa.baseUrl = oaBaseUrl.text
        oa.apiKey = oaApiKey.text
        oa.model = oaModel.text
        s.translateOpenai = oa

        const ms = s.translateMicrosoft || {}
        ms.endpoint = msEndpoint.text
        ms.apiKey = msApiKey.text
        ms.region = msRegion.text
        s.translateMicrosoft = ms

        const cu = s.translateCustom || {}
        cu.url = cuUrl.text
        cu.method = cuMethod.currentIndex === 1 ? "GET" : "POST"
        cu.apiKey = cuApiKey.text
        cu.model = cuModel.text
        cu.headers = cuHeaders.text
        cu.bodyTemplate = cuBody.text
        cu.responsePath = cuPath.text
        s.translateCustom = cu

        s.translateTimeoutSecs = timeoutSpin.value

        s.proxy = {
            mode: ["system", "none", "manual"][proxyModeCombo.currentIndex],
            manual: {
                kind: proxyKindCombo.currentIndex === 1 ? "socks5" : "http",
                host: proxyHost.text.trim(),
                port: proxyPort.value,
                username: proxyUser.text,
                password: proxyPassword.text,
                bypass: proxyBypass.text
            }
        }

        const resp = Core.call("save_settings", s)
        statusLabel.text = resp.ok ? "已保存" : "保存失败:" + errText(resp)
        statusLabel.isError = !resp.ok
        if (resp.ok) {
            if (root.dataDirDirty) {
                statusLabel.text = "已保存。数据目录将在下次启动时生效(数据库与图片自动迁移)"
                statusLabel.isError = false
                root.dataDirDirty = false
            }
            Core.applyTheme(s.theme)   // 幂等:主题没变时不会重刷
            // 只有快捷键真的改过才重新注册 —— 每次自动保存都注销/重注册一遍会留下
            // 短暂的空窗,期间按热键会丢。
            if (root.hotkeysDirty) {
                Core.reloadHotkeys()
                root.hotkeysDirty = false
            }
            Entries.refreshNow()
        }
    }

    function probeProxy() {
        // 先保存再探测:proxy_probe 读的是已保存配置,不保存则刚填的代理不参与
        root.save()
        proxyProbeLabel.text = "测试中…"
        proxyProbeLabel.isError = false
        Core.probeProxy()
    }

    function probe() {
        probeLabel.text = "测试中…"
        probeLabel.isError = false
        Core.probeTranslate(provider)   // 网络请求:异步,完成走 translateProbeFinished
    }

    contentItem: Flickable {
        id: flick
        clip: true
        contentWidth: width
        contentHeight: form.implicitHeight + 12
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: form
            width: flick.width
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                Label { text: "设置"; font.pixelSize: 15; font.bold: true; color: Theme.text }
                Item { Layout.fillWidth: true }
                ToolButton { text: "✕"; onClicked: root.close() }
            }

            Label { text: "历史上限(条)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: historyLimit
                Layout.fillWidth: true
                from: 100; to: 100000; stepSize: 500
                editable: true
                onValueModified: root.scheduleSave()
            }

            Label { text: "单张图片上限(MB)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: maxImage
                Layout.fillWidth: true
                from: 1; to: 200
                editable: true
                onValueModified: root.scheduleSave()
            }

            CheckBox {
                id: monitorCheck
                text: "剪贴板监听"
                onClicked: root.scheduleSave()
            }
            CheckBox {
                id: ocrCheck
                text: "OCR 自动识别(截图文字可搜索)"
                onClicked: root.scheduleSave()
            }
            // 取消勾选后,框选截图会先进编辑器标注,「完成」才入库 + 写剪贴板
            CheckBox {
                id: skipEditorCheck
                text: "截图后直接入库(跳过编辑器)"
                onClicked: root.scheduleSave()
            }
            // 取消勾选后,选区保留并可拖 8 向手柄微调,Enter 或双击才确认
            CheckBox {
                id: autoConfirmCheck
                text: "框选后松开鼠标即确认"
                onClicked: root.scheduleSave()
            }
            CheckBox {
                id: rightClickCancelCheck
                text: "框选时鼠标右键取消截图"
                onClicked: root.scheduleSave()
            }

            Label { text: "主题"; color: Theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: themeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "浅色", "深色"]
                // 切换即生效(不必等保存),给用户即时反馈
                onActivated: {
                    Core.applyTheme(["system", "light", "dark"][currentIndex])
                    root.scheduleSave()
                }
            }

            // ---- 数据 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            Label { text: "数据"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            CheckBox {
                id: autoStartCheck
                text: "开机自启(写入当前用户注册表 Run 键)"
                checked: Core.autoStartEnabled()
                onClicked: {
                    // 注册表操作立即生效并回读实际结果,失败时还原勾选态
                    if (Core.setAutoStart(checked)) {
                        checked = Core.autoStartEnabled()
                    } else {
                        checked = !checked
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: "自定义数据目录(留空 = 默认位置)。变更后重启应用生效:数据库与图片自动迁移到新目录,原目录数据保留。"
                color: Theme.muted
                font.pixelSize: 11
                wrapMode: Text.WrapAnywhere
            }
            TextField {
                id: dataDirInput
                Layout.fillWidth: true
                placeholderText: "D:\\Data\\Nextboard"
                onTextEdited: {
                    root.dataDirDirty = true
                    root.scheduleSave()
                }
            }

            // ---- 贴图 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            Label { text: "贴图"; font.pixelSize: 14; font.bold: true; color: Theme.text }

            Label { text: "默认不透明度(%)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: pinOpacity
                Layout.fillWidth: true
                from: 30; to: 100; stepSize: 5
                editable: true
                onValueModified: root.scheduleSave()
            }
            CheckBox {
                id: pinDoubleClick
                text: "双击关闭贴图窗口"
                onClicked: root.scheduleSave()
            }

            Label { text: "文本贴图 · 背景色(点色块展开色板)"; color: Theme.muted; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 34; implicitHeight: 22; radius: 4
                    color: root.isHexColor(pinTextBg.text) ? pinTextBg.text : Theme.surface2
                    border.color: root.paletteTarget === "bg" ? Theme.accent : Theme.border
                    border.width: root.paletteTarget === "bg" ? 2 : 1
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.paletteTarget = root.paletteTarget === "bg" ? "" : "bg"
                    }
                }
                TextField {
                    id: pinTextBg
                    Layout.fillWidth: true
                    placeholderText: "#26272e"
                    onTextEdited: root.scheduleSave()
                }
            }
            // 就地展开的色板(不用 Popup,原因见 ColorSwatchGrid 注释)
            ColorSwatchGrid {
                Layout.fillWidth: true
                visible: root.paletteTarget === "bg"
                current: pinTextBg.text
                onPicked: function(color) {
                    pinTextBg.text = color
                    root.paletteTarget = ""
                    root.scheduleSave()
                }
            }
            Label { text: "文本贴图 · 文字色(点色块展开色板)"; color: Theme.muted; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 34; implicitHeight: 22; radius: 4
                    color: root.isHexColor(pinTextColor.text) ? pinTextColor.text : Theme.surface2
                    border.color: root.paletteTarget === "color" ? Theme.accent : Theme.border
                    border.width: root.paletteTarget === "color" ? 2 : 1
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.paletteTarget = root.paletteTarget === "color" ? "" : "color"
                    }
                }
                TextField {
                    id: pinTextColor
                    Layout.fillWidth: true
                    placeholderText: "#dfe1e8"
                    onTextEdited: root.scheduleSave()
                }
            }
            ColorSwatchGrid {
                Layout.fillWidth: true
                visible: root.paletteTarget === "color"
                current: pinTextColor.text
                onPicked: function(color) {
                    pinTextColor.text = color
                    root.paletteTarget = ""
                    root.scheduleSave()
                }
            }
            Label { text: "文本贴图 · 背景不透明度(%)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: pinTextBgOpacity
                Layout.fillWidth: true
                from: 0; to: 100; stepSize: 5
                editable: true
                onValueModified: root.scheduleSave()
            }
            Label { text: "文本贴图 · 字号(像素)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: pinTextFontSize
                Layout.fillWidth: true
                from: 8; to: 96
                editable: true
                onValueModified: root.scheduleSave()
            }
            Label { text: "文本贴图 · 最大宽度(像素)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: pinTextMaxWidth
                Layout.fillWidth: true
                from: 120; to: 2000; stepSize: 40
                editable: true
                onValueModified: root.scheduleSave()
            }

            // ---- 快捷键 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            Label { text: "快捷键"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            Label {
                Layout.fillWidth: true
                text: "点击后按下组合键即可录制。需含至少一个修饰键(Ctrl/Alt/Shift/Win),单按 F1–F24 也可;保存后立即生效。"
                color: Theme.muted
                font.pixelSize: 11
                wrapMode: Text.WrapAnywhere
            }
            Label { text: "呼出/隐藏面板"; color: Theme.muted; font.pixelSize: 12 }
            HotkeyField {
                id: hkToggleMain
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            Label { text: "矩形截图"; color: Theme.muted; font.pixelSize: 12 }
            HotkeyField {
                id: hkCaptureRegion
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            Label { text: "全屏截图"; color: Theme.muted; font.pixelSize: 12 }
            HotkeyField {
                id: hkCaptureFullscreen
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            Label { text: "一键贴图"; color: Theme.muted; font.pixelSize: 12 }
            HotkeyField {
                id: hkPinLatest
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            Label { text: "打开翻译"; color: Theme.muted; font.pixelSize: 12 }
            HotkeyField {
                id: hkOpenTranslate
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            Label { text: "截图翻译"; color: Theme.muted; font.pixelSize: 12 }
            HotkeyField {
                id: hkCaptureTranslate
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }

            // ---- 翻译服务 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            Label { text: "翻译服务"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            ComboBox {
                id: providerCombo
                Layout.fillWidth: true
                model: ["OpenAI 兼容接口", "微软翻译", "自定义 HTTP"]
                onActivated: root.scheduleSave()
            }

            // OpenAI 兼容(含国内兼容端点)
            Label { text: "接口地址"; visible: root.provider === "openai"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: oaBaseUrl
                onTextEdited: root.scheduleSave()
                visible: root.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "https://api.openai.com/v1"
            }
            Label { text: "API Key"; visible: root.provider === "openai"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: oaApiKey
                onTextEdited: root.scheduleSave()
                visible: root.provider === "openai"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "sk-…"
            }
            Label { text: "模型"; visible: root.provider === "openai"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: oaModel
                onTextEdited: root.scheduleSave()
                visible: root.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "gpt-4o-mini"
            }

            // 微软翻译
            Label { text: "接口地址"; visible: root.provider === "microsoft"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: msEndpoint
                onTextEdited: root.scheduleSave()
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                placeholderText: "https://api.translator.azure.cn"
            }
            Label { text: "API Key"; visible: root.provider === "microsoft"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: msApiKey
                onTextEdited: root.scheduleSave()
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                echoMode: TextInput.Password
            }
            Label { text: "区域(可选)"; visible: root.provider === "microsoft"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: msRegion
                onTextEdited: root.scheduleSave()
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                placeholderText: "如 eastasia"
            }

            // 自定义 HTTP
            Label { text: "请求地址"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuUrl
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "https://…"
            }
            Label { text: "请求方法"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: cuMethod
                visible: root.provider === "custom"
                Layout.fillWidth: true
                model: ["POST", "GET"]
                onActivated: root.scheduleSave()
            }
            Label { text: "API Key(可选)"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuApiKey
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                echoMode: TextInput.Password
            }
            Label { text: "模型(可选)"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuModel
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
            }
            Label { text: "请求头(每行一条 Name: Value)"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextArea {
                id: cuHeaders
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 54
                wrapMode: TextArea.WrapAnywhere
                placeholderText: "Content-Type: application/json"
            }
            Label { text: "请求体模板"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextArea {
                id: cuBody
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 66
                wrapMode: TextArea.WrapAnywhere
                placeholderText: "可用 ${text} ${from} ${to} ${apiKey} ${model}"
            }
            Label { text: "响应取值路径"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuPath
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "如 data.translations[0].text"
            }

            Label { text: "超时(秒)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: timeoutSpin
                Layout.fillWidth: true
                from: 5; to: 120
                editable: true
                onValueModified: root.scheduleSave()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button { text: "测试连接"; onClicked: root.probe() }
                Item { Layout.fillWidth: true }
            }
            Label {
                id: probeLabel
                property bool isError: false
                Layout.fillWidth: true
                color: isError ? Theme.danger : Theme.muted
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }

            // ---- 网络与代理 ----
            // core 的 proxy 校验/分派/proxy_probe 早已就绪,这里只补 UI 入口。
            // manual 字段始终提交,与 core 一致:仅 mode=manual 时才参与校验与生效。
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            Label { text: "网络与代理"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            Label { text: "代理模式(作用于翻译等网络请求)"; color: Theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: proxyModeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "直连(不使用代理)", "手动配置"]
                onActivated: root.scheduleSave()
            }
            Label { text: "协议类型"; visible: root.isProxyManual; color: Theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: proxyKindCombo
                visible: root.isProxyManual
                Layout.fillWidth: true
                model: ["HTTP", "SOCKS5"]
                onActivated: root.scheduleSave()
            }
            Label { text: "服务器地址"; visible: root.isProxyManual; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: proxyHost
                visible: root.isProxyManual
                Layout.fillWidth: true
                placeholderText: "127.0.0.1"
                onTextEdited: root.scheduleSave()
            }
            Label { text: "端口"; visible: root.isProxyManual; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: proxyPort
                visible: root.isProxyManual
                Layout.fillWidth: true
                from: 0; to: 65535
                editable: true
                onValueModified: root.scheduleSave()
            }
            Label { text: "用户名(可选)"; visible: root.isProxyManual; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: proxyUser
                visible: root.isProxyManual
                Layout.fillWidth: true
                onTextEdited: root.scheduleSave()
            }
            Label { text: "密码(可选)"; visible: root.isProxyManual; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: proxyPassword
                visible: root.isProxyManual
                Layout.fillWidth: true
                echoMode: TextInput.Password
                onTextEdited: root.scheduleSave()
            }
            // SOCKS5 路径(ureq)不支持绕过列表,仅 HTTP 显示该字段(与 core 注释一致)
            Label { text: "绕过列表(仅 HTTP,分号分隔)"; visible: root.isProxyManual && proxyKindCombo.currentIndex === 0; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: proxyBypass
                visible: root.isProxyManual && proxyKindCombo.currentIndex === 0
                Layout.fillWidth: true
                placeholderText: "localhost;127.0.0.1;*.internal"
                onTextEdited: root.scheduleSave()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button { text: "测试代理链路"; onClicked: root.probeProxy() }
                Item { Layout.fillWidth: true }
            }
            Label {
                id: proxyProbeLabel
                property bool isError: false
                Layout.fillWidth: true
                color: isError ? Theme.danger : Theme.muted
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }

            // 改动即时保存(去抖 400ms),故不再有「保存 / 关闭」按钮 ——
            // 关闭抽屉走标题栏的 ✕。状态行只用于反馈结果(尤其是校验失败)。
            Label {
                id: statusLabel
                property bool isError: false
                Layout.fillWidth: true
                Layout.topMargin: 12
                color: isError ? Theme.danger : Theme.muted
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }
        }
    }

    // 探测结果回投(异步,避免同步网络请求冻结界面)
    Connections {
        target: Core
        function onTranslateProbeFinished(resp) {
            if (!resp.ok) {
                probeLabel.text = "测试失败:" + root.errText(resp)
                probeLabel.isError = true
                return
            }
            const d = resp.data || {}
            probeLabel.text = (d.available ? "✓ " : "✗ ") + (d.message || "")
            probeLabel.isError = !d.available
        }
        function onProxyProbeFinished(resp) {
            if (!resp.ok) {
                proxyProbeLabel.text = "测试失败:" + root.errText(resp)
                proxyProbeLabel.isError = true
                return
            }
            const d = resp.data || {}
            const head = d.detail ? d.detail + " · " : ""
            proxyProbeLabel.text = (d.ok ? "✓ " : "✗ ") + head
                    + (d.ok ? d.elapsedMs + " ms" : (d.error || "链路不可用"))
            proxyProbeLabel.isError = !d.ok
        }
    }
}
