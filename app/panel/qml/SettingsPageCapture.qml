// 设置页 · 截图分类:框选行为 / 延时 / 遮罩超时 / 滚动长截图参数(docs/19 §6)。
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
        // 单一出口(§18.4):由 regionAutoConfirm 推导 ——
        //   true  = direct (松手即入库)
        //   false = toolbar(浮出工具栏)
        // 兼容旧配置:旧版 "skipEditor=false"(想进编辑器)的用户,其意图是"要编辑能力",
        // 在新模型里对应 toolbar(浮条带标注),故只要 autoConfirm=false 就落 toolbar。
        regionExitCombo.currentIndex = s.regionAutoConfirm === false ? 1 : 0
        // 智能框选(docs/23):缺失时按 core 默认 true —— 显示与实际行为必须同源。
        smartCaptureCheck.checked = s.smartCapture !== false
        rightClickCancelCheck.checked = s.regionRightClickCancel === true
        // 多屏遮罩范围:缺失(true=历史行为)时不等价于 false,须显式区分
        allScreensCheck.checked = s.captureAllScreens !== false
        captureDelay.value = s.captureDelaySecs > 0 ? s.captureDelaySecs : 3
        // 长截图(docs/19 §6):上限与重叠门槛都来自 core,兜底值必须与
        // settings.rs 的默认一致(30000 / 3),否则"显示与行为分叉"。
        const stitchH = (s.stitchMaxHeight === undefined || s.stitchMaxHeight === null)
                        ? 30000 : s.stitchMaxHeight
        stitchMaxHeight.value = stitchH
        const stitchR = (s.stitchOverlapRatio === undefined || s.stitchOverlapRatio === null)
                        ? 3 : s.stitchOverlapRatio
        stitchOverlap.value = stitchR
        // v2 新增:采样间隔(默认 150ms)与字节预算(默认 128MB)。
        const stitchMs = (s.stitchSampleIntervalMs === undefined
                          || s.stitchSampleIntervalMs === null) ? 150 : s.stitchSampleIntervalMs
        stitchSampleMs.value = stitchMs
        const stitchBytes = (s.stitchMaxBytes === undefined || s.stitchMaxBytes === null)
                            ? 134217728 : s.stitchMaxBytes
        stitchMaxBytesMb.value = Math.round(stitchBytes / (1024 * 1024))
        // 0 = 不自动取消,必须原样保留;缺失时按 core 默认 60 秒
        // ⚠️ 兜底值必须与 settings.rs / capture_session.cpp 一致(都取 60):
        //    这里原先写 600,get_settings 失败时显示"10 分钟"而实际行为是 60s ——
        //    显示与行为分叉,用户改不动也看不出来。
        const idleSecs = (s.overlayIdleSecs === undefined || s.overlayIdleSecs === null)
                         ? 60 : s.overlayIdleSecs
        overlayIdle.value = Math.round(idleSecs / 60)
    }

    function collect(s) {
        // 单一出口 → 回写成 regionAutoConfirm(direct=true / toolbar=false)。
        // regionSkipEditor 已废弃(§18.5):不写它,让它保持读入时的原值(见 core 注释)。
        s.regionAutoConfirm = regionExitCombo.currentIndex === 0
        s.smartCapture = smartCaptureCheck.checked
        s.regionRightClickCancel = rightClickCancelCheck.checked
        s.captureAllScreens = allScreensCheck.checked
        s.captureDelaySecs = captureDelay.value
        s.stitchMaxHeight = stitchMaxHeight.value
        s.stitchOverlapRatio = stitchOverlap.value
        s.stitchSampleIntervalMs = stitchSampleMs.value
        s.stitchMaxBytes = stitchMaxBytesMb.value * 1024 * 1024
        s.overlayIdleSecs = overlayIdle.value * 60
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

            SectionTitle { text: "框选截图" }
            // 框选之后去哪儿(§18.4):**单一出口**取代原先两个复选框。
            // 原先 autoConfirm × skipEditor 叠出四种组合,其中「松开即确认 + 进编辑器」
            // 自相矛盾、无法向用户解释;且"框选进编辑器"这条出口已下线(§18.5)——
            // 编辑器只保留唯一入口 = 右键编辑(主面板条目右键 / 贴图右键)。
            FieldLabel { text: "框选后" }
            ReadableComboBox {
                id: regionExitCombo
                objectName: "regionExitCombo"   // 验收钩子
                Layout.fillWidth: true
                /// 与 model 一一对应的标识:
                ///   direct  —— 松手即入库 + 写剪贴板
                ///   toolbar —— 选区保留并浮出工具栏(就地标注/OCR/贴图/保存/完成)
                readonly property var exitCodes: ["direct", "toolbar"]
                model: ["直接入库并复制(松手即完成)",
                        "浮出工具栏(可标注 / OCR / 贴图 / 保存)"]
                onActivated: view.scheduleSave()
            }
            // 智能框选(docs/23):鼠标移动即自动框住光标下的窗口,单击采纳。
            // 排在与「框选后」相邻的位置 —— 它决定的是**怎么框**,与"框完去哪儿"
            // 是同一件事的两半,分开摆用户对不上。
            ReadableCheckBox {
                id: smartCaptureCheck
                objectName: "smartCaptureCheck"   // 验收钩子
                text: "智能框选(移动鼠标自动框住光标下的窗口,单击即可;滚轮切换重叠窗口)"
                onClicked: view.scheduleSave()
            }
            HintLabel {
                text: "关闭后回到纯手动拖拽框选。鼠标在目标上停稳约 0.4 秒,"
                      + "若目标支持 UI Automation(浏览器 / IDE / Office),还会进一步框住其中的元素。"
            }
            ReadableCheckBox {
                id: rightClickCancelCheck
                text: "框选时鼠标右键取消截图"
                onClicked: view.scheduleSave()
            }
            ReadableCheckBox {
                id: allScreensCheck
                text: "多屏截图时每块屏都建立选区(关闭则仅光标所在屏,更省内存)"
                onClicked: view.scheduleSave()
            }
            // 延时截图(F16):托盘「延时截图」入口的等待秒数
            FieldLabel { text: "延时截图等待(秒)" }
            ReadableSpinBox {
                id: captureDelay
                Layout.fillWidth: true
                from: 1; to: 10
                editable: true
                onValueModified: view.scheduleSave()
            }
            // 内存收口(S6.1):框选遮罩持有该屏的整屏冻结底图(4K 约 33MB),
            // 用户触发截图后离开会一直占着 —— 超时无任何鼠标/键盘操作即自动取消。
            FieldLabel { text: "遮罩空闲超时(分钟,0=不自动取消)" }
            ReadableSpinBox {
                id: overlayIdle
                Layout.fillWidth: true
                from: 0; to: 60
                editable: true
                onValueModified: view.scheduleSave()
            }

            SectionDivider {}
            SectionTitle { text: "滚动长截图" }
            //
            // 上限要与「单张图片上限」配合看:入库按编码后的 PNG 体积卡
            // maxImageMb,超了只写剪贴板、不入库(R3 会在入库超限时再提示一次)。
            // 这里做**软提示**而不是硬拦 —— 生成时的体积取决于内容重复度,
            // 事先算不准,拦死会误伤。
            FieldLabel { text: "长截图高度上限(px)" }
            ReadableSpinBox {
                id: stitchMaxHeight
                Layout.fillWidth: true
                from: 2000; to: 100000; stepSize: 1000
                editable: true
                onValueModified: view.scheduleSave()
            }
            HintLabel {
                text: "超过此高度会停止采集并提示分段。长图还需满足「通用」里的「单张图片上限」:"
                      + "太高时可能存不进历史(此时仍会进剪贴板)。"
            }
            FieldLabel { text: "长截图重叠判定门槛(%)" }
            ReadableSpinBox {
                id: stitchOverlap
                Layout.fillWidth: true
                from: 1; to: 20
                editable: true
                onValueModified: view.scheduleSave()
            }
            HintLabel {
                text: "门槛越低,越容易把重复图案(表格线/纯色块)误认为已重叠 —— 长图会缺行;"
                      + "越高则要求每次滚动幅度更小。默认 3。"
            }
            FieldLabel { text: "长截图采样间隔(ms)" }
            ReadableSpinBox {
                id: stitchSampleMs
                Layout.fillWidth: true
                from: 80; to: 1000; stepSize: 10
                editable: true
                onValueModified: view.scheduleSave()
            }
            HintLabel {
                text: "两次采样之间的间隔。越小越跟手,但快速滚动时相邻画面重叠更少、"
                      + "漏行风险更高;越大越省 CPU。默认 150。"
            }
            FieldLabel { text: "长截图内存预算(MB)" }
            ReadableSpinBox {
                id: stitchMaxBytesMb
                Layout.fillWidth: true
                from: 16; to: 1024; stepSize: 16
                editable: true
                onValueModified: view.scheduleSave()
            }
            HintLabel {
                text: "拼接结果的内存上限,与上面的高度上限**取更严的那个**。"
                      + "宽选区(如 4K 全屏)会被它先卡住 —— 只按像素限高会让内存先爆。默认 128。"
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
