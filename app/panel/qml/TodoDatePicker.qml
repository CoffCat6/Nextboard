// 待办日期时间选择器(docs/05-todo.md §6.2):
//   左列 = 年月日(三段 Tumbler:年/月/日),右列 = 时/分(两段 Tumbler);
//   顶部一排快捷钮(今天 18:00 / 明天 09:00 / +1 小时 / 清空)覆盖高频场景;
//   底部实时回显 + 「确定」按钮。
//
// ⚠️ 月份换算口径(全文件只此一条,改动须两处同步):
//    values 一律**数值本身**(月=1..12、日=1..n),与 JS 的 0 基月份之间只在
//    "进/出 new Date" 两处换算 —— 预填 setFromMs 用 getMonth()+1,
//    重组 rebuildFromTumblers 用 currentValue-1,日列永远用日期号本身。
//    首版三处口径互相打架("预填偏一个月 + 选完偏一天"),是时间设定错乱的根源。
//
// ⚠️ 整个预填/清空过程必须包在 syncing 里:setCurrent 会逐个触发
//    onCurrentIndexChanged → rebuildFromTumblers,若只包住 selectedMs 赋值,
//    "年已定月未定"的中间组合会一路 changed() 出去,把目标字段(乃至提醒的
//    默认联动)写成中间值 —— 时间设定乱跳的第二个根源。
//
// ⚠️ 交互契约(第三轮修订):滚轮/快捷钮改动**只更新本组件内部状态与回显**,
//    **不**写回目标字段 —— 写回统一在用户点「确定」时经 accepted(ms) 发生。
//    (第二轮的"实时写回"在按钮命中缺陷期间是保命设计;按钮修复后回归
//    "确定才写回":拨到一半点取消/外部不会污染字段,且截止的提醒联动
//    只在确定时算一次,不会随预填滚归位而误触发。)
//    「确定」= accepted(commit());「取消」= cancelled()。
//
// ⚠️ 取选中值必须用 valueAt(values[currentIndex]),**不是** Tumbler.currentValue
//    (那是 ComboBox 的 API,Tumbler 没有它)—— 详见 valueAt 注释。
//    首版读 currentValue 恒为 undefined,rebuildFromTumblers 永远提前返回,
//    拨滚轮从不产生有效值 —— "只有快捷钮能用"的根因。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Nextboard.panel

