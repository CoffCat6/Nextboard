// 翻译视图(theme.md §22/§23):原文(Source)与译文(Target)两块 Panel,
// 中置语言栏与主操作,底部为取内容/清空快捷动作。
//
// 这是**主面板窗口内的一页**,不是独立窗口、也不是抽屉(见 Main.qml 的 mode 切换):
// - 与剪贴板列表互斥显示:同一窗口、同一 QQuickWidget、同一 QQmlEngine,
//   因此不再需要第二个 QML 引擎与它自己的场景图/渲染栈;
// - 进入本页时 C++ 侧会把窗口尺寸切到翻译尺寸并**自动固定**(失焦不隐藏),
//   保住"输入长文时不会因为切走焦点而消失"的原有体验;
// - 标题栏拖动走 panelWindow.startSystemMove()(同一个窗口)。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: root
    focus: true

    /// 翻译进行中:结果区显示细进度条,主按钮进入等待态(§35,不用大 Spinner)。
    property bool translating: false

    /// 目标语言持久化:settings.translateTargetLang 为唯一来源。
    /// ComboBox 用纯字符串 model(见 ReadableComboBox 的注入约束),
    /// 语言代码由 targetLang.langCodes 平行数组维护。
    ///
    /// ⚠️ **代码输入框才是权威**:下拉只覆盖常用语言,代码框可填任意服务商代码
    /// (如彩云小译要 `zh`、DeepL 要大写 `ZH`)。发请求时以代码框为准。
    function loadTargetLang() {
        const resp = Core.call("get_settings")
        if (!resp.ok) return
        const code = resp.data.translateTargetLang
        targetCode.text = code || ""
        const idx = targetLang.langCodes.indexOf(code)
        if (idx >= 0) targetLang.currentIndex = idx
    }

    /// 当前生效的目标语言代码:优先取代码框(用户可自定义),空则回退下拉选中项。
    function targetCodeValue() {
        const typed = targetCode.text.trim()
        if (typed.length > 0) return typed
        const idx = targetLang.currentIndex
        return idx >= 0 ? targetLang.langCodes[idx] : ""
    }

    /// 同步目标语言:下拉与代码框一起更新,并持久化。
    function applyTargetLang(code) {
        targetCode.text = code
        const idx = targetLang.langCodes.indexOf(code)
        if (idx >= 0) targetLang.currentIndex = idx
        saveTargetLang(code)
    }

    function saveTargetLang(code) {
        const resp = Core.call("get_settings")
        if (!resp.ok) return
        const s = resp.data
        if (s.translateTargetLang === code) return
        s.translateTargetLang = code
        Core.call("save_settings", s)
    }

    function reset() {
        input.text = ""
        output.text = ""
        statusLabel.text = ""
        statusLabel.isError = false
        root.translating = false
        historyOverlay.visible = false
    }

    /// C++ 侧进入本页时调用:预填原文(空 = 手动输入)并按需自动翻译。
    function openWith(text) {
        reset()
        loadTargetLang()
        if (text && text.trim().length > 0) {
            input.text = text
            doTranslate()
        } else {
            input.forceActiveFocus()
        }
    }

    function doTranslate() {
        const text = input.text.trim()
        if (!text) {
            statusLabel.text = "请输入要翻译的文本"
            statusLabel.isError = true
            input.forceActiveFocus()
            return
        }
        statusLabel.text = "翻译中…"
        statusLabel.isError = false
        root.translating = true
        Core.translateText(text, "", root.targetCodeValue())
    }

    function copyResult() {
        if (output.text.length === 0) return
        const resp = Core.call("copy_text_to_clipboard", { text: output.text })
        Core.showToast(resp.ok ? "译文已复制"
                      : "复制失败:" + (resp.error ? resp.error.message : ""))
    }

    // ---- 翻译历史(F26) ----
    /// 最近 50 条翻译记录(core 仅内存保存、不落库,第一代 D15)。
    property var historyItems: []

    /// 打开历史抽屉:每次拉取最新列表(core 侧为唯一数据源)。
    function openHistory() {
        const resp = Core.call("translate_history")
        historyItems = (resp.ok && resp.data) ? resp.data : []
        historyOverlay.visible = true
    }

    function clearHistory() {
        Core.call("clear_translate_history")
        historyItems = []
    }

    /// 回填某条历史:原文/译文**与当时所用的语言**(第一代 D16 —— 否则语言栏
    /// 会与内容不符)。
    function applyHistoryItem(item) {
        if (!item) return
        input.text = item.sourceText || ""
        output.text = item.targetText || ""
        // 当时用的语言也要回填(含自定义代码)
        root.applyTargetLang(item.to)
        statusLabel.text = "已回填历史条目"
        statusLabel.isError = false
        historyOverlay.visible = false
    }

    // ---- 统一素材库(S5.1) ----
    /// 把原文/译文存为历史条目(source=translate):入库即参与搜索、可固定/贴图。
    function saveAsEntry(text, label) {
        if (!text || text.trim().length === 0) return
        const resp = Core.call("save_text_entry", { text: text, source: "translate" })
        Core.showToast(resp.ok ? label + "已存为条目"
                               : "保存失败:" + (resp.error ? resp.error.message : ""))
    }

    /// Esc 分层由 Main.qml 统一排队(只有它知道当前是列表页还是翻译页):
    /// 本函数只关本页最上层的浮层,返回**是否消费掉**这次 Esc。
    function dismissTopLayer() {
        if (historyOverlay.visible) {
            historyOverlay.visible = false
            return true
        }
        return false
    }

    /// 被隐藏(切回列表页 / 收起面板)时收掉自己的浮层 ——
    /// 否则下次进翻译页,历史抽屉会毫无征兆地又冒出来。
    onVisibleChanged: if (!visible) dismissTopLayer()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- 标题栏(§16:48px;整条可拖动,关闭按钮浮于其上) ----
        // 圆角与描边由主面板的窗口壳提供(本页铺满整个窗口),故这里不再自带 radius。
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

            DragHandler {
                // §42:拖动交给系统(startSystemMove),不做逐帧 setPosition 跟手。
                // 带拖动阈值,不会因为"点一下"就把窗口挪走。
                //
                // ⚠️ `target: null` 不能省:PointerHandler 的 target **默认就是声明它的父项**,
                // 于是 DragHandler 会去改这个标题栏 Rectangle 的 x/y —— 布局下次重排才弹回,
                // 拖动过程中整条标题栏(连同里面的标题与 ✕)就被挪走了(实测症状)。
                // 置 null 即"不操作任何 Item",只保留 active 信号。
                target: null
                onActiveChanged: if (active) panelWindow.startSystemMove()
            }
            HoverHandler { cursorShape: Qt.SizeAllCursor }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingSm
                spacing: Theme.metrics.spacingSm

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 8; implicitHeight: 8; radius: 4
                    color: Theme.colors.accent
                }
                Label {
                    text: "翻译"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontPageTitle
                    font.weight: Font.DemiBold
                    color: Theme.colors.textPrimary
                    Layout.alignment: Qt.AlignVCenter
                }
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "ghost"
                    square: true
                    text: "✕"
                    Layout.alignment: Qt.AlignVCenter
                    // 直接收起整个面板(而不是切回列表页):关闭翻译就到此为止,
                    // 不再顺带把主界面顶出来。
                    onClicked: panelWindow.hidePanel()
                }
            }
        }

        // ---- 内容区 ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            // 原文
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Label {
                    text: "原文"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontSection
                    font.weight: Font.DemiBold
                    color: Theme.colors.textPrimary
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: input.text.length + " 字符"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Theme.colors.textTertiary
                }
                ThemedButton {
                    variant: "ghost"
                    size: "sm"
                    text: "存为条目"
                    enabled: input.text.trim().length > 0
                    onClicked: root.saveAsEntry(input.text, "原文")
                }
                ThemedButton {
                    variant: "ghost"
                    size: "sm"
                    text: "清空"
                    enabled: input.text.length > 0
                    onClicked: {
                        input.clear()
                        output.text = ""
                        statusLabel.text = ""
                        statusLabel.isError = false
                        input.forceActiveFocus()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 110
                Layout.preferredHeight: 150
                radius: Theme.metrics.radiusCard
                color: Theme.colors.surface
                border.width: Theme.metrics.borderWidth
                border.color: input.activeFocus ? Theme.colors.accent : Theme.colors.border
                Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.metrics.spacingSm
                    clip: true
                    TextArea {
                        id: input
                        placeholderText: "输入或粘贴要翻译的文本…"
                        placeholderTextColor: Theme.colors.textTertiary
                        color: Theme.colors.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontBody
                        wrapMode: TextArea.WrapAnywhere
                        background: null
                        selectionColor: Theme.colors.accent
                        selectedTextColor: Theme.colors.accentText
                        // 右键菜单:整体替换 Basic 风格默认的 TextEditingContextMenu
                        // (英文标签 + 应用字体 14px + Basic 原生样式)。
                        // ⚠️ 菜单不能带 id,否则破坏 ContextMenu 的懒加载。
                        ContextMenu.menu: TextEditMenu { target: input }
                        // Esc 不在此处理:让它冒泡到 Main.qml 的根,由那里的分层逻辑
                        // 统一决定(先关本页浮层 → 再退出翻译页 → 再收起面板)。
                        Keys.onPressed: function(event) {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    && (event.modifiers & Qt.ControlModifier)) {
                                root.doTranslate()
                                event.accepted = true
                            }
                        }
                    }
                }
            }

            // ---- 语言栏 + 主操作(§22) ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: autoLabel.implicitWidth + 16
                    implicitHeight: Theme.metrics.controlHeightSm
                    radius: Theme.metrics.radiusInput
                    color: Theme.colors.surfaceSecondary
                    border.width: Theme.metrics.borderWidth
                    border.color: Theme.colors.border
                    Label {
                        id: autoLabel
                        anchors.centerIn: parent
                        text: "自动检测"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        color: Theme.colors.textSecondary
                    }
                }
                Text {
                    text: "→"
                    font.pixelSize: Theme.metrics.fontBody
                    color: Theme.colors.textTertiary
                    Layout.alignment: Qt.AlignVCenter
                }
                ReadableComboBox {
                    id: targetLang
                    // 宽度按**内容自适应**(语言名长短不一),并给一个下限。
                    // ⚠️ 不要写死宽度:写死后「简体中文」这类较长的名字会被省略成「…」。
                    Layout.preferredWidth: Math.max(140, implicitWidth)
                    Layout.preferredHeight: Theme.metrics.controlHeight
                    /// 与 model 一一对应的语言代码(持久化到 settings.translateTargetLang)
                    readonly property var langCodes: ["zh-Hans", "zh-Hant", "en", "ja",
                                                      "ko", "fr", "de", "ru"]
                    model: ["简体中文", "繁體中文", "English", "日本語",
                            "한국어", "Français", "Deutsch", "Русский"]
                    // 选中常用语言 → 同步写进右边的代码框并持久化
                    onActivated: root.applyTargetLang(langCodes[currentIndex])
                }
                // 语种代码:可自定义 —— 下拉只覆盖常用语言,这里能填任意服务商要求的代码
                // (彩云小译简体中文是 zh、DeepL 用大写 ZH),发请求时以它为准。
                TextField {
                    id: targetCode
                    Layout.preferredWidth: 78
                    Layout.preferredHeight: Theme.metrics.controlHeight
                    placeholderText: "语种代码"
                    horizontalAlignment: TextInput.AlignHCenter
                    // 技术字段:语言代码(zh-Hans / ZH / en)是服务商标识符,
                    // 等宽下字符可逐一核对;长度固定也便于横向对齐
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Theme.colors.textPrimary
                    placeholderTextColor: Theme.colors.textTertiary
                    selectionColor: Theme.colors.accent
                    selectedTextColor: Theme.colors.accentText
                    ToolTip.visible: hovered
                    ToolTip.text: "发请求时实际使用的语言代码,可直接改写(如 zh、ZH)"
                    background: Rectangle {
                        radius: Theme.metrics.radiusInput
                        color: Theme.colors.surfaceSecondary
                        border.width: Theme.metrics.borderWidth
                        border.color: targetCode.activeFocus ? Theme.colors.accent
                                                             : Theme.colors.border
                    }
                    // 回车或失焦即保存
                    onEditingFinished: root.saveTargetLang(text.trim())
                }
                ThemedButton {
                    variant: "ghost"
                    size: "sm"
                    text: "历史"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: root.openHistory()
                }
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "primary"
                    text: root.translating ? "翻译中…" : "翻译"
                    enabled: !root.translating
                    onClicked: root.doTranslate()
                }
            }

            // ---- 状态行 + 进行中细进度条(§35) ----
            Label {
                id: statusLabel
                property bool isError: false
                Layout.fillWidth: true
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontCaption
                color: isError ? Theme.colors.error : Theme.colors.textTertiary
                wrapMode: Text.WrapAnywhere
            }

            Rectangle {
                id: busyTrack
                Layout.fillWidth: true
                Layout.preferredHeight: 3
                visible: root.translating
                radius: 1.5
                color: Theme.colors.surfaceSecondary
                clip: true
                Rectangle {
                    id: busyThumb
                    width: busyTrack.width * 0.3
                    height: parent.height
                    radius: 1.5
                    color: Theme.colors.accent
                    NumberAnimation on x {
                        running: root.translating
                        from: -busyThumb.width
                        to: busyTrack.width
                        duration: 900
                        loops: Animation.Infinite
                    }
                }
            }

            // 译文
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Label {
                    text: "译文"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontSection
                    font.weight: Font.DemiBold
                    color: Theme.colors.textPrimary
                }
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "ghost"
                    size: "sm"
                    text: "存为条目"
                    enabled: output.text.length > 0
                    onClicked: root.saveAsEntry(output.text, "译文")
                }
                ThemedButton {
                    variant: "ghost"
                    size: "sm"
                    text: "复制译文"
                    enabled: output.text.length > 0
                    onClicked: root.copyResult()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 120
                Layout.preferredHeight: 190
                radius: Theme.metrics.radiusCard
                color: Theme.colors.surface
                border.width: Theme.metrics.borderWidth
                border.color: Theme.colors.border

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.metrics.spacingSm
                    clip: true
                    TextArea {
                        id: output
                        readOnly: true
                        text: ""
                        color: Theme.colors.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontBody
                        wrapMode: TextArea.WrapAnywhere
                        background: null
                        selectionColor: Theme.colors.accentSoft
                        selectedTextColor: Theme.colors.textPrimary
                        // 只读框:右键菜单只给「复制 / 全选」(TextEditMenu 按 readOnly 分流)
                        ContextMenu.menu: TextEditMenu { target: output }
                    }
                }

                // 空结果占位(§41:小字提示,非插画)
                Column {
                    anchors.centerIn: parent
                    spacing: Theme.metrics.spacingXs
                    visible: output.text === "" && !root.translating
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "译文将显示在这里"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        color: Theme.colors.textTertiary
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Ctrl+Enter 翻译 · 也可用「截图取字」从图片取文本"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        color: Theme.colors.textDisabled
                    }
                }
            }

            // ---- 底部动作 ----
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                ThemedButton {
                    variant: "secondary"
                    text: "截图取字"
                    onClicked: {
                        // 先收起整个面板(本页在面板窗口内,不收起来会被拍进画面)
                        panelWindow.hidePanel()
                        Core.captureOcr()
                    }
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: "Esc 关闭窗口"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Theme.colors.textTertiary
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }
    }

    // ---- 翻译历史抽屉(F26,D16:覆盖式抽屉,不改动五段布局) ----
    // 点选某条即回填原文/译文与当时语言;空白处点击或 ESC 关闭。
    Rectangle {
        id: historyOverlay
        anchors.fill: parent
        z: 100
        visible: false
        radius: Theme.metrics.radiusWindow
        color: "#00000066"   // 半透明遮罩

        // 点空白关闭(下层内容不可点,避免误操作)
        MouseArea {
            anchors.fill: parent
            onClicked: historyOverlay.visible = false
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: Theme.metrics.spacingMd
            radius: Theme.metrics.radiusCard
            color: Theme.colors.surface
            border.width: Theme.metrics.borderWidth
            border.color: Theme.colors.border

            // 吃掉点击,避免穿透到遮罩把抽屉关掉
            MouseArea { anchors.fill: parent }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.metrics.spacingSm
                spacing: Theme.metrics.spacingSm

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    Label {
                        text: "翻译历史"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontSection
                        font.weight: Font.DemiBold
                        color: Theme.colors.textPrimary
                    }
                    Label {
                        text: root.historyItems.length + " 条"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        color: Theme.colors.textTertiary
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Item { Layout.fillWidth: true }
                    ThemedButton {
                        variant: "ghost"
                        size: "sm"
                        text: "清空"
                        enabled: root.historyItems.length > 0
                        onClicked: root.clearHistory()
                    }
                    ThemedButton {
                        variant: "ghost"
                        square: true
                        text: "✕"
                        onClicked: historyOverlay.visible = false
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        anchors.fill: parent
                        clip: true
                        spacing: Theme.metrics.spacingXs
                        model: root.historyItems

                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: card.implicitHeight + 2 * Theme.metrics.spacingSm
                            radius: Theme.metrics.radiusInput
                            color: cardMouse.containsMouse ? Theme.colors.surfaceHover
                                                           : Theme.colors.surfaceSecondary
                            border.width: Theme.metrics.borderWidth
                            border.color: Theme.colors.border
                            Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

                            ColumnLayout {
                                id: card
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.leftMargin: Theme.metrics.spacingSm
                                anchors.rightMargin: Theme.metrics.spacingSm
                                anchors.topMargin: Theme.metrics.spacingSm
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.sourceText || ""
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.metrics.fontSecondary
                                    color: Theme.colors.textPrimary
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.targetText || ""
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.metrics.fontCaption
                                    color: Theme.colors.textSecondary
                                }
                                Label {
                                    text: (modelData.from || "auto") + " → " + modelData.to
                                          + " · " + (modelData.provider || "")
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.metrics.fontMicro
                                    color: Theme.colors.textTertiary
                                }
                            }

                            MouseArea {
                                id: cardMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.applyHistoryItem(modelData)
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: root.historyItems.length === 0
                        text: "暂无翻译历史"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        color: Theme.colors.textTertiary
                    }
                }
            }
        }
    }

    // 翻译在线程池异步执行,结果经 translateFinished 回投(§23:只更新结果区)
    Connections {
        target: Core
        function onTranslateFinished(resp) {
            root.translating = false
            if (resp.ok) {
                const data = resp.data
                output.text = data && data.text ? data.text : ""
                statusLabel.text = "完成 · " + (data && data.provider ? data.provider : "")
                statusLabel.isError = false
            } else {
                statusLabel.text = "翻译失败:" + (resp.error ? resp.error.message : "")
                statusLabel.isError = true
            }
        }
    }
}
