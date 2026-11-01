// 设置页(theme.md §22/§23 + docs/22-settings-page.md):整窗页面形态,
// 左侧分类导航 + 右侧当前分类表单。
//
// 这是**主面板窗口内的一页**(与 TranslateView 同一模式),不是抽屉、也不是独立窗口:
// - 与列表/翻译互斥显示:同一窗口、同一 QQuickWidget、同一 QQmlEngine;
// - 抽屉(Popup)时代的问题:40+ 字段一次实例化(约 6.6MB 私有)且常驻不释放;
//   本页改为「壳常驻 + 内容区 Loader 只装当前分类」,离开设置页即销毁内容页;
// - 进入本页时 C++ 侧(PanelWindow::showSettings)把窗口切到 860×640 并抑制失焦隐藏
//   (填 API Key / 测试代理时切走焦点不该消失);
// - 标题栏拖动走 panelWindow.startSystemMove()(同一个窗口)。
//
// 分类页协议(所有 SettingsPage*.qml 一致):
//   property var view        —— 本壳(由 Loader onLoaded 注入);
//   function load(s)         —— 从设置对象回填(兜底默认值与 core Default 同口径);
//   function collect(s)      —— 把本页字段写回设置对象(未实例化的页不参与,天然不碰)。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Item {
    id: root
    focus: true

    // ---- 分类索引(与 categories / pageComponents 平行) ----
    readonly property int catGeneral: 0
    readonly property int catCapture: 1
    readonly property int catPin: 2
    readonly property int catHotkeys: 3
    readonly property int catQuick: 4
    readonly property int catOcr: 5
    readonly property int catTranslate: 6
    readonly property int catNetwork: 7
    readonly property int catData: 8
    readonly property int catAbout: 9

    /// 侧栏分类:label 显示文案,icon 是 HeaderIcon 的 kind,
    /// nav 是验收钩子 objectName(--settings-flow 等真实点击用)。
    readonly property var categories: [
        { label: "通用",       icon: "settings",  nav: "settingsNavGeneral" },
        { label: "截图",       icon: "capture",   nav: "settingsNavCapture" },
        { label: "贴图",       icon: "pin",       nav: "settingsNavPin" },
        { label: "快捷键",     icon: "keyboard",  nav: "settingsNavHotkeys" },
        { label: "快捷功能",   icon: "plus",      nav: "settingsNavQuick" },
        { label: "文字识别",   icon: "text",      nav: "settingsNavOcr" },
        { label: "翻译服务",   icon: "translate", nav: "settingsNavTranslate" },
        { label: "网络与代理", icon: "network",   nav: "settingsNavNetwork" },
        { label: "数据",       icon: "file",      nav: "settingsNavData" },
        { label: "关于",       icon: "info",      nav: "settingsNavAbout" },
    ]

    /// 当前分类(0 = 通用)。切换 = 换 Loader 的 sourceComponent(同步重建)。
    property int currentCategory: catGeneral

    /// get_settings 的结果(打开时重拉;各分类页 load(s) 从这里回填)。
    property var s: null

    /// 内容区是否实例化。⚠️ 不能直接绑 `visible`(Loader 隐藏时 item 的
    /// visible 自有属性仍是 true,绑它感知不到离开)—— 改由 mode/panelHidden
    /// 信号驱动:离开设置页**先落盘再卸载**(顺序由 Connections 保证,
    /// 避免"卸载先跑、pending 的编辑被销毁"的竞态)。
    property bool contentActive: panelWindow.mode === "settings"

    /// 改动去抖后自动落盘(不再有「保存」按钮)。
    /// 各控件只接**用户操作**信号(valueModified / clicked / textEdited / activated),
    /// 因此加载时回填值不会误触发保存,也就不需要 loading 守卫。
    Timer {
        id: autoSave
        interval: 400
        onTriggered: root.save()
    }

    // ---- 脏标志(各分类页置位,save() 成功后统一消费) ----
    /// 快捷键是否被改过 —— 只有改过才重新注册,避免每次自动保存都注销/重注册一遍。
    property bool hotkeysDirty: false
    /// 数据目录是否被改过(改动需重启生效,保存后要显式提示,不能混入静默自动保存)。
    property bool dataDirDirty: false
    /// 渲染后端是否被改过(同样重启生效;与数据目录分开提示,便于合并文案)。
    property bool renderDirty: false

    // ---- 共享状态行(所有分类共用:已保存 / 校验失败 / 重启生效提示) ----
    property string statusText: ""
    property bool statusError: false

    // ---- 异步结果状态(放壳根而非分类页:切分类不打断、不丢结果) ----
    /// 翻译服务「测试连接」与网络「测试代理」的结果文案。
    property string probeText: ""
    property bool probeError: false
    property string proxyProbeText: ""
    property bool proxyProbeError: false
    /// 备份 / 导出 / 恢复 / 导入:进行中标志 + 结果文案(结果经 Core 信号回投)。
    property bool backupRunning: false
    property string backupHintText: ""
    property bool backupHintError: false
    property bool exportRunning: false
    property string exportHintText: ""
    property bool exportHintError: false
    property bool restoreRunning: false
    property string restoreHintText: ""
    property bool restoreHintError: false
    /// 待确认的备份文件路径(确认卡点「确认」才发起;路径状态归数据页)。
    property bool importRunning: false
    property string importHintText: ""
    property bool importHintError: false
    /// 更新检查(docs/07-auto-update.md §9):进行中标志 + 结果里的下载页链接。
    /// `updateDownloadUrl` 非空才出现「打开下载页」—— core 只在拿到 http(s) 链接时填它。
    property bool updateChecking: false
    property string updateDownloadUrl: ""
    property string updateHintText: ""
    property bool updateHintError: false

    function errText(resp) {
        return resp && resp.error ? resp.error.message : ""
    }

    function scheduleSave() {
        autoSave.restart()
    }

    /// 立即落盘挂起的改动(切分类 / 离开设置页前调用 —— 页面一被销毁,
    /// 去抖窗口里最后一笔编辑就永远丢了;抽屉时代就有这个隐患,这里顺手修掉)。
    function flushPendingSave() {
        if (autoSave.running) {
            autoSave.stop()
            save()
        }
    }

    /// 打开设置页时重拉设置并回填当前分类页(每次打开都拿最新值:
    /// 开机自启是注册表实况、别的入口也可能改过配置)。
    function load() {
        const resp = Core.call("get_settings")
        if (!resp.ok) {
            root.statusText = "读取失败:" + errText(resp)
            root.statusError = true
            return
        }
        root.s = resp.data
        if (categoryLoader.item) categoryLoader.item.load(root.s)
    }

    /// 保存:读-改-写(先取当前设置,只让**已实例化**的分类页写入自己的字段,
    /// 未打开的分类保持原值不动)。成功后按脏标志做主题/热键的即时生效与提示。
    function save() {
        const cur = Core.call("get_settings")
        if (!cur.ok) {
            root.statusText = "保存失败:" + errText(cur)
            root.statusError = true
            return
        }
        const s = cur.data
        if (categoryLoader.item) categoryLoader.item.collect(s)
        const resp = Core.call("save_settings", s)
        root.statusText = resp.ok ? "已保存" : "保存失败:" + errText(resp)
        root.statusError = !resp.ok
        if (!resp.ok) return
        if (root.dataDirDirty) {
            root.statusText = "已保存。数据目录将在下次启动时生效(数据库与图片自动迁移)"
            root.statusError = false
            root.dataDirDirty = false
        }
        if (root.renderDirty) {
            root.statusText = "已保存。渲染后端**重启后**生效"
            root.statusError = false
            root.renderDirty = false
        }
        // 幂等:主题没变时不会重刷;只有通用页实例化过时 s.theme 才存在
        if (s.theme !== undefined) Core.applyTheme(s.theme)
        // 只有快捷键真的改过才重新注册 —— 每次自动保存都注销/重注册一遍会留下
        // 短暂的空窗,期间按热键会丢。
        if (root.hotkeysDirty) {
            Core.reloadHotkeys()
            root.hotkeysDirty = false
        }
        Entries.refreshNow()
        // 广播给设置页之外的 settings 消费方(QuickRail 的指令列表、
        // 托盘菜单文案等):没有这条信号,它们只能靠轮询或重启感知变化。
        Core.notifySettingsChanged()
    }

    /// QuickRail「添加/管理」钮的直达入口:开设置页并切到快捷功能分类。
    /// (Rail 侧不能引 catQuick 索引 —— 跨文件魔法数字会随分类增删漂移。)
    function openQuickCategory() {
        selectCategory(root.catQuick)
    }

    /// 切分类:先把挂起的改动落盘(此刻旧页还活着,collect 才收得到),再换页。
    function selectCategory(idx) {
        if (idx === root.currentCategory) return
        flushPendingSave()
        root.currentCategory = idx
    }

    // ---- 异步动作入口(分类页按钮 → 壳状态 + Core 调用;结果走 Connections) ----
    /// 翻译服务「测试连接」。provider 由翻译页的当前选择传入。
    function probeTranslate(provider) {
        root.probeText = "测试中…"
        root.probeError = false
        Core.probeTranslate(provider)   // 网络请求:异步,完成走 translateProbeFinished
    }

    /// 网络与代理「测试代理链路」。先保存再探测:proxy_probe 读的是已保存配置,
    /// 不保存则刚填的代理不参与。
    function probeProxy() {
        root.save()
        root.proxyProbeText = "测试中…"
        root.proxyProbeError = false
        Core.probeProxy()
    }

    /// 执行恢复:替换当前全部数据;nbcore 侧先自动打险备份。结果走 restoreFinished。
    /// path 由数据页的确认卡传入(产品路径 = 原生文件对话框选路径;验收路径 =
    /// Main.qml 的 acceptanceOpenRestore 直接喂路径,跳过只能人工操作的原生对话框)。
    function commitRestore(path) {
        if (categoryLoader.item) categoryLoader.item.hideRestoreConfirm()
        if (path === "") return
        root.restoreRunning = true
        root.restoreHintText = "正在恢复(大备份解包较慢,请勿关闭应用)…"
        root.restoreHintError = false
        Core.restoreBackup(path)
    }

    /// 执行导入:备份条目并入当前数据(去重跳过重复)。结果走 importFinished。
    function commitImport(path) {
        if (categoryLoader.item) categoryLoader.item.hideImportConfirm()
        if (path === "") return
        root.importRunning = true
        root.importHintText = "正在导入…"
        root.importHintError = false
        Core.importBackup(path)
    }

    /// 更新检查(关于页按钮):结果走 updateFinished。
    function startUpdateCheck() {
        root.updateChecking = true
        root.updateDownloadUrl = ""
        root.updateHintText = "正在检查更新…"
        root.updateHintError = false
        Core.checkUpdate()
    }

    // ---- 验收转发(Main.qml 的 acceptanceState / 验收函数经这里进分类页) ----
    /// 验收(--settings-flow):滚到当前分类内容底部(验证"滚动后 ✕ 仍可点")。
    function scrollToBottom() {
        if (categoryLoader.item) categoryLoader.item.scrollToBottom()
    }
    /// 验收读回:滚动位置(>0 = 确实滚过了)。
    readonly property real acceptanceScrollY:
        categoryLoader.item ? categoryLoader.item.acceptanceScrollY : -1
    /// 验收:把某个控件滚入视口(target 由 Main.qml 的 findChildByName 找好传入)。
    ///
    /// 为什么需要:`scrollToBottom()` 会把**靠上**的控件滚出视口顶部,
    /// 而 QML 的 `isVisible()` 对"被裁到视口外"的项**仍返回 true** ——
    /// 于是验收点击会落在一个窗口外的坐标上(实测 y=-207)而静默失败。
    /// 这里把目标项的上边缘对齐到视口顶部,保证它落在可见区内。
    function acceptanceScrollTo(target) {
        if (!target) return false
        if (!categoryLoader.item) return false
        return categoryLoader.item.scrollItemIntoView(target)
    }
    /// 验收(--restore-flow):切到数据分类并打开恢复确认卡。
    function openRestoreConfirm(path) {
        selectCategory(root.catData)
        if (categoryLoader.item) categoryLoader.item.openRestoreConfirm(path)
    }
    /// 验收(--restore-flow):切到数据分类并打开导入确认卡。
    function openImportConfirm(path) {
        selectCategory(root.catData)
        if (categoryLoader.item) categoryLoader.item.openImportConfirm(path)
    }
    /// 验收(--clear-all-flow):把「清空全部数据」按钮滚进可视区(危险区在表单尾部)。
    function revealClearAll() {
        selectCategory(root.catData)
        if (categoryLoader.item) categoryLoader.item.revealClearAll()
    }
    /// 验收(--update-flow):把「检查更新」按钮滚进可视区。
    function revealUpdateCheck() {
        selectCategory(root.catAbout)
        if (categoryLoader.item) categoryLoader.item.revealUpdateCheck()
    }
    /// 验收只读:确认卡可见性(只有数据页有此属性;--restore-flow 读回,
    /// QML 外部拿不到内部 id)。⚠️ 用分类守卫而非探测属性:var 上的缺属性
    /// 读出来是 undefined,直接进三目会把 undefined 灌给 bool 刷绑定警告。
    readonly property bool acceptanceRestoreCardVisible:
        root.currentCategory === root.catData && categoryLoader.item
            ? categoryLoader.item.acceptanceRestoreCardVisible : false
    readonly property bool acceptanceImportCardVisible:
        root.currentCategory === root.catData && categoryLoader.item
            ? categoryLoader.item.acceptanceImportCardVisible : false
    /// 验收读回:检查更新的文案与下载链接(状态机断言用,见 docs/07 §11.2)。
    readonly property string acceptanceUpdateHint: root.updateHintText
    readonly property string acceptanceUpdateUrl: root.updateDownloadUrl
    /// 验收读回:空置态判据 —— 按钮是否可用、"未配置"说明是否出现(D-c 的核心承诺)。
    readonly property bool acceptanceUpdateEnabled:
        Core.updateCheckUrl !== "" && !root.updateChecking
    readonly property bool acceptanceUpdateNoteVisible: Core.updateCheckUrl === ""

    // ---- 离开设置页:先落盘、再卸载内容页(顺序在这一个 Connections 里保证) ----
    Connections {
        target: panelWindow
        function onModeChanged() {
            if (panelWindow.mode === "settings") {
                root.contentActive = true
            } else {
                root.flushPendingSave()
                root.contentActive = false
            }
        }
        // 面板从设置页直接收起(✕ / 失焦隐藏):mode 不变,但内容页不该带着驻留 ——
        // 下次呼出必然走 showAtCursor → setMode(List),设置页反正要重建。
        function onPanelHidden() {
            root.flushPendingSave()
            root.contentActive = false
        }
    }

    // 分类页组件(与 categories 平行;由 contentLoader 按 currentCategory 取用)。
    Component { id: pageGeneral;  SettingsPageGeneral {} }
    Component { id: pageCapture;  SettingsPageCapture {} }
    Component { id: pagePin;      SettingsPagePin {} }
    Component { id: pageHotkeys;  SettingsPageHotkeys {} }
    Component { id: pageQuick;    SettingsPageQuick {} }
    Component { id: pageOcr;      SettingsPageOcr {} }
    Component { id: pageTranslate; SettingsPageTranslate {} }
    Component { id: pageNetwork;  SettingsPageNetwork {} }
    Component { id: pageData;     SettingsPageData {} }
    Component { id: pageAbout;    SettingsPageAbout {} }
    readonly property var pageComponents: [
        pageGeneral, pageCapture, pagePin, pageHotkeys, pageQuick, pageOcr,
        pageTranslate, pageNetwork, pageData, pageAbout,
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- 标题栏(§16:48px;整条可拖动,返回/关闭按钮浮于其上) ----
        // 圆角与描边由主面板的窗口壳提供(本页铺满整个窗口),故这里不再自带 radius。
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.metrics.headerHeight
            color: Theme.colors.surface

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.metrics.borderWidth
                color: Theme.colors.border
            }

            DragHandler {
                // §42:拖动交给系统(startSystemMove),不做逐帧 setPosition 跟手。
                // ⚠️ `target: null` 不能省:PointerHandler 的 target **默认就是声明它的父项**,
                //    于是 DragHandler 会去改这个标题栏 Rectangle 的 x/y(详见 TranslateView 同段注释)。
                target: null
                onActiveChanged: if (active) panelWindow.startSystemMove()
            }
            HoverHandler { cursorShape: Qt.SizeAllCursor }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingSm
                anchors.rightMargin: Theme.metrics.spacingSm
                spacing: Theme.metrics.spacingSm

                // 返回列表页(设置是"从列表进来调一调"的地方,回去比收起更常用;
                // 收起整个面板走右侧 ✕)。
                ThemedButton {
                    variant: "ghost"
                    square: true
                    text: "←"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: panelWindow.showList()
                }
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 8; implicitHeight: 8; radius: 4
                    color: Theme.colors.accent
                }
                Label {
                    text: "设置"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontPageTitle
                    font.weight: Font.DemiBold
                    color: Theme.colors.textPrimary
                    Layout.alignment: Qt.AlignVCenter
                }
                Item { Layout.fillWidth: true }
                ThemedButton {
                    objectName: "settingsClose"   // 验收钩子(--settings-flow 真实点击)
                    variant: "ghost"
                    square: true
                    text: "✕"
                    Layout.alignment: Qt.AlignVCenter
                    // 退出设置、回到列表页 —— 与「←」同效。⚠️ 不是翻译页的
                    // "hidePanel 收起整个面板"语义:设置的改动即时自动保存,没有
                    // "到此为止"的收尾意图;收起整个面板反而让"调完继续用列表"
                    // 的主流动线断掉(--settings-flow 实测:✕ 若 hidePanel,面板
                    // 隐藏后列表不可见、后续交互全部落空)。收起面板走列表页的 ✕/Esc。
                    onClicked: panelWindow.showList()
                }
            }
        }

        // ---- 主体:左分类导航 + 右内容区 ----
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 侧栏:选中态与列表页分类标签同一视觉语言
            // (§15:AccentSoft 底 + Accent 字 + 左侧 3px 指示条)。
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 176
                color: Theme.colors.surfaceSecondary

                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    width: Theme.metrics.borderWidth
                    color: Theme.colors.border
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.metrics.spacingSm
                    spacing: Theme.metrics.spacingXs

                    Repeater {
                        model: root.categories
                        delegate: Rectangle {
                            id: navItem
                            required property var modelData
                            required property int index
                            readonly property bool selected:
                                root.currentCategory === navItem.index
                            // 验收钩子(--settings-flow 等真实点击切分类)
                            objectName: navItem.modelData.nav
                            Layout.fillWidth: true
                            implicitHeight: 34
                            radius: Theme.metrics.radiusInput
                            color: navItem.selected ? Theme.colors.accentSoft
                                 : navMa.containsMouse ? Theme.colors.surfaceHover
                                 : "transparent"
                            border.width: Theme.metrics.borderWidth
                            border.color: navItem.selected ? Theme.colors.accent : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

                            // 左侧 Accent 指示条(§15:2~3px)
                            Rectangle {
                                visible: navItem.selected
                                anchors.left: parent.left
                                anchors.leftMargin: 3
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: parent.height - 12
                                radius: 1
                                color: Theme.colors.accent
                            }

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: Theme.metrics.spacingMd
                                spacing: Theme.metrics.spacingSm
                                HeaderIcon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    kind: navItem.modelData.icon
                                    size: 15
                                    color: navItem.selected ? Theme.colors.accent
                                                         : Theme.colors.textSecondary
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: navItem.modelData.label
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.metrics.fontSecondary
                                    color: navItem.selected ? Theme.colors.accent
                                                         : Theme.colors.textPrimary
                                }
                            }

                            MouseArea {
                                id: navMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectCategory(navItem.index)
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // 内容区:当前分类表单 + 共享状态行
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ⚠️ 内存策略的核心:只实例化**当前分类**(旧抽屉是 40+ 字段全量常驻)。
                // active 由 contentActive 驱动(见其注释),离开设置页即销毁。
                Loader {
                    id: categoryLoader
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: root.contentActive
                    sourceComponent: root.pageComponents[root.currentCategory]
                    // 构建是同步的(非异步 Loader),onLoaded 时 item 立即可用。
                    onLoaded: {
                        item.view = root
                        item.load(root.s)
                    }
                }

                // 状态行(改动即时保存的去抖反馈;校验失败/重启生效提示也在这里)
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.metrics.spacingMd
                    Layout.rightMargin: Theme.metrics.spacingMd
                    Layout.topMargin: Theme.metrics.spacingXs
                    Layout.bottomMargin: Theme.metrics.spacingSm
                    text: root.statusText
                    color: root.statusError ? Theme.colors.error : Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    wrapMode: Text.WrapAnywhere
                    // 恒占一行高度,避免结果出现/消失时内容区跳动
                    Layout.minimumHeight: Theme.metrics.fontCaption + 8
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // ---- 异步结果回投(避免同步网络请求冻结界面) ----
    // ⚠️ 挂在壳根:结果写进根属性,分类页 label 绑定 —— 用户切走分类再切回来,
    //    结果还在(挂在分类页会在页面销毁时丢掉)。
    Connections {
        target: Core
        function onTranslateProbeFinished(resp) {
            if (!resp.ok) {
                root.probeText = "测试失败:" + root.errText(resp)
                root.probeError = true
                return
            }
            const d = resp.data || {}
            root.probeText = (d.available ? "✓ " : "✗ ") + (d.message || "")
            root.probeError = !d.available
        }
        function onProxyProbeFinished(resp) {
            if (!resp.ok) {
                root.proxyProbeText = "测试失败:" + root.errText(resp)
                root.proxyProbeError = true
                return
            }
            const d = resp.data || {}
            const head = d.detail ? d.detail + " · " : ""
            root.proxyProbeText = (d.ok ? "✓ " : "✗ ") + head
                    + (d.ok ? d.elapsedMs + " ms" : (d.error || "链路不可用"))
            root.proxyProbeError = !d.ok
        }
        function onBackupFinished(resp) {
            root.backupRunning = false
            if (!resp.ok) {
                root.backupHintText = "备份失败:" + root.errText(resp)
                root.backupHintError = true
                return
            }
            const d = resp.data || {}
            root.backupHintText = "✓ 备份完成:" + d.entryCount + " 条条目、"
                    + d.blobCount + " 个图片文件(" + (d.zipBytes / 1048576).toFixed(1) + " MB)"
            root.backupHintError = false
        }
        function onExportFinished(resp) {
            root.exportRunning = false
            if (!resp.ok) {
                root.exportHintText = "导出失败:" + root.errText(resp)
                root.exportHintError = true
                return
            }
            const d = resp.data || {}
            root.exportHintText = "✓ 导出完成:" + d.entryCount + " 条条目("
                    + (d.fileBytes / 1048576).toFixed(1) + " MB)"
            root.exportHintError = false
        }
        function onRestoreFinished(resp) {
            root.restoreRunning = false
            if (!resp.ok) {
                root.restoreHintText = "恢复失败:" + root.errText(resp)
                root.restoreHintError = true
                return
            }
            const d = resp.data || {}
            root.restoreHintText = "✓ 已恢复:" + d.entryCount + " 条条目、"
                    + d.blobCount + " 个图片文件;恢复前数据已自动备份到 "
                    + d.safetyBackupPath
            root.restoreHintError = false
            Entries.refreshNow()   // 列表/计数切到恢复后的数据
        }
        function onImportFinished(resp) {
            root.importRunning = false
            if (!resp.ok) {
                root.importHintText = "导入失败:" + root.errText(resp)
                root.importHintError = true
                return
            }
            const d = resp.data || {}
            root.importHintText = "✓ 已导入 " + d.imported + " 条(跳过重复 " + d.skipped + " 条)"
                    + (d.blobMissing > 0
                       ? ";有 " + d.blobMissing + " 张图片的文件在备份中缺失" : "")
            root.importHintError = false
            Entries.refreshNow()
        }
        function onUpdateFinished(resp) {
            root.updateChecking = false
            if (!resp.ok) {
                root.updateHintText = "检查失败:" + root.errText(resp)
                root.updateHintError = true
                return
            }
            const d = resp.data || {}
            if (d.configured === false) {
                // 常量在运行期是空的(不该走到这里:按钮此时置灰)—— 兜底成一句人话
                root.updateHintText = "更新源未配置"
                root.updateHintError = false
                return
            }
            // ⚠️ 只在真的要更新时才交出下载链接:已最新 / 被忽略的预发布,把老版本(或 rc)
            //    的发布页摊在设置页上是噪音 —— --update-flow 的 /same 模式实测抓出过。
            root.updateDownloadUrl = d.hasUpdate ? (d.downloadUrl || "") : ""
            root.updateHintError = false
            if (d.hasUpdate) {
                const date = d.publishedAt ? "（" + d.publishedAt + "）" : ""
                root.updateHintText = "发现新版本 " + d.latestVersion + date
                        + (root.updateDownloadUrl === "" ? ";本次响应未给出下载链接" : "")
            } else {
                root.updateHintText = "已是最新版本 " + (d.currentVersion || Core.appVersion)
            }
        }
    }
}
