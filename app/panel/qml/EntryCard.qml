// 单条历史卡片:文本 / 图片缩略图 / 文件路径 / 待办 四种形态 + 元信息行 + 右键菜单。
// 角色名与 ClipEntryModel::roleNames 一一对应,由 ListView 按名注入。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Rectangle {
    id: root
    required property string entryId
    required property string kind
    required property string text
    required property string preview
    required property string imageUrl
    required property string timeText
    required property string sizeText
    required property bool pinned
    required property string source
    required property var tags      // QStringList → JS 数组(S5.2)
    /// 待办扩展(docs/05-todo.md §6.3):仅 kind === "todo" 时有意义。
    /// ⚠️ todoDueAt 必须是 real:unix 毫秒约 1.7e12,超出 QML int(2^31≈2.1e9),
    ///    声明成 int 会饱和 → 截止日期显示成 1970 年附近(首版实测)。
    required property real todoDueAt      // 截止毫秒;0 = 无截止
    required property real todoRemindAt   // 提醒毫秒;0 = 无
    required property bool todoDone
    required property bool todoOverdue
    /// 备注(docs/21):有内容时在标题下显示一行摘要(折叠,点击展开编辑)。
    /// 空串 = 无备注(整行不渲染,不给卡片留空位)。
    property string todoNote: ""

    /// 来源应用 exe 名(docs/25):如 "chrome.exe";空串 = 未知(v9 前条目/
    /// 采集失败/应用内部入库),元信息行不显示来源段。
    required property string sourceOwner
    /// 展示名:exe 去后缀,首字母大写以贴近应用普遍观感(不做厂商映射表)。
    readonly property string sourceOwnerLabel: {
        const n = sourceOwner
        if (n === "") return ""
        const stem = n.endsWith(".exe") ? n.slice(0, -4) : n
        return stem.charAt(0).toUpperCase() + stem.slice(1)
    }

    /// 文本条目是否展开显示全文(长文本默认折叠 4 行)。
    property bool textExpanded: false

    /// 当前搜索词(父级注入 = 搜索框文本)。非空时折叠态正文里的命中片段加底色,
    /// 让"为什么这条会被搜出来"一眼可见 —— 搜索框改了值,绑定自动重算。
    property string query: ""

    /// 参与高亮的正文:note 是"补充说明",不参与检索口径,故不参与高亮。
    readonly property string highlightableText:
        (kind === "text" || kind === "file" || kind === "todo") ? (text || preview || "") : ""

    /// 是否走富文本高亮。仅在搜索词非空且有正文时开 —— 空词时不切 RichText,
    /// 保留原纯文本路径(1MB 长文本在纯文本下也不做正则扫描)。
    readonly property bool highlighting:
        query.trim().length > 0 && highlightableText.length > 0

    /// 把正文按搜索词切段:命中段包一层底色 span,其余原样转义。
    ///
    /// ⚠️ 截断到 4000 字符再扫描:单条文本上限 1024KB,在这上面跑正则 + 拼富文本
    ///    会在滚动时逐帧重建,实测可感知卡顿;折叠态只显示 4 行,后面看不到。
    /// ⚠️ 必须**先按原串切段、再逐段转义**,不能先整体转义再找 —— 搜索词里若含
    ///    `<` `&` 之类,转义后位置与长度都变了,匹配会错位甚至切碎实体。
    /// ⚠️ 用 RegExp 而不是 indexOf 循环:大小写不敏感(FTS 口径)靠 `gi` 一次拿到,
    ///    免得自己做 toLowerCase 映射时踩"某些字符转小写后长度变化"的偏移错位。
    /// ⚠️ 转义用**字符串拼出的 alternation**而不是 `/[...]/g` 正则字面量:
    ///    qmllint(Qt 6.9.0)对"实参位置含 `\]` 转义字符类的正则字面量"解析发生
    ///    指数回溯,内存 4 秒冲破 2GB 直接拖死系统(2026-11-01 实测,bisect
    ///    定位到本行)。元字符集全是单字符,逐个加反斜杠再用 `|` 连接,与原
    ///    字符类替换语义完全等价。
    function highlightedRichText(s) {
        const raw = s.length > 4000 ? s.slice(0, 4000) : s
        const term = query.trim()
        if (term.length === 0) return Theme.escapeHtml(raw)
        // 搜索词转义成字面量(否则 `.` `*` 会被当通配)
        const escaped = '.*+?^${}()|[]\\'.split("").map(function(ch) {
            return "\\" + ch
        }).join("|")
        const re = new RegExp(escaped, "gi")
        const out0 = term.replace(re, "\\$&")
        const pattern = new RegExp(out0, "gi")
        const open = '<span style="background-color:' + Theme.colors.accentSoft
                   + ';color:' + Theme.colors.textPrimary + '">'
        let out = ""
        let last = 0
        let guard = 0
        let m = pattern.exec(raw)
        while (m !== null && guard++ < 500) {
            if (m.index > last) out += Theme.escapeHtml(raw.slice(last, m.index))
            out += open + Theme.escapeHtml(m[0]) + '</span>'
            last = m.index + m[0].length
            if (m[0].length === 0) pattern.lastIndex++   // 零长匹配防死循环
            m = pattern.exec(raw)
        }
        out += Theme.escapeHtml(raw.slice(last))
        return out
    }

    /// 内容类型的界面文案:kind 是存库标识,徽标展示中文(与导航标签页同口径)。
    readonly property string kindLabel:
        kind === "text" ? "文本" : kind === "image" ? "图片"
      : kind === "file" ? "文件" : kind === "todo" ? "待办" : kind

    /// 请求打开本条目的右键菜单。菜单由 Main.qml **统一持有**(全面板唯一实例),
    /// 这里只上抛一个内容快照 —— 委托被回收/重建也不影响已打开的菜单。
    signal contextMenuRequested(var entry)

    /// 菜单打开期间被点到:只请求关闭菜单,不执行卡片动作。
    /// 正常路径下由 Main.qml 的外部点击屏障吃掉这次点击,不会走到这里;
    /// 这是防御分支(平台/事件路由差异时的兜底),保证既不会误复制、也不会卡住菜单。
    signal menuDismissRequested()

    /// 待办的"编辑待办"按钮上抛(带正文/标签快照;时刻由 Main 侧走 todo_get 拉)。
    signal todoEditRequested(var entry)

    /// 文本条目的"编辑正文"按钮上抛(3.1):带正文快照 —— 由 Main 侧开内联卡片。
    /// ⚠️ 必须带**快照**而不是让 Main 按 id 回查:条目被翻页/筛掉时回查拿到空串,
    /// 一保存就把正文清空了(标签编辑卡片踩过同一坑)。
    signal textEditRequested(var entry)

    /// 本条目的右键菜单是否已打开(父级注入):驱动"当前操作对象"高亮。
    property bool menuOpen: false

    /// 面板内是否有右键菜单打开(父级注入):打开期间吞掉卡片点击 ——
    /// 关闭菜单的那一次点击不应顺带触发复制/收起面板。
    property bool menuOpenElsewhere: false

    // ---- 条目多选(选择模式) ----
    /// 是否处于**选择模式**(父级注入;模式内单击 = 切换选中,不复制)。
    /// ⚠️ 单击语义的替换是模式级的:复制动作在普通模式才有 —— 若让两种语义
    ///    并存(如"点复选框才选中"),普通点击仍会复制并收起面板,多选根本
    ///    操作不了(第一代待办"单击勾掉任务"是同一教训)。
    property bool selectMode: false
    /// 本条目当前是否被选中(父级按 entryId 注入)。
    property bool selected: false
    /// 单击上抛:参数是 Shift 键状态,父级据此做范围选择。
    signal toggleSelected(bool shiftHeld)

    // ---- 单击选中 / 双击复制(docs/progress.md 2026-11-01 交互变更) ----
    /// 普通模式下单击 = 选中当前行(键盘光标落位 + "当前操作对象"高亮),
    /// 不再复制并收起面板 —— 原单击复制误触率过高(点卡片只想看内容/拖选)。
    /// 上抛行号由父级统一定位键盘光标(与 ↑↓ 导航同一状态,高亮/Enter 复用)。
    /// 行号由父级注入(EntryCard 不在 ListView 委托根上,拿不到 index)。
    property int row: -1
    signal clickSelected(int row)

    /// 双击 = 复制本条并收起面板(替代原单击复制;保留双击文件 = 打开)。
    signal doubleClickedCopy()

    /// ⚠️ 列表开了 `reuseItems`(见 Main.qml):委托被回收进池、之后**复用**到别的行,
    /// 而不是销毁重建。模型角色(entryId/kind/text…)会在复用时被 ListView 自动刷新,
    /// 但委托里的**内部状态不会**。不重置就会串味:上一条文本卡的"展开"状态会被带进
    /// 复用的下一条(截图/长文时最明显)。这里在复用回池/取出时把它显式复位。
    ListView.onReused: {
        root.textExpanded = false
    }

    /// 卡片右下角的操作按钮,三种形态:
    ///   iconName != "" && !showLabel  → **纯图标**(方形,宽=高;label 作 ToolTip)
    ///   iconName != "" && showLabel   → 图标 + 文字
    ///   iconName == ""                → 纯文字
    ///
    /// 本轮四个按钮全用纯图标:元信息行同排还压着 time/size/类型/固定/来源/标签徽标,
    /// 按钮再带文字会把行宽顶爆;方形按钮(22)比原文字按钮(约 36)省 14px×4。
    /// 认知成本由 ToolTip 补偿。
    ///
    /// ⚠️ `label` 与 `showLabel` 是两个东西:label 是**提示文案**(纯图标形态不渲染,
    /// 只进 ToolTip);要不要把文字画出来由 showLabel 决定。两者混用会翻车 ——
    /// 实测:首版用 `iconOnly = iconName !== "" && label === ""` 判定,而四个按钮都传了
    /// label(给 ToolTip 用),iconOnly 恒 false → 全部退化为"图标+文字",
    /// 展开/收起的 rotation:180 把文字也转倒了(用户看到"文字颠倒")。
    ///
    /// ⚠️ 图标属性叫 iconName 不能叫 `icon`:Basic 的 AbstractButton 已有 FINAL 的
    /// `icon`,重声明会让组件加载失败(坑记在 ThemedButton.qml 注释里)。
    /// ⚠️ 方形分支不能写 `? implicitHeight : undefined`:给 implicitWidth 赋 undefined
    /// 会得到 "Unable to assign [undefined] to double",宽度失效为 0、按钮在布局里消失
    /// (同一条坑也记在 ThemedButton.qml)。
    component CardAction: Rectangle {
        id: action
        property string label: ""          // ToolTip 文案;showLabel 时兼作渲染文本
        property string iconName: ""       // "" = 纯文字按钮
        property bool showLabel: false     // true = 渲染文字(图标+文字 / 纯文字形态)
        property bool danger: false        // 危险操作(删除):悬停前景转 danger
        /// 只翻转**图标**(展开/收起的方向切换)。⚠️ 不能转整个按钮:
        /// rotation 在按钮上会把里面的文字一起倒置(实测用户反馈"文字颠倒")。
        property real iconRotation: 0
        signal triggered()
        readonly property bool iconOnly: iconName !== "" && !showLabel
        // 图标 12px(fontCaption 同号):纯图标时占满 22px 高,加文字时与文字同行
        readonly property real iconSize: Theme.metrics.fontCaption
        /// 按钮高度。⚠️ implicitWidth 不能写 `iconOnly ? implicitHeight : ...` ——
        /// 同对象隐式尺寸互引会让布局拿不到确定值(实测 4 个按钮在 RowLayout 里
        /// 全部叠到行中间,fillWidth spacer 失效);用同一常量显式写死。
        readonly property int buttonHeight: 22
        implicitWidth: iconOnly ? buttonHeight
                     : (iconName !== "" ? iconSize + 4 : 0) + actionLabel.implicitWidth + 14
        radius: Theme.metrics.radiusBadge
        implicitHeight: buttonHeight
        // 悬停底色统一 accentSoft,**危险操作只换前景色**。
        // ⚠️ 不能"底色也变 danger":红底 + 红图标/红字 = 全红一片,什么都看不见
        //    (实测用户反馈"整体太红")。与其它按钮保持同一悬停语言,仅色相不同 ——
        //    danger 前景压在 accentSoft 上对比度仍达标(浅 5.1:1 / 暗 4.6:1)。
        color: actionHover.containsMouse ? Theme.colors.accentSoft : Theme.colors.surfaceSecondary
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

        readonly property color _fg: actionHover.containsMouse
             ? (action.danger ? Theme.danger : Theme.colors.accent)
             : Theme.colors.textSecondary

        HeaderIcon {
            visible: action.iconName !== ""
            kind: action.iconName
            color: action._fg
            size: action.iconSize
            rotation: action.iconRotation
            anchors.verticalCenter: parent.verticalCenter
            x: action.iconOnly ? (parent.width - width) / 2 : 7
            Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
        }
        Text {
            id: actionLabel
            visible: action.showLabel && action.label !== ""
            // 图标+文字形态下给图标让位;纯图标形态不渲染
            x: action.iconName !== "" ? action.iconSize + 4 + 7 : 7
            anchors.verticalCenter: parent.verticalCenter
            text: action.label
            font.pixelSize: Theme.metrics.fontCaption
            color: action._fg
        }
        MouseArea {
            id: actionHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.triggered()
        }
        // 纯图标形态的认知补偿:悬停出文字提示。「展开/收起」的文案随 textExpanded
        // 自动切换,无需额外状态。delay 防止扫过一排按钮时 tooltip 刷屏。
        ToolTip.visible: action.iconOnly && actionHover.containsMouse
        ToolTip.text: action.label
        ToolTip.delay: 600
    }

    /// 元信息徽标:图标 + 文字(可选)。
    ///
    /// 图标与文字**同色**(调用方只给一个 fg),与"来源"徽标的既有口径一致:
    /// 徽标是弱化的元信息(§40),不该出现第二种颜色。
    /// 高度固定 22 与 CardAction 一致,元信息行不会因图标有无而参差。
    component MetaBadge: Rectangle {
        id: badge
        /// HeaderIcon 的种类:"" = 纯文字徽标
        property string iconKind: ""
        property string label: ""
        property color fg: Theme.colors.textSecondary
        property color bg: Theme.colors.surfaceSecondary
        implicitWidth: badgeRow.implicitWidth + 12
        implicitHeight: 22
        radius: Theme.metrics.radiusBadge
        color: badge.bg
        RowLayout {
            id: badgeRow
            anchors.centerIn: parent
            spacing: 4
            HeaderIcon {
                visible: badge.iconKind !== ""
                // 11px 与徽标文字同号:图标不高于文字,视觉上不被读成"按钮"
                size: Theme.metrics.fontMicro
                kind: badge.iconKind
                color: badge.fg
                Layout.alignment: Qt.AlignVCenter
            }
            Text {
                visible: badge.label !== ""
                text: badge.label
                font.pixelSize: Theme.metrics.fontMicro
                color: badge.fg
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }

    /// 图片卡片高度上限:截图多为 16:9,不封顶会把列表撑成"一屏一条"。
    readonly property int maxImageCardHeight: 300

    // §18/28:Surface + 1px Border,hover 换 SurfaceHover(强调色不铺满列表);
    // 菜单打开时用 AccentSoft + Accent 边框表"当前操作对象"(§18 Selected);
    // 选择模式下选中项用同一 Accent 语言 —— 选中态与"当前操作对象"共用
    // 强调语言是刻意的:两者都是"正在被操作的对象"。
    radius: Theme.metrics.radiusCard
    color: root.selected || root.menuOpen ? Theme.colors.surfaceSelected
         : ma.containsMouse ? Theme.colors.surfaceHover
         : Theme.colors.surface
    border.width: Theme.metrics.borderWidth
    border.color: root.selected || root.menuOpen ? Theme.colors.accent
                : ma.containsMouse ? Theme.colors.borderStrong
                : Theme.colors.border
    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
    Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
    implicitHeight: cardLayout.implicitHeight + 24

    // 选择模式:左缘 Accent 竖条(与分类行选中项的指示条同一视觉词汇),
    // 只在"选中"时显示 —— selectMode 本身不代表选中。
    Rectangle {
        visible: root.selected
        anchors.left: parent.left
        anchors.leftMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: parent.height - 16
        radius: 1
        color: Theme.colors.accent
    }

    // ---- 公共操作(右键菜单 / 快捷按钮 / 点击手势共用) ----

    /// 复制条目内容;成功即收起面板,便于直接粘贴到目标应用(第一代 use-entries.copy 语义)。
    function copyEntryAndHide() {
        const resp = Entries.copyEntry(root.entryId)
        if (!resp.ok) {
            Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            return
        }
        panelWindow.hidePanel()
        // 自动粘贴(N2):是否真的粘由设置 copyAutoPaste 决定(C++ 现读),
        // 这里无条件调一次 —— 关掉时它立即返回。
        Core.autoPasteIfEnabled()
    }

    /// 图片条目:编辑器会遮住面板,先收起再打开(第一代同语义)。
    function editEntryAfterHide() {
        panelWindow.hidePanel()
        Core.editEntry(root.entryId)
    }

    function deleteEntryWithToast() {
        const resp = Entries.deleteEntry(root.entryId)
        if (resp.ok) Core.showToast("已删除")
        else Core.showToast("删除失败:" + (resp.error ? resp.error.message : ""))
    }

    // 文件条目双击打开不经计时器:单击已不再复制(见 MouseArea 注释),
    // 双击作为独立手势直接处理,无需"延迟一拍防误复制"的旧机制。

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        // 滚动转发:卡片不拦截滚轮,显式转交父级 ListView 滚动。
        // (Sciter 时代需宿主手动转发滚轮进引擎;QML 化后由场景图直接派发,
        //  此处仅固定语义、防后续给卡片加 onWheel 时误吞滚动。)
        onWheel: function(wheel) { wheel.accepted = false }
        onClicked: function(mouse) {
            // 有右键菜单打开时:这一次点击只用于关闭菜单,不触发卡片动作
            if (root.menuOpenElsewhere) {
                root.menuDismissRequested()
                return
            }
            if (mouse.button === Qt.RightButton) {
                // 菜单由 Main.qml 统一持有:只上抛内容快照,不依赖本实例存活
                root.contextMenuRequested({
                    entryId: root.entryId,
                    kind: root.kind,
                    pinned: root.pinned,
                    text: root.text,
                    tags: root.tags ? root.tags.join(",") : "",
                    // 待办态快照:菜单里「完成/取消完成」的文案与动作都靠它
                    todoDone: root.todoDone
                })
            } else if (root.selectMode) {
                // ⚠️ 选择模式内单击 = 切换选中,**不复制**(复制会收起面板,
                //    多选根本没法连续点)。右键菜单在模式内仍可用(右键不受影响)。
                root.toggleSelected(mouse.modifiers & Qt.ShiftModifier)
            } else {
                // 普通模式单击 = 选中当前行(不复制、不收面板):
                // 与键盘 ↑↓ 落位同一状态,Enter/双击才复制 ——
                // 原单击即复制在"点开看一眼正文/拖窗口"时极易误触(用户反馈)。
                root.clickSelected(root.row)
            }
        }
        onDoubleClicked: function(mouse) {
            if (mouse.button !== Qt.LeftButton) return
            if (root.selectMode || root.menuOpenElsewhere) return
            if (root.kind === "file") {
                // 双击文件 = 用系统默认程序打开(保持原语义,不复制)
                Core.openEntry(root.entryId)
            } else {
                // 文本/图片/待办:双击 = 复制并收起面板(替代原单击复制)。
                // 待办也统一双击复制:右键菜单/快捷按钮已覆盖完成态等操作。
                root.doubleClickedCopy()
            }
        }
    }

    ColumnLayout {
        id: cardLayout
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Loader {
            id: bodyLoader
            Layout.fillWidth: true
            sourceComponent: root.kind === "image" && root.imageUrl !== "" ? imageBody : textBody
        }

        // 待办备注(docs/21):标题之下、正文之上的一行摘要。
        // 只在 todo 且有备注时渲染 —— 普通条目与无备注待办不多占一点高度。
        // 折叠成 2 行:卡片是"扫一眼"的尺寸,备注全文进编辑卡片看。
        Text {
            id: notePreview
            visible: root.kind === "todo" && root.todoNote !== ""
            Layout.fillWidth: true
            Layout.topMargin: -4        // 与标题贴紧(它是标题的补充说明)
            text: root.todoNote
            color: Theme.colors.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.metrics.fontCaption
            font.strikeout: root.todoDone
            opacity: root.todoDone ? 0.55 : 1.0
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            // 明确让点击穿透到卡片(否则文本会吃掉 ClickableMouseArea 的点击)
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                // 不处理任何按钮 → 事件继续派发给下层(卡片)
                propagateComposedEvents: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 待办复选框(§6.3):放元信息行最左 —— textBody 是 Loader 内的文本布局,
            // 再包一层会牵连折叠/展开的宽度计算;最左位也是"勾选完成"高频操作的稳定落点。
            // ⚠️ 独立 MouseArea 会吞掉点击:勾选不触发卡片动作。
            Rectangle {
                visible: root.kind === "todo"
                Layout.preferredWidth: 18
                Layout.fillHeight: true
                color: "transparent"
                Rectangle {
                    anchors.centerIn: parent
                    width: 14; height: 14
                    radius: Theme.metrics.radiusBadge
                    color: root.todoDone ? Theme.colors.accent : "transparent"
                    border.width: Theme.metrics.borderWidth
                    border.color: root.todoDone ? Theme.colors.accent
                                                : Theme.colors.borderStrong
                    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                    // 对勾:两根短杆,仅完成态显示(与 HeaderIcon 的 todo 图标同形)
                    Rectangle {
                        visible: root.todoDone
                        width: 6; height: 1.6; radius: 0.8
                        x: 2.4; y: 7.4; rotation: 45
                        color: Theme.colors.accentText
                    }
                    Rectangle {
                        visible: root.todoDone
                        width: 9; height: 1.6; radius: 0.8
                        x: 4.6; y: 6.2; rotation: -45
                        color: Theme.colors.accentText
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4          // 命中区略大于视觉框(≥22px 见方)
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Entries.todoSetDone(root.entryId, !root.todoDone)
                }
            }

            // 元数据弱化(§40:12px / TextSecondary)
            Label { text: root.timeText; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            Label { text: root.sizeText; color: Theme.colors.textSecondary; font.pixelSize: Theme.metrics.fontCaption }
            // 来源应用(docs/25):「来自 Chrome」—— 回答"这段话从哪复制的";
            // 空串(旧条目/未知)不渲染,不占位。
            Label {
                visible: root.sourceOwnerLabel !== ""
                text: "来自 " + root.sourceOwnerLabel
                color: Theme.colors.textTertiary
                font.pixelSize: Theme.metrics.fontCaption
            }

            // 内容类型(§38 图标 + 文字):一眼可辨文本/图片/文件/待办,
            // 不必再逐张读卡片正文才知道是什么类型。
            MetaBadge {
                iconKind: root.kind
                label: root.kindLabel
                fg: Theme.colors.textTertiary
            }

            // 待办的截止/逾期(§6.3):
            //   逾期未完成 → error 色 + "已逾期"(不带时长 —— 显示粒度只有
            //   天/小时两级,不足 1 小时会被 Math.max 强制成"1 小时",
            //   明明刚过点也显示"已逾期 1 小时",误导;且该文本不随时间
            //   自动刷新,挂多久数字就旧多久。状态本身(红字)已足够醒目);
            //   今天到期 → "今天 HH:mm";其余 → "截止 MM-dd HH:mm"。
            //   done 后不再显示逾期(归档态弱化)。
            Label {
                visible: root.kind === "todo" && root.todoDueAt > 0
                         && !(root.todoDone && root.todoOverdue)
                text: {
                    if (!root.todoDone && root.todoOverdue) {
                        return "已逾期"
                    }
                    const d = new Date(root.todoDueAt)
                    const today = new Date()
                    if (d.getFullYear() === today.getFullYear()
                            && d.getMonth() === today.getMonth()
                            && d.getDate() === today.getDate()) {
                        return "今天 " + Qt.formatDateTime(d, "HH:mm") + " 截止"
                    }
                    return "截止 " + Qt.formatDateTime(d, "MM-dd HH:mm")
                }
                font.pixelSize: Theme.metrics.fontMicro
                color: root.todoOverdue && !root.todoDone ? Theme.colors.error
                                                          : Theme.colors.textSecondary
            }

            // 待办的提醒时刻(§6.3):已设且未完成 → 一个铃铛徽标。
            // (提醒/通知已合并为一个 UI 时刻,故只显示一枚,不再分两个通道。)
            MetaBadge {
                visible: root.kind === "todo" && root.todoRemindAt > 0 && !root.todoDone
                iconKind: "bell"
                label: "提醒 " + Qt.formatDateTime(new Date(root.todoRemindAt), "MM-dd HH:mm")
                fg: Theme.colors.textSecondary
            }

            // 固定:实心琥珀底保留其"状态标记"的地位(§40 warning 语义),
            // 但补一个图钉图标 —— 只靠文字在扫视时不比类型徽标更醒目。
            MetaBadge {
                visible: root.pinned
                iconKind: "pin"
                label: "固定"
                fg: Theme.colors.onWarning
                bg: Theme.colors.warning
            }

            // 来源徽标:图标与文字同色(色相表意与 Theme.colors.source* 一致)
            MetaBadge {
                visible: root.source !== "" && root.source !== "clipboard"
                iconKind: root.source === "capture" ? "image"
                        : root.source === "translate" ? "translate"
                        : root.source === "ocr" ? "text" : ""
                label: root.source
                fg: root.source === "capture" ? Theme.colors.sourceCapture
                  : root.source === "translate" ? Theme.colors.sourceTranslate
                  : root.source === "ocr" ? Theme.colors.sourceOcr
                  : Theme.colors.textSecondary
            }

            // 标签(S5.2):每个标签用自己的派生色(RichText 单 Label,见下)。
            // ⚠️ 用**单个 Label 拼接**而不是 Layout 里放 Repeater —— 在 RowLayout 中动态
            // 增删委托会让布局的隐式尺寸失效(清空标签后卡片会塌陷、内容不可见)。
            // 上色因此走 RichText:textFormat 会吃掉 `text` 里的 HTML 标记,
            // 但这恰好也断掉了"标签名被当标记渲染"的注入路径(Theme.tagRichText 仍显式转义)。
            //
            // 底色用**中性** surfaceSecondary(与类型/来源徽标同底),不用 accentSoft:
            // 标签文字现在自带色相(橄榄/品红/青…),压在一层蓝色调底上会互相打架;
            // 而且调色板的对比度是按中性底 surfaceSecondary 验的,换底即失效。
            // 本处只表达"有哪些标签","筛选了哪个"由筛选条自己的边框/色点表达。
            Label {
                visible: root.tags !== undefined && root.tags !== null && root.tags.length > 0
                text: visible ? Theme.tagRichText(root.tags) : ""
                textFormat: Text.RichText
                font.pixelSize: Theme.metrics.fontMicro
                padding: 3; leftPadding: 6; rightPadding: 6
                background: Rectangle { color: Theme.colors.surfaceSecondary; radius: Theme.metrics.radiusBadge }
            }
            Item { Layout.fillWidth: true }

            // 展开/收起:仅文本被 4 行截断时显示(截断与否由 textBody 内部判定)。
            // 纯图标 + iconRotation 翻转:展开态箭头向下(点了收起),收起态向上 ——
            // 只造一个 chevron 形,方向由状态翻转,不画两个图形。
            // ⚠️ 必须用 iconRotation 而不是 rotation:后者会把按钮里的文字一起倒置
            //    (实测用户反馈"展开图标的文字颠倒了")。
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind === "text"
                         && bodyLoader.item !== null
                         && bodyLoader.item.truncated === true
                iconName: "chevron"
                label: root.textExpanded ? "收起" : "展开"
                iconRotation: root.textExpanded ? 0 : 180
                onTriggered: root.textExpanded = !root.textExpanded
            }

            // ---- 右下角快捷按钮(纯图标,悬停见 ToolTip) ----
            // 贴图:图片直贴;文本渲染成位图后贴(文件条目无可贴内容,故隐藏)。
            // 图标复用 pin 图钉 —— 与右键菜单「贴到屏幕」同一符号口径。
            // 已知取舍:同排的「固定」徽标也是 pin,但那是琥珀实心胶囊,与这里的
            // 中性方形按钮并排仍可辨;若后续实测仍混淆,改"浮层"造型(圆角矩形+右上斜箭头)。
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind !== "file"
                iconName: "pin"
                label: "贴图"
                onTriggered: Core.pinEntry(root.entryId)
            }
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind === "image"
                iconName: "pencil"
                label: "编辑"
                onTriggered: root.editEntryAfterHide()
            }
            // 文本专属:编辑正文(3.1)。与右键菜单「编辑正文…」同一语义 ——
            // 面板内联卡片,原地改写同一条目(不动标签/固定/来源)。
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind === "text"
                iconName: "pencil"
                label: "编辑正文"
                onTriggered: root.textEditRequested({
                    entryId: root.entryId, kind: root.kind, text: root.text
                })
            }
            // 待办专属:编辑(复用新建卡片做编辑,整卡预填)。
            // 与右键菜单「编辑待办…」同一语义;普通「编辑」按钮只对 image 有意义。
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                visible: root.kind === "todo"
                iconName: "pencil"
                label: "编辑待办"
                onTriggered: root.todoEditRequested({
                    entryId: root.entryId, kind: root.kind, text: root.text,
                    tags: root.tags ? root.tags.join(",") : ""
                })
            }
            CardAction {
                Layout.alignment: Qt.AlignVCenter
                iconName: "trash"
                label: "删除"
                danger: true
                onTriggered: root.deleteEntryWithToast()
            }
        }
    }

    Component {
        id: textBody
        Column {
            id: textWrap
            width: parent ? parent.width : 0

            /// 折叠态是否被省略号截断(决定"展开"按钮是否显示;
            /// Text 的布局在不可见时也会计算,truncated 不会失效)
            readonly property bool truncated: collapsedText.truncated

            // 折叠态:最多 4 行,超出省略号
            Text {
                id: collapsedText
                visible: !root.textExpanded
                width: parent.width
                // 搜索命中高亮时走 RichText;否则保持纯文本(不付富文本布局的代价)
                text: root.highlighting
                      ? root.highlightedRichText(root.highlightableText)
                      : (root.text || root.preview || "")
                textFormat: root.highlighting ? Text.RichText : Text.PlainText
                color: Theme.colors.textPrimary
                // 待办完成后正文划线 + 弱化(§6.3:归档展示)
                font.strikeout: root.kind === "todo" && root.todoDone
                opacity: root.kind === "todo" && root.todoDone ? 0.55 : 1.0
                // 文件条目的正文就是**路径**(反斜杠、扩展名、中文目录名混排),
                // 等宽下才逐字符可读、也与「复制路径」取到的字符串形态一致;
                // 文本/图片条目是自然语言内容,保持界面正文字体。
                font.family: root.kind === "file" ? Theme.fontMono : Theme.fontFamily
                font.pixelSize: Theme.metrics.fontBody
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 4
                elide: Text.ElideRight
            }

            // 展开态:完整文本;超过 320px 高度改为内部滚动。
            // 用只读 TextEdit 而非 Text:支持选中部分文字手动复制
            // (TextEdit 是交互项,点击它不会落到卡片的复制 MouseArea 上)
            Flickable {
                visible: root.textExpanded
                width: parent.width
                height: Math.min(fullEdit.contentHeight + 12, 320)
                clip: true
                contentWidth: width
                contentHeight: fullEdit.contentHeight + 12
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                TextEdit {
                    id: fullEdit
                    x: 6
                    width: textWrap.width - 12
                    text: root.text || ""
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.colors.textPrimary
                    // 与折叠态同一口径:待办完成态划线 + 弱化
                    font.strikeout: root.kind === "todo" && root.todoDone
                    opacity: root.kind === "todo" && root.todoDone ? 0.55 : 1.0
                    // 与折叠态同一口径:文件条目按等宽渲染路径(见 collapsedText)
                    font.family: root.kind === "file" ? Theme.fontMono : Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontBody
                    selectByMouse: true
                    selectionColor: Theme.colors.accentSoft
                    selectedTextColor: Theme.colors.textPrimary
                }
            }
        }
    }

    Component {
        id: imageBody
        Item {
            id: imageWrap
            width: parent ? parent.width : 0

            // 按图片真实宽高比铺满卡片宽度得到应占高度。
            // 注意用 implicitWidth/implicitHeight(加载后的实际尺寸),而不是
            // sourceSize——后者是"请求加载的尺寸",拿它算高度会得到错误结果。
            readonly property real fittedHeight:
                (image.implicitWidth > 0 && image.implicitHeight > 0)
                ? width * (image.implicitHeight / image.implicitWidth)
                : 0

            // 卡片高度 = 图片显示高度(未就绪时占位 60,加载完自动撑开)。
            // 原先固定 110 而 Image 自身按隐式尺寸画到 220,图片会**溢出卡片**
            // 盖住下方条目;这里让两者同源,并把 Image 显式约束到本项尺寸。
            implicitHeight: image.status === Image.Ready
                            ? Math.min(Math.max(fittedHeight, 40), root.maxImageCardHeight)
                            : 60

            Image {
                id: image
                // 显式宽高:确保图片永远不超出卡片(PreserveAspectFit 负责留白居中)
                width: imageWrap.width
                height: imageWrap.implicitHeight
                source: root.imageUrl
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: true
                // 缩略图最长边 320(入库时预生成),解码成本已受控,无需再设 sourceSize
            }
        }
    }
}