Rectangle {
    id: picker

    // ---- 对外契约 ----
    /// 当前选中时刻(unix 毫秒);清空态为 0。外部只读。
    readonly property real currentMs: selectedMs
    /// 是否处于"未设置"状态。
    readonly property bool cleared: selectedMs === 0
    /// 选择变化信号(仅"确定"或快捷钮等明确动作时发出;预填不发)。
    signal changed(real ms)
    /// 用户点「确定」:外部把 currentMs 写回目标字段并收起浮层。
    signal accepted(real ms)
    /// 用户点「取消」:外部直接收起浮层(丢弃本次拨动)。
    signal cancelled()

    /// 预填(毫秒;0/null = 清空态)。内部按值换算各 Tumbler 的 currentIndex。
    /// ⚠️ Tumbler 没有 `positionAt()`(那是 ListView 的);定位选中项只能给
    ///    `currentIndex` 赋下标 —— 值 → 下标的换算靠 values 数组 indexOf。
    function setFromMs(ms) {
        // 每次打开选择器都会走这里 → 顺带刷新年份表(见 yearTumbler 的注释:
        // 年份不该是"首次求值就固定"的绑定,否则跨年运行的实例会拿着旧年份)。
        yearTumbler.refreshYears()
        syncing = true
        if (!ms || ms <= 0) {
            selectedMs = 0
            positionAtNow()
        } else {
            const d = new Date(ms)
            setCurrent(yearTumbler, d.getFullYear())
            setCurrent(monthTumbler, d.getMonth() + 1)
            setCurrent(dayTumbler, d.getDate())
            setCurrent(hourTumbler, d.getHours())
            setCurrent(minuteTumbler, d.getMinutes())
            selectedMs = ms
        }
        syncing = false
        // 预填不发 changed(回显与联动由用户后续操作/确定驱动)
    }

    /// 清空(快捷钮「清空」)。滚轮归位到"现在"作为托底位置。
    function clear() {
        syncing = true
        selectedMs = 0
        positionAtNow()
        syncing = false
        changed(0)
    }

    /// 把手轮当前位置作为一个确定值交出去(外部「确定」按钮调用)。
    /// 与 changed 的区别:changed 是"值变了"的通知,本函数是"就用这个值"的命令。
    function commit() {
        rebuildFromTumblers()
        return selectedMs
    }

    /// 滚轮归位到"现在"。
    /// ⚠️ 为什么必须有:空值打开时若把滚轮留在默认下标(3 年前),用户接下来
    ///    拨的两下多半是在纠年份而不是选时间。
    function positionAtNow() {
        const now = new Date()
        setCurrent(yearTumbler, now.getFullYear())
        setCurrent(monthTumbler, now.getMonth() + 1)
        setCurrent(dayTumbler, now.getDate())
        setCurrent(hourTumbler, now.getHours())
        setCurrent(minuteTumbler, now.getMinutes())
    }

    /// **拨滚轮到目标时刻**(验收用,等价用户逐列拨动):只改"时"列的
    /// currentIndex —— 与用户拨轮同链路(onCurrentIndexChanged →
    /// rebuildFromTumblers → changed),不走 setFromMs 的预填路径。
    /// 返回拨动后的内部选中值(仅供断言;写回仍由「确定」负责)。
    function spinHourTo(ms) {
        const target = new Date(ms)
        const idx = hourTumbler.values.indexOf(target.getHours())
        if (idx >= 0) hourTumbler.currentIndex = idx
        return selectedMs
    }

    /// 取某列当前选中的值。
    /// ⚠️ **不能用 `Tumbler.currentValue`** —— 那是 ComboBox 的 API,Tumbler 没有;
    ///    读它恒为 undefined,导致 rebuildFromTumblers 的 undefined 守卫**永远
    ///    提前返回**:拨滚轮从不发 changed、字段永远收不到值,只有快捷钮
    ///    (自带显式 changed)有效 —— 正是"拨轮设不上时间"的根因。
    ///    标准取法:model 就是我们的 values 数组,按 currentIndex 取下标。
    function valueAt(w) {
        const i = w.currentIndex
        return (i >= 0 && i < w.values.length) ? w.values[i] : undefined
    }

    /// 值 → 下标:Tumbler 的 model 是**数值数组**,按数值匹配;找不到给 0
    /// (首项),避免 -1 空选。
    function setCurrent(w, value) {
        const idx = w.values.indexOf(value)
        w.currentIndex = idx >= 0 ? idx : 0
    }

    // ---- 内部状态 ----
    // selectedMs 是唯一事实源;syncing 抑制预填过程中的连锁重算与 changed 外发。
    property real selectedMs: 0
    property bool syncing: false

    implicitWidth: 300
    implicitHeight: column.implicitHeight + 2 * Theme.metrics.spacingSm
    radius: Theme.metrics.radiusInput
    color: Theme.colors.surfaceSecondary
    border.width: Theme.metrics.borderWidth
    border.color: Theme.colors.border

    function daysInMonth(y, m) {
        // m: 0..11(JS 月份);new Date(y, m+1, 0).getDate() = 该月天数(含闰年)
        return new Date(y, m + 1, 0).getDate()
    }

    // 由 Tumbler 组合出时刻;非法组合(如 2 月 30 日)由日列 model 收敛,不会出现。
    // ⚠️ 列的 values 都是**数值**:年=年份、月=1..12、日=1..n、时/分=自身。
    //    取值用 valueAt(values[currentIndex]) —— 月进 new Date 前要 -1 转 0 基,
    //    日**不再加减**(首版 +1 把选中的日子整体推后一天)。
    // ⚠️ 任何一列取不到值就直接返回:**不能**把 selectedMs 清 0 —— 用户拨到一半
    //    的状态不是"未设置",此时点「确定」应落在最后有效的组合上。
    function rebuildFromTumblers() {
        if (syncing) return
        const y = valueAt(yearTumbler)
        const m = valueAt(monthTumbler)
        const d = valueAt(dayTumbler)
        const h = valueAt(hourTumbler)
        const mi = valueAt(minuteTumbler)
        if (y === undefined || m === undefined || d === undefined
                || h === undefined || mi === undefined) return
        const ms = new Date(y, m - 1, d, h, mi).getTime()
        syncing = true
        selectedMs = ms
        syncing = false
        changed(ms)
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.metrics.spacingSm
        spacing: Theme.metrics.spacingSm

        // ---- 快捷钮(高频场景一次点击) ----
        Row {
            Layout.alignment: Qt.AlignHCenter
            spacing: Theme.metrics.spacingXs

            component QuickButton: Rectangle {
                id: qb
                property string label: ""
                signal picked()
                width: qbLabel.implicitWidth + 12
                height: 20
                radius: Theme.metrics.radiusBadge
                color: qbMa.containsMouse ? Theme.colors.accentSoft : Theme.colors.surface
                border.width: Theme.metrics.borderWidth
                border.color: Theme.colors.border
                Text {
                    id: qbLabel
                    anchors.centerIn: parent
                    text: qb.label
                    font.pixelSize: Theme.metrics.fontCaption
                    color: qbMa.containsMouse ? Theme.colors.accent : Theme.colors.textSecondary
                }
                MouseArea {
                    id: qbMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: qb.picked()
                }
            }

            QuickButton {
                label: "今天 18:00"
                onPicked: {
                    const d = new Date()
                    d.setHours(18, 0, 0, 0)
                    picker.setFromMs(d.getTime())
                    picker.changed(d.getTime())
                }
            }
            QuickButton {
                label: "明天 09:00"
                onPicked: {
                    const d = new Date()
                    d.setDate(d.getDate() + 1)
                    d.setHours(9, 0, 0, 0)
                    picker.setFromMs(d.getTime())
                    picker.changed(d.getTime())
                }
            }
            QuickButton {
                label: "+1 小时"
                onPicked: {
                    const t = (picker.selectedMs > 0 ? picker.selectedMs : Date.now()) + 3600 * 1000
                    picker.setFromMs(t)
                    picker.changed(t)
                }
            }
            QuickButton {
                label: "清空"
                onPicked: picker.clear()
            }
        }

        // ---- 五列 Tumbler:年 / 月 / 日 | 时 / 分 ----
        Row {
            Layout.alignment: Qt.AlignHCenter
            spacing: 2

            component Wheel: Tumbler {
                id: wheel
                property string caption: ""
                property var values: []
                visibleItemCount: 5
                wrap: false
                implicitWidth: 52
                implicitHeight: 110
                model: values
                // 选中项高亮(displacement=0),其余按距离衰减
                delegate: Text {
                    required property var modelData
                    required property int index
                    // 时/分两位补零;年/月/日直接显示数值
                    text: wheel.caption === "时" || wheel.caption === "分"
                          ? (modelData < 10 ? "0" + modelData : String(modelData))
                          : String(modelData)
                    font.pixelSize: Theme.metrics.fontSecondary
                    font.family: Theme.fontMono
                    color: index === wheel.currentIndex ? Theme.colors.textPrimary
                         : Theme.colors.textTertiary
                    opacity: 1.0 - Math.abs(Tumbler.displacement) / (wheel.visibleItemCount / 2)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Wheel {
                id: yearTumbler
                caption: "年"
                // ⚠️ 年份表**一次性算好并存在属性里**(2026-11-03,外部审查 #37)。
                //    原先直接在 values 绑定里 new Date() —— new Date() 不产生绑定
                //    依赖,该绑定只在首次读取时求值一次,跨年运行的实例会一直拿着
                //    旧年份表(靠 ±3 年余量兜底,约 3 年后露出问题)。
                //    现在没有响应式依赖了,把"什么时候重算"写成明确契约:
                //    每次 setFromMs(打开选择器)调用 refreshYears() 重算一次。
                property var yearValues: []
                values: yearValues
                function refreshYears() {
                    const y = new Date().getFullYear()
                    const arr = []
                    for (let i = y - 3; i <= y + 3; ++i) arr.push(i)
                    yearValues = arr
                }
                Component.onCompleted: refreshYears()
                onCurrentIndexChanged: picker.rebuildFromTumblers()
            }
            Wheel {
                id: monthTumbler
                caption: "月"
                values: {
                    const arr = []
                    for (let i = 1; i <= 12; ++i) arr.push(i)
                    return arr
                }
                onCurrentIndexChanged: picker.rebuildFromTumblers()
            }
            Wheel {
                id: dayTumbler
                caption: "日"
                values: {
                    const y = valueAt(yearTumbler)
                    const m = valueAt(monthTumbler)
                    const yReal = (y === undefined) ? new Date().getFullYear() : y
                    // 月列 values 是 1..12,daysInMonth 的参数是 0 基(JS 月份)
                    const mReal = (m === undefined) ? new Date().getMonth() : m - 1
                    const n = picker.daysInMonth(yReal, mReal)
                    const arr = []
                    for (let i = 1; i <= n; ++i) arr.push(i)
                    return arr
                }
                onCurrentIndexChanged: picker.rebuildFromTumblers()
                // 月/年变化后"日"的 model 重建:越界的 currentIndex 由 Tumbler 收敛
                // 到有效范围(如 31 日切到 2 月 → 28/29)。
            }
            // 分隔线:日期与时间的视觉分组
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                width: Theme.metrics.borderWidth; height: 96
                color: Theme.colors.border
            }
            Wheel {
                id: hourTumbler
                caption: "时"
                values: {
                    const arr = []
                    for (let i = 0; i < 24; ++i) arr.push(i)
                    return arr
                }
                onCurrentIndexChanged: picker.rebuildFromTumblers()
            }
            Wheel {
                id: minuteTumbler
                caption: "分"
                values: {
                    const arr = []
                    for (let i = 0; i < 60; ++i) arr.push(i)
                    return arr
                }
                onCurrentIndexChanged: picker.rebuildFromTumblers()
            }
        }

        // ---- 实时回显(空态提示) ----
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: picker.cleared
                  ? "未设置"
                  : Qt.formatDateTime(new Date(picker.selectedMs), "yyyy-MM-dd HH:mm")
            font.pixelSize: Theme.metrics.fontCaption
            color: picker.cleared ? Theme.colors.textTertiary : Theme.colors.textSecondary
        }

        // ---- 动作行:确定 / 取消 ----
        // ⚠️ 必须放在 **TodoDatePicker 内部**,不能作为 Popup contentItem 的兄弟项:
        //    首版把这对按钮放在 contentItem 的 ColumnLayout 里(与 picker 平级),
        //    实测点击无效 —— 探针显示该位置的最顶层项是 Popup 的默认 background
        //    Rectangle,而不是按钮(按钮的 mapToScene 坐标与实际渲染位置错位)。
        //    放进 picker 自己的布局后与滚轮/快捷钮同一子树,命中正常。
        Rectangle {
            objectName: "todoPickerActionBar"
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: Theme.colors.surface
            radius: Theme.metrics.radiusInput
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.metrics.spacingSm
                anchors.rightMargin: Theme.metrics.spacingSm
                spacing: Theme.metrics.spacingSm
                Item { Layout.fillWidth: true }
                ThemedButton {
                    objectName: "todoPickerCancel"
                    variant: "secondary"
                    size: "sm"
                    text: "取消"
                    onClicked: picker.cancelled()
                }
                ThemedButton {
                    objectName: "todoPickerConfirm"
                    variant: "primary"
                    size: "sm"
                    text: "确定"
                    onClicked: picker.accepted(picker.commit())
                }
            }
        }
    }

    // selectedMs 变化时按需外发(预填/清空期间由 syncing 抑制)
    onSelectedMsChanged: if (!syncing) changed(selectedMs)
}
