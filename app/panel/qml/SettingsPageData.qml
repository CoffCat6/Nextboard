// 设置页 · 数据分类:备份 / 导出 / 恢复与导入 / 危险操作。
// 慢 I/O(zip 全量打包、解包)走 Core 线程池,结果经壳根状态回投(切分类不丢)。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    /// 待确认的备份文件路径(确认卡点「确认」才发起)。空 = 卡片未打开。
    property string restorePendingPath: ""
    property string importPendingPath: ""

    function load(s) {
        // 数据页的字段全部是动作按钮(无表单值),无需回填;
        // 确认卡每次打开设置页都应回到收起态(破坏性操作的确认不该跨会话残留)。
        restorePendingPath = ""
        importPendingPath = ""
        restoreConfirm.visible = false
        importConfirm.visible = false
        clearConfirm.visible = false
    }

    function collect(s) {
        // 本页无持久化字段
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
        view.exportRunning = true
        view.exportHintText = "正在导出…"
        view.exportHintError = false
        Core.exportEntries(path, format)
    }

    // ---- 恢复 / 导入确认卡(打开/收起;执行在壳根 commitRestore/commitImport) ----
    function openRestoreConfirm(path) {
        restorePendingPath = path
        restoreConfirm.visible = true
        revealItem(restoreConfirm)
    }
    function hideRestoreConfirm() {
        restoreConfirm.visible = false
    }
    function openImportConfirm(path) {
        importPendingPath = path
        importConfirm.visible = true
        revealItem(importConfirm)
    }
    function hideImportConfirm() {
        importConfirm.visible = false
    }

    /// 清空全部数据(core `clear_history`):删除**全部**条目(剪贴板历史 + 待办,
    /// todo_meta 由 core 同事务清理)并清理图片文件。破坏性操作,由「危险操作」区的
    /// 就地二次确认块调用。清空后立即重拉列表(refreshNow 内部一并刷新分类计数)。
    /// 同步调用,不需要异步回投。
    function clearHistory() {
        const resp = Core.call("clear_history")
        clearConfirm.visible = false
        if (!resp.ok) {
            view.statusText = "清空失败:" + (resp.error ? resp.error.message : "")
            view.statusError = true
            return
        }
        Entries.refreshNow()
        view.statusText = "已清空全部数据"
        view.statusError = false
    }

    // ---- 滚动辅助 ----
    /// 把表单里的某块滚进可视区(确认卡在表单中部,滚动过的用户点按钮后
    /// 确认卡可能在视口外 —— 不滚过去会像"点了没反应")。
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
        onTriggered: if (item) page.scrollItemIntoView(item)
    }
    /// 验收(--clear-all-flow):把「清空全部数据」按钮滚进可视区(危险区在表单尾部)。
    function revealClearAll() { revealItem(clearButton) }

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

            // ---- 备份与导出(手动入口;zip/Markdown 全量 I/O 秒级,走 Core 异步) ----
            SectionTitle { text: "备份" }
            HintLabel {
                text: "打包全部数据(数据库 + 图片)为单个 zip 快照,用于换机或留存。"
            }
            ThemedButton {
                Layout.fillWidth: true
                variant: "secondary"
                text: view && view.backupRunning ? "备份中…" : "创建备份…"
                enabled: !(view && view.backupRunning)
                onClicked: {
                    const path = Core.saveFileDialog(
                        "选择备份保存位置",
                        "备份文件 (*.zip)",
                        "nextboard-backup-" + new Date().toISOString().slice(0, 10) + ".zip")
                    if (path === "") return
                    view.backupRunning = true
                    view.backupHintText = "正在创建备份…"
                    view.backupHintError = false
                    Core.backupCreate(path)
                }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.backupHintText : ""
                color: (view && view.backupHintError) ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }

            SectionDivider {}
            SectionTitle { text: "导出" }
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
                    text: view && view.exportRunning ? "导出中…" : "Markdown…"
                    enabled: !(view && view.exportRunning)
                    onClicked: page.exportAs("markdown", "Markdown (*.md)", ".md")
                }
                ThemedButton {
                    objectName: "exportJsonButton"   // 验收钩子
                    Layout.fillWidth: true
                    variant: "secondary"
                    text: view && view.exportRunning ? "导出中…" : "JSON…"
                    enabled: !(view && view.exportRunning)
                    onClicked: page.exportAs("json", "JSON (*.json)", ".json")
                }
                ThemedButton {
                    objectName: "exportCsvButton"   // 验收钩子
                    Layout.fillWidth: true
                    variant: "secondary"
                    text: view && view.exportRunning ? "导出中…" : "CSV…"
                    enabled: !(view && view.exportRunning)
                    onClicked: page.exportAs("csv", "CSV (*.csv);;所有文件 (*)", ".csv")
                }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.exportHintText : ""
                color: (view && view.exportHintError) ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }

            // ---- 恢复 / 导入(备份 zip 的消费端) ----
            SectionDivider {}
            SectionTitle { text: "恢复与导入" }
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
                    text: view && view.restoreRunning ? "恢复中…" : "恢复备份…"
                    enabled: !(view && (view.restoreRunning || view.importRunning
                                        || view.backupRunning))
                    onClicked: {
                        const path = Core.openFileDialog("选择要恢复的备份文件",
                                                         "备份文件 (*.zip)")
                        if (path === "") return
                        page.openRestoreConfirm(path)
                    }
                }
                ThemedButton {
                    objectName: "importBackupButton"   // 验收钩子(--restore-flow 真实点击)
                    Layout.fillWidth: true
                    variant: "secondary"
                    text: view && view.importRunning ? "导入中…" : "导入备份…"
                    enabled: !(view && (view.restoreRunning || view.importRunning
                                        || view.backupRunning))
                    onClicked: {
                        const path = Core.openFileDialog("选择要导入的备份文件",
                                                         "备份文件 (*.zip)")
                        if (path === "") return
                        page.openImportConfirm(path)
                    }
                }
            }
            // 就地确认块(破坏性操作必须二次确认,但不弹 Dialog/Popup ——
            // 抽屉时代的嵌套弹层有定位/焦点问题;整窗页面里就地展开同样更自然)。
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
                        onClicked: view.commitRestore(page.restorePendingPath)
                    }
                    ThemedButton {
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: "取消"
                        onClicked: page.hideRestoreConfirm()
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.restoreHintText : ""
                color: (view && view.restoreHintError) ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
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
                        onClicked: view.commitImport(page.importPendingPath)
                    }
                    ThemedButton {
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: "取消"
                        onClicked: page.hideImportConfirm()
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.importHintText : ""
                color: (view && view.importHintError) ? Theme.colors.error : Theme.colors.textSecondary
                font.pixelSize: Theme.metrics.fontCaption
                wrapMode: Text.WrapAnywhere
            }

            // ---- 危险操作 ----
            SectionDivider {}
            SectionTitle { text: "危险操作" }
            HintLabel {
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
                    page.revealItem(clearConfirm)   // 同恢复确认卡:滚进可视区
                }
            }
            // 就地二次确认:破坏性操作必须二次确认(嵌套弹层的取舍同上)。
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
                        onClicked: page.clearHistory()
                    }
                    ThemedButton {
                        Layout.fillWidth: true
                        variant: "secondary"
                        text: "取消"
                        onClicked: clearConfirm.visible = false
                    }
                }
            }
        }
    }

    // ---- 验收只读 + 辅助 ----
    readonly property bool acceptanceRestoreCardVisible: restoreConfirm.visible
    readonly property bool acceptanceImportCardVisible: importConfirm.visible
    function scrollToBottom() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }
    readonly property real acceptanceScrollY: flick.contentY
}
