// 设置页 · 文字识别(OCR)分类:引擎选择与各引擎参数。
// 引擎由用户显式选择(决策 D6:不做 auto 兜底链)—— 选哪个就是哪个,
// 不会出现"我选了内置却跑了 RapidOCR"这种不可预测行为。
// 页面协议(load/collect/view)见 SettingsView.qml 头注释。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: page

    property var view: null

    /// 当前选的是否「系统内置」OCR 后端 —— 决定识别语言/放大倍数两个控件是否显示。
    /// 语义依据(core 侧注释):RapidOCR 自带 max_side_len 不消费 ocr_scale,且中英混排
    /// 一体不分语言;这两个字段对它无效,显示出来只会让人"调了没反应"。
    readonly property bool builtinOcr:
        ocrBackendCombo.backendCodes[ocrBackendCombo.currentIndex] === "system"

    /// OCR 当前后端是否可用(由 core 的 ocr_probe 回填)。
    /// 不可用时 HintLabel 转成警示色并给出可读原因(缺模型 / 缺 DLL / 缺语言包)。
    property bool backendAvailable: true
    property string backendHintText: ""

    function errText(resp) {
        return resp && resp.error ? resp.error.message : ""
    }

    /// 查询当前所选 OCR 后端的可用性,回填提示文案。
    ///
    /// 同步调用:core 侧 `ocr_probe` 只做**文件存在性**检查(不建 session、不加载模型),
    /// 耗时在微秒级,不需要异步。
    /// 注意 `reason` 字段仅在**不可用**时由 core 下发(可用时省略)。
    function refreshProbe() {
        const code = ocrBackendCombo.backendCodes[ocrBackendCombo.currentIndex]
        const resp = Core.call("ocr_probe", { "backend": code })
        if (!resp.ok) {
            page.backendAvailable = false
            page.backendHintText = "无法探测该引擎:" + errText(resp)
            return
        }
        const d = resp.data
        page.backendAvailable = d.available === true
        if (d.available === true) {
            // 语言列表仅内置引擎有(RapidOCR 中英混排一体,不按语言标签区分)
            const langs = d.languages || []
            page.backendHintText = langs.length > 0
                ? "可用 · 语言:" + langs.join("、")
                : "可用(中英混排一体识别)"
        } else {
            page.backendHintText = d.reason || "该引擎当前不可用"
        }
    }

    function load(s) {
        if (!s) return
        // OCR 后端:未知值回退 0(与 core 的 Backend::parse 同口径);
        // 同时触发一次可用性探测,把"为什么不可用"显示出来。
        const backendCodes = ocrBackendCombo.backendCodes
        const backendIdx = backendCodes.indexOf(s.ocrBackend)
        ocrBackendCombo.currentIndex = backendIdx < 0 ? 0 : backendIdx
        // 未知档位/驻留回退默认(与 core 的 normalize 同口径)
        const profileIdx = ocrProfileCombo.profileCodes.indexOf(s.ocrProfile)
        ocrProfileCombo.currentIndex = profileIdx < 0 ? 1 : profileIdx
        const modeIdx = ocrSessionCombo.modeCodes.indexOf(s.ocrSessionMode)
        ocrSessionCombo.currentIndex = modeIdx < 0 ? 2 : modeIdx
        // OCR 语言/放大:未知值回退默认(auto / 2),与 core 的 Default 同口径
        const langIdx = ocrLanguageCombo.languageCodes.indexOf(s.ocrLanguage)
        ocrLanguageCombo.currentIndex = langIdx < 0 ? 0 : langIdx
        const scale = (s.ocrScale === undefined || s.ocrScale === null) ? 2 : s.ocrScale
        ocrScaleSpin.value = Math.min(Math.max(scale, 1), 4)
        refreshProbe()
    }

    function collect(s) {
        s.ocrBackend = ocrBackendCombo.backendCodes[ocrBackendCombo.currentIndex]
        s.ocrProfile = ocrProfileCombo.profileCodes[ocrProfileCombo.currentIndex]
        s.ocrSessionMode = ocrSessionCombo.modeCodes[ocrSessionCombo.currentIndex]
        // OCR 语言/放大只在「系统内置」时显示,但**始终照写**:切换后端时值要保住,
        // 否则从内置切到 RapidOCR 再切回来,用户设定的语言会被清成默认。
        s.ocrLanguage = ocrLanguageCombo.languageCodes[ocrLanguageCombo.currentIndex]
        s.ocrScale = ocrScaleSpin.value
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

            SectionTitle { text: "识别引擎" }
            ReadableComboBox {
                id: ocrBackendCombo
                objectName: "ocrBackendCombo"   // 验收钩子
                Layout.fillWidth: true
                /// 与 model 一一对应的后端标识(持久化到 settings.ocrBackend)。
                /// ⚠️ 必须与 core 侧 `settings::ocr_backend::is_supported` **同口径**;
                /// 值域由该函数的单测交叉锁定,改这里要同步改 core。
                readonly property var backendCodes: ["system", "rapidocr"]
                model: ["系统内置(开箱即用,需语言包)", "RapidOCR(自带模型,中英混排更准)"]
                onActivated: {
                    view.scheduleSave()
                    page.refreshProbe()
                }
            }
            // 可用性与不可用原因:core 的 ocr_probe 聚合两路后端(命中缓存,无需防抖)。
            HintLabel {
                text: page.backendHintText
                color: page.backendAvailable ? Theme.colors.textSecondary : Theme.colors.danger
            }

            // ---- 仅「系统内置」后端有效 ----
            // 显隐依据(core 侧注释,不是猜测):
            //   ocr/rapid.rs:16-17 —— RapidOCR 自带 max_side_len,故**不应用** ocr_scale 放大;
            //   RapidOCR 中英混排一体,ocr_language 对它无效。
            // 之前这两个字段没有控件、两套参数语义分叉(docs/10 §15.6 B3.6);现在按后端
            // 显隐:选了 RapidOCR 就不显示,免得用户调了没反应还以为是 bug。
            FieldLabel {
                text: "识别语言(仅系统内置)"
                visible: page.builtinOcr
            }
            ReadableComboBox {
                id: ocrLanguageCombo
                objectName: "ocrLanguageCombo"   // 验收钩子
                Layout.fillWidth: true
                visible: page.builtinOcr
                /// 与 core settings::validate 同口径:auto 或语言标签(字母数字/-/_)
                readonly property var languageCodes: ["auto", "zh-Hans-CN", "en-US", "ja-JP"]
                model: ["自动识别(按系统语言)", "简体中文", "英语(美国)", "日语"]
                onActivated: view.scheduleSave()
            }
            FieldLabel {
                text: "放大倍数(仅系统内置)"
                visible: page.builtinOcr
            }
            ReadableSpinBox {
                id: ocrScaleSpin
                objectName: "ocrScaleSpin"   // 验收钩子
                Layout.fillWidth: true
                visible: page.builtinOcr
                from: 1; to: 4
                editable: true
                onValueModified: view.scheduleSave()
            }
            HintLabel {
                visible: page.builtinOcr
                text: "小字号截图放大后识别更准,代价是耗时随倍数上升。"
            }

            SectionDivider {}
            SectionTitle { text: "引擎参数(仅 RapidOCR)" }
            FieldLabel { text: "识别偏好" }
            ReadableComboBox {
                id: ocrProfileCombo
                Layout.fillWidth: true
                /// 与 core 的 settings::ocr_profile 同口径(validate 会拒绝其它值)。
                readonly property var profileCodes: ["quality", "balanced", "compact"]
                model: ["质量优先(小字最准,内存占用最高)",
                        "均衡(默认,内存与质量折中)",
                        "内存优先(最省内存,密集小字可能漏行)"]
                onActivated: view.scheduleSave()
            }
            HintLabel {
                // 全屏截图的 det 输入低于所有档位上限(docs/12 §14.2),
                // 档位只影响小图/宽扁图 —— 写明以防"选了没效果"的误解。
                text: "对全屏截图的内存占用无影响;影响小图与长条截图"
            }
            FieldLabel { text: "引擎驻留" }
            ReadableComboBox {
                id: ocrSessionCombo
                Layout.fillWidth: true
                readonly property var modeCodes: ["resident", "idle60", "ephemeral"]
                model: ["常驻(识别最快,后台约占 40MB)",
                        "空闲 1 分钟后释放",
                        "用完即销毁(最省内存,每次多约 0.3 秒)"]
                onActivated: view.scheduleSave()
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
