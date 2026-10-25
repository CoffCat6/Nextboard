// 设置页 · 通用分类:主题 / 剪贴板监听 / 历史上限 / 数据目录等全局项。
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
        themeCombo.currentIndex = s.theme === "light" ? 1 : s.theme === "dark" ? 2 : 0
        monitorCheck.checked = s.monitorEnabled === true
        autoPasteCheck.checked = s.copyAutoPaste === true
        historyLimit.value = s.historyLimit
        maxImage.value = s.maxImageMb
        // 文本上限:缺失时按 core 默认 1024(与 settings.rs 的 Default 一致)
        maxTextKb.value = (s.maxTextKb === undefined || s.maxTextKb === null)
                          ? 1024 : s.maxTextKb
        softwareRenderCheck.checked = s.softwareRender === true
        // 开机自启不进 settings,而是注册表实况 → 每次打开都回读同步。
        // (勾选态在首次点击时被赋值,声明式绑定即被破坏,不显式回读就会一直停留在旧值)
        autoStartCheck.checked = Core.autoStartEnabled()
        dataDirInput.text = s.dataDir || ""
        // 脏标志复位(load 的是已保存值,不该带上一次会话的"改过"状态)
        view.dataDirDirty = false
        view.renderDirty = false
    }

    function collect(s) {
        s.theme = ["system", "light", "dark"][themeCombo.currentIndex]
        s.monitorEnabled = monitorCheck.checked
        s.copyAutoPaste = autoPasteCheck.checked
        s.historyLimit = historyLimit.value
        s.maxImageMb = maxImage.value
        s.maxTextKb = maxTextKb.value
        s.softwareRender = softwareRenderCheck.checked
        s.dataDir = dataDirInput.text.trim()
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
            // 内容宽度收敛(超宽窗口下表单顶满整行可读性差),水平居中
            width: Math.min(flick.width - 2 * Theme.metrics.spacingLg, 680)
            x: (flick.width - width) / 2
            spacing: Theme.metrics.spacingXs

            SectionTitle { text: "外观与行为" }
            FieldLabel { text: "主题" }
            ReadableComboBox {
                id: themeCombo
                Layout.fillWidth: true
                model: ["跟随系统", "浅色", "深色"]
                // 切换即生效(不必等保存),给用户即时反馈
                onActivated: {
                    Core.applyTheme(["system", "light", "dark"][currentIndex])
                    view.scheduleSave()
                }
            }

            ReadableCheckBox {
                id: monitorCheck
                text: "剪贴板监听"
                onClicked: view.scheduleSave()
            }
            // 复制后自动粘贴(N2):默认关闭。文案点明"模拟 Ctrl+V"与失效场景 ——
            // 管理员权限窗口收不到模拟按键是 Windows 的 UIPI 限制,不写清楚
            // 用户会当成 bug。
            ReadableCheckBox {
                id: autoPasteCheck
                objectName: "autoPasteCheck"   // 验收钩子
                text: "复制后自动粘贴(模拟 Ctrl+V;管理员窗口无效)"
                onClicked: view.scheduleSave()
            }

            SectionDivider {}
            SectionTitle { text: "历史与上限" }
            FieldLabel { text: "历史上限(条)" }
            ReadableSpinBox {
                id: historyLimit
                objectName: "historyLimitSpin"   // 验收钩子(--settings-flow 手输链路)
                Layout.fillWidth: true
                from: 100; to: 100000; stepSize: 500
                editable: true
                onValueModified: view.scheduleSave()
            }
            FieldLabel { text: "单张图片上限(MB)" }
            ReadableSpinBox {
                id: maxImage
                Layout.fillWidth: true
                from: 1; to: 200
                editable: true
                onValueModified: view.scheduleSave()
            }
            // 文本条目上限:此前**只有字段没有控件**(settings.max_text_kb 一直吃默认 1024),
            // 复制大段日志/JSON 时撞上限只能看到"文本超过大小上限,未入库"却无从修改。
            FieldLabel { text: "单条文本上限(KB)" }
            ReadableSpinBox {
                id: maxTextKb
                objectName: "maxTextKbSpin"   // 验收钩子
                Layout.fillWidth: true
                from: 1; to: 4096; stepSize: 64
                editable: true
                onValueModified: view.scheduleSave()
            }

            SectionDivider {}
            SectionTitle { text: "启动与存储" }
            // 渲染后端(重启生效):软件(CPU 光栅)是**默认**——实测比硬件(D3D11)
            // 省 ~55MB 首帧 / ~101MB 稳态(隐藏态 231MB → 130MB),代价是渲染改由
            // CPU 承担(滚动/动画)。显卡充足且更在意跟手感的用户可关掉它。
            ReadableCheckBox {
                id: softwareRenderCheck
                text: "软件渲染(省内存,重启生效)"
                onClicked: {
                    view.renderDirty = true
                    view.scheduleSave()
                }
            }
            HintLabel {
                text: "默认开启:用 CPU 光栅替代 GPU(D3D11)渲染,约省 100MB 内存;滚动/动画更吃 CPU。关闭后重启生效(也可用 --hardware 启动参数临时切换)。"
            }
            ReadableCheckBox {
                id: autoStartCheck
                text: "开机自启(静默驻留托盘,不弹面板)"
                // 勾选态 = 注册表 Run 键实际存在(不是本地布尔)。
                // 命令行形态固定为 `"<exe>" --hidden`,与安装器同一写法;
                // 老版本留下的无 --hidden 值由启动时的 refreshCommandLineIfStale() 就地升级。
                checked: Core.autoStartEnabled()
                onClicked: {
                    // 注册表操作立即生效并回读实际结果,失败时还原勾选态并提示原因
                    if (Core.setAutoStart(checked)) {
                        checked = Core.autoStartEnabled()
                    } else {
                        checked = !checked
                        Core.showToast("开机自启设置失败:写入注册表被拒绝(可能被安全软件或组策略拦截)")
                    }
                }
            }
            HintLabel {
                text: "自定义数据目录(留空 = 默认位置)。变更后重启应用生效:数据库与图片自动迁移到新目录,原目录数据保留。"
            }
            ReadableTextField {
                id: dataDirInput
                Layout.fillWidth: true
                placeholderText: "D:\\Data\\Nextboard"
                onTextEdited: {
                    view.dataDirDirty = true
                    view.scheduleSave()
                }
            }
        }
    }

    // ---- 验收辅助(SettingsView 转发进来;滚动的目标永远在本页 flick 里) ----
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
