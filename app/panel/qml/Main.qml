// 主面板(theme.md 布局):Header(标题/搜索/翻译/设置) + 导航标签 + 历史列表
// + 底部状态栏(快捷键提示) + 设置抽屉 + 右下角 Toast。
//
// 面板有**两种形态**(见 PanelWindow::Mode),同一窗口内互斥显示:
//  - 列表页:呼出即用、用完即走的剪贴板历史(本文件 142 行起的 ColumnLayout);
//  - 翻译页:需要停留的翻译工作区(TranslateView.qml,由 Loader 延迟构建)。
// 合并成整窗模式切换(而不是独立窗口)是为了省掉第二个 QQmlEngine 与它自己的
// 场景图/渲染栈 —— 详见 docs/progress.md 的内存收口记录。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.Core 1.0
import Nextboard.panel

Rectangle {
    id: root
    color: Theme.colors.background
    border.width: Theme.metrics.borderWidth
    border.color: Theme.colors.border
    focus: true

    function showToast(message) { toast.showMsg(message) }

    // 自动化验收入口:C++ applySearchText 直调(等价用户输入,走同一防抖链路)
    function setSearch(text) {
        searchInput.text = text
        Entries.setQuery(text)
    }

    // 自动化验收入口:C++ openSettingsPanel 直调(等价点「设置」按钮)。
    // 设置抽屉已改 Loader 懒构建,这条链路同时验证"首次构建 + 尺寸对齐面板根"。
    function openSettings() { settingsDrawerLoader.openAndLoad() }

    /// 自动化验收入口(待办链路,**零参分步**直调,等价用户操作序列):
    ///   ① acceptanceOpenPicker():打开新建卡片 + 点「截止」字段开浮层(保持打开)
    ///   ② 宿主读 acceptanceState 断言浮层几何在可视区内
    ///   ③ acceptanceTargetMs 赋值 → acceptanceClickConfirm():**真实点击**「确定」
    ///   ④ acceptanceInputText 赋值 → acceptanceClickSave():**真实点击**「保存」
    ///
    /// ⚠️ 全部零参:QMetaObject::invokeMethod 按签名查找,带参与不带参是不同签名;
    ///    参数走属性传递可以避免签名不匹配(实测 "No such method ...(QVariant,QVariant)")。
    /// ⚠️ 动作只产生副作用、不返回结构体:invokeMethod 无法把 QJSValue 对象
    ///    转成 QVariantMap(实测 return type mismatch),状态一律走 acceptanceState 读回。
    /// ⚠️ **不能再直接调 pickerBody.commit()/apply() 来"模拟"点确定** ——
    ///    那会绕过按钮的命中路径,正是这个盲区漏掉了"点确定没反应"的真 bug。
    ///    现在改为把点击事件投到按钮项上,走与用户完全相同的链路。
    property real acceptanceTargetMs: 0
    property string acceptanceInputText: ""

    function acceptanceOpenPicker() {
        openTodoComposer(null)
        openTimePicker(todoDueField)          // 等价用户点「截止」字段
    }

    /// 等价用户**拨滚轮**:经 pickerBody.spinHourTo 走与用户拨动相同的
    /// onCurrentIndexChanged → rebuildFromTumblers 链路。拨轮后字段**不**写回
    /// (新契约:确定才写回),宿主在真实点击「确定」后从 dueFieldMs 断言。
    function acceptanceSetWheel() {
        pickerBody.spinHourTo(acceptanceTargetMs)
    }

    /// 等价用户在正文框输入(保存由真实点击「保存」按钮触发)。
    function acceptanceSetText() {
        todoInput.text = acceptanceInputText
    }

    /// 验收:切到待办视图并读回分组(§6.3 智能分组的分界点自检)。
    function acceptanceShowTodoGroups() {
        showTodos()
    }

    /// 验收:切回「全部」视图(用于读回该视图下的筛选条状态 ——
    /// 来源条在待办视图隐藏、在全部视图显示,两个视图都要能读到)。
    function acceptanceShowAll() {
        tabs.current = "all"
        Entries.setView("all")
        Entries.refreshNow()
    }

    /// 读回:是否处于编辑态(供断言"保存成功后卡片已关")。
    readonly property string acceptanceEditId: todoEditId

    /// 验收只读状态:浮层几何 + 字段值 + 卡片可见性 + 错误行 + 按钮几何。
    readonly property var acceptanceState: ({
        cardVisible: todoComposerCard.visible,
        pickerVisible: timePickerPopup.visible,
        pickerOpen: timePickerPopup.visible,
        pickerX: timePickerPopup.x,
        pickerY: timePickerPopup.y,
        pickerW: timePickerPopup.width,
        pickerH: timePickerPopup.height,
        pickerContentH: timePickerPopup.contentItem
                        ? timePickerPopup.contentItem.implicitHeight : -1,
        // 浮层内容在场景中的绝对原点与尺寸(用于把点击坐标算准)
        contentX: timePickerPopup.contentItem
                  ? timePickerPopup.contentItem.mapToItem(null, 0, 0).x : -1,
        contentY: timePickerPopup.contentItem
                  ? timePickerPopup.contentItem.mapToItem(null, 0, 0).y : -1,
        overlayW: Overlay.overlay ? Overlay.overlay.width : -1,
        overlayH: Overlay.overlay ? Overlay.overlay.height : -1,
        dueFieldMs: todoDueField.ms,
        remindFieldMs: todoRemindField.ms,
        errorText: todoError.text,
        // 待办分组自检(§6.3):当前视图 + 前若干行的分组键,
        // 用来验证"组标题是否落在正确分界点"。纯读回,不改状态。
        // ⚠️ 属性值位置只接受**表达式**,块语句必须包成 IIFE(直接写
        //    { const ...; for ...; return } 会被当成对象字面量而报语法错)。
        viewId: tabs.current,
        rowGroups: (function() {
            const out = []
            const n = Math.min(list.count, 8)
            for (let i = 0; i < n; ++i) out.push(Entries.rowGroupKey(i))
            return out.join(",")
        })(),
        // 筛选条状态:验证"哪些 chip 会出现"(来源条的把控逻辑较隐蔽:
        // 默认来源不列、待办视图整体不显示,靠截图目视易漏)。
        sourceChips: (function() {
            const out = []
            for (let i = 0; i < sourceBar.sourceModel.length; ++i)
                out.push(sourceBar.sourceModel[i].value)
            return out.join(",")
        })(),
        sourceBarVisible: sourceBar.visible,
        tagChips: (function() {
            const out = []
            for (let i = 0; i < Entries.tagStats.length; ++i)
                out.push(Entries.tagStats[i].tag)
            return out.join(",")
        })()
    })

    // ---- 右键菜单:全面板**唯一实例** ----
    // 原先每个 EntryCard 各自持有一个 Menu,存在两个问题:
    //   ① 委托被 ListView 回收/重建时,菜单与卡片的绑定可能失效;
    //   ② 点击菜单外部(尤其左键点其他卡片)时,这一次点击会**穿透**到下层卡片,
    //      触发"复制并收起面板",与菜单操作互相干扰、菜单也没能正常关闭。
    // 现改为面板级单例:卡片只上抛内容快照;菜单打开期间由**屏障**吃掉外部点击。
    /// 当前菜单的目标条目快照(entryId/kind/pinned/text/tags);null = 菜单未打开。
    property var menuTarget: null

    /// 标签筛选(S5.2):空串 = 不筛选。与分类标签/搜索叠加。
    property string activeTag: ""

    /// 来源筛选(S5.1):空串 = 不筛选。只看截图/翻译结果/识别文字等。
    /// 与卡片来源徽标同口径,可与分类/搜索/标签叠加。
    property string activeSource: ""

    /// 来源筛选条的可选项(值/文案/图标/计数),由 refreshSourceStats 填充。
    /// 计数取自 Core.counts 的按来源统计(entry_counts 已有各分类计数,
    /// 来源计数在 setSource 首次生效时按需拉取)。
    function toggleSourceFilter(value) {
        root.activeSource = (root.activeSource === value) ? "" : value
        Entries.setSource(root.activeSource)
    }

    /// 标签编辑:目标条目 id 与内联卡片可见性。
    property string tagEditId: ""

    /// 翻译页是否已构建过:首次进入才实例化,之后常驻 ——
    /// 这样"切走再回来"原文/译文还在,且从不翻译的用户完全不付这份开销。
    property bool translatePageBuilt: false

    /// 收掉列表页的全部浮层:设置抽屉 / 右键菜单 / 标签编辑。
    ///
    /// ⚠️ 设置抽屉(`AppDrawer` → `Drawer`)继承自 **Popup**,渲染在窗口的 overlay 层 ——
    /// 它永远盖在所有普通 Item 之上,与"谁先声明"无关。所以切到翻译页时若不清掉,
    /// 它会**浮在翻译页上面**,而且此时焦点已交给翻译页,抽屉自己的 Esc 也收不到。
    function dismissListOverlays() {
        // 设置抽屉已改 Loader 懒构建:item 为空 = 从未打开过,自然没有浮层要收
        if (settingsDrawerLoader.item && settingsDrawerLoader.item.opened)
            settingsDrawerLoader.item.close()
        if (root.menuTarget !== null) root.closeEntryMenu()
        if (root.tagEditId !== "") root.tagEditId = ""
        // 标签重命名卡片同理(新增时容易漏,漏了它会残留在翻译页上)
        if (tagRenameCard.visible) tagRenameCard.visible = false
    }

    /// C++ 侧(PanelWindow::showTranslate)调用:切到翻译页并预填原文。
    /// 页的构建是同步的(非异步 Loader),故置位后 item 立即可用。
    function openTranslate(text) {
        dismissListOverlays()   // 先把列表页的浮层收干净,再进翻译页
        translatePageBuilt = true
        if (translatePage.item) translatePage.item.openWith(text)
    }

    /// 打开标签编辑卡片(右键菜单「标签…」)。
    function openTagEditor(entry) {
        tagEditId = entry.entryId
        tagInput.text = entry.tags || ""
        tagEditorCard.visible = true
        tagInput.forceActiveFocus()
    }

    /// 提交标签:按逗号/空白切分,去空去重后交模型保存。
    function commitTagEditor() {
        const parts = tagInput.text.split(/[,，\s]+/)
        const tags = []
        for (let i = 0; i < parts.length; ++i) {
            const t = parts[i].trim()
            if (t.length > 0 && tags.indexOf(t) < 0) tags.push(t)
        }
        tagEditorCard.visible = false
        Entries.saveTags(tagEditId, tags)
    }

    // ---- 标签重命名(S5.2 标签管理) ----
    /// 待重命名的原标签名(空 = 卡片未打开)。
    property string tagRenameFrom: ""

    /// 打开重命名卡片(右键标签筛选条的 chip)。
    function openTagRename(tag) {
        tagRenameFrom = tag
        tagRenameInput.text = tag
        tagRenameError.text = ""
        tagRenameCard.visible = true
        tagRenameInput.forceActiveFocus()
        tagRenameInput.selectAll()
    }

    /// 提交重命名:空名 = 删除该标签(core 侧语义)。
    /// 失败(如与原标签一字不差的空名)把原因显示在卡片里,不关卡片。
    function commitTagRename() {
        const to = tagRenameInput.text.trim()
        if (to === tagRenameFrom) {
            tagRenameCard.visible = false   // 没改:静默关闭
            return
        }
        const resp = Entries.renameTag(tagRenameFrom, to)
        if (resp.ok !== true) {
            tagRenameError.text = resp.message ? resp.message : "重命名失败"
            return
        }
        tagRenameCard.visible = false
        // 当前筛选的就是被改名的标签 → 跟随到新名(否则列表空掉,见模型侧注释)
        if (root.activeTag === tagRenameFrom) {
            root.activeTag = to
        }
        Core.showToast(to === "" ? "标签已删除"
                                 : "已重命名为「" + to + "」(" + resp.changed + " 条)")
    }

    // ---- 待办(docs/05-todo.md §6) ----
    /// 正在编辑的待办 id;空串 = 新建。
    property string todoEditId: ""
    /// 当前展开的时间选择器浮层所属字段("due"/"remind"/"")。
    /// 仅用于字段高亮;浮层本体是 Popup,用自己的 visible 判定开关。
    property string openPickerKind: ""
    /// 打开浮层时目标字段的值:「取消」恢复用(0 = 打开前就是未设置)。
    property real openPickerMsOnOpen: 0
    /// 打开浮层时**提醒**字段的值:取消截止浮层时一并恢复(联动会改它)。
    property real openRemindMsOnOpen: 0

    /// 「确定」(截止浮层):写回截止;提醒未被用户手动设置时,
    /// 联动为"新截止前 10 分钟" —— **每次确定都重算**,改截止提醒跟着变。
    function confirmDuePicker(ms) {
        todoDueField.apply(ms)
        if (!remindManuallySet)
            todoRemindField.apply(ms > 0 ? ms - remindDefaultLeadMs : 0)
    }

    /// 「确定」(提醒浮层):写回提醒,并标记"用户手动设置" ——
    /// 此后改截止不再自动覆盖它。
    function confirmRemindPicker(ms) {
        todoRemindField.apply(ms)
        remindManuallySet = true
    }

    /// 「取消」:恢复打开浮层前的截止与提醒(联动可能已改过提醒)。
    function cancelDuePicker() {
        todoDueField.apply(openPickerMsOnOpen)
        todoRemindField.apply(openRemindMsOnOpen)
    }
    /// 程序化回填中(openTodoComposer 预填):抑制"截止 → 提醒"默认联动 ——
    /// 联动是给**用户改截止**用的,回填旧数据时不该顺手替用户建一条提醒。
    property bool todoPrefilling: false

    /// 待办到点后由 C++(PanelWindow::showCenteredOnTodos)直调:切到"待办"视图。
    /// 托盘「今日待办」子菜单点击某条 → C++ 传条目 id 调本函数,定位并高亮该条。
    /// ⚠️ 必须先清搜索词/标签/来源筛选:残留条件会把待办过滤掉,用户会看到空列表。
    function showTodos(focusId) {
        if (searchInput.text !== "") {
            searchInput.clear()
            Entries.setQuery("")
        }
        if (root.activeTag !== "") {
            root.activeTag = ""
            Entries.setTag("")
        }
        if (root.activeSource !== "") {
            root.activeSource = ""
            Entries.setSource("")
        }
        tabs.current = "todo"
        Entries.setView("todo")
        Entries.refreshNow()
        if (focusId !== undefined && focusId !== "") locateTodo(focusId)
    }

    /// 在待办视图里定位并高亮指定条目(托盘子菜单点击链路)。
    /// ⚠️ refreshNow 是同步 FFI 重拉,行集此刻已就绪,可直接定位;
    ///    条目不在首屏(理论上不会:托盘展示的就是排序前 8 条)则退化不定位。
    property string highlightTodoId: ""
    Timer {
        id: todoHighlightTimer
        interval: 1800
        onTriggered: root.highlightTodoId = ""
    }
    function locateTodo(id) {
        const idx = Entries.indexOfId(id)
        if (idx < 0) return
        highlightTodoId = id
        list.positionViewAtIndex(idx, ListView.Center)
        todoHighlightTimer.restart()
    }

    /// 时间字段联动:截止**确定后**给提醒做默认 —— 提醒未被用户手动设置时,
    /// 默认"截止前 10 分钟"(原 30 分钟偏久,实测反馈改为 10 分钟)。
    /// ⚠️ 截止本身在 10 分钟内的,提醒时刻会落在过去 → 视为"立即提醒"
    ///    (core §3 语义:下一轮调度触发一次),行为合理无需特判。
    readonly property int remindDefaultLeadMs: 10 * 60 * 1000
    /// 提醒是否被**用户手动**设置过(浮层确定/✕清空都算)。
    /// ⚠️ 必须区分"回填的旧值":打开卡片时回填不算手动 —— 否则用户改了截止,
    ///    提醒却永远停留在旧数据的联动值上不跟随(实测反馈的第二个问题)。
    property bool remindManuallySet: false

    /// 打开新建(头部按钮)或编辑(右键菜单/卡片按钮)待办的卡片。
    function openTodoComposer(entry) {
        dismissListOverlays()
        closeTimePicker()
        todoEditId = entry ? entry.entryId : ""
        // 正文优先用快照(右键菜单/卡片上抛的 text):按 id 回查只保证当前页,
        // 条目被翻页/筛选排除时会拿不到,编辑一下就把正文清空了。
        const snapshot = entry && entry.text ? entry.text : ""
        todoInput.text = entry ? snapshot : ""
        // 时刻与标签走 todo_get 一次取全(meta 为空 = 非待办/不存在 → 全空)
        const meta = entry ? Entries.todoMeta(entry.entryId) : null
        todoPrefilling = true
        todoDueField.apply(meta && meta.dueAt > 0 ? meta.dueAt : 0)
        // 旧数据 remind/notify 可能不同值:取较早的(先响的那个)展示
        const r = meta && meta.remindAt > 0 ? meta.remindAt : 0
        const n = meta && meta.notifyAt > 0 ? meta.notifyAt : 0
        todoRemindField.apply((r > 0 && n > 0) ? Math.min(r, n) : Math.max(r, n))
        todoPrefilling = false
        // 回填**不算**用户手动设置:此后改截止,提醒应跟随新的默认联动
        remindManuallySet = false
        todoTagInput.text = entry && entry.tags ? entry.tags : ""
        todoError.text = ""
        todoComposerCard.visible = true
        todoInput.forceActiveFocus()
    }

    /// 收起新建/编辑卡片:连带收掉时间选择器浮层 ——
    /// ⚠️ 浮层渲染在 Overlay 层,**不随卡片隐藏而消失**,会孤零零留在面板上。
    function closeTodoComposer() {
        closeTimePicker()
        todoComposerCard.visible = false
    }

    /// 打开时间选择器浮层。同一字段再点一次 = 收起。
    ///
    /// ⚠️ 两个关键点(首版交互问题的根因):
    ///  1. 定位必须在 open() **之后**算:内容(pickerBody)是 Popup 挂载时才创建的,
    ///     open() 前读 height 得到 0 → 误判"下方空间足够"→ 浮层溢出面板底边,
    ///     滚轮落在可视区外,表现就是"点了打不开"。
    ///  2. 关闭交给屏障显式 closeTimePicker(),不依赖 CloseOnPressOutside ——
    ///     后者会吞掉点「保存」的那一下(点击被用于关浮层,不穿透到按钮),
    ///     表现就是"必须点别处才能关轮子、否则保存没反应"。
    function openTimePicker(field) {
        const wasOpen = timePickerPopup.visible
            && timePickerPopup.targetKind === field.pickerKind
        if (wasOpen) {
            closeTimePicker()
            return
        }
        timePickerPopup.targetField = field
        timePickerPopup.targetKind = field.pickerKind
        openPickerKind = field.pickerKind
        openPickerMsOnOpen = field.ms        // 「取消」恢复基准
        // 截止浮层的「取消」要一并恢复提醒(确定时联动会改它)
        openRemindMsOnOpen = todoRemindField.ms
        // ⚠️ 定位必须在 open() **之前**:open() 后再改 x/y,渲染位置会跟过去,
        //    但 Overlay 的**命中区域**不跟着更新(探针证实:官方 childAt 在按钮
        //    位置命中的是 Overlay 容器而非按钮)→ 浮层看得见、滚轮拨得动、
        //    按钮却点不着。先摆好位置再打开,两者一致。
        positionTimePicker(field)
        timePickerPopup.open()
        // 预填仍需在 open() 之后:contentItem 挂载前 Tumbler 的 currentIndex
        // 赋值会被视图重置(实测:快捷钮设置后打开,滚轮停在错误位置)。
        pickerBody.setFromMs(field.ms)
    }

    /// 浮层定位:默认贴字段下方;下方放不下(最小高面板)翻到字段上方。
    /// ⚠️ 高度取 contentItem 的隐式高度,而不是 Popup.height:Padding 与
    ///    contentItem 的隐式尺寸叠加规则在首次 open() 时可能还没落定,
    ///    用后者偶发算出 0 高度 → 误判"下方放得下" → 浮层溢出面板底边,
    ///    滚轮落到可视区之外,表现就是"点了打不开"(首版实测)。
    function positionTimePicker(field) {
        const overlay = Overlay.overlay
        const pos = field.mapToItem(overlay, 0, field.height)
        const w = timePickerPopup.width
        const h = timePickerPopup.contentItem
                  ? timePickerPopup.contentItem.implicitHeight : timePickerPopup.height
        timePickerPopup.x = Math.max(8, Math.min(pos.x, overlay.width - w - 8))
        const below = overlay.height - pos.y
        timePickerPopup.y = below >= h + 8
                           ? pos.y
                           : Math.max(8, pos.y - field.height - h - 8)
    }

    function closeTimePicker() {
        timePickerPopup.close()
        openPickerKind = ""
    }

    /// 推迟待办(右键菜单):due/remind/notify 各 +delta(已过点的同样顺延)。
    function postponeTodo(id, delta) {
        const meta = Entries.todoMeta(id)
        if (!meta || meta.dueAt === undefined) return
        const r = meta.remindAt > 0 ? meta.remindAt
                  : (meta.notifyAt > 0 ? meta.notifyAt : 0)
        Entries.todoUpdateTimes(id, {
            "dueAt": meta.dueAt > 0 ? meta.dueAt + delta : null,
            "remindAt": r > 0 ? r + delta : null,
            "notifyAt": r > 0 ? r + delta : null
        })
        Core.showToast("已推迟")
    }

    /// 提交新建/编辑待办。失败时**保留卡片与输入**,只把原因写到错误行。
    function commitTodoComposer() {
        const text = todoInput.text.trim()
        if (text === "") {
            todoError.text = "写点什么吧 —— 正文不能为空"
            return
        }
        // 提醒/通知合并为一个 UI 时刻:写同值 —— 到点时面板高亮 + 托盘气泡
        // 一起发生(core 两通道各自去重)。数据层两字段保留以兼容旧数据。
        const remind = todoRemindField.ms > 0 ? todoRemindField.ms : null
        const times = {
            "dueAt": todoDueField.ms > 0 ? todoDueField.ms : null,
            "remindAt": remind,
            "notifyAt": remind
        }
        const tags = []
        const parts = todoTagInput.text.split(/[,，\s]+/)
        for (let i = 0; i < parts.length; ++i) {
            const t = parts[i].trim()
            if (t.length > 0 && tags.indexOf(t) < 0) tags.push(t)
        }
        todoError.text = ""
        const resp = Entries.todoSave(todoEditId, text, times, tags)
        if (resp.ok !== true) {
            // core 原文直接进错误行(如"正文超过 4KB"),卡片不关、输入不丢
            todoError.text = resp.message ? resp.message : "保存失败"
            return
        }
        closeTodoComposer()
        Core.showToast(todoEditId === "" ? "已添加待办" : "待办已更新")
    }

    /// EntryCard 右键时调用:记住目标条目并在光标处弹出菜单。
    function openEntryMenu(entry) {
        menuTarget = entry
        entryMenu.popup()
    }
    /// 收起菜单并清空目标(屏障点击、菜单自身关闭都会走到这里)。
    function closeEntryMenu() {
        menuTarget = null
        entryMenu.close()
    }

    // ---- 菜单动作 ----
    // 与卡片内的同名逻辑等价,但只作用于**快照里的 id** —— 不依赖委托实例是否还在。
    function copyEntryAndHide(id) {
        const resp = Entries.copyEntry(id)
        if (resp.ok) panelWindow.hidePanel()
        else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
    }
    function copyPathAndHide(text) {
        const resp = Core.call("copy_text_to_clipboard", { text: text })
        if (resp.ok) panelWindow.hidePanel()
        else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
    }
    function editEntryAfterHide(id) {
        panelWindow.hidePanel()
        Core.editEntry(id)
    }
    function deleteEntryWithToast(id) {
        const resp = Entries.deleteEntry(id)
        if (resp.ok) Core.showToast("已删除")
        else Core.showToast("删除失败:" + (resp.error ? resp.error.message : ""))
    }
    /// 翻译:文本条目取全文交翻译窗口;图片/文件明确提示而非静默失败。
    function translateEntry(entry) {
        if (entry.kind !== "text") {
            Core.showToast(entry.kind === "image" ? "图片请先「识别文字」再翻译"
                                                 : "文件条目不支持翻译")
            return
        }
        const resp = Core.call("get_entry", { id: entry.entryId })
        if (resp.ok && resp.data.textContent) Core.openTranslateWith(resp.data.textContent)
        else Core.showToast("读取条目内容失败")
    }

    Keys.onEscapePressed: {
        // 分层处理,一次 Esc 只消费一层(自上而下):
        //   设置抽屉 → 翻译页自己的浮层 → 收起面板
        // 翻译页的 Esc 语义与 ✕ 一致:**直接收起**,不再顺带切回列表页。
        if (settingsDrawerLoader.item && settingsDrawerLoader.item.opened) {
            settingsDrawerLoader.item.close()
            return
        }
        if (panelWindow.mode === "translate") {
            // 翻译页存在时先问它有没有浮层要关(如历史抽屉),没有就收起面板
            if (!translatePage.item || !translatePage.item.dismissTopLayer())
                panelWindow.hidePanel()
            return
        }
        panelWindow.hidePanel()
    }
    // Ctrl+F / Ctrl+K 聚焦搜索(§27);Ctrl+1..9 复制当前视图第 N 条(第一代 F2)。
    Keys.onPressed: function(event) {
        if ((event.key === Qt.Key_F || event.key === Qt.Key_K)
                && (event.modifiers & Qt.ControlModifier)) {
            searchInput.forceActiveFocus()
            searchInput.selectAll()
            event.accepted = true
            return
        }
        if ((event.modifiers & Qt.ControlModifier)
                && !(settingsDrawerLoader.item && settingsDrawerLoader.item.opened)
                && panelWindow.mode !== "translate"
                && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            const resp = Entries.copyByIndex(event.key - Qt.Key_1)
            if (resp.ok) panelWindow.hidePanel()
            else Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            event.accepted = true
        }
    }

    // ---- 页 1:剪贴板历史(列表) ----
    // 与翻译页互斥显示:不可见时不产生场景图节点,切到翻译页时这块的渲染开销归零。
    ColumnLayout {
        anchors.fill: parent
        visible: panelWindow.mode === "list"
        spacing: 0

        // ---- Header(§16/§17:Surface 底 + 底边线,标题 + 搜索 + 动作) ----
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

            // ⚠️ 拖动**只挂在下方的"品牌+标题"展示区**,不要挂整个头部:
            // 曾经把整个头部做成拖动区,结果在搜索框里按住拖选文字、指针一旦移出
            // 搜索框,DragHandler 就被激活 → 窗口跟着跑,体验很差。
            // 挂在"不参与任何交互的展示区"上,就与搜索框/按钮各不相干。
            // 这里是窗口移动的**唯一**入口(原先底部状态栏的把手已移除)。

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingMd
                spacing: Theme.metrics.spacingSm

                // 品牌标记 + 标题:纯展示,不参与任何鼠标交互 ——
                // 正因为"不参与交互",它才是头部唯一安全的窗口拖动把手。
                RowLayout {
                    id: headerDragArea
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillHeight: true   // 命中区撑满头部高度,好按
                    spacing: Theme.metrics.spacingSm

                    AppMark {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 20
                        implicitHeight: 20
                    }
                    Label {
                        text: "Nextboard"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontPageTitle
                        font.weight: Font.DemiBold
                        color: Theme.colors.textPrimary
                        Layout.alignment: Qt.AlignVCenter
                    }

                    HoverHandler { cursorShape: Qt.SizeAllCursor }
                    DragHandler {
                        // ⚠️ `target: null` 不能省:PointerHandler 的 target **默认是
                        // 声明它的父项**,否则拖动会去改这个 RowLayout 的 x/y(布局下次
                        // 重排才弹回,拖动过程中标题就被挪走了)。置 null 即只留 active 信号。
                        target: null
                        onActiveChanged: if (active) panelWindow.startSystemMove()
                    }
                }

                // 搜索框(§27:34px,聚焦 Accent 边框;Focus 动画不改变布局)
                Rectangle {
                    id: searchBox
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.metrics.searchHeight
                    radius: Theme.metrics.radiusInput
                    color: Theme.colors.surfaceSecondary
                    border.width: Theme.metrics.borderWidth
                    border.color: searchInput.activeFocus ? Theme.colors.accent
                                : searchBoxMa.containsMouse ? Theme.colors.borderStrong
                                : Theme.colors.border
                    Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }

                    // 整框 hover 态与"点空白处聚焦"(放在 RowLayout 之后声明以覆盖空白区,
                    // 但 searchInput 与清空按钮在其上层,输入与清除不受影响)
                    MouseArea {
                        id: searchBoxMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.IBeamCursor
                        onClicked: searchInput.forceActiveFocus()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.metrics.spacingSm
                        anchors.rightMargin: 6
                        spacing: 6

                        // 放大镜图标(Outline:圆环 + 手柄,§38)
                        Item {
                            Layout.preferredWidth: 14
                            Layout.preferredHeight: 14
                            Layout.alignment: Qt.AlignVCenter
                            Rectangle {
                                anchors.centerIn: parent
                                anchors.verticalCenterOffset: -1
                                width: 9; height: 9; radius: 5
                                color: "transparent"
                                border.color: searchInput.activeFocus ? Theme.colors.accent
                                                                      : Theme.colors.textTertiary
                                border.width: 1.5
                                Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
                            }
                            Rectangle {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                width: 5; height: 1.5; radius: 1
                                rotation: 45
                                color: searchInput.activeFocus ? Theme.colors.accent
                                                               : Theme.colors.textTertiary
                                Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                            }
                        }

                        TextInput {
                            id: searchInput
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            color: Theme.colors.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.metrics.fontBody
                            clip: true
                            verticalAlignment: TextInput.AlignVCenter
                            selectByMouse: true
                            selectionColor: Theme.colors.accent
                            selectedTextColor: Theme.colors.accentText
                            Text {
                                visible: searchInput.text === "" && !searchInput.activeFocus
                                anchors.verticalCenter: parent.verticalCenter
                                text: "搜索历史(文本 / OCR / 文件名)…"
                                color: Theme.colors.textTertiary
                                font: searchInput.font
                            }
                            onTextEdited: Entries.setQuery(text)
                        }

                        Text {
                            visible: searchInput.text !== ""
                            text: "✕"
                            color: Theme.colors.textTertiary
                            font.pixelSize: Theme.metrics.fontSecondary
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: { searchInput.clear(); Entries.setQuery(""); Entries.refreshNow() }
                            }
                        }
                    }
                }

                // 待办:新建一条待办条目(docs/05-todo.md §6.1;位置=翻译左侧)。
                // 有未完成待办时转 primary(面板内"有提醒"的常驻线索,§6.3)。
                ThemedButton {
                    variant: Core.counts.todoOpen > 0 ? "primary" : "ghost"
                    iconName: "todo"
                    text: "待办"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: root.openTodoComposer(null)
                }
                ThemedButton {
                    variant: "ghost"
                    iconName: "translate"
                    text: "翻译"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: Core.openTranslate()
                }
                ThemedButton {
                    variant: "ghost"
                    iconName: "settings"
                    text: "设置"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: settingsDrawerLoader.openAndLoad()
                }
                // 固定:开启后点其它地方不会自动隐藏(选中态用 accent 底反馈)
                ThemedButton {
                    variant: panelWindow.pinned ? "primary" : "ghost"
                    iconName: "pin"
                    text: "固定"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: panelWindow.setPinned(!panelWindow.pinned)
                }
                ThemedButton {
                    variant: "ghost"
                    square: true
                    text: "✕"
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: panelWindow.hidePanel()
                }
            }
        }

        // ---- 分类标签(§15 选中态:AccentSoft 底 + Accent 字 + 左侧指示条) ----
        Row {
            id: tabs
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            Layout.topMargin: Theme.metrics.spacingSm
            Layout.bottomMargin: Theme.metrics.spacingXs + 2
            spacing: Theme.metrics.spacingXs

            property string current: "all"

            Repeater {
                model: ListModel {
                    ListElement { label: "全部"; viewId: "all" }
                    ListElement { label: "文本"; viewId: "text" }
                    ListElement { label: "图片"; viewId: "image" }
                    ListElement { label: "文件"; viewId: "file" }
                    ListElement { label: "待办"; viewId: "todo" }
                    ListElement { label: "固定"; viewId: "pinned" }
                }
                delegate: Rectangle {
                    id: tabDelegate
                    required property string label
                    required property string viewId
                    // 计数来自 Core.counts(单一数据源 + countsChanged 通知)。
                    // 待办页显示**未完成数**(todoOpen),归档不占导航计数(§6.3)。
                    readonly property var count: viewId === "todo"
                        ? Core.counts.todoOpen : Core.counts[viewId]
                    readonly property bool selected: tabs.current === tabDelegate.viewId
                    /// 图标种类与 viewId 同源;"pinned" 复用图钉图标(与头部「固定」按钮同符号)。
                    readonly property string iconKind:
                        tabDelegate.viewId === "pinned" ? "pin" : tabDelegate.viewId
                    radius: Theme.metrics.radiusInput
                    height: Theme.metrics.navItemHeight
                    // 图标 + 间距 + 文字(与 ThemedButton 的 hasIcon 同口径)
                    // + 左右内边距 + 左侧指示条占位。总内边距刻意与改造前一致(22px),
                    // 免得 5 个标签页一起变宽把头部挤变形(Main.qml 的 PANEL_MIN_W 有下限)。
                    width: tabInner.implicitWidth + 6 + 2 * Theme.metrics.spacingSm
                    color: selected ? Theme.colors.accentSoft
                         : tabMa.containsMouse ? Theme.colors.surfaceHover
                         : "transparent"
                    border.width: Theme.metrics.borderWidth
                    border.color: selected ? Theme.colors.accent : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

                    // 左侧 Accent 指示条(§15:2~3px)
                    Rectangle {
                        visible: tabDelegate.selected
                        anchors.left: parent.left
                        anchors.leftMargin: 3
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: parent.height - 12
                        radius: 1
                        color: Theme.colors.accent
                    }

                    Row {
                        id: tabInner
                        anchors.centerIn: parent
                        spacing: 6
                        HeaderIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            kind: tabDelegate.iconKind
                            // 图标跟随文字色:选中态 textPrimary,未选中 textSecondary
                            color: tabDelegate.selected ? Theme.colors.textPrimary
                                                      : Theme.colors.textSecondary
                            size: 14
                        }
                        Text {
                            id: tabLabel
                            anchors.verticalCenter: parent.verticalCenter
                            text: tabDelegate.count !== undefined
                                  ? tabDelegate.label + " " + tabDelegate.count
                                  : tabDelegate.label
                            color: tabDelegate.selected ? Theme.colors.textPrimary
                                 : Theme.colors.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.metrics.fontSecondary
                        }
                    }
                    MouseArea {
                        id: tabMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            tabs.current = tabDelegate.viewId
                            // 切分类时清掉来源筛选:来源与分类是**两个正交维度**,
                            // 叠加起来(如"图片"页 + "截图"来源)容易得到空列表,
                            // 让用户以为是数据丢了。标签筛选保留(细分维度)。
                            if (root.activeSource !== "") {
                                root.activeSource = ""
                                Entries.setSource("")
                            }
                            Entries.setView(tabDelegate.viewId)
                        }
                    }
                }
            }
        }

        // ---- 来源筛选条(S5.1):只看截图/翻译结果/识别文字 ----
        // 与标签筛选同一交互语言(再点一次取消),但**不是动态列表**:
        // 来源是固定枚举,条数由数据决定,只在有对应条目时出现。
        // 位置在标签条之上:来源是更粗的分类,标签是更细的维度。
        Flickable {
            id: sourceBar
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            implicitHeight: visible ? 24 : 0
            // 至少有一个非 clipboard 来源时才显示(纯剪贴板用户不占这一行)
            visible: sourceModel.length > 0
            contentWidth: sourceRow.implicitWidth
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            /// 来源选项:值 + 中文文案 + 图标 + 计数,由 Entries.sourceStats 映射而来。
            /// ⚠️ 计数来自 core 的 list_sources(SQL 精确 COUNT),
            ///    不写死"5 种来源"——没有数据的来源不出现,不占筛选条。
            ///    文案/图标与卡片来源徽标同词汇(EntryCard 的来源徽标)。
            property var sourceModel: {
                const out = []
                // ⚠️ 待办视图不显示来源条:待办来源恒为 'todo',而"待办"本身
                //    已是顶层导航标签页 —— 在这里再列一个「待办」chip 是冗余
                //    (点它也不生效:模型对 todo 视图不下发 source 过滤)。
                if (tabs.current === "todo") return out
                const stats = Entries.sourceStats
                for (let i = 0; i < stats.length; ++i) {
                    const s = stats[i].source
                    // 剪贴板是默认来源、待办是顶层标签,都不需要来源筛选项
                    // (用户关心的是"我截的/我翻的")
                    if (s === "clipboard" || s === "todo") continue
                    out.push({
                        value: s,
                        label: s === "capture" ? "截图"
                             : s === "translate" ? "翻译"
                             : s === "ocr" ? "识别" : s,
                        icon: s === "capture" ? "image"
                            : s === "translate" ? "translate"
                            : s === "ocr" ? "text" : "",
                        count: stats[i].count
                    })
                }
                return out
            }

            Row {
                id: sourceRow
                spacing: 6
                Repeater {
                    model: sourceBar.sourceModel
                    delegate: Rectangle {
                        id: sourceChip
                        required property var modelData
                        readonly property bool active: root.activeSource === modelData.value
                        height: 22
                        width: sourceChipText.implicitWidth + 20
                        radius: Theme.metrics.radiusBadge
                        color: sourceChipMa.containsMouse ? Theme.colors.surfaceHover
                             : Theme.colors.surfaceSecondary
                        border.width: Theme.metrics.borderWidth
                        border.color: sourceChip.active ? Theme.colors.accent
                                                        : Theme.colors.border
                        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
                        Row {
                            id: sourceChipText
                            anchors.centerIn: parent
                            spacing: 5
                            // 图标与卡片上的来源徽标同词汇(HeaderIcon 的 kind)
                            HeaderIcon {
                                anchors.verticalCenter: parent.verticalCenter
                                kind: sourceChip.modelData.icon
                                color: sourceChip.active ? Theme.colors.accent
                                                         : Theme.colors.textSecondary
                                size: Theme.metrics.fontMicro
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: sourceChip.modelData.label + " " + sourceChip.modelData.count
                                font.pixelSize: Theme.metrics.fontCaption
                                color: sourceChip.active ? Theme.colors.accent
                                                         : Theme.colors.textSecondary
                            }
                        }
                        MouseArea {
                            id: sourceChipMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.toggleSourceFilter(sourceChip.modelData.value)
                        }
                    }
                }
            }
        }

        // ---- 标签筛选条(S5.2):有标签时才出现;再点一次取消筛选 ----
        Flickable {
            id: tagBar
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            implicitHeight: visible ? 24 : 0
            visible: Entries.tagStats.length > 0
            contentWidth: tagRow.implicitWidth
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: tagRow
                spacing: 6
                Repeater {
                    model: Entries.tagStats
                    delegate: Rectangle {
                        id: tagChip
                        required property var modelData
                        readonly property bool active: root.activeTag === modelData.tag
                        /// 标签自己的派生色:**与卡片上的同名标签同色**(同一纯函数、
                        /// 无状态,筛选条与条目两端不可能取到不同值)。
                        readonly property color tagFg: Theme.tagColor(modelData.tag)
                        height: 22
                        width: tagChipText.implicitWidth + 16
                        radius: Theme.metrics.radiusBadge
                        // 活跃态:文字用标签自己的色,底色**保持中性**(surfaceSecondary,
                        // 与非活跃同底),靠"同色边框 + 色点实心"表达选中。
                        //
                        // ⚠️ 为什么不用"标签色半透明叠底"(更直觉的做法):浅色调色板是按
                        // "在 surfaceSecondary 上 ≥ 4.7:1"定出来的,余量只有 0.2 —— 压一层
                        // 同色相叠底会立刻跌破 4.5:1(实测最差 4.02)。要两者兼得只能把浅色
                        // 调色板整体压暗,那又会让标签在卡片上显得沉重。故活跃态改由
                        // 边框/色点承担(实测边框与底的 ΔE ≥ 58,足够醒目)。
                        color: tagChipMa.containsMouse ? Theme.colors.surfaceHover
                             : Theme.colors.surfaceSecondary
                        border.width: Theme.metrics.borderWidth
                        border.color: tagChip.active ? tagChip.tagFg : Theme.colors.border
                        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
                        Row {
                            id: tagChipText
                            anchors.centerIn: parent
                            spacing: 4
                            // 色点:标签色在筛选条上的"锚",不依赖文字颜色即可对号入座。
                            // 活跃态实心、非活跃态空心 —— 这样"选中"在灰度屏/色弱下也读得出
                            // (只靠颜色深浅区分对色觉障碍不友好)。
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 6; height: 6; radius: 3
                                color: tagChip.active ? tagChip.tagFg : "transparent"
                                border.width: tagChip.active ? 0 : 1
                                border.color: tagChip.tagFg
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: tagChip.modelData.tag + " " + tagChip.modelData.count
                                font.pixelSize: Theme.metrics.fontCaption
                                // 活跃态用标签色,非活跃态中性灰:
                                // 全部着色会盖住分类标签页的层级(§47 配色比例)
                                color: tagChip.active ? tagChip.tagFg : Theme.colors.textSecondary
                            }
                        }
                        MouseArea {
                            id: tagChipMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function(mouse) {
                                // 左键 = 筛选/取消;右键 = 重命名该标签(管理入口)
                                // 放在筛选项上:标签的"全集"就在这条筛选条上,
                                // 不必先进某条目的标签编辑器。
                                if (mouse.button === Qt.RightButton)
                                    root.openTagRename(tagChip.modelData.tag)
                                else {
                                    root.activeTag = tagChip.active ? "" : tagChip.modelData.tag
                                    Entries.setTag(root.activeTag)
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- 列表(§19 新条目淡入,§43 ListView 回收) ----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            clip: true
            spacing: 6
            model: Entries

            // ---- 内存收口:委托回收复用 + 限制预建缓冲 ----
            // ① reuseItems:委托滚出视野后进复用池,而不是销毁重建。原先每次滚动都
            //    重新实例化 EntryCard(含 Loader/CardAction 等子对象)并重解码图片,
            //    是滚动期内存/CPU 反复抖动的来源。复用后这层开销只在首屏付一次。
            //    代价:委托内部状态会跨行保留 —— 已在 EntryCard 的 ListView.onReused
            //    里复位 textExpanded 与双击计时器(见该文件)。
            // ② cacheBuffer:显式压到 240px。默认值是"平台相关且通常大于零",会预建
            //    较多视野外委托(图片条目下每张都带解码图,内存代价高)。240px 约等于
            //    上下各一块卡片高度,足够平滑预取,又不去缓存整屏之外的委托。
            reuseItems: true
            cacheBuffer: 240

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle {
                    implicitWidth: 6
                    radius: 3
                    color: Theme.colors.textTertiary
                    opacity: 0.4
                }
            }

            // 新条目加入(§19:Opacity 0→1 + Y -6→0,OutCubic,不用弹跳)
            add: Transition {
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animation.normal }
                    NumberAnimation { property: "y"; from: -6; to: 0; duration: Theme.animation.normal; easing.type: Easing.OutCubic }
                }
            }

            // ---- 待办视图智能分组(docs/05-todo.md §6.3) ----
            // 只在"待办"视图插组标题;其它视图保持原扁平列表。
            // 分组边界由模型排序保证(core §5:逾期>今日>未来>无期限,归档垫底),
            // 这里只在"当前行与上一行不同组"时插标题 —— 不额外算分组、不额外取数。
            delegate: Item {
                id: rowWrap
                required property int index
                // 角色透传:委托根不再是 EntryCard,角色注入改由本层显式转发。
                // ⚠️ 逐个列出而不是只在 EntryCard 上写同名绑定 —— 委托根的
                //    required property 才是 ListView 注入角色的落点。
                required property string entryId
                required property string kind
                required property string text
                required property string preview
                required property string imageUrl
                required property string timeText
                required property string sizeText
                required property bool pinned
                required property string source
                required property var tags
                required property real todoDueAt
                required property real todoRemindAt
                required property bool todoDone
                required property bool todoOverdue

                width: list.width
                implicitHeight: groupLabel.visible
                                ? groupLabel.implicitHeight + 6 + card.implicitHeight
                                : card.implicitHeight

                /// 当前行所属分组键;仅待办视图有意义(其它视图返回空 = 不分组)。
                /// ⚠️ 判定放 C++(Entries.rowGroupKey):按行号**现算**,
                ///    且与 core §5 的 ORDER BY 同源;QML 侧若再算一遍,
                ///    既与委托复用状态纠缠,两份口径也迟早漂移。
                readonly property string groupKey: Entries.rowGroupKey(index)
                /// 是否显示组标题:与上一行不同组即插一个。
                readonly property bool needsHeader: {
                    if (groupKey === "") return false
                    if (index === 0) return true
                    return Entries.rowGroupKey(index - 1) !== groupKey
                }
                readonly property string groupLabelText: {
                    switch (groupKey) {
                    case "overdue": return "已逾期"
                    case "today": return "今天"
                    case "future": return "以后"
                    case "none": return "无期限"
                    case "done": return "已完成"
                    default: return ""
                    }
                }

                Text {
                    id: groupLabel
                    visible: rowWrap.needsHeader
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.leftMargin: 2
                    text: rowWrap.groupLabelText
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    font.weight: Font.DemiBold
                    // 逾期组用 error 色,与卡片上的逾期红字同一语义
                    color: rowWrap.groupKey === "overdue" ? Theme.colors.error
                                                          : Theme.colors.textSecondary
                }

                EntryCard {
                    id: card
                    anchors.top: groupLabel.visible ? groupLabel.bottom : parent.top
                    anchors.topMargin: groupLabel.visible ? 6 : 0
                    width: rowWrap.width
                    entryId: rowWrap.entryId
                    kind: rowWrap.kind
                    text: rowWrap.text
                    preview: rowWrap.preview
                    imageUrl: rowWrap.imageUrl
                    timeText: rowWrap.timeText
                    sizeText: rowWrap.sizeText
                    pinned: rowWrap.pinned
                    source: rowWrap.source
                    tags: rowWrap.tags
                    todoDueAt: rowWrap.todoDueAt
                    todoRemindAt: rowWrap.todoRemindAt
                    todoDone: rowWrap.todoDone
                    todoOverdue: rowWrap.todoOverdue
                    menuOpen: (root.menuTarget !== null
                               && root.menuTarget.entryId === rowWrap.entryId)
                              || root.highlightTodoId === rowWrap.entryId
                    menuOpenElsewhere: root.menuTarget !== null
                    onContextMenuRequested: function(entry) { root.openEntryMenu(entry) }
                    onMenuDismissRequested: root.closeEntryMenu()
                    onTodoEditRequested: function(entry) { root.openTodoComposer(entry) }
                }
            }
        }

        // ---- 空状态(§41:小图标 + 标题 + 说明 + 快捷键帽) ----
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: list.count === 0
            Column {
                anchors.centerIn: parent
                spacing: Theme.metrics.spacingSm

                // 空剪贴板图标:Outline 剪贴板(§41 禁大插画)
                Item {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 36; height: 40
                    Rectangle {
                        radius: 4
                        anchors.fill: parent
                        anchors.topMargin: 6
                        color: "transparent"
                        border.color: Theme.colors.textTertiary
                        border.width: 1.5
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 14; height: 6; radius: 2
                        color: Theme.colors.background
                        border.color: Theme.colors.textTertiary
                        border.width: 1.5
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontSection
                    font.weight: Font.DemiBold
                    color: Theme.colors.textPrimary
                    text: {
                        if (Entries.errorText !== "") return "加载失败"
                        if (searchInput.text !== "") return "没有匹配的历史记录"
                        if (tabs.current === "pinned") return "暂无固定条目"
                        if (tabs.current === "todo") return "还没有待办"
                        return "剪贴板是空的"
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Entries.errorText !== "" ? Theme.colors.error
                         : Theme.colors.textSecondary
                    text: {
                        if (Entries.errorText !== "") return Entries.errorText
                        if (searchInput.text !== "") return "换个关键词试试,或清除搜索条件"
                        if (tabs.current === "pinned") return "右键条目选择「固定」即可常驻这里"
                        if (tabs.current === "todo") return "点右上角「待办」按钮,新建一条"
                        return "复制任意内容,它会自动出现在这里"
                    }
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Theme.metrics.spacingXs
                    visible: Entries.errorText === "" && searchInput.text === ""
                             && tabs.current !== "todo"
                    KeyCap { label: "Ctrl" }
                    KeyCap { label: "Shift" }
                    KeyCap { label: "V" }
                    Text {
                        text: "呼出 / 隐藏面板"
                        color: Theme.colors.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }

        // ---- 状态栏(§37:快捷键可视化) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: Theme.colors.surfaceSecondary
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: Theme.metrics.borderWidth
                color: Theme.colors.border
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingMd
                anchors.rightMargin: Theme.metrics.spacingMd
                spacing: Theme.metrics.spacingXs

                // 窗口拖动已上移到头部「品牌 + 标题」区(见 Header 的 headerDragArea),
                // 状态栏这里不再需要专用把手。

                KeyCap { label: "Ctrl" }
                KeyCap { label: "K" }
                Text {
                    text: "搜索"
                    color: Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                    Layout.rightMargin: Theme.metrics.spacingSm
                }
                KeyCap { label: "Ctrl" }
                KeyCap { label: "1-9" }
                Text {
                    text: "复制条目"
                    color: Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "拖动窗口边缘可调整大小"
                    color: Theme.colors.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                    Layout.rightMargin: Theme.metrics.spacingSm
                }
                Text {
                    text: panelWindow.pinned ? "已固定 · 不自动隐藏"
                          : Entries.loading ? "加载中…" : "Nextboard"
                    color: panelWindow.pinned ? Theme.colors.accent : Theme.colors.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontMicro
                }
            }
        }
    }

    // ---- 页 2:翻译 ----
    // 与列表**同一窗口、同一 QQuickWidget、同一 QQmlEngine** —— 这是本轮内存收口的关键:
    // 原先翻译是独立顶层窗口,自带第二个 QML 引擎与它自己的场景图/渲染栈(见 docs/progress.md)。
    // 延迟构建 + 之后常驻:从不翻译的用户不付这份开销,常用的人切走再回来内容还在。
    Loader {
        id: translatePage
        active: root.translatePageBuilt
        anchors.fill: parent
        visible: panelWindow.mode === "translate"
        sourceComponent: Component { TranslateView { } }
        // 页切换时把焦点交接清楚:进翻译页→页内拿焦点(否则 Ctrl+Enter 不生效);
        // 回列表页→面板根拿焦点(否则 Esc/Ctrl+F 这些根级按键会留在已隐藏的页里)。
        onVisibleChanged: {
            // 进入:把焦点交给本页(否则 Ctrl+Enter 之类不生效)。
            // 离开时的清理由 TranslateView 自己处理(它最清楚自己有哪些浮层)。
            if (visible) { if (item) item.forceActiveFocus() }
            else root.forceActiveFocus()
        }
    }

    // ---- 抽屉(设置仍留在面板内;翻译已并入面板为"整窗模式切换") ----
    // 懒构建:SettingsDrawer 是一个 1200+ 行的重表单(6 组设置 + 多个自绘控件),
    // 原先作为 Main.qml 的直接子项**启动即整棵实例化**,哪怕从不打开 —— 它单独
    // 吃掉数 MB 的常驻(含委托/控件对象)。这里改为首次点「设置」才构建,之后常驻
    // (再次打开省去重建,且保留上次的滚动位置)。
    //
    // item 为空 = 从未打开:调用方据此判空(opened 之类属性经 item 访问)。
    // openAndLoad() 供 Header 的设置按钮调用:先建后开,再拉取设置回填。
    Loader {
        id: settingsDrawerLoader
        active: false
        // ⚠️ 尺寸必须对齐面板根:AppDrawer 用 `parent.width * 0.52` / `parent.height`
        // 给自己定尺寸,而经 Loader 构建后它的 `parent` 就是**本 Loader** ——
        // 不设尺寸会让抽屉拿不到宽高(0×0),打开时不可见。对齐后行为与原先
        // `SettingsDrawer` 直接挂在 root 下完全一致。
        width: root.width
        height: root.height
        sourceComponent: Component { SettingsDrawer { } }
        function openAndLoad() {
            active = true                 // 构建是同步的,置位后 item 立即可用
            item.openAndLoad()
        }
    }

    // ---- Core 信号订阅 ----
    Connections {
        target: Core
        function onToastMessage(message) { toast.showMsg(message) }
    }

    // ---- Toast(§36:右下角通知卡,Surface+Border+柔和层级,非 accent 大色块) ----
    Rectangle {
        id: toast
        z: 1000   // 高于菜单外部点击屏障,通知不会被挡
        function showMsg(message) {
            toastText.text = message
            toast.x = -12   // 进入:X +12 → 0(配合 right anchor 用偏移)
            toast.opacity = 1
            hideTimer.restart()
        }
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 44
        radius: Theme.metrics.radiusPopup
        color: Theme.colors.surface
        border.color: Theme.colors.border
        border.width: Theme.metrics.borderWidth
        width: Math.max(toastText.implicitWidth + Theme.metrics.spacingLg + 16, 120)
        height: 32
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: Theme.animation.fast } }
        Behavior on x { NumberAnimation { duration: Theme.animation.normal; easing.type: Easing.OutCubic } }
        Text {
            id: toastText
            anchors.centerIn: parent
            color: Theme.colors.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.metrics.fontCaption
        }
        Timer {
            id: hideTimer
            interval: 1600
            onTriggered: toast.opacity = 0
        }
    }

    // ---- 右键菜单(面板级单例;内容按目标条目类型在弹出前重建) ----
    // 动态构建而非 visible 开关:Menu 底层是 ListView,visible:false 的委托仍占高度,
    // 会在菜单里留出空白间隙。
    ThemedMenu {
        id: entryMenu

        // 外部点击**只由屏障处理**,这里必须关掉 Menu 默认的 CloseOnPressOutside:
        // 否则按下时 Qt 会先关菜单 → onClosed 把 menuTarget 置空 → 屏障立刻隐藏,
        // 紧接着这次点击继续派发给下层卡片,于是"关菜单"的同时又触发了一次复制。
        closePolicy: Popup.CloseOnEscape

        Component { id: menuItemComp; ThemedMenuItem {} }
        Component { id: menuSepComp; ThemedMenuSeparator {} }

        /// 菜单关闭即清空目标:屏障随之撤掉(点菜单项、点外部、Esc 都走这里)
        onClosed: root.menuTarget = null

        function rebuild() {
            while (count > 0) takeItem(0).destroy()
            const entry = root.menuTarget
            if (!entry) return

            function addAction(label, fn, danger) {
                const it = menuItemComp.createObject(entryMenu, { text: label, danger: danger === true })
                it.triggered.connect(fn)
                addItem(it)
            }
            function addSep() { addItem(menuSepComp.createObject(entryMenu)) }

            // 组 0:待办专属(放最前 —— 完成是待办的高频操作)
            if (entry.kind === "todo") {
                addAction(entry.todoDone ? "取消完成" : "完成",
                          function() { Entries.todoSetDone(entry.entryId, !entry.todoDone) })
                addAction("推迟 1 小时", function() { root.postponeTodo(entry.entryId, 3600 * 1000) })
                addAction("推迟到明天", function() { root.postponeTodo(entry.entryId, 24 * 3600 * 1000) })
                addAction("编辑待办…", function() { root.openTodoComposer(entry) })
                addSep()
            }

            // 组 1:类型专属操作
            if (entry.kind === "file") {
                addAction("打开", function() { Core.openEntry(entry.entryId) })
                addAction("定位文件", function() { Core.revealEntry(entry.entryId) })
            } else if (entry.kind === "image") {
                addAction("编辑", function() { root.editEntryAfterHide(entry.entryId) })
                addAction("识别文字", function() { Core.ocrEntry(entry.entryId) })
                addAction("翻译", function() { root.translateEntry(entry) })
            } else if (entry.kind !== "todo") {
                addAction("翻译", function() { root.translateEntry(entry) })
            }
            addSep()

            // 组 2:通用操作(复制 / 贴图)
            addAction("复制", function() { root.copyEntryAndHide(entry.entryId) })
            if (entry.kind === "file")
                addAction("复制路径", function() { root.copyPathAndHide(entry.text) })
            else
                addAction("贴到屏幕", function() { Core.pinEntry(entry.entryId) })
            addSep()

            // 组 3:标签(S5.2)
            addAction("标签…", function() { root.openTagEditor(entry) })
            // 有标签时给出**一键清除**入口(不必进编辑器手动删空)
            if (entry.tags) addAction("清除标签", function() { Entries.saveTags(entry.entryId, []) })
            addSep()

            // 组 4:固定;组 5:删除(危险项置底)
            addAction(entry.pinned ? "取消固定" : "固定",
                      function() { Entries.togglePin(entry.entryId, !entry.pinned) })
            addSep()
            addAction("删除", function() { root.deleteEntryWithToast(entry.entryId) }, true)
        }

        // 每次弹出前重建,保证「固定/取消固定」文案与当前状态一致
        onAboutToShow: rebuild()
    }

    // 面板收起时收掉菜单:否则下次呼出会残留菜单与点击屏障
    Connections {
        target: panelWindow
        function onPanelHidden() { root.closeEntryMenu() }
    }

    // ---- 菜单外部点击屏障 ----
    // 菜单打开时铺满面板,**吃掉**这一次点击:否则左键会穿透到下层卡片触发
    // "复制并收起面板",与菜单操作互相干扰。z 高于所有内容、低于 Toast。
    // (菜单本身在 Popup 层,位于内容之上,不受本屏障影响。)
    MouseArea {
        id: menuDismissBarrier
        anchors.fill: parent
        z: 900
        visible: root.menuTarget !== null
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: root.closeEntryMenu()
    }

    // ---- 标签编辑卡片(S5.2) ----
    // 与右键菜单同样的"外部点击关闭"思路:先铺屏障,再放卡片(z 介于菜单与 Toast 之间)。
    MouseArea {
        id: tagEditorBarrier
        anchors.fill: parent
        z: 950
        visible: tagEditorCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: tagEditorCard.visible = false
    }

    Rectangle {
        id: tagEditorCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(320, root.width - 48)
        // 只锚上/左右:高度由内容隐式决定(anchors.fill 会与 implicitHeight 互相依赖)
        height: tagEditorLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: tagEditorLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: "编辑标签"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
            }
            TextField {
                id: tagInput
                Layout.fillWidth: true
                placeholderText: "用逗号分隔,例如:工作, 重要;留空保存即清除全部标签"
                placeholderTextColor: Theme.colors.textTertiary
                color: Theme.colors.textPrimary
                selectionColor: Theme.colors.accent
                selectedTextColor: Theme.colors.accentText
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: tagInput.activeFocus ? Theme.colors.accent : Theme.colors.border
                }
                Keys.onReturnPressed: root.commitTagEditor()
                Keys.onEnterPressed: root.commitTagEditor()
                Keys.onEscapePressed: tagEditorCard.visible = false
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"
                    size: "sm"
                    text: "取消"
                    onClicked: tagEditorCard.visible = false
                }
                ThemedButton {
                    variant: "primary"
                    size: "sm"
                    text: "保存"
                    onClicked: root.commitTagEditor()
                }
            }
        }
    }

    // ---- 标签重命名卡片(S5.2 标签管理) ----
    // 与标签编辑卡片同一"屏障 + 居中卡片"模式;入口是右键标签筛选条的 chip。
    MouseArea {
        id: tagRenameBarrier
        anchors.fill: parent
        z: 950
        visible: tagRenameCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: tagRenameCard.visible = false
    }

    Rectangle {
        id: tagRenameCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(320, root.width - 48)
        height: tagRenameLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: tagRenameLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: "重命名标签「" + root.tagRenameFrom + "」"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                // 说明影响面:这是**跨条目**操作,不是改当前这一条
                text: "所有带此标签的条目会一起改名;留空保存 = 删除该标签"
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            TextField {
                id: tagRenameInput
                Layout.fillWidth: true
                placeholderText: "新标签名(留空 = 删除)"
                placeholderTextColor: Theme.colors.textTertiary
                color: Theme.colors.textPrimary
                selectionColor: Theme.colors.accent
                selectedTextColor: Theme.colors.accentText
                font.pixelSize: Theme.metrics.fontSecondary
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: tagRenameInput.activeFocus ? Theme.colors.accent
                                                             : Theme.colors.border
                }
                Keys.onReturnPressed: root.commitTagRename()
                Keys.onEnterPressed: root.commitTagRename()
                Keys.onEscapePressed: tagRenameCard.visible = false
            }
            Text {
                id: tagRenameError
                Layout.fillWidth: true
                visible: text !== ""
                text: ""
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.error
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"
                    size: "sm"
                    text: "取消"
                    onClicked: tagRenameCard.visible = false
                }
                ThemedButton {
                    objectName: "tagRenameSave"
                    variant: "primary"
                    size: "sm"
                    text: "保存"
                    onClicked: root.commitTagRename()
                }
            }
        }
    }

    // ---- 待办新建/编辑卡片(docs/05-todo.md §6.2) ----
    // 复用 tagEditorCard 的"屏障 + 居中卡片"模式;宽一点(380)给时间字段留位置。
    MouseArea {
        id: todoComposerBarrier
        anchors.fill: parent
        z: 950
        visible: todoComposerCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        // 两段式:浮层开着时这一次点击只用于关浮层,卡片保留 ——
        // 否则"点空白处想收轮子"会连卡片一起收掉,刚填的正文全丢(首版实测)。
        onClicked: {
            if (timePickerPopup.visible) root.closeTimePicker()
            else root.closeTodoComposer()
        }
    }

    Rectangle {
        id: todoComposerCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(380, root.width - 48)
        height: todoComposerLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: todoComposerLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: root.todoEditId === "" ? "新建待办" : "编辑待办"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
            }

            // 正文:多行;Enter 提交、Shift+Enter 换行(§6.2)。
            // ⚠️ TextEdit 是基础项,没有 `background` 属性(那是 Control 的):
            //    直接写会让整个 Main.qml 加载失败。用 Rectangle 包一层做底板。
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(todoInput.implicitHeight + 16, 44)
                color: Theme.colors.surfaceSecondary
                radius: Theme.metrics.radiusInput
                border.width: Theme.metrics.borderWidth
                border.color: todoInput.activeFocus ? Theme.colors.accent : Theme.colors.border
                TextEdit {
                    id: todoInput
                    anchors.fill: parent
                    anchors.margins: 8
                    wrapMode: TextEdit.Wrap
                    color: Theme.colors.textPrimary
                    selectionColor: Theme.colors.accent
                    selectedTextColor: Theme.colors.accentText
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontBody
                    Text {
                        visible: todoInput.text === "" && !todoInput.activeFocus
                        anchors.top: parent.top
                        anchors.left: parent.left
                        text: "要做什么?"
                        color: Theme.colors.textTertiary
                        font: todoInput.font
                    }
                    Keys.onReturnPressed: function(event) {
                        if (event.modifiers & Qt.ShiftModifier) {
                            todoInput.insert(todoInput.cursorPosition, "\n")
                            return
                        }
                        root.commitTodoComposer()
                    }
                    Keys.onEnterPressed: function(event) {
                        if (event.modifiers & Qt.ShiftModifier) {
                            todoInput.insert(todoInput.cursorPosition, "\n")
                            return
                        }
                        root.commitTodoComposer()
                    }
                    Keys.onEscapePressed: root.closeTodoComposer()
                }
            }

            // ---- 时间字段(截止 / 提醒) ----
            // 点字段区域打开时间选择器浮层(滚轮 + 快捷钮),不手打日期。
            //
            // ⚠️ 热区声明顺序即命中优先级(QML 同父级重叠项**后声明者优先**):
            //    "打开浮层"的热区必须排在 RowLayout **之前** —— 首版排在后面,
            //    铺满整行盖住了 ✕ 清空钮,于是"点 ✕ 变成重新打开浮层";
            //    用户以为时刻设好了,提交时字段其实仍是 0 或被清掉,
            //    表现就是"设了时间却保存不上"。
            component TodoTimeField: Rectangle {
                id: timeField
                property string pickerKind: ""    // due / remind
                property real ms: 0               // 回显值;0 = 未设置
                function apply(v) {
                    // ⚠️ 只写本字段,不做任何联动 —— 截止→提醒的默认联动
                    //    已收敛到 confirmDuePicker(用户点「确定」时算一次)。
                    //    首版在这里调 pickerChanged:预填滚轮归位也会触发,
                    //    把提醒锁死在"打开时刻-10分钟"且不再跟随截止变化。
                    ms = v
                }
                Layout.fillWidth: true
                implicitHeight: 30
                radius: Theme.metrics.radiusInput
                color: root.openPickerKind === pickerKind ? Theme.colors.surfaceHover
                     : Theme.colors.surfaceSecondary
                border.width: Theme.metrics.borderWidth
                border.color: root.openPickerKind === pickerKind ? Theme.colors.accent
                                                                 : Theme.colors.border
                Behavior on color { ColorAnimation { duration: Theme.animation.fast } }

                // 打开浮层(声明在最前 = 命中优先级最低,✕ 才能压在它上面)
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openTimePicker(timeField)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 6
                    Text {
                        Layout.fillWidth: true
                        text: timeField.ms > 0
                              ? Qt.formatDateTime(new Date(timeField.ms), "yyyy-MM-dd HH:mm")
                              : "未设置"
                        font.pixelSize: Theme.metrics.fontSecondary
                        color: timeField.ms > 0 ? Theme.colors.textPrimary
                                                : Theme.colors.textTertiary
                        elide: Text.ElideRight
                    }
                    // 清空钮:有值才显示。命中区略大于字形便于点中。
                    Text {
                        visible: timeField.ms > 0
                        text: "✕"
                        font.pixelSize: Theme.metrics.fontCaption
                        color: clearArea.containsMouse ? Theme.colors.error
                                                       : Theme.colors.textTertiary
                        MouseArea {
                            id: clearArea
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // 只清本字段,不打开浮层(声明在打开热区之后 → 优先命中)
                            // ⚠️ 清**提醒**字段是用户的明确选择 → 标记"手动",
                            //    此后改截止不再自动补提醒。
                            onClicked: {
                                timeField.apply(0)
                                if (timeField.pickerKind === "remind")
                                    root.remindManuallySet = true
                            }
                        }
                    }
                }
            }

            Text {
                text: "截止(可空)"
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            TodoTimeField { id: todoDueField; pickerKind: "due" }

            Text {
                text: "提醒(可空;到点弹气泡并高亮)"
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            TodoTimeField { id: todoRemindField; pickerKind: "remind" }

            Text {
                text: "标签(逗号分隔,可空)"
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            TextField {
                id: todoTagInput
                Layout.fillWidth: true
                placeholderText: "工作, 重要"
                placeholderTextColor: Theme.colors.textTertiary
                color: Theme.colors.textPrimary
                selectionColor: Theme.colors.accent
                selectedTextColor: Theme.colors.accentText
                font.pixelSize: Theme.metrics.fontSecondary
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: todoTagInput.activeFocus ? Theme.colors.accent
                                                           : Theme.colors.border
                }
                Keys.onReturnPressed: root.commitTodoComposer()
                Keys.onEnterPressed: root.commitTodoComposer()
                Keys.onEscapePressed: root.closeTodoComposer()
            }

            // 错误行:core 的原文直接显示(不弹 Toast,免得一闪而过看不见)
            Text {
                id: todoError
                Layout.fillWidth: true
                visible: text !== ""
                text: ""
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.error
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"
                    size: "sm"
                    text: "取消"
                    onClicked: root.closeTodoComposer()
                }
                ThemedButton {
                    objectName: "todoComposerSave"   // 验收:定位按钮
                    variant: "primary"
                    size: "sm"
                    text: "保存"
                    onClicked: root.commitTodoComposer()
                }
            }
        }
    }

    // ---- 时间选择器浮层(单实例,两个时间字段共用) ----
    // ⚠️ 关闭策略刻意**不含** CloseOnPressOutside(见 openTimePicker 注释):
    //    它会把"点保存"那一下吃掉用于关浮层,点击不穿透到按钮,用户就会感觉
    //    "必须点别处才能关掉轮子,否则保存没反应"。外部点击由屏障统一处理:
    //    屏障的 onClicked 先 closeTimePicker(),浮层开着时卡片不会被误关。
    // 高度 = 选择器内容 + 底部按钮行;按下「确定」才把值写回字段。
    Popup {
        id: timePickerPopup
        parent: Overlay.overlay
        width: 320
        // padding 由 contentItem 自己贡献,这里只留 border 的呼吸位
        padding: 0
        modal: false
        closePolicy: Popup.CloseOnEscape
        // 目标字段("due"/"remind");"" = 关闭
        property string targetKind: ""
        property Item targetField: null

        contentItem: Item {
            objectName: "todoPickerContent"   // 验收:定位浮层内容几何
            implicitWidth: 320
            implicitHeight: pickerColumn.implicitHeight

            ColumnLayout {
                id: pickerColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: 0

                TodoDatePicker {
                    id: pickerBody
                    Layout.fillWidth: true
                    // ⚠️ 滚轮/快捷钮改动**只更新本组件内部状态与回显**,
                    //    **不**实时写回字段 —— 写回统一在「确定」(见 onAccepted),
                    //    "拨到一半点外部/取消"不会把半成品时刻写进待办。
                    // ⚠️ 也**不要**在这里做截止→提醒的联动:预填滚轮归位会触发
                    //    changed,把提醒写成"打开时刻-10分钟"且从此不再跟随
                    //    用户后续的截止修改(实测反馈的第二个问题)。联动在
                    //    confirmDuePicker 里做,只在用户点「确定」时发生一次。
                    // 「确定」= 把滚轮当前值写回目标字段(按字段分派)并关闭浮层;
                    // 「取消」= 恢复打开前的值并关闭浮层。
                    onAccepted: function(ms) {
                        if (timePickerPopup.targetKind === "due")
                            root.confirmDuePicker(ms)
                        else if (timePickerPopup.targetKind === "remind")
                            root.confirmRemindPicker(ms)
                        root.closeTimePicker()
                    }
                    onCancelled: function() {
                        if (timePickerPopup.targetKind === "due")
                            root.cancelDuePicker()
                        root.closeTimePicker()
                    }
                }
            }
        }
    }
}
