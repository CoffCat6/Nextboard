// 设置页 · 关于分类:版本信息与更新检查(docs/07-auto-update.md)。
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
        // 无表单值
    }

    function collect(s) {
        // 无持久化字段
    }

    /// 把某块滚进可视区(检查更新按钮在表单中部;数据页 revealItem 同款)。
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
        onTriggered: if (item) page.scrollItemIntoView(item)
    }
    /// 验收(--update-flow):把「检查更新」按钮滚进可视区。
    function revealUpdateCheck() { revealItem(updateCheckButton) }

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

            SectionTitle { text: "版本" }
            HintLabel {
                text: "Nextboard " + Core.appVersion
                      + "\n内核 nbcore " + (Core.coreVersion !== "" ? Core.coreVersion : "未加载")
                      + "\nWindows 桌面一体化效率板:剪贴板历史 + 截图 / 贴图 + 翻译"
            }

            SectionDivider {}
            SectionTitle { text: "更新" }
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
                    text: view && view.updateChecking ? "检查中…" : "检查更新"
                    enabled: Core.updateCheckUrl !== "" && !(view && view.updateChecking)
                    onClicked: view.startUpdateCheck()
                }
                ThemedButton {
                    // ⚠️ 条件 objectName:按钮只在有下载链接时出现,隐藏期间若仍带名字,
                    //    验收 DFS 会把它当命中目标、点到别处(clearDoneLink 同教训)。
                    objectName: visible ? "updateOpenPageBtn" : ""
                    variant: "secondary"
                    visible: view && view.updateDownloadUrl !== ""
                    text: "打开下载页"
                    onClicked: Core.openExternalUrl(view.updateDownloadUrl)
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.updateHintText : ""
                color: (view && view.updateHintError) ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }
        }
    }

    // ---- 验收辅助(同 SettingsPageGeneral) ----
    function scrollToBottom() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }
    readonly property real acceptanceScrollY: flick.contentY
}
