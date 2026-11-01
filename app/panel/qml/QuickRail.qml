// 主面板右侧快捷栏(列表页专属):固定窄条 + 纵向图标按钮。
//
// 定位:类比 VS Code 的 Activity Bar —— 常驻、窄(52)、只放图标,
// 给"一击直达"的高频功能一个固定落点,不与头部命令按钮(动作型)混用。
// 头部与状态栏通宽,本栏只占列表内容区右侧(见 Main.qml 的中部内容区)。
//
// 数据驱动:按钮来自 settings.quickRailCommands(docs/24,设置页「快捷功能」
// 管理)+ 末尾固定的「添加/管理」钮(原占位钮转正)。enabled=false 的指令
// 不渲染;数组顺序即显示顺序。设置保存(Core.settingsChanged)后重读。
//
// 交互约定:
//  - 悬停 surfaceHover + 图标提亮(与分类标签/筛选 chip 同一套中性语言);
//  - 占位钮(dim)整体弱化到 0.55,表达"这里可以放东西"而非可用命令,
//    点击给出 toast 而不是静默无响应;
//  - 悬停 ToolTip 显示功能名:纯图标按钮没有文字,必须有tooltip兜底。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Rectangle {
    id: root
    objectName: "quickRail"

    /// 栏宽(布局子项用 implicitWidth):40 钮 + 左右各 6 呼吸边。
    implicitWidth: 52
    color: Theme.colors.surfaceSecondary

    /// 按钮模型:{ id, iconKind, label, dim, cmd }。
    /// 来源两段:settings.quickRailCommands 中 enabled 的指令(顺序即数组序)
    /// + 末尾固定「添加/管理」钮。
    ///
    /// ⚠️ 为什么可写 + 手动 refresh:get_settings 是同步 FFI 调用,不是响应式
    ///    依赖 —— readonly 绑定只在其它依赖变化时重算,设置保存后不会自动重跑。
    ///    构造期算一次初值,此后由 Connections(settingsChanged)驱动 refresh()。
    property var commands: []
    Component.onCompleted: refresh()
    function refresh() {
        const out = []
        const resp = Core.call("get_settings")
        const list = resp.ok && resp.data ? resp.data.quickRailCommands || [] : []
        for (let i = 0; i < list.length; ++i) {
            const it = list[i]
            if (it.enabled === false) continue
            out.push({
                id: it.id,
                // 图标解码:"@X"/"@" = 首字字母块(isChar),否则 HeaderIcon kind
                iconKind: it.icon || defaultIconFor(it.type),
                isChar: (it.icon || "").indexOf("@") === 0,
                char: (it.icon || "").indexOf("@") === 0
                      ? firstCharOf(it.icon.length > 1 ? it.icon.slice(1) : it.label)
                      : "",
                label: it.label,
                // tooltip 第二行:类型摘要(system/internal 附目标,app 附文件名)
                typeText: typeTextFor(it.type, it.target),
                dim: false,
                cmd: it
            })
        }
        commands = out
    }
    /// 类型默认图标(指令未指定 icon 时)。
    function defaultIconFor(type) {
        return type === "app" ? "app"
             : type === "system" ? "settings"
             : "todo"   // internal 兜底(动作通常自带 iconKind)
    }
    /// 取字符串首字符(代理对场景极少,charAt 按 UTF-16 码元,接受)。
    function firstCharOf(label) {
        const s = (label || "").trim()
        return s.length > 0 ? s.charAt(0) : "?"
    }
    /// tooltip 第二行:类型摘要(system/internal 附目标,app 附文件名)。
    function typeTextFor(type, target) {
        if (type === "system") return "系统 · " + (target || "")
        if (type === "internal") return "内部功能 · " + (target || "")
        if (type === "app") {
            const t = (target || "").replace(/\\/g, "/")
            const name = t.substring(t.lastIndexOf("/") + 1)
            return "应用 · " + (name || target || "")
        }
        return type
    }
    /// Repeater 的最终模型(指令 + 固定添加钮)。
    readonly property var actions: commands.concat([
        { id: "addQuick", iconKind: "plus", label: "添加 / 管理快捷功能",
          typeText: "打开设置页的「快捷功能」分类", isChar: false, char: "", dim: false }
    ])
    // 设置保存后重读(设置页增删改排序启用都走 save → settingsChanged)
    Connections {
        target: Core
        function onSettingsChanged() { root.refresh() }
    }

    // ---- 栏级共享悬停提示 ----
    // 自绘卡片而非 ToolTip 组件:ToolTip(Popup)的 delay/closePolicy 与验收 grab
    // 的时序纠缠(实测 visible 置 true 又被内部关闭);卡片是普通 Item,
    // 行为完全可控、grab 必然包含。挂在 root 上但 x 为负(越出栏体),root 无 clip。
    Rectangle {
        id: railTip
        x: -width - 8
        y: Math.max(0, Math.min(tipButtonY, root.height - height - 8))
        width: tipLayout.implicitWidth + 16
        height: tipLayout.implicitHeight + 10
        radius: Theme.metrics.radiusPopup
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong
        visible: tipLabel !== ""   // 有文本即显示(delegate 离开时清空 label)
        z: 100

        ColumnLayout {
            id: tipLayout
            anchors.centerIn: parent
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: root.tipLabel
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontCaption
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
            }
            Text {
                Layout.fillWidth: true
                visible: root.tipTypeText !== ""
                text: root.tipTypeText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontMicro
                color: Theme.colors.textSecondary
            }
        }
    }
    // 悬停提示的属性驱动入口(delegate hover 与验收直调共用):
    // C++/QML 先写 tipLabel/tipTypeText/tipButtonY,再调无参 showTip()。
    property string tipLabel: ""
    property string tipTypeText: ""
    property real tipButtonY: 0
    function showTip() { /* 文本/位置均由 tipLabel/tipTypeText/tipButtonY 绑定驱动 */ }
    function hideTip() { tipLabel = ""; tipTypeText = "" }
    /// 验收读回:tooltip 当前是否可见(--quick-commands-flow 断言用)。
    readonly property bool tipShown: railTip.visible

    /// 点击上抛(带动作 id + 指令对象;addQuick 固定钮 cmd = null)。
    /// 执行由使用方(Main.qml)接线 —— 本组件只管"长什么样"。
    signal actionTriggered(string actionId, var cmd)

    // 左侧分隔线:与状态栏同一"框架色"语言,把内容区与快捷栏分开
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.metrics.borderWidth
        color: Theme.colors.border
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: Theme.metrics.spacingSm
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.metrics.spacingXs

        Repeater {
            model: root.actions

            delegate: Item {
                id: railButton
                required property var modelData
                // 验收钩子:占位钮(首个流程后续接验收时真实点击用)
                objectName: railButton.modelData.id === "addQuick"
                            ? "quickRailAdd" : "quickRailCmd_" + railButton.modelData.id
                width: 40
                height: 40
                opacity: railButton.modelData.dim ? 0.55 : 1.0
                Behavior on opacity { NumberAnimation { duration: Theme.animation.fast } }

                // 悬停底(无选中态:占位阶段没有"激活"语义,选中态随二期动作一起加)
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.metrics.radiusButton
                    color: railMa.containsMouse ? Theme.colors.surfaceHover : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                }

                HeaderIcon {
                    anchors.centerIn: parent
                    visible: !railButton.modelData.isChar
                    kind: railButton.modelData.iconKind
                    size: 20
                    color: railMa.containsMouse ? Theme.colors.textPrimary
                                                : Theme.colors.textSecondary
                }
                // 首字图标(icon = "@"):字母块,天然逐条可辨
                Rectangle {
                    anchors.centerIn: parent
                    visible: railButton.modelData.isChar
                    width: 22; height: 22
                    radius: Theme.metrics.radiusBadge
                    color: railMa.containsMouse ? Theme.colors.accentSoft
                                                : Theme.colors.surfaceHover
                    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                    Text {
                        anchors.centerIn: parent
                        text: railButton.modelData.char
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontSecondary
                        font.weight: Font.DemiBold
                        color: railMa.containsMouse ? Theme.colors.accent
                                                    : Theme.colors.textSecondary
                    }
                }

                MouseArea {
                    id: railMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.actionTriggered(
                        railButton.modelData.id, railButton.modelData.cmd ?? null)
                    // 悬停提示走**栏级共享实例**(root 的 ToolTip):逐按钮声明
                    // attached ToolTip 在兄弟间共享 overlay 实例,visible 绑定互相
                    // 干扰导致不弹(实测);集中一处由 hover 驱动,文本切换即可。
                    onContainsMouseChanged: {
                        if (containsMouse) {
                            root.tipLabel = railButton.modelData.label
                            root.tipTypeText = railButton.modelData.typeText
                            root.tipButtonY = railButton.y
                            root.showTip()
                        } else {
                            root.hideTip()
                        }
                    }
                }
            }
        }
    }
}
