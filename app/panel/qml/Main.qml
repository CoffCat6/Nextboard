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

    /// 验收:打开「新建待办」卡片(等价点头部按钮)。只做这一件事。
    function acceptanceOpenTodoComposer() {
        root.openTodoComposer(null)
    }

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

    /// 验收:打开筛选浮层(读回其内容与几何)。
    function acceptanceOpenFilter() {
        openFilterPopup()
    }

    /// 验收:按值切换来源筛选(等价点浮层里的来源项)。
    function acceptanceToggleSource(value) {
        toggleSourceFilter(value)
    }

    /// 验收:设「截图」来源筛选(有 capture 条目时)并打开浮层。
    function acceptanceSetSource() {
        openFilterPopup()
        if (Entries.sourceStats.length > 0) toggleSourceFilter("capture")
    }

    /// 验收:清掉来源筛选。
    function acceptanceClearSource() {
        if (activeSource !== "") {
            activeSource = ""
            Entries.setSource("")
        }
    }

    /// 验收:把设置抽屉的内容滚到底(验证"滚动后 ✕ 仍可点" —— 固定头部)。
    function acceptanceSettingsToBottom() {
        if (settingsDrawerLoader.item) settingsDrawerLoader.item.scrollToBottom()
    }

    /// 验收:把设置抽屉里的某个控件滚入视口(按 objectName 定位)。
    ///
    /// 为什么需要:`scrollToBottom()` 会把**靠上**的控件滚出视口顶部,
    /// 而 QML 的 `isVisible()` 对"被裁到视口外"的项**仍返回 true** ——
    /// 验收点击会落在窗口外坐标上(实测 y=-207)而静默失败。
    function acceptanceSettingsScrollTo(objectName) {
        const drawer = settingsDrawerLoader.item
        if (!drawer) return false
        const target = findChildByName(drawer, objectName)
        return drawer.acceptanceScrollTo(target)
    }

    /// 在 QML 项树里按 objectName 递归查找(QML 无内置 findChild)。
    function findChildByName(node, name) {
        if (!node) return null
        if (node.objectName === name) return node
        // Control / Popup 的内容挂在 contentItem 上(Drawer 是 Popup,
        // 只遍历 children **找不到**其中的控件 —— 实测 target found=false)。
        const inner = node.contentItem
        if (inner) {
            const hitInner = findChildByName(inner, name)
            if (hitInner) return hitInner
        }
        const kids = node.children || []
        for (let i = 0; i < kids.length; i++) {
            const hit = findChildByName(kids[i], name)
            if (hit) return hit
        }
        return null
    }

    /// 验收:由宿主设置要滚入视口的控件 objectName(--settings-flow 用)。
    property string acceptanceScrollTarget: ""

    /// 验收:无参包装 —— 宿主 invokeAcceptance 只能调无参方法。
    function acceptanceSettingsScrollToTarget() {
        return acceptanceSettingsScrollTo(acceptanceScrollTarget)
    }

    /// 验收(--restore-flow):待恢复/导入的备份文件路径。
    /// ⚠️ 产品路径里这个路径来自原生文件对话框 —— 进程内验收无法操作它,
    ///    故由宿主直写属性跳过"选文件"这一步;确认卡 → **真实点击确认按钮**
    ///    → 真实 Core 调用这一段全走真链路。
    property string acceptanceBackupPath: ""
    function acceptanceOpenRestore() {
        if (settingsDrawerLoader.item) {
            settingsDrawerLoader.item.openRestoreConfirm(acceptanceBackupPath)
        }
    }
    function acceptanceOpenImport() {
        if (settingsDrawerLoader.item) {
            settingsDrawerLoader.item.openImportConfirm(acceptanceBackupPath)
        }
    }
    /// 验收(--clear-all-flow):把设置抽屉的「清空全部数据」按钮滚进可视区
    /// (危险区在表单尾部,不滚过去点击会落空)。
    function acceptanceRevealClearAll() {
        if (settingsDrawerLoader.item) settingsDrawerLoader.item.revealClearAll()
    }
    /// 验收(--update-flow):把设置抽屉的「检查更新」按钮滚进可视区(关于区在最末)。
    function acceptanceRevealUpdate() {
        if (settingsDrawerLoader.item) settingsDrawerLoader.item.revealUpdateCheck()
    }

    /// 读回:是否处于编辑态(供断言"保存成功后卡片已关")。
    readonly property string acceptanceEditId: todoEditId

    /// 验收只读状态:浮层几何 + 字段值 + 卡片可见性 + 错误行 + 按钮几何。
    readonly property var acceptanceState: ({
        cardVisible: todoComposerCard.visible,
        clearDoneVisible: clearDoneCard.visible,   // --clear-done-flow:确认卡片开合
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
        // 筛选状态自检:浮层里的可选项 + 分类行上的生效 chip。
        // ⚠️ 来源/标签条已并入筛选浮层(方案 A),这里跟着改为读浮层的数据源
        //    (filterSourceFlow.model / Entries.tagStats)—— 读已删除的
        //    sourceBar 会让整个 acceptanceState 绑定失效(实测踩过)。
        sourceChips: (function() {
            const out = []
            const model = filterSourceFlow.model
            for (let i = 0; i < model.length; ++i) out.push(model[i].value)
            return out.join(",")
        })(),
        filterActive: root.filterActive,
        filterCount: root.filterCount,
        filterPopupOpen: root.filterPopupOpen,
        activeSource: root.activeSource,
        activeTag: root.activeTag,
        tagChips: (function() {
            const out = []
            for (let i = 0; i < Entries.tagStats.length; ++i)
                out.push(Entries.tagStats[i].tag)
            return out.join(",")
        })(),
        // 设置抽屉(--settings-flow):开合状态 + 内容滚动位置(验证固定头部)。
        settingsOpen: !!(settingsDrawerLoader.item && settingsDrawerLoader.item.opened),
        settingsScrollY: settingsDrawerLoader.item
                         ? settingsDrawerLoader.item.acceptanceScrollY : -1,
        // 恢复/导入确认卡(--restore-flow):点按钮后确认卡应可见。
        restoreCardVisible: settingsDrawerLoader.item
                            ? settingsDrawerLoader.item.acceptanceRestoreCardVisible : false,
        importCardVisible: settingsDrawerLoader.item
                           ? settingsDrawerLoader.item.acceptanceImportCardVisible : false,
        // 更新检查(--update-flow):结果文案 + 交给浏览器的下载链接。
        updateHintText: settingsDrawerLoader.item
                        ? settingsDrawerLoader.item.acceptanceUpdateHint : "",
        updateDownloadUrl: settingsDrawerLoader.item
                           ? settingsDrawerLoader.item.acceptanceUpdateUrl : "",
        updateEnabled: settingsDrawerLoader.item
                       ? settingsDrawerLoader.item.acceptanceUpdateEnabled : false,
        updateNoteVisible: settingsDrawerLoader.item
                           ? settingsDrawerLoader.item.acceptanceUpdateNoteVisible : false,
        // 页签可见性(0 个未完成待办时「待办」页签必须仍在)。
        todoTabVisible: tabRepeater.itemAt(4) ? tabRepeater.itemAt(4).visible : false
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

    /// 筛选是否处于"生效"状态(任一维度被设置)。
    readonly property bool filterActive: activeSource !== "" || activeTag !== ""
    /// 生效的筛选维度个数(「筛选」按钮上的角标)。
    /// ⚠️ 只数**维度**不数条件:目前来源与标签各只能选一个(模型侧就是单值),
    ///    将来若支持多选,这里改成"条件总数"即可,按钮文案与角标口径不变。
    readonly property int filterCount: (activeSource !== "" ? 1 : 0)
                                      + (activeTag !== "" ? 1 : 0)

    /// 清空全部筛选条件(浮层底部「清除筛选」与激活 chip 的 ✕ 共用)。
    function clearFilters() {
        if (activeSource !== "") {
            activeSource = ""
            Entries.setSource("")
        }
        if (activeTag !== "") {
            activeTag = ""
            Entries.setTag("")
        }
    }

    /// 筛选浮层的开关。Esc / 点外部由屏障与 dismissListOverlays 收口。
    property bool filterPopupOpen: false
    function openFilterPopup() {
        filterPopupOpen = true
    }
    function closeFilterPopup() {
        filterPopupOpen = false
    }

    /// 批量删除确认卡片:将删除的条数(确认文案用)。
    property int bulkDeleteCount: 0
    // ---- 条目多选:批量操作 ----
    /// 批量删除确认卡片:打开前把"将删除条数"写进卡片文案(决策点 4:不设
    /// 硬上限,但确认必须显示数量 —— 批量删除不可逆)。
    function openBulkDeleteConfirm() {
        if (selectedIds.length === 0) return
        bulkDeleteCount = selectedIds.length
        bulkDeleteCard.visible = true
    }
    /// 执行批量删除:成功后**保留选择模式**(决策点 1:不自动退出),
    /// 清空选择集 —— 被删的 id 已无意义,残留会让工具条显示虚数。
    function commitBulkDelete() {
        bulkDeleteCard.visible = false
        const r = Entries.deleteBulk(selectedIds)
        if (!r.ok) { Core.showToast(r.message); return }
        const n = r.deleted ? r.deleted.length : 0
        selectedIds = []
        Core.showToast("已删除 " + n + " 条")
    }
    /// 批量加标签:打开输入卡片(复用标签编辑卡片的形态)。
    function openBulkTagEditor() {
        if (selectedIds.length === 0) return
        bulkTagInput.text = ""
        bulkTagCard.visible = true
        bulkTagInput.forceActiveFocus()
    }
    /// 提交批量加标签:解析/去重与单条标签编辑同一套口径。
    function commitBulkTag() {
        const parts = bulkTagInput.text.split(/[,，\\s]+/)
        const tags = []
        for (let i = 0; i < parts.length; ++i) {
            const t = parts[i].trim()
            if (t.length > 0 && tags.indexOf(t) < 0) tags.push(t)
        }
        if (tags.length === 0) { bulkTagCard.visible = false; return }
        bulkTagCard.visible = false
        const r = Entries.addTagsBulk(selectedIds, tags)
        if (!r.ok) { Core.showToast(r.message); return }
        Core.showToast("已为 " + r.changed + " 条添加标签")
    }

    // ---- 清空已完成待办(待办视图「已完成」组头入口) ----
    /// 将清空的条数(确认文案用):打开时从计数快照,避免清空后列表刷新导致数字漂移。
    property int clearDoneCount: 0
    /// 打开确认卡片(入口 = 「已完成」组头右侧文字链)。
    function openClearDoneConfirm() {
        clearDoneCount = Core.counts.todoDone || 0
        clearDoneCard.visible = true
    }
    /// 执行清空:归档条目连同数据一起删除,不可恢复 —— 必须点确认;
    /// 成功 toast 里带上条数快照,失败提示由模型侧 toast() 统一发出。
    function commitClearDone() {
        clearDoneCard.visible = false
        const n = clearDoneCount
        if (Entries.todoClearDone())
            Core.showToast("已清空 " + n + " 条已完成待办")
    }

    // ---- 条目多选(选择模式) ----
    /// 是否处于选择模式。进入/退出见 toggleSelectMode / exitSelectMode。
    property bool selectMode: false
    /// 已选条目 id 集合(字符串数组;选择态归 QML 而非 C++ 模型 ——
    /// 模型 reset 时行集会变,选择集按 id 维护反而稳定)。
    property var selectedIds: []
    /// 范围选择(Shift)的锚点行号;-1 = 尚无锚点。
    property int selectionAnchor: -1

    /// 进入/退出选择模式。退出时清空选择集。
    function toggleSelectMode() {
        selectMode = !selectMode
        if (!selectMode) selectedIds = []
        selectionAnchor = -1
    }
    function exitSelectMode() {
        if (selectMode) toggleSelectMode()
    }

    /// 单击切换选中(Shift = 从锚点行到当前行的范围)。
    /// ⚠️ 范围只覆盖**已加载**的行(≤ 首屏 limit 50)—— 全选范围同口径,
    ///    "选该条件下全部"需要 core 传全量 id,方案里明确不做。
    function toggleSelectedIndex(index, shiftHeld) {
        const id = Entries.idAt(index)
        if (id === "") return
        if (shiftHeld && selectionAnchor >= 0) {
            const from = Math.min(selectionAnchor, index)
            const to = Math.max(selectionAnchor, index)
            const ids = selectedIds.slice()
            for (let i = from; i <= to; ++i) {
                const rowId = Entries.idAt(i)
                if (rowId === "") continue
                if (ids.indexOf(rowId) < 0) ids.push(rowId)
            }
            selectedIds = ids
        } else {
            const ids = selectedIds.slice()
            const pos = ids.indexOf(id)
            if (pos >= 0) ids.splice(pos, 1)
            else ids.push(id)
            selectedIds = ids
            selectionAnchor = index
        }
    }

    // ---- 键盘导航(↑↓ / Enter / PageUp·PageDown) ----
    /// 键盘光标所在行号;-1 = 无光标(尚未按过方向键)。
    ///
    /// 为什么归 QML 而不是模型:它是**纯视图态**(与滚动位置、hover 同层),
    /// 模型 reset 后行号必然失效 —— 所以每次刷新都要夹回(clampKeyboardCursor),
    /// 而不是让模型去维护一个"光标 id"。
    property int kbIndex: -1

    /// 列表当前是否该接管方向键:浮层(菜单/筛选/标签卡/待办卡/抽屉)打开时
    /// **不接管** —— 那些浮层里有自己的焦点与按键语义,抢过来会误触发复制。
    ///
    /// ⚠️ 必须写成**函数**而不是 readonly property:这些卡片/列表都在本对象**之后**
    ///    创建,根对象的属性绑定在构造期就求值,那时 id 还是 null —— 绑定会静默算成
    ///    false(或抛一次 TypeError)且不自我修复,表现为"方向键永远没反应"。
    function listKeysUsable() {
        return panelWindow.mode === "list" && !selectMode && !filterPopupOpen
            && menuTarget === null
            && !(settingsDrawerLoader.item && settingsDrawerLoader.item.opened)
            && !tagEditorCard.visible && !tagRenameCard.visible
            && !todoComposerCard.visible && !bulkDeleteCard.visible
            && !bulkTagCard.visible && !clearDoneCard.visible
    }

    /// 行数变化后把光标夹回有效范围(条目被删/筛选变化后行号会失效)。
    /// 光标原本就在列表外(-1)则保持不变 —— 不要因为一次刷新就"凭空选中一条"。
    function clampKeyboardCursor() {
        if (kbIndex < 0) return
        if (list.count === 0) { kbIndex = -1; return }
        if (kbIndex >= list.count) kbIndex = list.count - 1
    }

    /// 移动键盘光标并保证该行可见。
    function moveKeyboardCursor(delta) {
        if (list.count === 0) return
        if (kbIndex < 0) {
            // 首次按方向键:↓ 从第一条开始,↑ 从最后一条开始(与 Win+V 的直觉一致)
            kbIndex = delta > 0 ? 0 : list.count - 1
        } else {
            const next = Math.max(0, Math.min(list.count - 1, kbIndex + delta))
            if (next === kbIndex) return
            kbIndex = next
        }
        list.positionViewAtIndex(kbIndex, ListView.Contain)
    }

    /// 键盘 Home/End:首条 / 末条。
    function jumpKeyboardCursor(toEnd) {
        if (list.count === 0) return
        kbIndex = toEnd ? list.count - 1 : 0
        list.positionViewAtIndex(kbIndex, ListView.Contain)
    }

    /// 复制第 row 行并收起面板(N1 + N2 的公共出口)。
    /// 收起后按设置决定是否自动粘贴 —— 判断在 C++(autoPasteIfEnabled 现读设置),
    /// 这里无条件调一次即可。
    function copyRowAndHide(row) {
        const id = Entries.idAt(row)
        if (id === "") return
        const resp = Entries.copyEntry(id)
        if (!resp.ok) {
            Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            return
        }
        panelWindow.hidePanel()
        Core.autoPasteIfEnabled()
    }

    /// 全选当前视图的**已加载行**(Ctrl+A / 工具条按钮共用)。
    /// ⚠️ 行数上限 = 已加载行数,不追页:工具条会提示范围,见 selectedCountText。
    function selectAllLoaded() {
        const ids = []
        const n = list.count
        for (let i = 0; i < n; ++i) {
            const id = Entries.idAt(i)
            if (id !== "") ids.push(id)
        }
        selectedIds = ids
    }

    /// 工具条与选择态的文案(含"已加载行"范围提示,决策点 2 的落点)。
    readonly property string selectedCountText:
        selectedIds.length >= list.count && list.count > 0
            ? "已选 " + selectedIds.length + " 项(当前视图全部)"
            : "已选 " + selectedIds.length + " 项"

    /// 选中集合里是否有待办(工具条「完成」按钮的可用性)。
    readonly property bool selectionHasTodo: (function() {
        for (let i = 0; i < selectedIds.length; ++i)
            if (Entries.kindOfId(selectedIds[i]) === "todo") return true
        return false
    })()

    /// 选中集合是否全部已固定(「固定/取消固定」文案切换)。
    readonly property bool selectionAllPinned: (function() {
        if (selectedIds.length === 0) return false
        for (let i = 0; i < selectedIds.length; ++i)
            if (!Entries.isPinned(selectedIds[i])) return false
        return true
    })()

    /// 生效筛选的展示文案(用于分类行右侧的活动 chip):
    /// 让用户一眼看出"列表明明少了东西,是因为这两个条件"。
    readonly property string activeSourceLabel:
        activeSource === "capture" ? "截图"
      : activeSource === "translate" ? "翻译"
      : activeSource === "ocr" ? "识别"
      : activeSource
    readonly property string activeSourceIcon:
        activeSource === "capture" ? "image"
      : activeSource === "translate" ? "translate"
      : activeSource === "ocr" ? "text" : ""

    // ---- 筛选相关内联组件(仅本文件用;与 TodoTimeField 同一手法) ----

    /// 生效筛选的「可移除 chip」:显示在分类行右侧,点击 ✕ 解除该条件。
    /// 与 FilterOption 的区别:本组件是"已生效"的呈现(带 ✕),不承担选择。
    component FilterChip: Rectangle {
        id: chip
        property string label: ""
        property string iconKind: ""
        /// 标签类用彩色点(与卡片标签同色);来源类用图标,不需要点。
        property color dotColor: "transparent"
        signal removeRequested()

        height: 22
        width: chipRow.implicitWidth + 16
        radius: Theme.metrics.radiusBadge
        // 生效态用 accentSoft 底 + accent 边框:与分类行里选中的导航项同一语言,
        // 表达"这是一个正在起作用的条件",而不是普通按钮。
        color: Theme.colors.accentSoft
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.accent

        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: 4
            HeaderIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: chip.iconKind !== ""
                kind: chip.iconKind
                color: Theme.colors.accent
                size: Theme.metrics.fontMicro
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                visible: chip.dotColor !== "transparent"
                width: 6; height: 6; radius: 3
                color: chip.dotColor
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: chip.label
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textPrimary
            }
            // ✕ 解除:声明在最后 → 命中优先级最高(同父级重叠后声明者优先),
            // 不会与 chip 的悬停/其它热区互相吞点击。
            Item {
                anchors.verticalCenter: parent.verticalCenter
                width: 14; height: 14
                HeaderIcon {
                    anchors.centerIn: parent
                    kind: "chevron"
                    color: chipRemoveMa.containsMouse ? Theme.colors.error
                                                     : Theme.colors.textSecondary
                    size: 10
                    rotation: 45   // chevron 转 45° = ✕(省一个图标)
                }
                MouseArea {
                    id: chipRemoveMa
                    anchors.fill: parent
                    anchors.margins: -3
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: chip.removeRequested()
                }
            }
        }
    }

    /// 筛选浮层里的一个可选项(来源/标签共用)。
    /// 与 FilterChip 同一视觉语言,但表达"可选中"而非"已生效":
    /// 选中态由 accent 边框 + 实心色点承担(与旧筛选条同一套判定,不改观感)。
    component FilterOption: Rectangle {
        id: option
        property string label: ""
        property string iconKind: ""
        property int count: 0
        property bool selected: false
        property color dotColor: "transparent"
        signal clicked()
        signal rightClicked()

        height: 22
        width: optionRow.implicitWidth + 16
        radius: Theme.metrics.radiusBadge
        color: optionMa.containsMouse ? Theme.colors.surfaceHover
                                      : Theme.colors.surfaceSecondary
        border.width: Theme.metrics.borderWidth
        border.color: option.selected ? Theme.colors.accent : Theme.colors.border
        Behavior on color { ColorAnimation { duration: Theme.animation.fast } }
        Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }

        Row {
            id: optionRow
            anchors.centerIn: parent
            spacing: 5
            HeaderIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: option.iconKind !== ""
                kind: option.iconKind
                color: option.selected ? Theme.colors.accent
                                       : Theme.colors.textSecondary
                size: Theme.metrics.fontMicro
            }
            // 色点:选中实心、未选中空心 —— "选中"在灰度屏/色弱下也读得出
            // (只靠颜色深浅区分对色觉障碍不友好;与旧筛选条同一取舍)。
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                visible: option.dotColor !== "transparent"
                width: 6; height: 6; radius: 3
                color: option.selected ? option.dotColor : "transparent"
                border.width: option.selected ? 0 : 1
                border.color: option.dotColor
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                // `·` 分隔名与计数:直接拼空格时形如 "1 1" 会糊成一片,
                // 分不清哪个是名字哪个是数量(实测反馈)。
                text: option.count > 0 ? option.label + " · " + option.count : option.label
                font.pixelSize: Theme.metrics.fontCaption
                // ⚠️ 选中态文字统一用 accent,不用标签自己的派生色(dotColor):
                //    dotColor 在暗色下是**亮柔彩**(设计给"点"用),直接拿来当
                //    文字色对比度不足且观感发虚,实测"看不清"。标签色由色点承担,
                //    文字保持 accent 这一统一强调语言,与来源选中态同口径。
                color: option.selected ? Theme.colors.accent
                                       : Theme.colors.textSecondary
            }
        }
        MouseArea {
            id: optionMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) option.rightClicked()
                else option.clicked()
            }
        }
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
        // 筛选浮层(Popup)渲染在 overlay 层,不清会浮在翻译页上(同抽屉的坑)
        if (root.filterPopupOpen) root.closeFilterPopup()
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
        // 备注回填走 todo_get(快照里没有备注字段;条目可能被分页挡掉,故不查行集)
        todoNoteInput.text = ""
        // 时刻与标签走 todo_get 一次取全(meta 为空 = 非待办/不存在 → 全空)
        const meta = entry ? Entries.todoMeta(entry.entryId) : null
        todoPrefilling = true
        todoDueField.apply(meta && meta.dueAt > 0 ? meta.dueAt : 0)
        // 旧数据 remind/notify 可能不同值:取较早的(先响的那个)展示
        const r = meta && meta.remindAt > 0 ? meta.remindAt : 0
        const n = meta && meta.notifyAt > 0 ? meta.notifyAt : 0
        todoRemindField.apply((r > 0 && n > 0) ? Math.min(r, n) : Math.max(r, n))
        todoNoteInput.text = meta && meta.noteText ? meta.noteText : ""
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
            todoError.text = "标题不能为空"
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
        // 备注(docs/21):可空,多行原样提交
        const noteText = todoNoteInput.text
        const resp = Entries.todoSave(todoEditId, text, times, tags, noteText)
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
        if (!resp.ok) {
            Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            return
        }
        panelWindow.hidePanel()
        Core.autoPasteIfEnabled()
    }
    function copyPathAndHide(text) {
        const resp = Core.call("copy_text_to_clipboard", { text: text })
        if (!resp.ok) {
            Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            return
        }
        panelWindow.hidePanel()
        Core.autoPasteIfEnabled()
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
        //   选择模式 → 筛选浮层 → 设置抽屉 → 翻译页浮层 → 收起面板
        // 翻译页的 Esc 语义与 ✕ 一致:**直接收起**,不再顺带切回列表页。
        // ⚠️ 选择模式排在最前:它是"覆盖整列表的操作态",用户在模式内按 Esc
        //    最可能是想退出多选;且退出比关浮层"损失"更小(选择集可重建)。
        // ⚠️ 筛选浮层次之:它是"贴在某按钮下的轻浮层",若排在抽屉之后,
        //    抽屉没开时这一层就永远轮不到。
        if (root.selectMode) {
            root.exitSelectMode()
            return
        }
        if (root.filterPopupOpen) {
            root.closeFilterPopup()
            return
        }
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
    // Ctrl+F / Ctrl+K 聚焦搜索(§27);Ctrl+1..9 复制当前视图第 N 条(第一代 F2);
    // Ctrl+A = 选择模式内全选已加载行(模式外不拦截 —— 系统约定 Ctrl+A 是
    // "全选文本",抢了会破坏搜索框等输入处的行为)。
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_A
                && (event.modifiers & Qt.ControlModifier)
                && root.selectMode
                && panelWindow.mode === "list"
                && !(searchInput.activeFocus)) {
            root.selectAllLoaded()
            event.accepted = true
            return
        }
        if ((event.key === Qt.Key_F || event.key === Qt.Key_K)
                && (event.modifiers & Qt.ControlModifier)) {
            searchInput.forceActiveFocus()
            searchInput.selectAll()
            event.accepted = true
            return
        }
        // ---- 键盘导航(N1):↑↓ 选行 / Enter 复制 / PgUp·PgDn 翻页 / Home·End ----
        // 搜索框有焦点时**不接管**:方向键在输入框里是移光标,抢走会让用户改不了错字。
        // ⚠️ 判定用 activeFocus 而不是 text !== "":用户可能留着搜索词却想翻列表。
        const listFocused = root.listKeysUsable() && !searchInput.activeFocus
        if (listFocused) {
            const noMods = !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                              | Qt.ShiftModifier | Qt.MetaModifier))
            if (noMods && event.key === Qt.Key_Down) {
                root.moveKeyboardCursor(1); event.accepted = true; return
            }
            if (noMods && event.key === Qt.Key_Up) {
                root.moveKeyboardCursor(-1); event.accepted = true; return
            }
            if (noMods && event.key === Qt.Key_PageDown) {
                root.moveKeyboardCursor(5); event.accepted = true; return
            }
            if (noMods && event.key === Qt.Key_PageUp) {
                root.moveKeyboardCursor(-5); event.accepted = true; return
            }
            if (noMods && event.key === Qt.Key_Home && root.kbIndex >= 0) {
                root.jumpKeyboardCursor(false); event.accepted = true; return
            }
            if (noMods && event.key === Qt.Key_End && root.kbIndex >= 0) {
                root.jumpKeyboardCursor(true); event.accepted = true; return
            }
            // Enter = 复制光标行并收起(与单击卡片同语义:不走多选、不改卡片状态)
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    && root.kbIndex >= 0) {
                root.copyRowAndHide(root.kbIndex)
                event.accepted = true
                return
            }
        }
        if ((event.modifiers & Qt.ControlModifier)
                && !(settingsDrawerLoader.item && settingsDrawerLoader.item.opened)
                && panelWindow.mode !== "translate"
                && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            const resp = Entries.copyByIndex(event.key - Qt.Key_1)
            if (resp.ok) {
                panelWindow.hidePanel()
                Core.autoPasteIfEnabled()
            } else {
                Core.showToast("复制失败:" + (resp.error ? resp.error.message : ""))
            }
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
                            // 在搜索框里按 ↓ 直接进入列表(Win+V / Spotlight 的通行习惯):
                            // 输入完关键词后不该还要先用鼠标点一下列表。
                            // ⚠️ 这里**只**接管 ↓:↑ 在输入框里保持默认(移光标/不动),
                            //    否则用户在搜索词里按 ↑ 会突然跳走焦点,很难理解。
                            Keys.onDownPressed: function(event) {
                                if (!root.listKeysUsable()) return
                                root.moveKeyboardCursor(1)
                                event.accepted = true
                            }
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
                    objectName: "settingsButton"   // 验收钩子(--settings-flow 真实点击)
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

        // ---- 分类标签 + 筛选入口(§15 选中态:AccentSoft 底 + Accent 字 + 左侧指示条) ----
        // 布局:分类项左对齐,「筛选」按钮与生效条件右对齐。
        // ⚠️ 用 RowLayout 而非 Row:右对齐需要一个 fillWidth 的弹性空隙,
        //    Row 没有 Layout 附件属性。
        RowLayout {
            id: tabs
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            Layout.topMargin: Theme.metrics.spacingSm
            Layout.bottomMargin: Theme.metrics.spacingSm
            spacing: Theme.metrics.spacingXs

            property string current: "all"

            // 分类项本身仍用 Row 包住(Repeater 需要行容器),宽度自然收窄不占满,
            // 后面的 Item{fillWidth} 负责把「筛选」推到右端。
            Row {
                id: tabRow
                Layout.alignment: Qt.AlignVCenter
                spacing: Theme.metrics.spacingXs

                Repeater {
                    id: tabRepeater
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
                        // 验收钩子(--settings-flow:零待办时该页签的可见性 + 真实点击)
                        objectName: tabDelegate.viewId === "todo" ? "todoTab" : ""
                        // 计数来自 Core.counts(单一数据源 + countsChanged 通知)。
                        // 待办页显示**未完成数**(todoOpen),归档不占导航计数(§6.3)。
                        readonly property var count: viewId === "todo"
                            ? Core.counts.todoOpen : Core.counts[viewId]
                        readonly property bool selected: tabs.current === tabDelegate.viewId
                        // ⚠️ 零计数分类**隐藏**("文件 0" 这类空标签页占位是纯噪音,
                        //    用户点进去只会看到空列表)。"全部"与"待办"恒显示:
                        //    "全部"是归位锚点;待办的计数只算**未完成**(todoOpen),
                        //    全部完成时计数为 0 —— 按零计数隐藏的话,归档里的已完成
                        //    条目连同「清空已完成」入口一起消失,再没有按钮能翻到
                        //    (用户实测反馈)。当前选中项恒显示 —— 否则用户在
                        //    "文本"页删光文本后,连"自己现在在哪"都看不到了。
                        readonly property bool empty: tabDelegate.count === 0
                        visible: !empty || tabDelegate.viewId === "all"
                                 || tabDelegate.viewId === "todo" || selected
                        /// 图标种类与 viewId 同源;"pinned" 复用图钉图标(与头部「固定」按钮同符号)。
                        readonly property string iconKind:
                            tabDelegate.viewId === "pinned" ? "pin" : tabDelegate.viewId
                        radius: Theme.metrics.radiusInput
                        height: Theme.metrics.navItemHeight
                        // 计数并入文字,用 `·` 分开(与筛选 chip 同一口径:
                        // "全部 32" 里的 32 是数量,不是名字的一部分)。
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

            // 弹性空隙:把下面的「筛选」按钮与激活条件推到行尾
            Item { Layout.fillWidth: true }

            // ---- 筛选入口 ----
            ThemedButton {
                id: filterButton
                objectName: "filterButton"
                Layout.alignment: Qt.AlignVCenter
                size: "sm"
                // 有筛选时用 primary 强调,并显示维度数角标(如「筛选 2」)
                variant: root.filterActive ? "primary" : "secondary"
                iconName: "filter"
                text: root.filterCount > 0
                      ? "筛选 " + root.filterCount : "筛选"
                onClicked: root.filterPopupOpen ? root.closeFilterPopup()
                                                : root.openFilterPopup()
                // 供筛选浮层的"移出自动收回"判定:浮层锚在本按钮下方,
                // 从按钮滑入浮层的路径上鼠标会短暂离开按钮,故本按钮的 hover
                // 也算"用户还在筛选区域"(与浮层内 hover 同源判定)。
                MouseArea {
                    id: filterButtonMouseMa
                    anchors.fill: parent
                    hoverEnabled: true
                    // 只提供 hover 信号,点击仍归按钮(onClicked 已在上层)
                    onClicked: filterButton.clicked()
                }
            }

            // ---- 多选入口 ----
            // ⚠️ 放分类行行尾(与「筛选」并排):多选是对**当前视图**的操作,
            //    归属同一行;选择模式的完整工具条在进入后才出现(见 selectToolBar)。
            // 图标复用 "todo"(复选框):多选的本义就是"勾选若干条"。
            ThemedButton {
                objectName: "selectModeButton"
                Layout.alignment: Qt.AlignVCenter
                size: "sm"
                // 进入选择模式后转 primary(模式开关的状态可见性)
                // ⚠️ 图标用 multiselect 而非 todo:todo(单复选框)已被头部
                //    「待办」按钮占用,复用会让两个相邻按钮长得一样(用户实测)。
                variant: root.selectMode ? "primary" : "secondary"
                iconName: "multiselect"
                text: root.selectMode ? "退出多选" : "多选"
                onClicked: root.toggleSelectMode()
            }
        }
        // ---- 选择模式工具条(仅 selectMode 时出现) ----
        // 操作集合:复制(不提供,见方案决策点 3)/固定/完成/加标签/删除。
        // ⚠️ 与"生效筛选行"同一占位规则:进入模式才占一行;条目按钮在
        //    selectedIds 为空时禁用(避免点了没反应)。
        // ⚠️ 删除必须走确认卡片(bulkConfirmCard),不直接删 —— 批量删除不可逆。
        RowLayout {
            id: selectToolBar
            objectName: "selectToolBar"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            Layout.bottomMargin: Theme.metrics.spacingSm
            spacing: Theme.metrics.spacingXs
            visible: root.selectMode

            Text {
                // 含范围提示(决策点 2):全选只覆盖已加载行,选满时明示
                // "当前视图全部",未满时说明是已加载部分 —— 用户不会误以为
                // "全选 = 该条件下所有条目都进选择集"。
                text: root.selectedCountText
                       + (root.selectedIds.length > 0
                          && root.selectedIds.length < list.count
                          ? "(已加载行)" : "")
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            ThemedButton {
                variant: "ghost"; size: "sm"; iconName: "all"
                text: "全选"
                enabled: list.count > 0
                onClicked: root.selectAllLoaded()
            }
            ThemedButton {
                variant: "ghost"; size: "sm"; iconName: "pin"
                text: root.selectionAllPinned ? "取消固定" : "固定"
                enabled: root.selectedIds.length > 0
                onClicked: {
                    const r = Entries.setPinnedBulk(
                        root.selectedIds, !root.selectionAllPinned)
                    if (!r.ok) { Core.showToast(r.message); return }
                    Core.showToast((root.selectionAllPinned ? "已取消固定 " : "已固定 ")
                                   + r.changed + " 条")
                }
            }
            // 「完成/取消完成」只对选中集里的待办生效(core 侧已过滤非 todo),
            // 按钮在有 todo 时才可用 —— 对纯文本集禁用,避免"完成 0 条"的空操作。
            ThemedButton {
                variant: "ghost"; size: "sm"; iconName: "todo"
                text: "完成"
                visible: root.selectionHasTodo
                enabled: root.selectedIds.length > 0
                onClicked: {
                    // 只把选中集里的 **todo id** 传下去(文本混选时剔除)
                    const todoIds = []
                    for (let i = 0; i < root.selectedIds.length; ++i)
                        if (Entries.kindOfId(root.selectedIds[i]) === "todo")
                            todoIds.push(root.selectedIds[i])
                    const r = Entries.todoSetDoneBulk(todoIds, true)
                    if (!r.ok) { Core.showToast(r.message); return }
                    Core.showToast("已完成 " + r.changed + " 条待办")
                }
            }
            ThemedButton {
                variant: "ghost"; size: "sm"; iconName: "pencil"
                text: "加标签"
                enabled: root.selectedIds.length > 0
                onClicked: root.openBulkTagEditor()
            }
            ThemedButton {
                objectName: "bulkDeleteButton"
                variant: "ghost"; size: "sm"; iconName: "trash"
                text: "删除"
                enabled: root.selectedIds.length > 0
                onClicked: root.openBulkDeleteConfirm()
            }
            Item { Layout.fillWidth: true }
            ThemedButton {
                variant: "secondary"; size: "sm"
                text: "取消"
                onClicked: root.exitSelectMode()
            }
        }


        // ---- 生效筛选行(仅在有筛选时出现;可换行,不再挤分类行) ----
        // ⚠️ 为什么从分类行里搬出来:RowLayout 空间不足时会**按比例压缩**各子项,
        //    chip 多时把分类项与按钮挤变形、文字截断(实测:两个 chip + 5 个分类
        //    + 筛选按钮在 800px 下已放不下)。独立行 + Flow 可换行,宽度天然够。
        // 目的不是"再放一行按钮",而是告诉用户"列表变少是因为这些条件";
        // 点击 ✕ 即解除该维度 —— 比"回到浮层里找那个已选中的项"快得多。
        // 仅在有筛选时出现:默认零筛选时本行整体不占高度(方案 A 的省空间承诺)。
        Flow {
            id: activeFilterBar
            Layout.fillWidth: true
            Layout.leftMargin: Theme.metrics.spacingMd
            Layout.rightMargin: Theme.metrics.spacingMd
            Layout.bottomMargin: visible ? Theme.metrics.spacingSm : 0
            spacing: Theme.metrics.spacingXs
            visible: root.filterActive

            FilterChip {
                visible: root.activeSource !== ""
                label: root.activeSourceLabel
                iconKind: root.activeSourceIcon
                onRemoveRequested: {
                    root.activeSource = ""
                    Entries.setSource("")
                }
            }
            FilterChip {
                visible: root.activeTag !== ""
                label: root.activeTag
                // 标签用标签自己的色(与卡片/浮层里的同名标签同色)
                dotColor: Theme.tagColor(root.activeTag)
                onRemoveRequested: {
                    root.activeTag = ""
                    Entries.setTag("")
                }
            }
            // chip 多时兜底:一键全清,不用逐个点 ✕
            ThemedButton {
                // Flow 是 positioner:子项 anchors 会被忽略;垂直对齐用 y 微调
                // (chip 高 22、sm 按钮 28,y=(28-22)/2 让按钮与 chip 齐平)。
                objectName: "activeClearFilters"
                variant: "ghost"
                size: "sm"
                text: "清除全部"
                y: (height - 22) / 2
                onClicked: root.clearFilters()
            }
        }



        // ---- 筛选浮层(S5.1/S5.2 合并入口) ----
        // 方案 A:来源与标签收进一个浮层,默认**不占**任何垂直空间。
        // 动机:三行筛选(分类/来源/标签)共约 90px,在 800x600 面板里占 15%
        // (最小高 380 时占 24%),且三者语义不同却渲染成同一套胶囊 ——
        // 分类是**导航**,来源/标签是**过滤**,平铺在一起没有主次。
        // 现结构:分类行常驻(导航),过滤条件按需在浮层里设置,
        // 生效后以可移除 chip 显示在其下方的独立行(activeFilterBar,可换行)。
        //
        // ⚠️ 用 Popup 而非普通 Item + z:面板高度有限,浮层若内联展开会挤压列表;
        //    Popup 渲染在 overlay 层,不参与主布局(与时间选择器同一取舍)。
        Popup {
            id: filterPopup
            objectName: "filterPopup"
            // 锚到「筛选」按钮下方,右边缘与按钮对齐(贴住行尾,不超出面板)
            x: Math.max(Theme.metrics.spacingMd,
                        filterButton.mapToItem(null, filterButton.width, 0).x
                        - width)
            y: filterButton.mapToItem(null, 0, filterButton.height).y + 6
            width: Math.min(320, root.width - 2 * Theme.metrics.spacingMd)
            padding: Theme.metrics.spacingMd
            // 关闭策略:点外部/按 Esc 都关。与时间选择器不同,这里**没有**
            // "关闭会吞掉后续点击"的顾虑 —— 浮层里只有筛选开关,点外部本来就
            // 期望"关掉并继续之前的事"。
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            modal: false
            visible: root.filterPopupOpen
            onClosed: root.filterPopupOpen = false
            // ---- 鼠标移出自动收回 ----
            // 用户从筛选浮层滑到别处(去列表/搜索/别的按钮)时期望它自己关掉,
            // 不必专门按 Esc 或点外部(用户反馈"鼠标不在筛选窗口自动收回")。
            // 判定"鼠标离开"需要两处同源:浮层内(hoverHandler)+ 触发它的
            // 「筛选」按钮(filterButtonMouseMa)。浮层锚在按钮下方,从按钮滑进
            // 浮层时鼠标会"离开"按钮进入浮层 —— 中间短暂两个都不 hover,故
            // 用延迟判定(留给用户跨区滑动的时间)。
            //
            // ⚠️ 触发条件加"**鼠标先进过浮层**"(hasHovered):只开浮层但鼠标没
            //    进去过(如程序化验收、键盘导航)时**不自动关** —— 否则用户点开
            //    后手在别处做选择,浮层立刻被关,选项都来不及看。真实路径是
            //    "滑进去看选项 → 选完移开",先进入过浮层才谈得上"移出"。
            HoverHandler {
                id: filterPopupHover
                // 缺省会抢触控/鼠标以外的设备;只关心鼠标/触控笔就够
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                 | PointerDevice.Stylus
                onHoveredChanged: if (hovered) filterPopup.hasHovered = true
            }
            property bool hasHovered: false
            Timer {
                id: filterAutoHideTimer
                interval: 600
                repeat: true
                onTriggered: {
                    // 满足"曾进入过 + 现在浮层和按钮都不在 hover"才关;
                    // 仍在 hover 则持续计时(等用户真离开)。
                    if (filterPopup.hasHovered
                        && !filterPopupHover.hovered
                        && !(filterButtonMouseMa.hovered && filterButtonMouseMa.containsMouse)) {
                        root.closeFilterPopup()
                    }
                }
            }
            onVisibleChanged: {
                if (visible) {
                    filterPopup.hasHovered = false
                    filterAutoHideTimer.start()
                } else {
                    filterAutoHideTimer.stop()
                }
            }

            background: Rectangle {
                radius: Theme.metrics.radiusCard
                color: Theme.colors.surface
                border.width: Theme.metrics.borderWidth
                border.color: Theme.colors.borderStrong
            }

            contentItem: ColumnLayout {
                spacing: Theme.metrics.spacingSm

                // ---- 标题行:标题 + 「清除筛选」 ----
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingSm
                    Text {
                        text: "筛选"
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontSection
                        font.weight: Font.DemiBold
                        color: Theme.colors.textPrimary
                    }
                    Item { Layout.fillWidth: true }
                    ThemedButton {
                        objectName: "filterClear"
                        variant: "ghost"
                        size: "sm"
                        text: "清除筛选"
                        enabled: root.filterActive
                        onClicked: root.clearFilters()
                    }
                }

                // ---- 来源组(仅有可选来源时显示) ----
                // 来源选项:值 + 中文文案 + 图标 + 计数。
                // ⚠️ 计数来自 core 的 list_sources(SQL 精确 COUNT),不写死
                //    "5 种来源" —— 没有数据的来源不出现,不占位置;
                //    文案/图标与卡片来源徽标同词汇(EntryCard 的来源徽标)。
                Text {
                    Layout.fillWidth: true
                    visible: filterSourceFlow.count > 0
                    text: "来源"
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Theme.colors.textSecondary
                }
                Flow {
                    id: filterSourceFlow
                    Layout.fillWidth: true
                    spacing: Theme.metrics.spacingXs
                    // ⚠️ 待办视图不列来源:待办来源恒为 'todo',而"待办"本身已是
                    //    顶层导航标签页 —— 再列一个「待办」是冗余且点不生效
                    //    (模型对 todo 视图不下发 source 过滤)。
                    readonly property var model: {
                        const out = []
                        if (tabs.current === "todo") return out
                        const stats = Entries.sourceStats
                        for (let i = 0; i < stats.length; ++i) {
                            const s = stats[i].source
                            // 剪贴板是默认来源、待办是顶层标签,都不需要筛选项;
                            // 零计数来源也跳过(点了会得到空列表,属纯噪音)。
                            if (s === "clipboard" || s === "todo"
                                || stats[i].count <= 0) continue
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
                    Repeater {
                        model: filterSourceFlow.model
                        delegate: FilterOption {
                            required property var modelData
                            label: modelData.label
                            iconKind: modelData.icon
                            count: modelData.count
                            selected: root.activeSource === modelData.value
                            onClicked: root.toggleSourceFilter(modelData.value)
                        }
                    }
                }

                // ---- 标签组(仅有标签时显示) ----
                Text {
                    Layout.fillWidth: true
                    visible: Entries.tagStats.length > 0
                    text: "标签"
                    font.pixelSize: Theme.metrics.fontCaption
                    color: Theme.colors.textSecondary
                }
                // ⚠️ 标签数量可能很多,浮层里给**限高 + 滚动**:
                //    直接铺开会把浮层撑得比屏幕还高(标签条此前靠横向滚动回避,
                //    浮层里改用纵向滚动,更符合"分组列表"的形态)。
                Flickable {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(filterTagFlow.implicitHeight, 140)
                    visible: Entries.tagStats.length > 0
                    contentWidth: width
                    contentHeight: filterTagFlow.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    Flow {
                        id: filterTagFlow
                        width: parent.width
                        spacing: Theme.metrics.spacingXs
                        Repeater {
                            model: Entries.tagStats
                            delegate: FilterOption {
                                required property var modelData
                                label: modelData.tag
                                count: modelData.count
                                // 标签用它自己的派生色(与卡片上的同名标签同色,
                                // 同一纯函数无状态,两端不可能取到不同值)
                                dotColor: Theme.tagColor(modelData.tag)
                                selected: root.activeTag === modelData.tag
                                // 右键 = 重命名(与旧筛选条同一交互:标签的"全集"
                                // 就在这里,不必先进某条目的标签编辑器)
                                onRightClicked: root.openTagRename(modelData.tag)
                                onClicked: {
                                    root.activeTag = (root.activeTag === modelData.tag)
                                                     ? "" : modelData.tag
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
                // 验收钩子(--settings-flow:点"抽屉外的列表区域"用;只挂首行,
                // 避免每个委托同名导致 DFS 命中隐藏实例 —— 同 clearDoneLink 的坑)。
                objectName: rowWrap.index === 0 ? "entryCard0" : ""
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

                // 「已完成」组头的清空入口:只在归档组首行出现(needsHeader 与组标题同一条件)。
                // 用行内文字链而非按钮:与组标题同字号,不改变行高公式
                // (implicitHeight 只按 groupLabel 计算,按钮会顶开卡片)。
                Text {
                    id: clearDoneLink
                    // ⚠️ objectName 必须与 visible 同条件:委托每个实例都挂名的话,
                    //    验收的 DFS 会命中首行(通常是隐藏实例),点击落到空气上。
                    objectName: visible ? "clearDoneLink" : ""
                    visible: rowWrap.needsHeader && rowWrap.groupKey === "done"
                             && !root.selectMode   // 选择模式内隐藏:清空会让选中集出现失效 id
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.rightMargin: 2
                    text: "清空已完成"
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.metrics.fontCaption
                    color: clearDoneArea.containsMouse ? Theme.colors.error
                                                       : Theme.colors.textSecondary
                    MouseArea {
                        id: clearDoneArea
                        // 负边距扩大热区:文字链本体只有一行高,点不中会像"没反应"
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.openClearDoneConfirm()
                    }
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
                    todoNote: model.todoNote ? model.todoNote : ""
                    // 搜索命中高亮:直接绑搜索框文本(改词即重算,无需模型参与)
                    query: searchInput.text
                    // 键盘光标行:与"菜单打开"同用强调语言(两者都是"正在被操作的对象"),
                    // 但键盘光标只在无浮层时出现 —— 有菜单时焦点语义归菜单。
                    menuOpen: (root.menuTarget !== null
                               && root.menuTarget.entryId === rowWrap.entryId)
                              || root.highlightTodoId === rowWrap.entryId
                              || (root.listKeysUsable() && root.kbIndex === rowWrap.index)
                    menuOpenElsewhere: root.menuTarget !== null
                    // ---- 条目多选 ----
                    selectMode: root.selectMode
                    selected: root.selectMode
                              && root.selectedIds.indexOf(rowWrap.entryId) >= 0
                    // Shift 范围选择:锚点与目标行号都在委托层可得(index)。
                    // ⚠️ toggleSelected 的参数是 Shift 键状态,范围选择在
                    //    toggleSelectedIndex 里完成(锚点行号由 Main 侧维护)。
                    onToggleSelected: function(shiftHeld) {
                        root.toggleSelectedIndex(index, shiftHeld)
                    }
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
                        // ⚠️ 有筛选条件时列表为空 ≠ 剪贴板是空的 —— 数据就在那里,
                        //    只是被条件筛掉了。沿用"剪贴板是空的"会误导用户以为
                        //    数据丢了(实测:来源+标签叠加后命中的空状态)。
                        if (root.filterActive) return "没有匹配的条目"
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
                        // 筛选无结果的副文案:给出最快的出口(一键清除),并隐藏
                        // "复制任意内容"引导 —— 那句在筛选态下是错误指引。
                        if (root.filterActive) return "当前筛选条件下没有条目"
                        if (tabs.current === "pinned") return "右键条目选择「固定」即可常驻这里"
                        if (tabs.current === "todo") return "点右上角「待办」按钮,新建一条"
                        return "复制任意内容,它会自动出现在这里"
                    }
                }
                // 筛选无结果的快捷出口:一键清除全部条件(搜索态没有等价动作,
                // 故与快捷键提示同规则地只在对应态显示)。
                ThemedButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    objectName: "emptyClearFilters"
                    variant: "secondary"
                    size: "sm"
                    visible: root.filterActive && Entries.errorText === ""
                             && searchInput.text === ""
                    text: "清除筛选"
                    onClicked: root.clearFilters()
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Theme.metrics.spacingXs
                    // 筛选态下这一行换成「清除筛选」按钮,不重复占位
                    visible: Entries.errorText === "" && searchInput.text === ""
                             && tabs.current !== "todo" && !root.filterActive
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
                // ↑↓ / Enter 是键盘导航(N1)的唯一提示位:没有它用户不会想到
                // 可以用键盘选条 —— 提示成本低于在每张卡片上画序号。
                KeyCap { label: "↑↓" }
                KeyCap { label: "Enter" }
                Text {
                    text: "选中并复制"
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

    // ---- 模型信号订阅 ----
    // ⚠️ Entries.toast 与 Core.toastMessage 是两条独立信号:模型内的失败路径
    //    (固定/保存标签/更新待办/推迟/保存待办/清空已完成)不经 Core 中转,
    //    此前 6 处 emit 全无接收者 —— 失败提示静默消失,补上这条接线。
    Connections {
        target: Entries
        function onToast(message) { toast.showMsg(message) }
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
                // 桌面便签(docs/20):把这条待办贴成常驻桌面的小窗。
                // 便签是**待办的第二个视图**(不是贴图):可编辑、勾完成、贴边半隐藏,
                // 几何/收起态落库,重启复原。
                addAction("贴到桌面（便签）", function() { Core.openTodoNote(entry.entryId) })
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

            // 组 1.5:多选入口(进入选择模式并预选本条 —— 从单条自然过渡到批量)
            addAction("多选…", function() {
                if (!root.selectMode) root.toggleSelectMode()
                // 已在集合里则不重复加入(幂等,重复右键不会"选两次")
                if (root.selectedIds.indexOf(entry.entryId) < 0)
                    root.selectedIds = root.selectedIds.concat([entry.entryId])
            })
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

    // 面板收起时收掉菜单与键盘光标:否则下次呼出会残留菜单与点击屏障,
    // 键盘光标也会停在"上次那条"上 —— 用户按 ↑/Enter 时以为是第一条。
    Connections {
        target: panelWindow
        function onPanelHidden() {
            root.closeEntryMenu()
            root.kbIndex = -1
        }
    }

    // 列表行集变化(刷新/筛选/翻页)后把键盘光标夹回有效范围。
    // ⚠️ 用 onCountChanged 而不是 onRowsInserted:模型 reset 时插入/删除信号
    //    未必成对出现,而 count 一定跟着变 —— 这是唯一可靠的"行集已变"信号。
    Connections {
        target: Entries
        function onCountChanged() { root.clampKeyboardCursor() }
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
    // ---- 批量删除确认卡片(条目多选) ----
    // 复用 tagRenameCard 的"屏障 + 居中卡片"模式;删除不可逆,必须显式确认,
    // 且确认文案里**必须显示将删除的条数**(方案决策点 4:不设硬上限,
    // 但数量必须让用户在按下前看到)。
    MouseArea {
        id: bulkDeleteBarrier
        anchors.fill: parent
        z: 950
        visible: bulkDeleteCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: bulkDeleteCard.visible = false
    }

    Rectangle {
        id: bulkDeleteCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(340, root.width - 48)
        height: bulkDeleteLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: bulkDeleteLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: "删除 " + root.bulkDeleteCount + " 条条目?"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.error
            }
            Text {
                Layout.fillWidth: true
                text: "选中的条目将连同图片与待办数据一起删除,不可恢复。"
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"; size: "sm"
                    text: "取消"
                    onClicked: bulkDeleteCard.visible = false
                }
                ThemedButton {
                    objectName: "bulkDeleteConfirm"
                    variant: "danger"; size: "sm"
                    text: "删除"
                    onClicked: root.commitBulkDelete()
                }
            }
        }
    }

    // ---- 批量加标签卡片(条目多选) ----
    // 与批量删除同一模式;追加语义(add_tags_bulk 是"加"不是"覆盖"),
    // 已有同标签的条目不会重复。
    MouseArea {
        id: bulkTagBarrier
        anchors.fill: parent
        z: 950
        visible: bulkTagCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: bulkTagCard.visible = false
    }

    Rectangle {
        id: bulkTagCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(340, root.width - 48)
        height: bulkTagLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: bulkTagLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: "为 " + root.selectedIds.length + " 条条目加标签"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.textPrimary
            }
            Text {
                Layout.fillWidth: true
                text: "逗号分隔,可多个;是追加而不是覆盖,已有标签保留。"
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            TextField {
                id: bulkTagInput
                Layout.fillWidth: true
                placeholderText: "标签,多个用逗号分隔"
                placeholderTextColor: Theme.colors.textTertiary
                color: Theme.colors.textPrimary
                selectionColor: Theme.colors.accent
                selectedTextColor: Theme.colors.accentText
                font.pixelSize: Theme.metrics.fontSecondary
                background: Rectangle {
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: bulkTagInput.activeFocus ? Theme.colors.accent
                                                           : Theme.colors.border
                }
                Keys.onReturnPressed: root.commitBulkTag()
                Keys.onEnterPressed: root.commitBulkTag()
                Keys.onEscapePressed: bulkTagCard.visible = false
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"; size: "sm"
                    text: "取消"
                    onClicked: bulkTagCard.visible = false
                }
                ThemedButton {
                    objectName: "bulkTagConfirm"
                    variant: "primary"; size: "sm"
                    text: "添加"
                    onClicked: root.commitBulkTag()
                }
            }
        }
    }

    // ---- 清空已完成待办确认卡片 ----
    // 与批量删除同一模式(屏障 + 居中卡片):清空归档是老条目的**删除**,
    // 不可恢复,必须二次确认;文案显示将删除的条数(快照于打开时)。
    MouseArea {
        id: clearDoneBarrier
        anchors.fill: parent
        z: 950
        visible: clearDoneCard.visible
        enabled: visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onClicked: clearDoneCard.visible = false
    }

    Rectangle {
        id: clearDoneCard
        z: 960
        visible: false
        anchors.centerIn: parent
        width: Math.min(340, root.width - 48)
        height: clearDoneLayout.implicitHeight + 2 * Theme.metrics.spacingMd
        radius: Theme.metrics.radiusCard
        color: Theme.colors.surface
        border.width: Theme.metrics.borderWidth
        border.color: Theme.colors.borderStrong

        ColumnLayout {
            id: clearDoneLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.metrics.spacingMd
            spacing: Theme.metrics.spacingSm

            Text {
                Layout.fillWidth: true
                text: "清空 " + root.clearDoneCount + " 条已完成待办?"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.metrics.fontSection
                font.weight: Font.DemiBold
                color: Theme.colors.error
            }
            Text {
                Layout.fillWidth: true
                text: "已完成的待办将连同其数据一起删除,不可恢复;未完成的待办不受影响。"
                wrapMode: Text.Wrap
                font.pixelSize: Theme.metrics.fontCaption
                color: Theme.colors.textSecondary
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    variant: "secondary"; size: "sm"
                    text: "取消"
                    onClicked: clearDoneCard.visible = false
                }
                ThemedButton {
                    objectName: "clearDoneConfirm"
                    variant: "danger"; size: "sm"
                    text: "清空"
                    onClicked: root.commitClearDone()
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

            // 标题(docs/21):**第一行**,必填,Enter 提交。
            // ⚠️ 与下面的备注框语义相反 —— 备注多行、Enter 换行,写反了就会
            //    "打一句话就提交"或"按回车提交不了"(两处都踩过)。
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
                    // 验收用:断言"标题在备注上方"(2026-10-18 曾把两块写反)
                    objectName: "todoTitleInput"
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

            // 备注(docs/21):可多行、默认可空。标题是一句话,细节写这里。
            // 折叠态只占一行高度:卡片本身不大,常驻一个多行框会把时间/标签挤出视野;
            // 有内容或有焦点时自动展开,避免"写了却看不见"。
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    // ⚠️ 必须给 id:折叠状态 noteExpanded 定义在**这一行**上,
                    //    而下面的备注正文框 Rectangle 是 ColumnLayout 的子项(兄弟级),
                    //    它的 parent 是 ColumnLayout 而**不是**这个 RowLayout ——
                    //    原先写 parent.noteExpanded 取到 undefined。
                    //
                    //    实际后果**不是**"一直隐藏",而是**一直展开**:
                    //    QML 里 `visible: undefined` 属于**赋值失败**,visible 保持
                    //    默认值 true —— 于是"空备注时收起"这条设计整个失效(卡片永远
                    //    很高),同时每帧刷一条 "Unable to assign [undefined] to bool"。
                    //    (我最初按"恒 false"推断,被反向验证纠正 —— 见 progress.md)
                    id: noteHeader
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        text: "备注"
                        color: Theme.colors.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                    }
                    Text {
                        visible: !noteHeader.noteExpanded
                        text: (todoNoteInput.text.length > 0
                               ? "（" + todoNoteInput.text.split("\n").length + " 行）"
                               : "（可选，点右侧展开）")
                        // textFaint 是**不存在的令牌**(Theme.qml 里从没有它):
                        // 绑定失败 → 颜色取默认值,日志每帧刷 "Unable to assign
                        // [undefined] to QColor"。改回 textTertiary —— 与上方标题框的
                        // 占位符(2889 行"要做什么?")同一档,同类文字同一个色。
                        //
                        // 为什么不是"补一个更弱的 textFaint":浅色主题下 textTertiary
                        // (#8994A1 压白)本身只有 3.0:1,再弱一档必然跌破 3:1,
                        // 提示文字会糊成一片。弱化到不可读不叫层级,叫缺陷。
                        color: Theme.colors.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontCaption
                    }
                    Item { Layout.fillWidth: true }
                    // ⚠️ 不能用 ReadableCheckBox:它只在 SettingsDrawer.qml 内部定义,
                    //    不是模块级类型 —— 引用会整份 Main.qml 加载失败
                    //    ("ReadableCheckBox is not a type"),面板直接空白。
                    //    这里用主面板已有的 ThemedButton,状态靠属性 + 文案表达。
                    property bool noteExpanded:
                        todoNoteInput.text.length > 0 || todoNoteInput.activeFocus
                    ThemedButton {
                        id: todoNoteToggle
                        // 验收用(acceptanceItemVisible/clickByObjectName 按 objectName 查找;
                        // 只写 id 是找不到的 —— 这一条曾让"展开后备注仍不可见"的断言误报)
                        objectName: "todoNoteToggle"
                        text: noteHeader.noteExpanded ? "收起" : "展开"
                        variant: "ghost"
                        onClicked: {
                            // 收起时把焦点交还标题框,避免正文框仍持有隐式焦点却不可见
                            if (noteHeader.noteExpanded) todoInput.forceActiveFocus()
                            noteHeader.noteExpanded = !noteHeader.noteExpanded
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(todoNoteInput.implicitHeight + 16, 44)
                    // 走 id 而不是 parent:见 noteHeader 上的说明
                    visible: noteHeader.noteExpanded
                    color: Theme.colors.surfaceSecondary
                    radius: Theme.metrics.radiusInput
                    border.width: Theme.metrics.borderWidth
                    border.color: todoNoteInput.activeFocus
                                  ? Theme.colors.accent : Theme.colors.border
                    TextEdit {
                        id: todoNoteInput
                        // 验收用:与标题框一起校验**纵向顺序**
                        objectName: "todoNoteInput"
                        anchors.fill: parent
                        anchors.margins: 8
                        wrapMode: TextEdit.Wrap
                        color: Theme.colors.textPrimary
                        selectionColor: Theme.colors.accent
                        selectedTextColor: Theme.colors.accentText
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.metrics.fontBody
                        // ⚠️ 备注**不能**把 Enter 当提交:它是多行输入,
                        //    与标题框(Enter 提交)语义相反。
                        Keys.onReturnPressed: function(event) {
                            event.accepted = true
                            todoNoteInput.insert(todoNoteInput.cursorPosition, "\n")
                        }
                        Text {
                            visible: todoNoteInput.text === "" && !todoNoteInput.activeFocus
                            anchors.top: parent.top
                            anchors.left: parent.left
                            text: "补充说明、清单、链接…"
                            color: Theme.colors.textTertiary   // 同上:不存在 textFaint
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.metrics.fontBody
                        }
                    }
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
