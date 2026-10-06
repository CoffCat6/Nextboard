// 设置抽屉:通用设置 + 翻译服务配置,读写 nbcore settings。
//
// 内容较多,故整体放在 Flickable 里可滚动;抽屉宽度只有主面板的 46%,不滚动会溢出。
// 翻译配置是必需项:core 侧会读 provider/apiKey 等,但此前没有任何界面入口,
// 只能手改 app_config.json —— 等于翻译功能不可用。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

AppDrawer {
    id: root

    /// 服务提供方,取值与 core 的 Provider::parse 一致
    readonly property var providers: ["openai", "microsoft", "custom"]
    readonly property string provider: providers[Math.max(0, providerCombo.currentIndex)]

    function errText(resp) {
        return resp && resp.error ? resp.error.message : ""
    }

    function openAndLoad() {
        open()
        const resp = Core.call("get_settings")
        if (!resp.ok) {
            statusLabel.text = "读取失败:" + errText(resp)
            statusLabel.isError = true
            return
        }
        const s = resp.data
        historyLimit.value = s.historyLimit
        maxImage.value = s.maxImageMb
        monitorCheck.checked = s.monitorEnabled === true
        ocrCheck.checked = s.ocrEnabled === true
        skipEditorCheck.checked = s.regionSkipEditor === true
        autoConfirmCheck.checked = s.regionAutoConfirm === true
        themeCombo.currentIndex = s.theme === "light" ? 1 : s.theme === "dark" ? 2 : 0

        const index = providers.indexOf(s.translateProvider)
        providerCombo.currentIndex = index < 0 ? 0 : index

        const oa = s.translateOpenai || {}
        oaBaseUrl.text = oa.baseUrl || ""
        oaApiKey.text = oa.apiKey || ""
        oaModel.text = oa.model || ""

        const ms = s.translateMicrosoft || {}
        msEndpoint.text = ms.endpoint || ""
        msApiKey.text = ms.apiKey || ""
        msRegion.text = ms.region || ""

        const cu = s.translateCustom || {}
        cuUrl.text = cu.url || ""
        cuMethod.currentIndex = (cu.method === "GET") ? 1 : 0
        cuApiKey.text = cu.apiKey || ""
        cuModel.text = cu.model || ""
        cuHeaders.text = cu.headers || ""
        cuBody.text = cu.bodyTemplate || ""
        cuPath.text = cu.responsePath || ""

        timeoutSpin.value = s.translateTimeoutSecs > 0 ? s.translateTimeoutSecs : 30
        probeLabel.text = ""
        statusLabel.text = ""
    }

    function save() {
        const cur = Core.call("get_settings")
        if (!cur.ok) {
            statusLabel.text = "保存失败:" + errText(cur)
            statusLabel.isError = true
            return
        }
        const s = cur.data
        s.historyLimit = historyLimit.value
        s.maxImageMb = maxImage.value
        s.monitorEnabled = monitorCheck.checked
        s.ocrEnabled = ocrCheck.checked
        s.regionSkipEditor = skipEditorCheck.checked
        s.regionAutoConfirm = autoConfirmCheck.checked
        s.theme = ["system", "light", "dark"][themeCombo.currentIndex]

        s.translateProvider = provider
        const oa = s.translateOpenai || {}
        oa.baseUrl = oaBaseUrl.text
        oa.apiKey = oaApiKey.text
        oa.model = oaModel.text
        s.translateOpenai = oa

        const ms = s.translateMicrosoft || {}
        ms.endpoint = msEndpoint.text
        ms.apiKey = msApiKey.text
        ms.region = msRegion.text
        s.translateMicrosoft = ms

        const cu = s.translateCustom || {}
        cu.url = cuUrl.text
        cu.method = cuMethod.currentIndex === 1 ? "GET" : "POST"
        cu.apiKey = cuApiKey.text
        cu.model = cuModel.text
        cu.headers = cuHeaders.text
        cu.bodyTemplate = cuBody.text
        cu.responsePath = cuPath.text
        s.translateCustom = cu

        s.translateTimeoutSecs = timeoutSpin.value

        const resp = Core.call("save_settings", s)
        statusLabel.text = resp.ok ? "已保存" : "保存失败:" + errText(resp)
        statusLabel.isError = !resp.ok
        if (resp.ok) {
            Core.applyTheme(s.theme)   // 落盘后立即按新主题重刷
            Entries.refreshNow()
        }
    }

    function probe() {
        probeLabel.text = "测试中…"
        probeLabel.isError = false
        Core.probeTranslate(provider)   // 网络请求:异步,完成走 translateProbeFinished
    }

    contentItem: Flickable {
        id: flick
        clip: true
        contentWidth: width
        contentHeight: form.implicitHeight + 12
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: form
            width: flick.width
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
            // 取消勾选后,选区保留并可拖 8 向手柄微调,Enter 或双击才确认
            CheckBox {
                id: autoConfirmCheck
                text: "框选后松开鼠标即确认"
            }

            Label { text: "主题"; color: Theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: themeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "浅色", "深色"]
                // 切换即生效(不必等"保存"),给用户即时反馈
                onActivated: Core.applyTheme(["system", "light", "dark"][currentIndex])
            }

            // ---- 翻译服务 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            Label { text: "翻译服务"; font.pixelSize: 14; font.bold: true; color: Theme.text }
            ComboBox {
                id: providerCombo
                Layout.fillWidth: true
                model: ["OpenAI 兼容接口", "微软翻译", "自定义 HTTP"]
            }

            // OpenAI 兼容(含国内兼容端点)
            Label { text: "接口地址"; visible: root.provider === "openai"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: oaBaseUrl
                visible: root.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "https://api.openai.com/v1"
            }
            Label { text: "API Key"; visible: root.provider === "openai"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: oaApiKey
                visible: root.provider === "openai"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "sk-…"
            }
            Label { text: "模型"; visible: root.provider === "openai"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: oaModel
                visible: root.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "gpt-4o-mini"
            }

            // 微软翻译
            Label { text: "接口地址"; visible: root.provider === "microsoft"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: msEndpoint
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                placeholderText: "https://api.translator.azure.cn"
            }
            Label { text: "API Key"; visible: root.provider === "microsoft"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: msApiKey
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                echoMode: TextInput.Password
            }
            Label { text: "区域(可选)"; visible: root.provider === "microsoft"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: msRegion
                visible: root.provider === "microsoft"
                Layout.fillWidth: true
                placeholderText: "如 eastasia"
            }

            // 自定义 HTTP
            Label { text: "请求地址"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuUrl
                visible: root.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "https://…"
            }
            Label { text: "请求方法"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: cuMethod
                visible: root.provider === "custom"
                Layout.fillWidth: true
                model: ["POST", "GET"]
            }
            Label { text: "API Key(可选)"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuApiKey
                visible: root.provider === "custom"
                Layout.fillWidth: true
                echoMode: TextInput.Password
            }
            Label { text: "模型(可选)"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuModel
                visible: root.provider === "custom"
                Layout.fillWidth: true
            }
            Label { text: "请求头(每行一条 Name: Value)"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextArea {
                id: cuHeaders
                visible: root.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 54
                wrapMode: TextArea.WrapAnywhere
                placeholderText: "Content-Type: application/json"
            }
            Label { text: "请求体模板"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextArea {
                id: cuBody
                visible: root.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 66
                wrapMode: TextArea.WrapAnywhere
                placeholderText: "可用 ${text} ${from} ${to} ${apiKey} ${model}"
            }
            Label { text: "响应取值路径"; visible: root.provider === "custom"; color: Theme.muted; font.pixelSize: 12 }
            TextField {
                id: cuPath
                visible: root.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "如 data.translations[0].text"
            }

            Label { text: "超时(秒)"; color: Theme.muted; font.pixelSize: 12 }
            SpinBox {
                id: timeoutSpin
                Layout.fillWidth: true
                from: 5; to: 120
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button { text: "测试连接"; onClicked: root.probe() }
                Item { Layout.fillWidth: true }
            }
            Label {
                id: probeLabel
                property bool isError: false
                Layout.fillWidth: true
                color: isError ? Theme.danger : Theme.muted
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }

            // ---- 操作 ----
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 10
                implicitHeight: 1
                color: Theme.border
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: "保存"
                    highlighted: true
                    onClicked: root.save()
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
        }
    }

    // 探测结果回投(异步,避免同步网络请求冻结界面)
    Connections {
        target: Core
        function onTranslateProbeFinished(resp) {
            if (!resp.ok) {
                probeLabel.text = "测试失败:" + root.errText(resp)
                probeLabel.isError = true
                return
            }
            const d = resp.data || {}
            probeLabel.text = (d.available ? "✓ " : "✗ ") + (d.message || "")
            probeLabel.isError = !d.available
        }
    }
}
