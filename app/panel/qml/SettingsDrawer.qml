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
    readonly property var providers: ["microsoft", "openai", "caiyun", "custom"]
    readonly property string provider: providers[Math.max(0, providerCombo.currentIndex)]

    /// 代理是否处于手动配置模式(控制 manual 字段组的可见性)。
    readonly property bool isProxyManual: proxyModeCombo.currentIndex === 2

    /// 快捷键是否被改过 —— 只有改过才重新注册,避免每次自动保存都注销/重注册一遍。
    property bool hotkeysDirty: false

    /// 当前取色目标("" / "bg" / "color")——两个颜色字段共用一个色板弹层。
    property string paletteTarget: ""

    /// 数据目录是否被改过(改动需重启生效,保存后要显式提示,不能混入静默自动保存)。
    property bool dataDirDirty: false

    /// 渲染后端是否被改过(同样重启生效;与数据目录分开提示,便于合并文案)。
    property bool renderDirty: false

    /// 改动去抖后自动落盘(不再有底部「保存」按钮)。
    /// 各控件只接**用户操作**信号(valueModified / clicked / textEdited / activated),
    /// 因此加载时回填值不会误触发保存,也就不需要 loading 守卫。
    Timer {
        id: autoSave
        interval: 400
        onTriggered: root.save()
    }

    // ---- Basic 控件深色可读性覆盖(截图确认:勾选框文字/下拉框底板过暗) ----
    // Basic 的 CheckBox 文字与 ComboBox 下拉底板在深色下取调色板混合值,
    // 对比度不足。统一换成 Readable* 变体,contentItem/背景全部走 Theme 令牌。
    component ReadableCheckBox: CheckBox {
        contentItem: Text {
            text: parent.text
            font: parent.font
            color: Theme.colors.textPrimary
            verticalAlignment: Text.AlignVCenter
            leftPadding: parent.indicator ? parent.indicator.width + parent.spacing : 0
        }
        indicator: Rectangle {
            implicitWidth: 18
            implicitHeight: 18
            x: parent.leftPadding
            y: parent.height / 2 - height / 2
            radius: Theme.metrics.radiusBadge
            color: parent.checked ? Theme.colors.accent : Theme.colors.surfaceSecondary
            border.width: Theme.metrics.borderWidth
            border.color: parent.checked ? Theme.colors.accent : Theme.colors.borderStrong
            Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
            Text {
                visible: parent.parent.checked
                anchors.centerIn: parent
                text: "✓"
                color: Theme.colors.accentText
                font.pixelSize: 12
                font.bold: true
            }
        }
    }

    component ReadableSpinBox: SpinBox {
        contentItem: TextInput {
            text: parent.textFromValue(parent.value, parent.locale)
            validator: parent.validator
            font: parent.font
            color: Theme.colors.textPrimary
            selectionColor: Theme.colors.accent
            selectedTextColor: Theme.colors.accentText
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
            readOnly: !parent.editable
            selectByMouse: true
            onEditingFinished: parent.value = parent.valueFromText(text, parent.locale)
        }
        background: Rectangle {
            color: Theme.colors.surface
            border.color: parent.activeFocus ? Theme.colors.accent : Theme.colors.border
            border.width: Theme.metrics.borderWidth
            radius: Theme.metrics.radiusInput
        }
    }

    component ReadableTextField: TextField {
        color: Theme.colors.textPrimary
        placeholderTextColor: Theme.colors.textTertiary
        selectionColor: Theme.colors.accent
        selectedTextColor: Theme.colors.accentText
        background: Rectangle {
            color: Theme.colors.surface
            border.color: parent.activeFocus ? Theme.colors.accent : Theme.colors.border
            border.width: Theme.metrics.borderWidth
            radius: Theme.metrics.radiusInput
        }
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
        implicitHeight: Theme.metrics.controlHeight

        function keyName(key) {
            if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(key)
            if (key >= Qt.Key_0 && key <= Qt.Key_9) return String.fromCharCode(key)
            if (key >= Qt.Key_F1 && key <= Qt.Key_F24) return "F" + (key - Qt.Key_F1 + 1)
            return ""
        }

        Rectangle {
            anchors.fill: parent
            radius: Theme.metrics.radiusInput
            color: Theme.colors.surfaceSecondary
            border.width: Theme.metrics.borderWidth
            border.color: hk.recording ? Theme.colors.accent : Theme.colors.border
            Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
            Text {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                font.pixelSize: Theme.metrics.fontCaption
                color: hk.recording ? Theme.colors.accent
                     : hk.value === "" ? Theme.colors.textSecondary : Theme.colors.textPrimary
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
            // Delete / Backspace = 清空绑定(允许"不设置快捷键",该动作就不注册)
            if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                hk.recording = false
                if (hk.value !== "") {
                    hk.value = ""
                    hk.valueModified()
                }
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
        rowSpacing: Theme.metrics.spacingXs
        columnSpacing: Theme.metrics.spacingXs
        Repeater {
            model: grid.swatches
            delegate: Rectangle {
                readonly property bool isCurrent:
                    String(modelData).toLowerCase() === grid.current.toLowerCase()
                implicitWidth: 24
                implicitHeight: 24
                radius: Theme.metrics.radiusBadge
                color: modelData
                border.width: isCurrent ? 2 : Theme.metrics.borderWidth
                border.color: isCurrent ? Theme.colors.accent : Theme.colors.borderStrong
                Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
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
        captureDelay.value = s.captureDelaySecs > 0 ? s.captureDelaySecs : 3
        // 0 = 不自动取消,必须原样保留;缺失时按默认 10 分钟
        const idleSecs = (s.overlayIdleSecs === undefined || s.overlayIdleSecs === null)
                         ? 600 : s.overlayIdleSecs
        overlayIdle.value = Math.round(idleSecs / 60)
        themeCombo.currentIndex = s.theme === "light" ? 1 : s.theme === "dark" ? 2 : 0
        softwareRenderCheck.checked = s.softwareRender === true
        dataDirInput.text = s.dataDir || ""
        root.dataDirDirty = false
        root.renderDirty = false

        pinOpacity.value = s.pinDefaultOpacity > 0 ? s.pinDefaultOpacity : 100
        pinDoubleClick.checked = s.pinDoubleclickClose === true
        pinPreferImage.checked = s.pinPreferImage === true
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
        hkCaptureOcr.value = hk.captureOcr || ""
        hkCaptureDelayed.value = hk.captureDelayed || ""

        const index = providers.indexOf(s.translateProvider)
        providerCombo.currentIndex = index < 0 ? 0 : index

        const oa = s.translateOpenai || {}
        oaBaseUrl.text = oa.baseUrl || ""
        oaApiKey.text = oa.apiKey || ""
        oaModel.text = oa.model || ""

        const ms = s.translateMicrosoft || {}
        msEndpoint.text = ms.endpoint || ""

        const cy = s.translateCaiyun || {}
        cyToken.text = cy.token || ""
        cyEndpoint.text = cy.endpoint || ""

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
        s.captureDelaySecs = captureDelay.value
        s.overlayIdleSecs = overlayIdle.value * 60
        s.theme = ["system", "light", "dark"][themeCombo.currentIndex]
        s.softwareRender = softwareRenderCheck.checked
        s.dataDir = dataDirInput.text.trim()

        s.pinDefaultOpacity = pinOpacity.value
        s.pinDoubleclickClose = pinDoubleClick.checked
        s.pinPreferImage = pinPreferImage.checked
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
        hk.captureOcr = hkCaptureOcr.value
        hk.captureDelayed = hkCaptureDelayed.value
        s.hotkeys = hk

        s.translateProvider = provider
        const oa = s.translateOpenai || {}
        oa.baseUrl = oaBaseUrl.text
        oa.apiKey = oaApiKey.text
        oa.model = oaModel.text
        s.translateOpenai = oa

        const ms = s.translateMicrosoft || {}
        ms.endpoint = msEndpoint.text
        s.translateMicrosoft = ms

        const cy = s.translateCaiyun || {}
        cy.token = cyToken.text.trim()
        cy.endpoint = cyEndpoint.text.trim()
        s.translateCaiyun = cy

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
            if (root.renderDirty) {
                statusLabel.text = "已保存。渲染后端**重启后**生效"
                statusLabel.isError = false
                root.renderDirty = false
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

    /// 清空历史数据(core `clear_history`):删除全部剪贴板条目并清理图片文件。
    /// 破坏性操作,由「危险操作」区的就地二次确认块调用。
    /// 清空后立即重拉列表(refreshNow 内部一并刷新分类计数),避免界面残留旧条目。
    function clearHistory() {
        const resp = Core.call("clear_history")
        clearConfirm.visible = false
        if (!resp.ok) {
            statusLabel.text = "清空失败:" + errText(resp)
            statusLabel.isError = true
            return
        }
        Entries.refreshNow()
        statusLabel.text = "已清空历史数据"
        statusLabel.isError = false
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
            spacing: Theme.metrics.spacingXs

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "设置"; font.pixelSize: Theme.metrics.fontPageTitle
                    font.weight: Font.DemiBold; color: Theme.colors.textPrimary
                }
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "ghost"
                    square: true
                    text: "✕"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: root.close()
                }
            }

            Label { text: "历史上限(条)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            ReadableSpinBox {
                id: historyLimit
                Layout.fillWidth: true
                from: 100; to: 100000; stepSize: 500
                editable: true
                onValueModified: root.scheduleSave()
            }

            Label { text: "单张图片上限(MB)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            ReadableSpinBox {
                id: maxImage
                Layout.fillWidth: true
                from: 1; to: 200
                editable: true
                onValueModified: root.scheduleSave()
            }

            ReadableCheckBox {
                id: monitorCheck
                text: "剪贴板监听"
                onClicked: root.scheduleSave()
            }
            ReadableCheckBox {
                id: ocrCheck
                text: "OCR 自动识别(截图文字可搜索)"
                onClicked: root.scheduleSave()
            }
            // 取消勾选后,框选截图会先进编辑器标注,「完成」才入库 + 写剪贴板
            ReadableCheckBox {
                id: skipEditorCheck
                text: "截图后直接入库(跳过编辑器)"
                onClicked: root.scheduleSave()
            }
            // 取消勾选后,选区保留并可拖 8 向手柄微调,Enter 或双击才确认
            ReadableCheckBox {
                id: autoConfirmCheck
                text: "框选后松开鼠标即确认"
                onClicked: root.scheduleSave()
            }
            ReadableCheckBox {
                id: rightClickCancelCheck
                text: "框选时鼠标右键取消截图"
                onClicked: root.scheduleSave()
            }

            // 延时截图(F16):托盘「延时截图」入口的等待秒数
            Label { text: "延时截图等待(秒)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            ReadableSpinBox {
                id: captureDelay
                Layout.fillWidth: true
                from: 1; to: 10
                editable: true
                onValueModified: root.scheduleSave()
            }

            // 内存收口(S6.1):框选遮罩持有该屏的整屏冻结底图(4K 约 33MB),
            // 用户触发截图后离开会一直占着 —— 超时无任何鼠标/键盘操作即自动取消。
            Label { text: "遮罩空闲超时(分钟,0=不自动取消)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            ReadableSpinBox {
                id: overlayIdle
                Layout.fillWidth: true
                from: 0; to: 60
                editable: true
                onValueModified: root.scheduleSave()
            }

            Label { text: "主题"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            ReadableComboBox {
                id: themeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "浅色", "深色"]
                // 切换即生效(不必等保存),给用户即时反馈
                onActivated: {
                    Core.applyTheme(["system", "light", "dark"][currentIndex])
                    root.scheduleSave()
                }
            }

            // ---- 分组:数据(§10 分组标题 + Border Subtle 分隔线) ----
            component SectionDivider: Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: Theme.metrics.spacingMd
                implicitHeight: Theme.metrics.borderWidth
                color: Theme.colors.border
            }
            component SectionTitle: Label {
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
                Layout.topMargin: Theme.metrics.spacingXs
            }
            component FieldLabel: Label {
                // 字段标签是与控件直接交互的引导文字,暗色下用 Secondary 的提亮值
                font.family: Theme.fontFamily
                color: Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
            }
            component HintLabel: Label {
                Layout.fillWidth: true
                // 说明文字较长,是"看不清"的重灾区:暗色下改用 Secondary(提亮档)
                font.family: Theme.fontFamily
                color: Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }

            SectionDivider {}
            SectionTitle { text: "数据" }
            // 渲染后端(重启生效):软件(CPU 光栅)可省掉 GPU 场景图/RHI 设备约 70MB,
            // 代价是渲染改由 CPU 承担 —— 供显卡受限/内存紧张的用户选用。
            ReadableCheckBox {
                id: softwareRenderCheck
                text: "软件渲染(省内存,重启生效)"
                onClicked: {
                    root.renderDirty = true
                    root.scheduleSave()
                }
            }
            HintLabel {
                text: "用 CPU 光栅替代 GPU(D3D11)渲染,约省 70MB 内存;滚动/动画更吃 CPU。重启后生效。"
            }
            ReadableCheckBox {
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
            HintLabel {
                text: "自定义数据目录(留空 = 默认位置)。变更后重启应用生效:数据库与图片自动迁移到新目录,原目录数据保留。"
            }
            ReadableTextField {
                id: dataDirInput
                Layout.fillWidth: true
                placeholderText: "D:\\Data\\Nextboard"
                onTextEdited: {
                    root.dataDirDirty = true
                    root.scheduleSave()
                }
            }

            // ---- 贴图 ----
            SectionDivider {}
            SectionTitle { text: "贴图" }

            FieldLabel { text: "默认不透明度(%)" }
            ReadableSpinBox {
                id: pinOpacity
                Layout.fillWidth: true
                from: 30; to: 100; stepSize: 5
                editable: true
                onValueModified: root.scheduleSave()
            }
            ReadableCheckBox {
                id: pinDoubleClick
                text: "双击关闭贴图窗口"
                onClicked: root.scheduleSave()
            }
            ReadableCheckBox {
                id: pinPreferImage
                text: "一键贴图 · 图片优先(否则贴最近复制的内容)"
                onClicked: root.scheduleSave()
            }
            HintLabel {
                text: "关闭(默认):按热键贴出最近复制的文本或图片;开启:只要历史里有图片就贴最近图片,无图时才贴最近文本。"
            }

            FieldLabel { text: "文本贴图 · 背景色(点色块展开色板)" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 34; implicitHeight: 22; radius: Theme.metrics.radiusBadge
                    color: root.isHexColor(pinTextBg.text) ? pinTextBg.text : Theme.colors.surfaceSecondary
                    border.color: root.paletteTarget === "bg" ? Theme.colors.accent : Theme.colors.border
                    border.width: root.paletteTarget === "bg" ? 2 : Theme.metrics.borderWidth
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.paletteTarget = root.paletteTarget === "bg" ? "" : "bg"
                    }
                }
                ReadableTextField {
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
            FieldLabel { text: "文本贴图 · 文字色(点色块展开色板)" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 34; implicitHeight: 22; radius: Theme.metrics.radiusBadge
                    color: root.isHexColor(pinTextColor.text) ? pinTextColor.text : Theme.colors.surfaceSecondary
                    border.color: root.paletteTarget === "color" ? Theme.colors.accent : Theme.colors.border
                    border.width: root.paletteTarget === "color" ? 2 : Theme.metrics.borderWidth
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.paletteTarget = root.paletteTarget === "color" ? "" : "color"
                    }
                }
                ReadableTextField {
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
            FieldLabel { text: "文本贴图 · 背景不透明度(%)" }
            ReadableSpinBox {
                id: pinTextBgOpacity
                Layout.fillWidth: true
                from: 0; to: 100; stepSize: 5
                editable: true
                onValueModified: root.scheduleSave()
            }
            FieldLabel { text: "文本贴图 · 字号(像素)" }
            ReadableSpinBox {
                id: pinTextFontSize
                Layout.fillWidth: true
                from: 8; to: 96
                editable: true
                onValueModified: root.scheduleSave()
            }
            FieldLabel { text: "文本贴图 · 最大宽度(像素)" }
            ReadableSpinBox {
                id: pinTextMaxWidth
                Layout.fillWidth: true
                from: 120; to: 2000; stepSize: 40
                editable: true
                onValueModified: root.scheduleSave()
            }

            // ---- 快捷键 ----
            SectionDivider {}
            SectionTitle { text: "快捷键" }
            HintLabel {
                text: "点击后按下组合键即可录制。需含至少一个修饰键(Ctrl/Alt/Shift/Win),单按 F1–F24 也可;"
                      + "按 Delete 清空 = 不绑定该动作。保存后立即生效。"
            }
            FieldLabel { text: "呼出/隐藏面板" }
            HotkeyField {
                id: hkToggleMain
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "矩形截图" }
            HotkeyField {
                id: hkCaptureRegion
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "全屏截图" }
            HotkeyField {
                id: hkCaptureFullscreen
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "一键贴图" }
            HotkeyField {
                id: hkPinLatest
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "打开翻译" }
            HotkeyField {
                id: hkOpenTranslate
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "截图翻译" }
            HotkeyField {
                id: hkCaptureTranslate
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "截图取字（入列表）" }
            HotkeyField {
                id: hkCaptureOcr
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }
            FieldLabel { text: "延时截图" }
            HotkeyField {
                id: hkCaptureDelayed
                Layout.fillWidth: true
                onValueModified: {
                    root.hotkeysDirty = true
                    root.scheduleSave()
                }
            }

            // ---- 翻译服务 ----
            SectionDivider {}
            SectionTitle { text: "翻译服务" }
            ReadableComboBox {
                id: providerCombo
                Layout.fillWidth: true
                model: ["微软翻译(免密钥,开箱可用)", "AI 翻译(OpenAI 兼容)", "彩云小译(内置测试 Token)", "自定义 HTTP"]
                onActivated: root.scheduleSave()
            }

            // OpenAI 兼容(含国内兼容端点)
            FieldLabel { text: "接口地址"; visible: root.provider === "openai" }
            ReadableTextField {
                id: oaBaseUrl
                onTextEdited: root.scheduleSave()
                visible: root.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "https://api.openai.com/v1"
            }
            FieldLabel { text: "API Key"; visible: root.provider === "openai" }
            ReadableTextField {
                id: oaApiKey
                onTextEdited: root.scheduleSave()
                visible: root.provider === "openai"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "sk-…"
            }
            FieldLabel { text: "模型"; visible: root.provider === "openai" }
            ReadableTextField {
                id: oaModel
                onTextEdited: root.scheduleSave()
                visible: root.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "gpt-4o-mini"
            }

            // 微软翻译(免授权端点)
            HintLabel {
                visible: root.provider === "microsoft"
                text: "默认通道:使用 Edge 内置翻译的免授权端点,**无需申请任何密钥**即可翻译。"
                      + "该端点非微软公开 API(无 SLA),若日后失效,可改下方地址或切到其它通道。"
            }
            FieldLabel { text: "接口地址"; visible: root.provider === "microsoft" }
            ReadableTextField {
                id: msEndpoint
                onTextEdited: root.scheduleSave()
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                placeholderText: "https://edge.microsoft.com/translate"
            }

            // 彩云小译(内置)
            HintLabel {
                visible: root.provider === "caiyun"
                text: "内置通道:已预填**官方测试 Token**,装上即可试。官方不保证该 Token 的"
                      + "稳定可用性,持续使用请到彩云科技开放平台申请正式 Token 后替换下方内容"
                      + "(新用户有 100 万字 / 1 个月免费额度)。"
            }
            FieldLabel { text: "访问 Token"; visible: root.provider === "caiyun" }
            ReadableTextField {
                id: cyToken
                onTextEdited: root.scheduleSave()
                visible: root.provider === "caiyun"
                Layout.fillWidth: true
                placeholderText: "3975l6lr5pcbvidl6jl2"
            }
            FieldLabel { text: "接口地址"; visible: root.provider === "caiyun" }
            ReadableTextField {
                id: cyEndpoint
                onTextEdited: root.scheduleSave()
                visible: root.provider === "caiyun"
                Layout.fillWidth: true
                placeholderText: "https://api.interpreter.caiyunai.com"
            }

            // 自定义 HTTP
            HintLabel {
                visible: root.provider === "custom"
                text: "按自己的接口拼请求:下面用占位符描述「发什么」与「从哪取译文」。"
                      + "占位符 ${text} ${from} ${to} ${apiKey} ${model} 会在发送前替换成实际值"
                      + "(支持 ${model:-默认值} 写法);未知占位符会直接报错,不会静默留空。"
            }
            FieldLabel { text: "请求地址"; visible: root.provider === "custom" }
            ReadableTextField {
                id: cuUrl
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "https://…"
            }
            HintLabel {
                visible: root.provider === "custom"
                text: "完整接口地址(含查询参数也可以);值里同样可用占位符,会按 URL 编码转义。"
            }
            FieldLabel { text: "请求方法"; visible: root.provider === "custom" }
            ReadableComboBox {
                id: cuMethod
                visible: root.provider === "custom"
                Layout.fillWidth: true
                model: ["POST", "GET"]
                onActivated: root.scheduleSave()
            }
            FieldLabel { text: "API Key(可选)"; visible: root.provider === "custom" }
            ReadableTextField {
                id: cuApiKey
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                echoMode: TextInput.Password
            }
            HintLabel {
                visible: root.provider === "custom"
                text: "只用于替换 ${apiKey} 占位符,**不会**自动加到请求头 —— 需要哪种鉴权头请自己写在请求头里。"
            }
            FieldLabel { text: "模型 / 部署名(可选)"; visible: root.provider === "custom" }
            ReadableTextField {
                id: cuModel
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
            }
            HintLabel {
                visible: root.provider === "custom"
                text: "只用于替换 ${model} 占位符(Azure 风格接口常把部署名写在地址里)。"
            }
            // ---- 请求头(可滚动:内容多时不再被截断) ----
            FieldLabel { text: "请求头"; visible: root.provider === "custom" }
            HintLabel {
                visible: root.provider === "custom"
                text: "每行一条,格式 Name: Value;值里可用占位符,如 Authorization: Bearer ${apiKey}。"
            }
            ScrollView {
                id: cuHeadersScroll
                visible: root.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: cuHeaders.activeFocus ? Theme.colors.accent : Theme.colors.border
                }
                TextArea {
                    id: cuHeaders
                    onTextEdited: root.scheduleSave()
                    wrapMode: TextArea.WrapAnywhere
                    placeholderText: "Content-Type: application/json"
                    color: Theme.colors.textPrimary
                    placeholderTextColor: Theme.colors.textTertiary
                    selectionColor: Theme.colors.accent
                    selectedTextColor: Theme.colors.accentText
                    padding: 8
                }
            }

            // ---- 请求体模板(可滚动) ----
            FieldLabel { text: "请求体模板"; visible: root.provider === "custom" }
            HintLabel {
                visible: root.provider === "custom"
                text: "按目标接口的 JSON 结构写,原文放在 ${text} 的位置;"
                      + "原文里的引号与换行会自动转义,不会破坏 JSON。GET 请求可留空。"
            }
            ScrollView {
                id: cuBodyScroll
                visible: root.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 96
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: cuBody.activeFocus ? Theme.colors.accent : Theme.colors.border
                }
                TextArea {
                    id: cuBody
                    onTextEdited: root.scheduleSave()
                    wrapMode: TextArea.WrapAnywhere
                    placeholderText: "{\"text\": [\"${text}\"], \"target_lang\": \"${to}\"}"
                    color: Theme.colors.textPrimary
                    placeholderTextColor: Theme.colors.textTertiary
                    selectionColor: Theme.colors.accent
                    selectedTextColor: Theme.colors.accentText
                    padding: 8
                }
            }

            // ---- 响应取值路径 ----
            FieldLabel { text: "响应取值路径"; visible: root.provider === "custom" }
            HintLabel {
                visible: root.provider === "custom"
                text: "从响应 JSON 里取译文的路径,用点号与下标逐层往下写。"
            }
            ReadableTextField {
                id: cuPath
                onTextEdited: root.scheduleSave()
                visible: root.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "如 translations[0].text"
            }
            HintLabel {
                visible: root.provider === "custom"
                text: "完整示例(以 DeepL 为例):\n"
                      + "地址 https://api-free.deepl.com/v2/translate\n"
                      + "请求头 Authorization: DeepL-Auth-Key ${apiKey}\n"
                      + "请求体 {\"text\":[\"${text}\"],\"target_lang\":\"${to}\"}\n"
                      + "响应路径 translations[0].text\n"
                      + "注意:各服务的语言代码写法可能不同(DeepL 用大写 ZH)。"
                      + "语言代码在翻译窗口顶部可选可改,按服务要求填即可。"
            }

            FieldLabel { text: "超时(秒)" }
            ReadableSpinBox {
                id: timeoutSpin
                Layout.fillWidth: true
                from: 5; to: 120
                editable: true
                onValueModified: root.scheduleSave()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                ThemedButton { variant: "secondary"; text: "测试连接"; onClicked: root.probe() }
                Item { Layout.fillWidth: true }
            }
            Label {
                id: probeLabel
                property bool isError: false
                Layout.fillWidth: true
                color: isError ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }

            // ---- 网络与代理 ----
            // core 的 proxy 校验/分派/proxy_probe 早已就绪,这里只补 UI 入口。
            // manual 字段始终提交,与 core 一致:仅 mode=manual 时才参与校验与生效。
            SectionDivider {}
            SectionTitle { text: "网络与代理" }
            FieldLabel { text: "代理模式(作用于翻译等网络请求)" }
            ReadableComboBox {
                id: proxyModeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "直连(不使用代理)", "手动配置"]
                onActivated: root.scheduleSave()
            }
            FieldLabel { text: "协议类型"; visible: root.isProxyManual }
            ReadableComboBox {
                id: proxyKindCombo
                visible: root.isProxyManual
                Layout.fillWidth: true
                model: ["HTTP", "SOCKS5"]
                onActivated: root.scheduleSave()
            }
            FieldLabel { text: "服务器地址"; visible: root.isProxyManual }
            ReadableTextField {
                id: proxyHost
                visible: root.isProxyManual
                Layout.fillWidth: true
                placeholderText: "127.0.0.1"
                onTextEdited: root.scheduleSave()
            }
            FieldLabel { text: "端口"; visible: root.isProxyManual }
            ReadableSpinBox {
                id: proxyPort
                visible: root.isProxyManual
                Layout.fillWidth: true
                from: 0; to: 65535
                editable: true
                onValueModified: root.scheduleSave()
            }
            FieldLabel { text: "用户名(可选)"; visible: root.isProxyManual }
            ReadableTextField {
                id: proxyUser
                visible: root.isProxyManual
                Layout.fillWidth: true
                onTextEdited: root.scheduleSave()
            }
            FieldLabel { text: "密码(可选)"; visible: root.isProxyManual }
            ReadableTextField {
                id: proxyPassword
                visible: root.isProxyManual
                Layout.fillWidth: true
                echoMode: TextInput.Password
                onTextEdited: root.scheduleSave()
            }
            // SOCKS5 路径(ureq)不支持绕过列表,仅 HTTP 显示该字段(与 core 注释一致)
            FieldLabel { text: "绕过列表(仅 HTTP,分号分隔)"; visible: root.isProxyManual && proxyKindCombo.currentIndex === 0 }
            ReadableTextField {
                id: proxyBypass
                visible: root.isProxyManual && proxyKindCombo.currentIndex === 0
                Layout.fillWidth: true
                placeholderText: "localhost;127.0.0.1;*.internal"
                onTextEdited: root.scheduleSave()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                ThemedButton { variant: "secondary"; text: "测试代理链路"; onClicked: root.probeProxy() }
                Item { Layout.fillWidth: true }
            }
            Label {
                id: proxyProbeLabel
                property bool isError: false
                Layout.fillWidth: true
                color: isError ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }

            // ---- 危险操作 ----
            SectionDivider {}
            SectionTitle { text: "危险操作" }
            HintLabel {
                id: clearHint
                text: Core.counts.all > 0
                      ? "删除全部 " + Core.counts.all + " 条剪贴板历史(含图片文件),此操作不可恢复。"
                      : "当前没有历史数据。"
            }
            ThemedButton {
                id: clearButton
                Layout.fillWidth: true
                variant: "danger"
                text: "清空历史数据"
                // 无数据时置灰,避免"点了没反应"的困惑
                enabled: Core.counts.all > 0
                onClicked: clearConfirm.visible = true
            }
            // 就地二次确认:破坏性操作必须二次确认,但不弹 Dialog/Popup ——
            // 外层 SettingsDrawer 本身即 Popup,嵌套弹层有定位/焦点问题
            // (同 ColorSwatchGrid 的取舍,见上方注释)。
            ColumnLayout {
                id: clearConfirm
                Layout.fillWidth: true
                visible: false
                spacing: Theme.metrics.spacingXs
                HintLabel {
                    text: "确认清空全部历史?此操作不可恢复。"
                    color: Theme.colors.error
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ThemedButton {
                        Layout.fillWidth: true
                        variant: "danger"
                        text: "确认清空"
                        onClicked: root.clearHistory()
                    }
                    ThemedButton {
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: "取消"
                        onClicked: clearConfirm.visible = false
                    }
                }
            }

            // ---- 关于 ----
            SectionDivider {}
            SectionTitle { text: "关于" }
            HintLabel {
                text: "Nextboard " + Core.appVersion
                      + "\n内核 nbcore " + (Core.coreVersion !== "" ? Core.coreVersion : "未加载")
                      + "\nWindows 桌面一体化效率板:剪贴板历史 + 截图 / 贴图 + 翻译"
            }

            // 改动即时保存(去抖 400ms),故不再有「保存 / 关闭」按钮 ——
            // 关闭抽屉走标题栏的 ✕。状态行只用于反馈结果(尤其是校验失败)。
            Label {
                id: statusLabel
                property bool isError: false
                Layout.fillWidth: true
                Layout.topMargin: Theme.metrics.spacingMd
                color: isError ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
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
