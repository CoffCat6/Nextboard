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

    /// 当前选的是否「系统内置」OCR 后端 —— 决定识别语言/放大倍数两个控件是否显示。
    /// 语义依据(core 侧注释):RapidOCR 自带 max_side_len 不消费 ocr_scale,且中英混排
    /// 一体不分语言;这两个字段对它无效,显示出来只会让人"调了没反应"。
    readonly property bool builtinOcr:
        ocrBackendCombo.backendCodes[ocrBackendCombo.currentIndex] === "system"

    /// OCR 当前后端是否可用(由 core 的 ocr_probe 回填)。
    /// 不可用时 HintLabel 转成警示色并给出可读原因(缺模型 / 缺 DLL / 缺语言包)。
    property bool ocrBackendAvailable: true
    property string ocrBackendHintText: ""

    /// 备份/导出进行中标志:按钮置灰 + 文案切换(结果经 backupFinished/exportFinished 回投)。
    property bool backupRunning: false
    property bool exportRunning: false

    /// 更新检查(docs/07-auto-update.md §9):进行中标志 + 结果里的下载页链接。
    /// `updateDownloadUrl` 非空才出现「打开下载页」—— core 只在拿到 http(s) 链接时填它。
    property bool updateChecking: false
    property string updateDownloadUrl: ""

    /// 恢复/导入:进行中标志 + 待确认的备份文件路径(确认卡点"确认"才发起)。
    property bool restoreRunning: false
    property bool importRunning: false
    property string restorePendingPath: ""
    property string importPendingPath: ""

    /// 验收只读:确认卡可见性(--restore-flow 读回;QML 外部拿不到内部 id)。
    readonly property bool acceptanceRestoreCardVisible: restoreConfirm.visible
    readonly property bool acceptanceImportCardVisible: importConfirm.visible

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
            // ⚠️ 这里**不要**写 `onEditingFinished: parent.value = valueFromText(...)`。
            //    手输提交由 QQuickSpinBox 原生路径负责(回车键释放 / 失焦 →
            //    updateValue),它以 modified=true 提交并发射 valueModified,自动保存
            //    才被触发;QML 侧的程序化赋值会抢先写入同一个值,原生路径随后
            //    认为"值没变"而不再发射 valueModified —— 保存被静默吞掉
            //    (qquickspinbox.cpp:setValue 的 modified 标志仅在
            //    value != correctedValue 时才发 valueModified)。实测症状:手输数值
            //    关掉抽屉再打开又变回去,只有 +/- 加减能保存。
        }
        background: Rectangle {
            color: Theme.colors.surface
            border.color: parent.activeFocus ? Theme.colors.accent : Theme.colors.border
            border.width: Theme.metrics.borderWidth
            radius: Theme.metrics.radiusInput
        }
    }

    /// 单行文本输入框(数据目录 / 接口地址 / API Key / Token / 模型名 /
    /// 响应取值路径 / 代理主机与账号 / 十六进制色值…)。
    ///
    /// 全部是**技术字段**,所以统一用等宽字体:URL 与 JSON 里的字符
    /// (0/O、1/l/I、{}[])在等宽下才对得齐、肉眼可核对。中文回退由 C++ 侧
    /// app_font.hpp 的替换表钉到雅黑,不会掉宋体。
    component ReadableTextField: TextField {
        color: Theme.colors.textPrimary
        font.family: Theme.fontMono
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
                // 技术字段:"Ctrl+Shift+V" 这类组合键在等宽下各段严格对齐,
                // 不同动作之间横向可比(一眼看出哪个键位冲突)
                font.family: Theme.fontMono
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

    /// 验收专用:滚到设置内容底部(--settings-flow 验证"滚动后关闭按钮仍可点")。
    function scrollToBottom() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }

    /// 验收专用:读回滚动位置(>0 = 确实滚过了)。
    readonly property real acceptanceScrollY: flick.contentY

    /// 验收专用:把某个控件滚入视口。
    ///
    /// 为什么需要它:`scrollToBottom()` 会把**靠上**的控件滚出视口顶部,
    /// 而 QML 的 `isVisible()` 对"被裁到视口外"的项**仍返回 true** ——
    /// 于是验收点击会落在一个窗口外的坐标上(实测 y=-207)而静默失败。
    /// 这里把目标项的上边缘对齐到视口顶部,保证它落在可见区内。
    function acceptanceScrollTo(item) {
        if (!item) return false
        const pos = item.mapToItem(flick.contentItem, 0, 0)
        flick.contentY = Math.max(0, Math.min(pos.y - 8,
                                    flick.contentHeight - flick.height))
        return true
    }

    /// 查询当前所选 OCR 后端的可用性,回填提示文案。
    ///
    /// 同步调用:core 侧 `ocr_probe` 只做**文件存在性**检查(不建 session、不加载模型),
    /// 耗时在微秒级,不需要异步。
    /// 注意 `reason` 字段仅在**不可用**时由 core 下发(可用时省略)。
    function refreshOcrProbe() {
        const code = ocrBackendCombo.backendCodes[ocrBackendCombo.currentIndex]
        const resp = Core.call("ocr_probe", { "backend": code })
        if (!resp.ok) {
            root.ocrBackendAvailable = false
            root.ocrBackendHintText = "无法探测该引擎:" + errText(resp)
            return
        }
        const d = resp.data
        root.ocrBackendAvailable = d.available === true
        if (d.available === true) {
            // 语言列表仅内置引擎有(RapidOCR 中英混排一体,不按语言标签区分)
            const langs = d.languages || []
            root.ocrBackendHintText = langs.length > 0
                ? "可用 · 语言:" + langs.join("、")
                : "可用(中英混排一体识别)"
        } else {
            root.ocrBackendHintText = d.reason || "该引擎当前不可用"
        }
    }

    /// 导出到用户选定的位置。format 决定**内容**(核心按它渲染),后缀/过滤器
    /// 只决定默认文件名与对话框过滤 —— 两者必须一起给,否则会出现"选了 JSON
    /// 却存成 .md"这种后缀与内容不符的产物。
    function exportAs(format, filter, suffix) {
        const path = Core.saveFileDialog(
            "选择导出保存位置",
            filter,
            "nextboard-export-" + new Date().toISOString().slice(0, 10) + suffix)
        if (path === "") return
        exportRunning = true
        exportHint.text = "正在导出…"
        exportHint.isError = false
        Core.exportEntries(path, format)
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
        // OCR 后端:未知值回退 0(与 core 的 Backend::parse 同口径);
        // 同时触发一次可用性探测,把"为什么不可用"显示出来。
        const backendCodes = ocrBackendCombo.backendCodes
        const backendIdx = backendCodes.indexOf(s.ocrBackend)
        ocrBackendCombo.currentIndex = backendIdx < 0 ? 0 : backendIdx
        // 未知档位/驻留回退默认(与 core 的 normalize 同口径)
        const profileIdx = ocrProfileCombo.profileCodes.indexOf(s.ocrProfile)
        ocrProfileCombo.currentIndex = profileIdx < 0 ? 1 : profileIdx
        const modeIdx = ocrSessionCombo.modeCodes.indexOf(s.ocrSessionMode)
        ocrSessionCombo.currentIndex = modeIdx < 0 ? 2 : modeIdx
        // OCR 语言/放大:未知值回退默认(auto / 2),与 core 的 Default 同口径
        const langIdx = ocrLanguageCombo.languageCodes.indexOf(s.ocrLanguage)
        ocrLanguageCombo.currentIndex = langIdx < 0 ? 0 : langIdx
        const scale = (s.ocrScale === undefined || s.ocrScale === null) ? 2 : s.ocrScale
        ocrScaleSpin.value = Math.min(Math.max(scale, 1), 4)
        root.refreshOcrProbe()
        maxImage.value = s.maxImageMb
        // 文本上限:缺失时按 core 默认 1024(与 settings.rs 的 Default 一致)
        maxTextKb.value = (s.maxTextKb === undefined || s.maxTextKb === null) ? 1024 : s.maxTextKb
        monitorCheck.checked = s.monitorEnabled === true
        autoPasteCheck.checked = s.copyAutoPaste === true
        // 单一出口(§18.4):由 regionAutoConfirm 推导 ——
        //   true  = direct (松手即入库)
        //   false = toolbar(浮出工具栏)
        // 兼容旧配置:旧版 "skipEditor=false"(想进编辑器)的用户,其意图是"要编辑能力",
        // 在新模型里对应 toolbar(浮条带标注),故只要 autoConfirm=false 就落 toolbar。
        regionExitCombo.currentIndex = s.regionAutoConfirm === false ? 1 : 0
        rightClickCancelCheck.checked = s.regionRightClickCancel === true
        captureDelay.value = s.captureDelaySecs > 0 ? s.captureDelaySecs : 3
        // 长截图(docs/19 §6):上限与重叠门槛都来自 core,兜底值必须与
        // settings.rs 的默认一致(30000 / 3),否则"显示与行为分叉"。
        const stitchH = (s.stitchMaxHeight === undefined || s.stitchMaxHeight === null)
                        ? 30000 : s.stitchMaxHeight
        stitchMaxHeight.value = stitchH
        const stitchR = (s.stitchOverlapRatio === undefined || s.stitchOverlapRatio === null)
                        ? 3 : s.stitchOverlapRatio
        stitchOverlap.value = stitchR
        // 0 = 不自动取消,必须原样保留;缺失时按 core 默认 60 秒
        // ⚠️ 兜底值必须与 settings.rs / capture_session.cpp 一致(都取 60):
        //    这里原先写 600,get_settings 失败时显示"10 分钟"而实际行为是 60s ——
        //    显示与行为分叉,用户改不动也看不出来。
        const idleSecs = (s.overlayIdleSecs === undefined || s.overlayIdleSecs === null)
                         ? 60 : s.overlayIdleSecs
        overlayIdle.value = Math.round(idleSecs / 60)
        themeCombo.currentIndex = s.theme === "light" ? 1 : s.theme === "dark" ? 2 : 0
        softwareRenderCheck.checked = s.softwareRender === true
        // 开机自启不进 settings,而是注册表实况 → 每次打开都回读同步。
        // (勾选态在首次点击时被赋值,声明式绑定即被破坏,不显式回读就会一直停留在旧值)
        autoStartCheck.checked = Core.autoStartEnabled()
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
        hkCaptureScrolling.value = hk.captureScrolling || ""

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
        s.ocrBackend = ocrBackendCombo.backendCodes[ocrBackendCombo.currentIndex]
        s.ocrProfile = ocrProfileCombo.profileCodes[ocrProfileCombo.currentIndex]
        s.ocrSessionMode = ocrSessionCombo.modeCodes[ocrSessionCombo.currentIndex]
        s.maxImageMb = maxImage.value
        s.maxTextKb = maxTextKb.value
        // OCR 语言/放大只在「系统内置」时显示,但**始终照写**:切换后端时值要保住,
        // 否则从内置切到 RapidOCR 再切回来,用户设定的语言会被清成默认。
        s.ocrLanguage = ocrLanguageCombo.languageCodes[ocrLanguageCombo.currentIndex]
        s.ocrScale = ocrScaleSpin.value
        s.monitorEnabled = monitorCheck.checked
        s.copyAutoPaste = autoPasteCheck.checked
        // 单一出口 → 回写成 regionAutoConfirm(direct=true / toolbar=false)。
        // regionSkipEditor 已废弃(§18.5):不写它,让它保持读入时的原值(见 core 注释)。
        s.regionAutoConfirm = regionExitCombo.currentIndex === 0
        s.regionRightClickCancel = rightClickCancelCheck.checked
        s.captureDelaySecs = captureDelay.value
        s.stitchMaxHeight = stitchMaxHeight.value
        s.stitchOverlapRatio = stitchOverlap.value
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
        hk.captureScrolling = hkCaptureScrolling.value
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

    /// 清空全部数据(core `clear_history`):删除**全部**条目(剪贴板历史 + 待办,
    /// todo_meta 由 core 同事务清理)并清理图片文件。破坏性操作,由「危险操作」区的
    /// 就地二次确认块调用。
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
        statusLabel.text = "已清空全部数据"
        statusLabel.isError = false
    }

    // ---- 恢复 / 导入(备份 zip 的消费端;慢 I/O 走 Core 线程池) ----
    /// 把表单里的某块滚进可视区(确认块在表单中部,滚动过的用户点按钮后
    /// 确认块可能在视口外 —— 不滚过去会像"点了没反应")。
    /// ⚠️ 读位置必须等**布局结算**:visible 刚置 true 时该块还没有真实 y,
    ///    立刻读会滚到错位置(实测:第一次开确认卡滚对了,第二次滚到 y≈96
    ///    的位置 —— 起因就是 Qt.callLater 与布局 polish 的竞争,--restore-flow 抓出)。
    ///    对策:callLater(快路径)+ 120ms 定时器(兜底,布局晚于事件循环时补滚)。
    function revealItem(it) {
        Qt.callLater(function() { scrollItemIntoView(it) })
        revealRetry.item = it
        revealRetry.restart()
    }
    function scrollItemIntoView(it) {
        const y = it.mapToItem(flick.contentItem, 0, 0).y
        flick.contentY = Math.max(0, Math.min(y - 40,
                                              Math.max(0, flick.contentHeight - flick.height)))
    }
    Timer {
        id: revealRetry
        interval: 120
        property var item: null
        onTriggered: if (item) root.scrollItemIntoView(item)
    }
    /// 验收(--clear-all-flow):把「清空全部数据」按钮滚进可视区(危险区在表单尾部)。
    function revealClearAll() { revealItem(clearButton) }
    /// 验收(--update-flow):把「检查更新」按钮滚进可视区(关于区在表单最末)。
    function revealUpdateCheck() { revealItem(updateCheckButton) }
    /// 验收读回:检查结果的文案与下载链接(状态机断言用,见 docs/07 §11.2)。
    readonly property string acceptanceUpdateHint: updateHint.text
    readonly property string acceptanceUpdateUrl: root.updateDownloadUrl
    /// 验收读回:空置态判据 —— 按钮是否可用、"未配置"说明是否出现(D-c 的核心承诺)。
    readonly property bool acceptanceUpdateEnabled: updateCheckButton.enabled
    readonly property bool acceptanceUpdateNoteVisible: updateConfigHint.visible
    /// 打开恢复确认卡(产品路径 = 原生文件对话框选路径后调用;验收路径 =
    /// Main.qml 的 acceptanceOpenRestore 直接喂路径,跳过只能人工操作的原生对话框)。
    function openRestoreConfirm(path) {
        restorePendingPath = path
        restoreConfirm.visible = true
        revealItem(restoreConfirm)
    }
    /// 执行恢复:替换当前全部数据;nbcore 侧先自动打险备份。结果走 restoreFinished。
    function commitRestore() {
        restoreConfirm.visible = false
        if (restorePendingPath === "") return
        restoreRunning = true
        restoreHint.text = "正在恢复(大备份解包较慢,请勿关闭应用)…"
        restoreHint.isError = false
        Core.restoreBackup(restorePendingPath)
    }
    /// 打开导入确认卡(同恢复:验收跳过原生对话框)。
    function openImportConfirm(path) {
        importPendingPath = path
        importConfirm.visible = true
        revealItem(importConfirm)
    }
    /// 执行导入:备份条目并入当前数据(去重跳过重复)。结果走 importFinished。
    function commitImport() {
        importConfirm.visible = false
        if (importPendingPath === "") return
        importRunning = true
        importHint.text = "正在导入…"
        importHint.isError = false
        Core.importBackup(importPendingPath)
    }

    contentItem: ColumnLayout {
        spacing: 0

        // ---- 固定头部:标题 + 关闭(不随内容滚动) ----
        // ⚠️ 此前这行是 Flickable 内容的第一行:滚到下半页后关闭按钮跟着滚出
        //    可视区,必须滑回顶部才点得到 ✕(用户实测反馈)。头部独立出来后,
        //    关闭入口**恒在**;Esc 与点抽屉外也能关(见 AppDrawer 的 closePolicy)。
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "设置"; font.pixelSize: Theme.metrics.fontPageTitle
                font.weight: Font.DemiBold; color: Theme.colors.textPrimary
            }
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "settingsClose"   // 验收钩子(--settings-flow 真实点击)
                variant: "ghost"
                square: true
                text: "✕"
                Layout.alignment: Qt.AlignVCenter
                onClicked: root.close()
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: form.implicitHeight + 12
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: form
                width: flick.width
                spacing: Theme.metrics.spacingXs

                Label { text: "历史上限(条)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
                ReadableSpinBox {
                    id: historyLimit
                    objectName: "historyLimitSpin"   // 验收钩子(--settings-flow 手输链路)
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

                // 文本条目上限:此前**只有字段没有控件**(settings.max_text_kb 一直吃默认 1024),
                // 复制大段日志/JSON 时撞上限只能看到"文本超过大小上限,未入库"却无从修改。
                Label { text: "单条文本上限(KB)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
                ReadableSpinBox {
                    id: maxTextKb
                    objectName: "maxTextKbSpin"   // 验收钩子
                    Layout.fillWidth: true
                    from: 1; to: 4096; stepSize: 64
                    editable: true
                    onValueModified: root.scheduleSave()
                }

                ReadableCheckBox {
                    id: monitorCheck
                    text: "剪贴板监听"
                    onClicked: root.scheduleSave()
                }
                // 复制后自动粘贴(N2):默认关闭。文案点明"模拟 Ctrl+V"与失效场景 ——
                // 管理员权限窗口收不到模拟按键是 Windows 的 UIPI 限制,不写清楚
                // 用户会当成 bug。
                ReadableCheckBox {
                    id: autoPasteCheck
                    objectName: "autoPasteCheck"   // 验收钩子
                    text: "复制后自动粘贴(模拟 Ctrl+V;管理员窗口无效)"
                    onClicked: root.scheduleSave()
                }
                // 框选之后去哪儿(§18.4):**单一出口**取代原先两个复选框。
                // 原先 autoConfirm × skipEditor 叠出四种组合,其中「松开即确认 + 进编辑器」
                // 自相矛盾、无法向用户解释;且"框选进编辑器"这条出口已下线(§18.5)——
                // 编辑器只保留唯一入口 = 右键编辑(主面板条目右键 / 贴图右键)。
                Label {
                    text: "框选后"
                    color: Theme.colors.textSecondary
                    font.pixelSize: Theme.metrics.fontCaption
                }
                ReadableComboBox {
                    id: regionExitCombo
                    objectName: "regionExitCombo"   // 验收钩子
                    Layout.fillWidth: true
                    /// 与 model 一一对应的标识:
                    ///   direct  —— 松手即入库 + 写剪贴板
                    ///   toolbar —— 选区保留并浮出工具栏(就地标注/OCR/贴图/保存/完成)
                    readonly property var exitCodes: ["direct", "toolbar"]
                    model: ["直接入库并复制(松手即完成)",
                            "浮出工具栏(可标注 / OCR / 贴图 / 保存)"]
                    onActivated: root.scheduleSave()
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

                // ---- 滚动长截图(docs/19 §6)----
                //
                // 上限要与「单张图片上限」配合看:入库按编码后的 PNG 体积卡
                // maxImageMb,超了只写剪贴板、不入库(R3 会在入库超限时再提示一次)。
                // 这里做**软提示**而不是硬拦 —— 生成时的体积取决于内容重复度,
                // 事先算不准,拦死会误伤。
                Label { text: "长截图高度上限(px)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
                ReadableSpinBox {
                    id: stitchMaxHeight
                    Layout.fillWidth: true
                    from: 2000; to: 100000; stepSize: 1000
                    editable: true
                    onValueModified: root.scheduleSave()
                }
                HintLabel {
                    text: "超过此高度会停止采集并提示分段。长图还需满足上面的「单张图片上限」:"
                          + "太高时可能存不进历史(此时仍会进剪贴板)。"
                }

                Label { text: "长截图重叠判定门槛(%)"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
                ReadableSpinBox {
                    id: stitchOverlap
                    Layout.fillWidth: true
                    from: 1; to: 20
                    editable: true
                    onValueModified: root.scheduleSave()
                }
                HintLabel {
                    text: "门槛越低,越容易把重复图案(表格线/纯色块)误认为已重叠 —— 长图会缺行;"
                          + "越高则要求每次滚动幅度更小。默认 3。"
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
                // 渲染后端(重启生效):软件(CPU 光栅)是**默认**——实测比硬件(D3D11)
                // 省 ~55MB 首帧 / ~101MB 稳态(隐藏态 231MB → 130MB),代价是渲染改由
                // CPU 承担(滚动/动画)。显卡充足且更在意跟手感的用户可关掉它。
                ReadableCheckBox {
                    id: softwareRenderCheck
                    text: "软件渲染(省内存,重启生效)"
                    onClicked: {
                        root.renderDirty = true
                        root.scheduleSave()
                    }
                }
                HintLabel {
                    text: "默认开启:用 CPU 光栅替代 GPU(D3D11)渲染,约省 100MB 内存;滚动/动画更吃 CPU。关闭后重启生效(也可用 --hardware 启动参数临时切换)。"
                }
                ReadableCheckBox {
                    id: autoStartCheck
                    text: "开机自启(静默驻留托盘,不弹面板)"
                    // 勾选态 = 注册表 Run 键实际存在(不是本地布尔)。
                    // 命令行形态固定为 `"<exe>" --hidden`,与安装器同一写法;
                    // 老版本留下的无 --hidden 值由启动时的 refreshCommandLineIfStale() 就地升级。
                    checked: Core.autoStartEnabled()
                    onClicked: {
                        // 注册表操作立即生效并回读实际结果,失败时还原勾选态并提示原因
                        if (Core.setAutoStart(checked)) {
                            checked = Core.autoStartEnabled()
                        } else {
                            checked = !checked
                            Core.showToast("开机自启设置失败:写入注册表被拒绝(可能被安全软件或组策略拦截)")
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

                // ---- 备份与导出(手动入口;zip/Markdown 全量 I/O 秒级,走 Core 异步) ----
                FieldLabel { text: "备份" }
                HintLabel {
                    text: "打包全部数据(数据库 + 图片)为单个 zip 快照,用于换机或留存。"
                }
                ThemedButton {
                    Layout.fillWidth: true
                    variant: "secondary"
                    text: backupRunning ? "备份中…" : "创建备份…"
                    enabled: !backupRunning
                    onClicked: {
                        const path = Core.saveFileDialog(
                            "选择备份保存位置",
                            "备份文件 (*.zip)",
                            "nextboard-backup-" + new Date().toISOString().slice(0, 10) + ".zip")
                        if (path === "") return
                        backupRunning = true
                        backupHint.text = "正在创建备份…"
                        backupHint.isError = false
                        Core.backupCreate(path)
                    }
                }
                HintLabel {
                    id: backupHint
                    property bool isError: false
                    color: isError ? Theme.colors.error : Theme.colors.textSecondary
                    wrapMode: Text.WrapAnywhere
                }

                FieldLabel { text: "导出" }
                HintLabel {
                    text: "Markdown:人读,图片以 base64 内嵌,可直接在 Typora/Obsidian 打开。"
                        + "JSON / CSV:给别的工具吃,图片只记库内相对路径(不内嵌)。"
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ThemedButton {
                        objectName: "exportMarkdownButton"   // 验收钩子
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: exportRunning ? "导出中…" : "Markdown…"
                        enabled: !exportRunning
                        onClicked: root.exportAs("markdown", "Markdown (*.md)", ".md")
                    }
                    ThemedButton {
                        objectName: "exportJsonButton"   // 验收钩子
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: exportRunning ? "导出中…" : "JSON…"
                        enabled: !exportRunning
                        onClicked: root.exportAs("json", "JSON (*.json)", ".json")
                    }
                    ThemedButton {
                        objectName: "exportCsvButton"   // 验收钩子
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: exportRunning ? "导出中…" : "CSV…"
                        enabled: !exportRunning
                        onClicked: root.exportAs("csv", "CSV (*.csv);;所有文件 (*)", ".csv")
                    }
                }
                HintLabel {
                    id: exportHint
                    property bool isError: false
                    color: isError ? Theme.colors.error : Theme.colors.textSecondary
                    wrapMode: Text.WrapAnywhere
                }

                // ---- 恢复 / 导入(备份 zip 的消费端) ----
                FieldLabel { text: "恢复与导入" }
                HintLabel {
                    text: "恢复:用备份**替换**当前全部数据(执行前自动把当前数据备份到所选文件同目录)。导入:把备份里的条目**并入**当前数据,内容相同的自动跳过。"
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ThemedButton {
                        objectName: "restoreBackupButton"   // 验收钩子(--restore-flow 真实点击)
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: root.restoreRunning ? "恢复中…" : "恢复备份…"
                        enabled: !root.restoreRunning && !root.importRunning
                                && !root.backupRunning
                        onClicked: {
                            const path = Core.openFileDialog("选择要恢复的备份文件",
                                                             "备份文件 (*.zip)")
                            if (path === "") return
                            root.openRestoreConfirm(path)
                        }
                    }
                    ThemedButton {
                        objectName: "importBackupButton"   // 验收钩子(--restore-flow 真实点击)
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: root.importRunning ? "导入中…" : "导入备份…"
                        enabled: !root.restoreRunning && !root.importRunning
                                && !root.backupRunning
                        onClicked: {
                            const path = Core.openFileDialog("选择要导入的备份文件",
                                                             "备份文件 (*.zip)")
                            if (path === "") return
                            root.openImportConfirm(path)
                        }
                    }
                }
                // 就地确认块(与「清空历史」的 clearConfirm 同型:破坏性操作必须二次确认)
                ColumnLayout {
                    id: restoreConfirm
                    Layout.fillWidth: true
                    visible: false
                    spacing: Theme.metrics.spacingXs
                    HintLabel {
                        text: "确认恢复?当前全部数据(条目 + 图片)将被该备份替换。\n执行前会自动把当前数据备份到所选文件同目录(nextboard-pre-restore-*.zip)。"
                        color: Theme.colors.error
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.metrics.spacingSm
                        ThemedButton {
                            objectName: "restoreConfirmBtn"   // 验收钩子
                            Layout.fillWidth: true
                            variant: "danger"
                            text: "确认恢复"
                            onClicked: root.commitRestore()
                        }
                        ThemedButton {
                            Layout.fillWidth: true
                            variant: "secondary"
                            text: "取消"
                            onClicked: restoreConfirm.visible = false
                        }
                    }
                }
                HintLabel {
                    id: restoreHint
                    property bool isError: false
                    color: isError ? Theme.colors.error : Theme.colors.textSecondary
                    wrapMode: Text.WrapAnywhere
                }
                ColumnLayout {
                    id: importConfirm
                    Layout.fillWidth: true
                    visible: false
                    spacing: Theme.metrics.spacingXs
                    HintLabel {
                        text: "确认导入?备份里的条目将并入当前数据,内容相同的自动跳过;不会删除或改动现有条目。"
                        color: Theme.colors.error
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.metrics.spacingSm
                        ThemedButton {
                            objectName: "importConfirmBtn"   // 验收钩子
                            Layout.fillWidth: true
                            variant: "danger"
                            text: "确认导入"
                            onClicked: root.commitImport()
                        }
                        ThemedButton {
                            Layout.fillWidth: true
                            variant: "secondary"
                            text: "取消"
                            onClicked: importConfirm.visible = false
                        }
                    }
                }
                HintLabel {
                    id: importHint
                    property bool isError: false
                    color: isError ? Theme.colors.error : Theme.colors.textSecondary
                    wrapMode: Text.WrapAnywhere
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
                // 长截图是**独立入口**:直通模式(框选后→直接入库)下浮条不出现,
                // 没有热键就完全够不到这个功能,故默认预置 Ctrl+Shift+L(docs/19 §9 Q3)。
                FieldLabel { text: "滚动长截图" }
                HotkeyField {
                    id: hkCaptureScrolling
                    Layout.fillWidth: true
                    onValueModified: {
                        root.hotkeysDirty = true
                        root.scheduleSave()
                    }
                }

                // ---- 文字识别(OCR)----
                // 引擎由用户显式选择(决策 D6:不做 auto 兜底链)—— 选哪个就是哪个,
                // 不会出现"我选了内置却跑了 RapidOCR"这种不可预测行为。
                SectionDivider {}
                SectionTitle { text: "文字识别" }
                ReadableComboBox {
                    id: ocrBackendCombo
                    objectName: "ocrBackendCombo"   // 验收钩子
                    Layout.fillWidth: true
                    /// 与 model 一一对应的后端标识(持久化到 settings.ocrBackend)。
                    /// ⚠️ 必须与 core 侧 `settings::ocr_backend::is_supported` **同口径**;
                    /// 值域由该函数的单测交叉锁定,改这里要同步改 core。
                    readonly property var backendCodes: ["system", "rapidocr"]
                    model: ["系统内置(开箱即用,需语言包)", "RapidOCR(自带模型,中英混排更准)"]
                    onActivated: {
                        root.scheduleSave()
                        root.refreshOcrProbe()
                    }
                }
                // 可用性与不可用原因:core 的 ocr_probe 聚合两路后端(命中缓存,无需防抖)。
                HintLabel {
                    id: ocrBackendHint
                    text: root.ocrBackendHintText
                    color: root.ocrBackendAvailable ? Theme.colors.textSecondary : Theme.colors.danger
                }
                // ---- 仅「系统内置」后端有效 ----
                // 显隐依据(core 侧注释,不是猜测):
                //   ocr/rapid.rs:16-17 —— RapidOCR 自带 max_side_len,故**不应用** ocr_scale 放大;
                //   RapidOCR 中英混排一体,ocr_language 对它无效。
                // 之前这两个字段没有控件、两套参数语义分叉(docs/10 §15.6 B3.6);现在按后端
                // 显隐:选了 RapidOCR 就不显示,免得用户调了没反应还以为是 bug。
                FieldLabel {
                    text: "识别语言(仅系统内置)"
                    visible: root.builtinOcr
                }
                ReadableComboBox {
                    id: ocrLanguageCombo
                    objectName: "ocrLanguageCombo"   // 验收钩子
                    Layout.fillWidth: true
                    visible: root.builtinOcr
                    /// 与 core settings::validate 同口径:auto 或语言标签(字母数字/-/_)
                    readonly property var languageCodes: ["auto", "zh-Hans-CN", "en-US", "ja-JP"]
                    model: ["自动识别(按系统语言)", "简体中文", "英语(美国)", "日语"]
                    onActivated: root.scheduleSave()
                }
                FieldLabel {
                    text: "放大倍数(仅系统内置)"
                    visible: root.builtinOcr
                }
                ReadableSpinBox {
                    id: ocrScaleSpin
                    objectName: "ocrScaleSpin"   // 验收钩子
                    Layout.fillWidth: true
                    visible: root.builtinOcr
                    from: 1; to: 4
                    editable: true
                    onValueModified: root.scheduleSave()
                }
                HintLabel {
                    visible: root.builtinOcr
                    text: "小字号截图放大后识别更准,代价是耗时随倍数上升。"
                    color: Theme.colors.textSecondary
                }
                FieldLabel { text: "识别偏好(仅 RapidOCR)" }
                ReadableComboBox {
                    id: ocrProfileCombo
                    Layout.fillWidth: true
                    /// 与 core 的 settings::ocr_profile 同口径(validate 会拒绝其它值)。
                    readonly property var profileCodes: ["quality", "balanced", "compact"]
                    model: ["质量优先(小字最准,内存占用最高)",
                            "均衡(默认,内存与质量折中)",
                            "内存优先(最省内存,密集小字可能漏行)"]
                    onActivated: root.scheduleSave()
                }
                HintLabel {
                    // 全屏截图的 det 输入低于所有档位上限(docs/12 §14.2),
                    // 档位只影响小图/宽扁图 —— 写明以防"选了没效果"的误解。
                    text: "对全屏截图的内存占用无影响;影响小图与长条截图"
                    color: Theme.colors.textSecondary
                }
                FieldLabel { text: "引擎驻留(仅 RapidOCR)" }
                ReadableComboBox {
                    id: ocrSessionCombo
                    Layout.fillWidth: true
                    readonly property var modeCodes: ["resident", "idle60", "ephemeral"]
                    model: ["常驻(识别最快,后台约占 40MB)",
                            "空闲 1 分钟后释放",
                            "用完即销毁(最省内存,每次多约 0.3 秒)"]
                    onActivated: root.scheduleSave()
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
                        // 技术字段:请求头逐行 "Name: Value",等宽才对齐
                        font.family: Theme.fontMono
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
                        // 技术字段:JSON 模板,等宽缩进与引号/括号配对最受益
                        font.family: Theme.fontMono
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
                          ? "删除全部 " + Core.counts.all + " 条数据(剪贴板历史 + 待办,含图片文件),此操作不可恢复。"
                          : "当前没有任何数据。"
                }
                ThemedButton {
                    id: clearButton
                    objectName: "clearAllBtn"   // 验收钩子(--clear-all-flow 真实点击)
                    Layout.fillWidth: true
                    variant: "danger"
                    text: "清空全部数据"
                    // 无数据时置灰,避免"点了没反应"的困惑
                    enabled: Core.counts.all > 0
                    onClicked: {
                        clearConfirm.visible = true
                        root.revealItem(clearConfirm)   // 同恢复确认卡:滚进可视区
                    }
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
                        text: "确认清空全部数据(含待办)?此操作不可恢复。"
                        color: Theme.colors.error
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.metrics.spacingSm
                        ThemedButton {
                            // ⚠️ 用父块 visible 做条件:子项自身的 visible 在父隐藏时仍是 true,
                            //    直接读会拿到"隐藏中"的按钮,验收点击会落空(clearDoneLink 同教训)
                            objectName: clearConfirm.visible ? "clearAllConfirmBtn" : ""
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

                // 更新检查(S6.4 v1,docs/07-auto-update.md):检查地址的**唯一**配置点是
                // CMake 的 NB_UPDATE_CHECK_URL。留空 = 功能禁用 —— 按钮仍可见但置灰,原因写在
                // 上面那行,不留"点了没反应"的歧义;渠道定了填地址重编即启用,这里零改动。
                HintLabel {
                    id: updateConfigHint
                    visible: Core.updateCheckUrl === ""
                    text: "更新源未配置:发布渠道确定后,在 app/CMakeLists.txt 填入"
                          + " NB_UPDATE_CHECK_URL 并重编即可启用检查更新。"
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    ThemedButton {
                        id: updateCheckButton
                        objectName: "updateCheckBtn"   // 验收钩子(--update-flow 真实点击)
                        variant: "secondary"
                        text: root.updateChecking ? "检查中…" : "检查更新"
                        enabled: Core.updateCheckUrl !== "" && !root.updateChecking
                        onClicked: {
                            root.updateChecking = true
                            root.updateDownloadUrl = ""
                            updateHint.isError = false
                            updateHint.text = "正在检查更新…"
                            Core.checkUpdate()
                        }
                    }
                    ThemedButton {
                        // ⚠️ 条件 objectName:按钮只在有下载链接时出现,隐藏期间若仍带名字,
                        //    验收 DFS 会把它当命中目标、点到别处(clearDoneLink 同教训)。
                        objectName: visible ? "updateOpenPageBtn" : ""
                        variant: "secondary"
                        visible: root.updateDownloadUrl !== ""
                        text: "打开下载页"
                        onClicked: Core.openExternalUrl(root.updateDownloadUrl)
                    }
                    Item { Layout.fillWidth: true }
                }
                HintLabel {
                    id: updateHint
                    property bool isError: false
                    color: isError ? Theme.colors.error : Theme.colors.textSecondary
                    wrapMode: Text.WrapAnywhere
                }

                // 改动即时保存(去抖 400ms),故不再有「保存 / 关闭」按钮 ——
                // 关闭走固定头部的 ✕、Esc、或点抽屉外(AppDrawer 的 closePolicy)。
                // 状态行只用于反馈结果(尤其是校验失败)。
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
        function onBackupFinished(resp) {
            root.backupRunning = false
            if (!resp.ok) {
                backupHint.text = "备份失败:" + root.errText(resp)
                backupHint.isError = true
                return
            }
            const d = resp.data || {}
            backupHint.text = "✓ 备份完成:" + d.entryCount + " 条条目、"
                    + d.blobCount + " 个图片文件(" + (d.zipBytes / 1048576).toFixed(1) + " MB)"
            backupHint.isError = false
        }
        function onExportFinished(resp) {
            root.exportRunning = false
            if (!resp.ok) {
                exportHint.text = "导出失败:" + root.errText(resp)
                exportHint.isError = true
                return
            }
            const d = resp.data || {}
            exportHint.text = "✓ 导出完成:" + d.entryCount + " 条条目("
                    + (d.fileBytes / 1048576).toFixed(1) + " MB)"
            exportHint.isError = false
        }
        function onRestoreFinished(resp) {
            root.restoreRunning = false
            if (!resp.ok) {
                restoreHint.text = "恢复失败:" + root.errText(resp)
                restoreHint.isError = true
                return
            }
            const d = resp.data || {}
            restoreHint.text = "✓ 已恢复:" + d.entryCount + " 条条目、"
                    + d.blobCount + " 个图片文件;恢复前数据已自动备份到 "
                    + d.safetyBackupPath
            restoreHint.isError = false
            Entries.refreshNow()   // 列表/计数切到恢复后的数据
        }
        function onImportFinished(resp) {
            root.importRunning = false
            if (!resp.ok) {
                importHint.text = "导入失败:" + root.errText(resp)
                importHint.isError = true
                return
            }
            const d = resp.data || {}
            importHint.text = "✓ 已导入 " + d.imported + " 条(跳过重复 " + d.skipped + " 条)"
                    + (d.blobMissing > 0
                       ? ";有 " + d.blobMissing + " 张图片的文件在备份中缺失" : "")
            importHint.isError = false
            Entries.refreshNow()
        }
        function onUpdateFinished(resp) {
            root.updateChecking = false
            if (!resp.ok) {
                updateHint.text = "检查失败:" + root.errText(resp)
                updateHint.isError = true
                return
            }
            const d = resp.data || {}
            if (d.configured === false) {
                // 常量在运行期是空的(不该走到这里:按钮此时置灰)—— 兜底成一句人话
                updateHint.text = "更新源未配置"
                updateHint.isError = false
                return
            }
            // ⚠️ 只在真的要更新时才交出下载链接:已最新 / 被忽略的预发布,把老版本(或 rc)
            //    的发布页摊在设置页上是噪音 —— --update-flow 的 /same 模式实测抓出过。
            root.updateDownloadUrl = d.hasUpdate ? (d.downloadUrl || "") : ""
            updateHint.isError = false
            if (d.hasUpdate) {
                const date = d.publishedAt ? "（" + d.publishedAt + "）" : ""
                updateHint.text = "发现新版本 " + d.latestVersion + date
                        + (root.updateDownloadUrl === "" ? ";本次响应未给出下载链接" : "")
            } else {
                updateHint.text = "已是最新版本 " + (d.currentVersion || Core.appVersion)
            }
        }
    }
}
