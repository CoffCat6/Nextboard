// 独立翻译窗口(theme.md §22/§23):原文(Source)与译文(Target)两块 Panel,
// 中置语言栏与主操作,底部为取内容/清空快捷动作。
//
// 这是**独立窗口**,不是主面板的抽屉:
// - 不随主面板失焦隐藏,输入长文时不会因为切走焦点而消失;
// - 可拖动标题栏移动(translateWindow.startSystemMove,交给系统处理);
// - 打开时由 C++ 侧收起主面板,两者不并存(见 main.cpp 的 translatePrefill 接线)。
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

    /// 翻译进行中:结果区显示细进度条,主按钮进入等待态(§35,不用大 Spinner)。
    property bool translating: false

    /// 目标语言持久化:settings.translateTargetLang 为唯一来源。
    /// ComboBox 用纯字符串 model(见 ReadableComboBox 的注入约束),
    /// 语言代码由 targetLang.langCodes 平行数组维护。
    function loadTargetLang() {
        const resp = Core.call("get_settings")
        if (!resp.ok) return
        const code = resp.data.translateTargetLang
        const idx = targetLang.langCodes.indexOf(code)
        if (idx >= 0) targetLang.currentIndex = idx
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

    /// C++ TranslateWindow::openWith 调用:预填原文(空 = 手动输入)并按需自动翻译。
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
        Core.translateText(text, "", targetLang.langCodes[targetLang.currentIndex])
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
        const idx = targetLang.langCodes.indexOf(item.to)
        if (idx >= 0) targetLang.currentIndex = idx
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

    Keys.onEscapePressed: {
        // 历史抽屉优先关闭,再退到关窗
        if (historyOverlay.visible) historyOverlay.visible = false
        else translateWindow.hideWindow()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- 标题栏(§16:48px;整条可拖动,关闭按钮浮于其上) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.metrics.headerHeight
            color: Theme.colors.surface
            radius: Theme.metrics.radiusWindow

            // 只圆上面两角:用一块覆盖底边的矩形把下方圆角抹平,与内容区衔接
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: parent.radius
                color: Theme.colors.surface
            }
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
                onActiveChanged: if (active) translateWindow.startSystemMove()
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
                    onClicked: translateWindow.hideWindow()
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
                        Keys.onEscapePressed: translateWindow.hideWindow()
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
                    Layout.preferredWidth: 150
                    Layout.preferredHeight: Theme.metrics.controlHeight
                    /// 与 model 一一对应的语言代码(持久化到 settings.translateTargetLang)
                    readonly property var langCodes: ["zh-Hans", "zh-Hant", "en", "ja",
                                                      "ko", "fr", "de", "ru"]
                    model: ["简体中文", "繁體中文", "English", "日本語",
                            "한국어", "Français", "Deutsch", "Русский"]
                    onActivated: root.saveTargetLang(langCodes[currentIndex])
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
                        // 先收起本窗口,再框选;否则会被拍进画面
                        translateWindow.hideWindow()
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
