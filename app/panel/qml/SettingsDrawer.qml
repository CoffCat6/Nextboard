// 设置抽屉:历史上限 / 图片上限 / 监听开关 / OCR / 主题,读写 nbcore settings。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

AppDrawer {
    id: root

    function openAndLoad() {
        open()
        const resp = Core.call("get_settings")
        if (!resp.ok) {
            statusLabel.text = "读取失败:" + (resp.error ? resp.error.message : "")
            statusLabel.isError = true
            return
        }
        const s = resp.data
        historyLimit.value = s.historyLimit
        maxImage.value = s.maxImageMb
        monitorCheck.checked = s.monitorEnabled === true
        ocrCheck.checked = s.ocrEnabled === true
        skipEditorCheck.checked = s.regionSkipEditor === true
        themeCombo.currentIndex = s.theme === "light" ? 1 : s.theme === "dark" ? 2 : 0
    }

    contentItem: ColumnLayout {
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Label { text: "设置"; font.pixelSize: 15; font.bold: true; color: Theme.text }
            Item { Layout.fillWidth: true }
            ToolButton { text: "✕"; onClicked: root.close() }
        }

        Label { text: "历史上限(条)"; color: Theme.muted; font.pixelSize: 12 }
        SpinBox {
            id: historyLimit
            Layout.fillWidth: true
            from: 100; to: 100000; stepSize: 500
        }

        Label { text: "单张图片上限(MB)"; color: Theme.muted; font.pixelSize: 12 }
        SpinBox {
            id: maxImage
            Layout.fillWidth: true
            from: 1; to: 200
        }

        CheckBox {
            id: monitorCheck
            text: "剪贴板监听"
        }
        CheckBox {
            id: ocrCheck
            text: "OCR 自动识别(截图文字可搜索)"
        }
        // 取消勾选后,框选截图会先进编辑器标注,「完成」才入库 + 写剪贴板
        CheckBox {
            id: skipEditorCheck
            text: "截图后直接入库(跳过编辑器)"
        }

        Label { text: "主题"; color: Theme.muted; font.pixelSize: 12 }
        ComboBox {
            id: themeCombo
            Layout.fillWidth: true
            model: ["跟随系统", "浅色", "深色"]
            // 切换即生效(不必等"保存"),给用户即时反馈
            onActivated: Core.applyTheme(["system", "light", "dark"][currentIndex])
        }

        RowLayout {
            Layout.topMargin: 12
            spacing: 8
            Button {
                text: "保存"
                highlighted: true
                onClicked: {
                    const cur = Core.call("get_settings")
                    if (!cur.ok) {
                        statusLabel.text = "保存失败:" + (cur.error ? cur.error.message : "")
                        statusLabel.isError = true
                        return
                    }
                    const s = cur.data
                    s.historyLimit = historyLimit.value
                    s.maxImageMb = maxImage.value
                    s.monitorEnabled = monitorCheck.checked
                    s.ocrEnabled = ocrCheck.checked
                    s.regionSkipEditor = skipEditorCheck.checked
                    s.theme = ["system", "light", "dark"][themeCombo.currentIndex]
                    const resp = Core.call("save_settings", s)
                    statusLabel.text = resp.ok ? "已保存"
                        : "保存失败:" + (resp.error ? resp.error.message : "")
                    statusLabel.isError = !resp.ok
                    if (resp.ok) {
                        Core.applyTheme(s.theme)   // 落盘后立即按新主题重刷
                        Entries.refreshNow()
                    }
                }
            }
            Button { text: "关闭"; onClicked: root.close() }
            Item { Layout.fillWidth: true }
        }

        Label {
            id: statusLabel
            property bool isError: false
            Layout.fillWidth: true
            color: isError ? Theme.danger : Theme.muted
            font.pixelSize: 12
            wrapMode: Text.WrapAnywhere
        }

        Item { Layout.fillHeight: true }
    }
}
