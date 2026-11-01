// 设置页 · 快捷键分类:六个动作的组合键录制。
// (截图翻译/截图取字/滚动长截图三项已移除,2026-11-01:截图工具栏按钮承担。)
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    function load(s) {
        if (!s) return
        const hk = s.hotkeys || {}
        hkToggleMain.value = hk.toggleMain || ""
        hkCaptureRegion.value = hk.captureRegion || ""
        hkCaptureFullscreen.value = hk.captureFullscreen || ""
        hkPinLatest.value = hk.pinLatest || ""
        hkOpenTranslate.value = hk.openTranslate || ""
        hkCaptureDelayed.value = hk.captureDelayed || ""
    }

    function collect(s) {
        const hk = s.hotkeys || {}
        hk.toggleMain = hkToggleMain.value
        hk.captureRegion = hkCaptureRegion.value
        hk.captureFullscreen = hkCaptureFullscreen.value
        hk.pinLatest = hkPinLatest.value
        hk.openTranslate = hkOpenTranslate.value
        hk.captureDelayed = hkCaptureDelayed.value
        s.hotkeys = hk
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

            SectionTitle { text: "动作与组合键" }
            HintLabel {
                text: "点击后按下组合键即可录制。需含至少一个修饰键(Ctrl/Alt/Shift/Win),单按 F1–F24 也可;"
                      + "按 Delete 清空 = 不绑定该动作。保存后立即生效。"
            }
            FieldLabel { text: "呼出/隐藏面板" }
            HotkeyField {
                id: hkToggleMain
                Layout.fillWidth: true
                onValueModified: {
                    view.hotkeysDirty = true
                    view.scheduleSave()
                }
            }
            FieldLabel { text: "矩形截图" }
            HotkeyField {
                id: hkCaptureRegion
                Layout.fillWidth: true
                onValueModified: {
                    view.hotkeysDirty = true
                    view.scheduleSave()
                }
            }
            FieldLabel { text: "全屏截图" }
            HotkeyField {
                id: hkCaptureFullscreen
                Layout.fillWidth: true
                onValueModified: {
                    view.hotkeysDirty = true
                    view.scheduleSave()
                }
            }
            FieldLabel { text: "一键贴图" }
            HotkeyField {
                id: hkPinLatest
                Layout.fillWidth: true
                onValueModified: {
                    view.hotkeysDirty = true
                    view.scheduleSave()
                }
            }
            FieldLabel { text: "打开翻译" }
            HotkeyField {
                id: hkOpenTranslate
                Layout.fillWidth: true
                onValueModified: {
                    view.hotkeysDirty = true
                    view.scheduleSave()
                }
            }
            // 截图翻译/截图取字/滚动长截图三行已移除(2026-11-01):
            // 三个功能由截图工具栏按钮承担(OCR取字/翻译/长截图),不再有全局热键。
            FieldLabel { text: "延时截图" }
            HotkeyField {
                id: hkCaptureDelayed
                Layout.fillWidth: true
                onValueModified: {
                    view.hotkeysDirty = true
                    view.scheduleSave()
                }
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
