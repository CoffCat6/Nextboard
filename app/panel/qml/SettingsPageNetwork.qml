// 设置页 · 网络与代理分类:代理模式与手动代理字段(core 的 proxy 校验/分派/
// proxy_probe 早已就绪,这里只是 UI 入口)。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    /// 代理是否处于手动配置模式(控制 manual 字段组的可见性)。
    readonly property bool isProxyManual: proxyModeCombo.currentIndex === 2

    function load(s) {
        if (!s) return
        const proxyMode = (s.proxy && s.proxy.mode) || "system"
        proxyModeCombo.currentIndex = proxyMode === "none" ? 1 : proxyMode === "manual" ? 2 : 0
        const pm = (s.proxy && s.proxy.manual) || {}
        proxyKindCombo.currentIndex = pm.kind === "socks5" ? 1 : 0
        proxyHost.text = pm.host || ""
        proxyPort.value = pm.port > 0 ? pm.port : 0
        proxyUser.text = pm.username || ""
        proxyPassword.text = pm.password || ""
        proxyBypass.text = pm.bypass || ""
    }

    function collect(s) {
        // manual 字段始终提交,与 core 一致:仅 mode=manual 时才参与校验与生效。
        s.proxy = {
            mode: ["system", "none", "manual"][proxyModeCombo.currentIndex],
            manual: {
                kind: proxyKindCombo.currentIndex === 1 ? "socks5" : "http",
                host: proxyHost.text.trim(),
                port: proxyPort.value,
                username: proxyUser.text,
                password: proxyPassword.text,
                bypass: proxyBypass.text
            }
        }
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

            SectionTitle { text: "代理" }
            FieldLabel { text: "代理模式(作用于翻译等网络请求)" }
            ReadableComboBox {
                id: proxyModeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "直连(不使用代理)", "手动配置"]
                onActivated: view.scheduleSave()
            }
            FieldLabel { text: "协议类型"; visible: page.isProxyManual }
            ReadableComboBox {
                id: proxyKindCombo
                visible: page.isProxyManual
                Layout.fillWidth: true
                model: ["HTTP", "SOCKS5"]
                onActivated: view.scheduleSave()
            }
            FieldLabel { text: "服务器地址"; visible: page.isProxyManual }
            ReadableTextField {
                id: proxyHost
                visible: page.isProxyManual
                Layout.fillWidth: true
                placeholderText: "127.0.0.1"
                onTextEdited: view.scheduleSave()
            }
            FieldLabel { text: "端口"; visible: page.isProxyManual }
            ReadableSpinBox {
                id: proxyPort
                visible: page.isProxyManual
                Layout.fillWidth: true
                from: 0; to: 65535
                editable: true
                onValueModified: view.scheduleSave()
            }
            FieldLabel { text: "用户名(可选)"; visible: page.isProxyManual }
            ReadableTextField {
                id: proxyUser
                visible: page.isProxyManual
                Layout.fillWidth: true
                onTextEdited: view.scheduleSave()
            }
            FieldLabel { text: "密码(可选)"; visible: page.isProxyManual }
            ReadableTextField {
                id: proxyPassword
                visible: page.isProxyManual
                Layout.fillWidth: true
                echoMode: TextInput.Password
                onTextEdited: view.scheduleSave()
            }
            // SOCKS5 路径(ureq)不支持绕过列表,仅 HTTP 显示该字段(与 core 注释一致)
            FieldLabel {
                text: "绕过列表(仅 HTTP,分号分隔)"
                visible: page.isProxyManual && proxyKindCombo.currentIndex === 0
            }
            ReadableTextField {
                id: proxyBypass
                visible: page.isProxyManual && proxyKindCombo.currentIndex === 0
                Layout.fillWidth: true
                placeholderText: "localhost;127.0.0.1;*.internal"
                onTextEdited: view.scheduleSave()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                ThemedButton {
                    variant: "secondary"
                    text: "测试代理链路"
                    // 先保存再探测(view.probeProxy):probe 读的是已保存配置
                    onClicked: view.probeProxy()
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.proxyProbeText : ""
                color: (view && view.proxyProbeError) ? Theme.colors.error : Theme.colors.textSecondary
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
    function scrollItemIntoView(it) {
        const pos = it.mapToItem(flick.contentItem, 0, 0)
        flick.contentY = Math.max(0, Math.min(pos.y - 8,
                                    flick.contentHeight - flick.height))
        return true
    }
}
