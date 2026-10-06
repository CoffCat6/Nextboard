// 翻译抽屉:原文 / 目标语言 / 译文,走 Core.translateText(线程池异步,不卡 UI)。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

AppDrawer {
    id: root

    function openAndFocus() {
        open()
        input.forceActiveFocus()
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
            Label { text: "翻译"; font.pixelSize: 15; font.bold: true; color: Theme.text }
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
            Label { text: "目标语言"; color: Theme.muted; font.pixelSize: 12 }
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
            }
            Button { text: "翻译"; highlighted: true; onClicked: root.doTranslate() }
        }

        Label {
            id: statusLabel
            property bool isError: false
            Layout.fillWidth: true
            color: isError ? Theme.danger : Theme.muted
            font.pixelSize: 12
            wrapMode: Text.WrapAnywhere
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            color: Theme.bg
            border.color: Theme.border
            border.width: 1
            radius: 6

            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                TextArea {
                    id: output
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    color: Theme.text
                    text: ""
                }
            }
        }

        RowLayout {
            spacing: 8
            Button {
                text: "复制译文"
                enabled: output.text.length > 0
                onClicked: {
                    const resp = Core.call("copy_text_to_clipboard", { text: output.text })
                    app.showToast(resp.ok ? "译文已复制"
                                  : "复制失败:" + (resp.error ? resp.error.message : ""))
                }
            }
            Item { Layout.fillWidth: true }
        }
    }
}
