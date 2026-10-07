// 翻译抽屉(§22):原文(Source)→ 译文(Target)两块 Panel,中置语言与动作行;
// 走 Core.translateText(线程池异步,不卡 UI),结果经 translateFinished 回投。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

AppDrawer {
    id: root

    /// 目标语言持久化:settings.translateTargetLang 为唯一来源(progress.md 遗留项)。
    function loadTargetLang() {
        const resp = Core.call("get_settings")
        if (!resp.ok) return
        const code = resp.data.translateTargetLang
        for (let i = 0; i < targetLang.count; ++i) {
            if (targetLang.valueAt(i) === code) { targetLang.currentIndex = i; break }
        }
    }

    function saveTargetLang(code) {
        const resp = Core.call("get_settings")
        if (!resp.ok) return
        const s = resp.data
        if (s.translateTargetLang === code) return
        s.translateTargetLang = code
        Core.call("save_settings", s)
    }

    function openAndFocus() {
        open()
        reset()
        loadTargetLang()
        input.forceActiveFocus()
    }

    /// 条目右键「翻译」入口:预填原文并立即翻译。
    function openWith(text) {
        open()
        reset()
        loadTargetLang()
        input.text = text
        if (text && text.trim().length > 0) doTranslate()
        else input.forceActiveFocus()
    }

    /// 重开抽屉即清空上一次会话(原文/译文/状态),残留内容易被误读为新结果;
    /// openWith 在 reset 之后预填,自动翻译链路不受影响。
    function reset() {
        input.clear()
        output.text = ""
        statusLabel.text = ""
        statusLabel.isError = false
        root.translating = false
    }

    function doTranslate() {
        const text = input.text.trim()
        if (!text) return
        statusLabel.text = "翻译中…"
        statusLabel.isError = false
        root.translating = true
        Core.translateText(text, "", targetLang.currentValue)
    }

    /// 翻译进行中:结果区显示细进度条(§35,不用大 Spinner)。
    /// 声明在函数之后无碍——QML 属性初始化先于任何函数调用。
    property bool translating: false

    contentItem: ColumnLayout {
        spacing: Theme.metrics.spacingXs

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "翻译"; font.pixelSize: Theme.metrics.fontPageTitle
                font.weight: Font.DemiBold; color: Theme.colors.textPrimary
            }
            Item { Layout.fillWidth: true }
            ToolButton { text: "✕"; onClicked: root.close() }
        }

        // ---- 原文 Panel(§22:Surface + Border + 8px 圆角) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            radius: Theme.metrics.radiusCard
            color: Theme.colors.surface
            border.color: input.activeFocus ? Theme.colors.accent : Theme.colors.border
            border.width: Theme.metrics.borderWidth
            Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }

            TextArea {
                id: input
                anchors.fill: parent
                anchors.margins: Theme.metrics.spacingSm
                background: null
                placeholderText: "输入或粘贴要翻译的文本…"
                placeholderTextColor: Theme.colors.textTertiary
                color: Theme.colors.textPrimary
                wrapMode: TextArea.WrapAnywhere
                Keys.onPressed: function(event) {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                            && (event.modifiers & Qt.ControlModifier)) {
                        root.doTranslate()
                        event.accepted = true
                    }
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: Theme.metrics.spacingSm

            Text {
                text: "自动检测"
                color: Theme.colors.textTertiary
                font.pixelSize: Theme.metrics.fontCaption
                Layout.alignment: Qt.AlignVCenter
            }
            // 流向提示(§22 的 Source → Target;源语言固定 auto,不做交换)
            Text {
                text: "→"
                color: Theme.colors.textTertiary
                font.pixelSize: Theme.metrics.fontBody
                Layout.alignment: Qt.AlignVCenter
            }
            ReadableComboBox {
                id: targetLang
                Layout.preferredWidth: 130
                Layout.preferredHeight: Theme.metrics.controlHeight
                textRole: "label"
                valueRole: "code"
                model: [
                    { label: "简体中文", code: "zh-Hans" },
                    { label: "English",  code: "en" },
                    { label: "日本語",   code: "ja" },
                    { label: "한국어",    code: "ko" }
                ]
                onActivated: root.saveTargetLang(currentValue)
            }
            Button {
                text: "翻译"
                highlighted: true
                Layout.preferredHeight: Theme.metrics.controlHeight
                onClicked: root.doTranslate()
            }
        }

        Label {
            id: statusLabel
            property bool isError: false
            Layout.fillWidth: true
            color: isError ? Theme.colors.error : Theme.colors.textTertiary
            font.pixelSize: Theme.metrics.fontCaption
            wrapMode: Text.WrapAnywhere
        }

        // ---- 译文 Panel(§23:结果区独立更新) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            color: Theme.colors.surface
            border.color: Theme.colors.border
            border.width: Theme.metrics.borderWidth
            radius: Theme.metrics.radiusCard

            ScrollView {
                anchors.fill: parent
                anchors.margins: Theme.metrics.spacingSm
                TextArea {
                    id: output
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    color: Theme.colors.textPrimary
                    background: null
                    text: ""
                }
            }

            // 空结果占位(§41:小字提示,非插画)
            Column {
                anchors.centerIn: parent
                visible: output.text === "" && !root.translating
                spacing: Theme.metrics.spacingXs
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "译文"
                    color: Theme.colors.textTertiary
                    font.pixelSize: Theme.metrics.fontCaption
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "输入原文后点击翻译,或使用截图取字"
                    color: Theme.colors.textDisabled
                    font.pixelSize: Theme.metrics.fontMicro + 1
                }
            }
        }

        RowLayout {
            spacing: Theme.metrics.spacingSm
            // 截图取字(F25 的取字分支):收起面板 → 框选 → OCR → 文本进剪贴板
            Button {
                text: "截图取字"
                Layout.preferredHeight: Theme.metrics.controlHeight
                onClicked: {
                    root.close()
                    panelWindow.hidePanel()
                    Core.captureOcr()
                }
            }
            Button {
                text: "复制译文"
                Layout.preferredHeight: Theme.metrics.controlHeight
                enabled: output.text.length > 0
                onClicked: {
                    const resp = Core.call("copy_text_to_clipboard", { text: output.text })
                    Core.showToast(resp.ok ? "译文已复制"
                                  : "复制失败:" + (resp.error ? resp.error.message : ""))
                }
            }
            Item { Layout.fillWidth: true }
        }
    }

    // 翻译是线程池异步执行(Core.translateText),结果经 translateFinished 回投。
    // 原实现漏接此信号,译文永远不会显示——补齐后翻译抽屉才真正可用。
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
