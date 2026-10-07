// 翻译抽屉:原文 / 目标语言 / 译文,走 Core.translateText(线程池异步,不卡 UI)。
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
        loadTargetLang()
        input.forceActiveFocus()
    }

    /// 条目右键「翻译」入口:预填原文并立即翻译。
    function openWith(text) {
        open()
        loadTargetLang()
        input.text = text
        if (text && text.trim().length > 0) doTranslate()
        else input.forceActiveFocus()
    }

    function doTranslate() {
        const text = input.text.trim()
        if (!text) return
        statusLabel.text = "翻译中…"
        statusLabel.isError = false
        Core.translateText(text, "", targetLang.currentValue)
    }

    contentItem: ColumnLayout {
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "翻译"; font.pixelSize: Theme.metrics.fontPageTitle
                font.weight: Font.DemiBold; color: Theme.colors.textPrimary
            }
            Item { Layout.fillWidth: true }
            ToolButton { text: "✕"; onClicked: root.close() }
        }

        TextArea {
            id: input
            Layout.fillWidth: true
            Layout.preferredHeight: 84
            placeholderText: "输入或粘贴要翻译的文本…"
            wrapMode: TextArea.WrapAnywhere
            Keys.onPressed: function(event) {
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        && (event.modifiers & Qt.ControlModifier)) {
                    root.doTranslate()
                    event.accepted = true
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "目标语言"; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            ComboBox {
                id: targetLang
                Layout.preferredWidth: 140
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
            Button { text: "翻译"; highlighted: true; onClicked: root.doTranslate() }
        }

        Label {
            id: statusLabel
            property bool isError: false
            Layout.fillWidth: true
            color: isError ? Theme.colors.error : Theme.colors.textSecondary
            font.pixelSize: Theme.metrics.fontCaption
            wrapMode: Text.WrapAnywhere
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            color: Theme.colors.surfaceSecondary
            border.color: Theme.colors.border
            border.width: Theme.metrics.borderWidth
            radius: Theme.metrics.radiusCard

            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                TextArea {
                    id: output
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    color: Theme.colors.textPrimary
                    text: ""
                }
            }
        }

        RowLayout {
            spacing: 8
            // 截图取字(F25 的取字分支):收起面板 → 框选 → OCR → 文本进剪贴板
            Button {
                text: "截图取字"
                onClicked: {
                    root.close()
                    panelWindow.hidePanel()
                    Core.captureOcr()
                }
            }
            Button {
                text: "复制译文"
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
