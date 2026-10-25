// 设置页 · 翻译服务分类:服务通道选择与各通道字段(core 侧会读 provider/apiKey 等)。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    /// 服务提供方,取值与 core 的 Provider::parse 一致
    readonly property var providers: ["microsoft", "openai", "caiyun", "custom"]
    readonly property string provider: providers[Math.max(0, providerCombo.currentIndex)]

    function load(s) {
        if (!s) return
        const index = providers.indexOf(s.translateProvider)
        providerCombo.currentIndex = index < 0 ? 0 : index

        const oa = s.translateOpenai || {}
        oaBaseUrl.text = oa.baseUrl || ""
        oaApiKey.text = oa.apiKey || ""
        oaModel.text = oa.model || ""

        const ms = s.translateMicrosoft || {}
        msEndpoint.text = ms.endpoint || ""

        const cy = s.translateCaiyun || {}
        cyToken.text = cy.token || ""
        cyEndpoint.text = cy.endpoint || ""

        const cu = s.translateCustom || {}
        cuUrl.text = cu.url || ""
        cuMethod.currentIndex = (cu.method === "GET") ? 1 : 0
        cuApiKey.text = cu.apiKey || ""
        cuModel.text = cu.model || ""
        cuHeaders.text = cu.headers || ""
        cuBody.text = cu.bodyTemplate || ""
        cuPath.text = cu.responsePath || ""

        timeoutSpin.value = s.translateTimeoutSecs > 0 ? s.translateTimeoutSecs : 30
    }

    function collect(s) {
        s.translateProvider = provider
        const oa = s.translateOpenai || {}
        oa.baseUrl = oaBaseUrl.text
        oa.apiKey = oaApiKey.text
        oa.model = oaModel.text
        s.translateOpenai = oa

        const ms = s.translateMicrosoft || {}
        ms.endpoint = msEndpoint.text
        s.translateMicrosoft = ms

        const cy = s.translateCaiyun || {}
        cy.token = cyToken.text.trim()
        cy.endpoint = cyEndpoint.text.trim()
        s.translateCaiyun = cy

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

            SectionTitle { text: "服务通道" }
            ReadableComboBox {
                id: providerCombo
                Layout.fillWidth: true
                model: ["微软翻译(免密钥,开箱可用)", "AI 翻译(OpenAI 兼容)", "彩云小译(内置测试 Token)", "自定义 HTTP"]
                onActivated: view.scheduleSave()
            }

            // OpenAI 兼容(含国内兼容端点)
            FieldLabel { text: "接口地址"; visible: page.provider === "openai" }
            ReadableTextField {
                id: oaBaseUrl
                onTextEdited: view.scheduleSave()
                visible: page.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "https://api.openai.com/v1"
            }
            FieldLabel { text: "API Key"; visible: page.provider === "openai" }
            ReadableTextField {
                id: oaApiKey
                onTextEdited: view.scheduleSave()
                visible: page.provider === "openai"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "sk-…"
            }
            FieldLabel { text: "模型"; visible: page.provider === "openai" }
            ReadableTextField {
                id: oaModel
                onTextEdited: view.scheduleSave()
                visible: page.provider === "openai"
                Layout.fillWidth: true
                placeholderText: "gpt-4o-mini"
            }

            // 微软翻译(免授权端点)
            HintLabel {
                visible: page.provider === "microsoft"
                text: "默认通道:使用 Edge 内置翻译的免授权端点,**无需申请任何密钥**即可翻译。"
                      + "该端点非微软公开 API(无 SLA),若日后失效,可改下方地址或切到其它通道。"
            }
            FieldLabel { text: "接口地址"; visible: page.provider === "microsoft" }
            ReadableTextField {
                id: msEndpoint
                onTextEdited: view.scheduleSave()
                visible: page.provider === "microsoft"
                Layout.fillWidth: true
                placeholderText: "https://edge.microsoft.com/translate"
            }

            // 彩云小译(内置)
            HintLabel {
                visible: page.provider === "caiyun"
                text: "内置通道:已预填**官方测试 Token**,装上即可试。官方不保证该 Token 的"
                      + "稳定可用性,持续使用请到彩云科技开放平台申请正式 Token 后替换下方内容"
                      + "(新用户有 100 万字 / 1 个月免费额度)。"
            }
            FieldLabel { text: "访问 Token"; visible: page.provider === "caiyun" }
            ReadableTextField {
                id: cyToken
                onTextEdited: view.scheduleSave()
                visible: page.provider === "caiyun"
                Layout.fillWidth: true
                placeholderText: "3975l6lr5pcbvidl6jl2"
            }
            FieldLabel { text: "接口地址"; visible: page.provider === "caiyun" }
            ReadableTextField {
                id: cyEndpoint
                onTextEdited: view.scheduleSave()
                visible: page.provider === "caiyun"
                Layout.fillWidth: true
                placeholderText: "https://api.interpreter.caiyunai.com"
            }

            // 自定义 HTTP
            HintLabel {
                visible: page.provider === "custom"
                text: "按自己的接口拼请求:下面用占位符描述「发什么」与「从哪取译文」。"
                      + "占位符 ${text} ${from} ${to} ${apiKey} ${model} 会在发送前替换成实际值"
                      + "(支持 ${model:-默认值} 写法);未知占位符会直接报错,不会静默留空。"
            }
            FieldLabel { text: "请求地址"; visible: page.provider === "custom" }
            ReadableTextField {
                id: cuUrl
                onTextEdited: view.scheduleSave()
                visible: page.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "https://…"
            }
            HintLabel {
                visible: page.provider === "custom"
                text: "完整接口地址(含查询参数也可以);值里同样可用占位符,会按 URL 编码转义。"
            }
            FieldLabel { text: "请求方法"; visible: page.provider === "custom" }
            ReadableComboBox {
                id: cuMethod
                visible: page.provider === "custom"
                Layout.fillWidth: true
                model: ["POST", "GET"]
                onActivated: view.scheduleSave()
            }
            FieldLabel { text: "API Key(可选)"; visible: page.provider === "custom" }
            ReadableTextField {
                id: cuApiKey
                onTextEdited: view.scheduleSave()
                visible: page.provider === "custom"
                Layout.fillWidth: true
                echoMode: TextInput.Password
            }
            HintLabel {
                visible: page.provider === "custom"
                text: "只用于替换 ${apiKey} 占位符,**不会**自动加到请求头 —— 需要哪种鉴权头请自己写在请求头里。"
            }
            FieldLabel { text: "模型 / 部署名(可选)"; visible: page.provider === "custom" }
            ReadableTextField {
                id: cuModel
                onTextEdited: view.scheduleSave()
                visible: page.provider === "custom"
                Layout.fillWidth: true
            }
            HintLabel {
                visible: page.provider === "custom"
                text: "只用于替换 ${model} 占位符(Azure 风格接口常把部署名写在地址里)。"
            }
            // ---- 请求头(可滚动:内容多时不再被截断) ----
            FieldLabel { text: "请求头"; visible: page.provider === "custom" }
            HintLabel {
                visible: page.provider === "custom"
                text: "每行一条,格式 Name: Value;值里可用占位符,如 Authorization: Bearer ${apiKey}。"
            }
            ScrollView {
                id: cuHeadersScroll
                visible: page.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: cuHeaders.activeFocus ? Theme.colors.accent : Theme.colors.border
                }
                TextArea {
                    id: cuHeaders
                    onTextEdited: view.scheduleSave()
                    wrapMode: TextArea.WrapAnywhere
                    placeholderText: "Content-Type: application/json"
                    // 技术字段:请求头逐行 "Name: Value",等宽才对齐
                    font.family: Theme.fontMono
                    color: Theme.colors.textPrimary
                    placeholderTextColor: Theme.colors.textTertiary
                    selectionColor: Theme.colors.accent
                    selectedTextColor: Theme.colors.accentText
                    padding: 8
                }
            }

            // ---- 请求体模板(可滚动) ----
            FieldLabel { text: "请求体模板"; visible: page.provider === "custom" }
            HintLabel {
                visible: page.provider === "custom"
                text: "按目标接口的 JSON 结构写,原文放在 ${text} 的位置;"
                      + "原文里的引号与换行会自动转义,不会破坏 JSON。GET 请求可留空。"
            }
            ScrollView {
                id: cuBodyScroll
                visible: page.provider === "custom"
                Layout.fillWidth: true
                Layout.preferredHeight: 96
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: cuBody.activeFocus ? Theme.colors.accent : Theme.colors.border
                }
                TextArea {
                    id: cuBody
                    onTextEdited: view.scheduleSave()
                    wrapMode: TextArea.WrapAnywhere
                    placeholderText: "{\"text\": [\"${text}\"], \"target_lang\": \"${to}\"}"
                    // 技术字段:JSON 模板,等宽缩进与引号/括号配对最受益
                    font.family: Theme.fontMono
                    color: Theme.colors.textPrimary
                    placeholderTextColor: Theme.colors.textTertiary
                    selectionColor: Theme.colors.accent
                    selectedTextColor: Theme.colors.accentText
                    padding: 8
                }
            }

            // ---- 响应取值路径 ----
            FieldLabel { text: "响应取值路径"; visible: page.provider === "custom" }
            HintLabel {
                visible: page.provider === "custom"
                text: "从响应 JSON 里取译文的路径,用点号与下标逐层往下写。"
            }
            ReadableTextField {
                id: cuPath
                onTextEdited: view.scheduleSave()
                visible: page.provider === "custom"
                Layout.fillWidth: true
                placeholderText: "如 translations[0].text"
            }
            HintLabel {
                visible: page.provider === "custom"
                text: "完整示例(以 DeepL 为例):\n"
                      + "地址 https://api-free.deepl.com/v2/translate\n"
                      + "请求头 Authorization: DeepL-Auth-Key ${apiKey}\n"
                      + "请求体 {\"text\":[\"${text}\"],\"target_lang\":\"${to}\"}\n"
                      + "响应路径 translations[0].text\n"
                      + "注意:各服务的语言代码写法可能不同(DeepL 用大写 ZH)。"
                      + "语言代码在翻译窗口顶部可选可改,按服务要求填即可。"
            }

            SectionDivider {}
            SectionTitle { text: "超时与连通性" }
            FieldLabel { text: "超时(秒)" }
            ReadableSpinBox {
                id: timeoutSpin
                Layout.fillWidth: true
                from: 5; to: 120
                editable: true
                onValueModified: view.scheduleSave()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                ThemedButton {
                    variant: "secondary"
                    text: "测试连接"
                    // 结果回投到壳根(view.probeText),切走分类再回来结果还在
                    onClicked: view.probeTranslate(page.provider)
                }
                Item { Layout.fillWidth: true }
            }
            Label {
                Layout.fillWidth: true
                text: view ? view.probeText : ""
                color: (view && view.probeError) ? Theme.colors.error : Theme.colors.textSecondary
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
