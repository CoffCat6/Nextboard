// 设置页 · 贴图分类:贴图默认行为与文本贴图外观(背景/文字色、字号、宽度)。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    /// 当前取色目标("" / "bg" / "color")——两个颜色字段共用一个色板。
    property string paletteTarget: ""

    /// 仅用于色块预览:非法值不喂给 color 属性(否则每敲一个字符就刷一条绑定警告)。
    function isHexColor(value) {
        return /^#[0-9a-fA-F]{6}$/.test(value)
    }

    function load(s) {
        if (!s) return
        pinOpacity.value = s.pinDefaultOpacity > 0 ? s.pinDefaultOpacity : 100
        pinDoubleClick.checked = s.pinDoubleclickClose === true
        pinPreferImage.checked = s.pinPreferImage === true
        pinTextBg.text = s.pinTextBg || ""
        pinTextColor.text = s.pinTextColor || ""
        pinTextBgOpacity.value = s.pinTextBgOpacity >= 0 ? s.pinTextBgOpacity : 92
        pinTextFontSize.value = s.pinTextFontSize > 0 ? s.pinTextFontSize : 15
        pinTextMaxWidth.value = s.pinTextMaxWidth > 0 ? s.pinTextMaxWidth : 520
        paletteTarget = ""
    }

    function collect(s) {
        s.pinDefaultOpacity = pinOpacity.value
        s.pinDoubleclickClose = pinDoubleClick.checked
        s.pinPreferImage = pinPreferImage.checked
        s.pinTextBg = pinTextBg.text
        s.pinTextColor = pinTextColor.text
        s.pinTextBgOpacity = pinTextBgOpacity.value
        s.pinTextFontSize = pinTextFontSize.value
        s.pinTextMaxWidth = pinTextMaxWidth.value
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

            SectionTitle { text: "贴图行为" }
            FieldLabel { text: "默认不透明度(%)" }
            ReadableSpinBox {
                id: pinOpacity
                Layout.fillWidth: true
                from: 30; to: 100; stepSize: 5
                editable: true
                onValueModified: view.scheduleSave()
            }
            ReadableCheckBox {
                id: pinDoubleClick
                text: "双击关闭贴图窗口"
                onClicked: view.scheduleSave()
            }
            ReadableCheckBox {
                id: pinPreferImage
                text: "一键贴图 · 图片优先(否则贴最近复制的内容)"
                onClicked: view.scheduleSave()
            }
            HintLabel {
                text: "关闭(默认):按热键贴出最近复制的文本或图片;开启:只要历史里有图片就贴最近图片,无图时才贴最近文本。"
            }

            SectionDivider {}
            SectionTitle { text: "文本贴图外观" }
            FieldLabel { text: "背景色(点色块展开色板)" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 34; implicitHeight: 22; radius: Theme.metrics.radiusBadge
                    color: page.isHexColor(pinTextBg.text) ? pinTextBg.text : Theme.colors.surfaceSecondary
                    border.color: page.paletteTarget === "bg" ? Theme.colors.accent : Theme.colors.border
                    border.width: page.paletteTarget === "bg" ? 2 : Theme.metrics.borderWidth
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.paletteTarget = page.paletteTarget === "bg" ? "" : "bg"
                    }
                }
                ReadableTextField {
                    id: pinTextBg
                    Layout.fillWidth: true
                    placeholderText: "#26272e"
                    onTextEdited: view.scheduleSave()
                }
            }
            // 就地展开的色板(不用 Popup,取舍见 ColorSwatchGrid.qml 注释)
            ColorSwatchGrid {
                Layout.fillWidth: true
                visible: page.paletteTarget === "bg"
                current: pinTextBg.text
                onPicked: function(color) {
                    pinTextBg.text = color
                    page.paletteTarget = ""
                    view.scheduleSave()
                }
            }
            FieldLabel { text: "文字色(点色块展开色板)" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 34; implicitHeight: 22; radius: Theme.metrics.radiusBadge
                    color: page.isHexColor(pinTextColor.text) ? pinTextColor.text : Theme.colors.surfaceSecondary
                    border.color: page.paletteTarget === "color" ? Theme.colors.accent : Theme.colors.border
                    border.width: page.paletteTarget === "color" ? 2 : Theme.metrics.borderWidth
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.paletteTarget = page.paletteTarget === "color" ? "" : "color"
                    }
                }
                ReadableTextField {
                    id: pinTextColor
                    Layout.fillWidth: true
                    placeholderText: "#dfe1e8"
                    onTextEdited: view.scheduleSave()
                }
            }
            ColorSwatchGrid {
                Layout.fillWidth: true
                visible: page.paletteTarget === "color"
                current: pinTextColor.text
                onPicked: function(color) {
                    pinTextColor.text = color
                    page.paletteTarget = ""
                    view.scheduleSave()
                }
            }
            FieldLabel { text: "背景不透明度(%)" }
            ReadableSpinBox {
                id: pinTextBgOpacity
                Layout.fillWidth: true
                from: 0; to: 100; stepSize: 5
                editable: true
                onValueModified: view.scheduleSave()
            }
            FieldLabel { text: "字号(像素)" }
            ReadableSpinBox {
                id: pinTextFontSize
                Layout.fillWidth: true
                from: 8; to: 96
                editable: true
                onValueModified: view.scheduleSave()
            }
            FieldLabel { text: "最大宽度(像素)" }
            ReadableSpinBox {
                id: pinTextMaxWidth
                Layout.fillWidth: true
                from: 120; to: 2000; stepSize: 40
                editable: true
                onValueModified: view.scheduleSave()
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
