// 文本编辑右键菜单:替换 Basic 风格默认挂的 TextEditingContextMenu。
//
// 为什么要替换(Basic 的 TextArea.qml:27 挂的是
// QtQuick.Controls.Basic.impl.TextEditingContextMenu):
//   1) **风格不统一** —— 那是 Basic 原生 Menu/MenuItem,不走本项目的
//      ThemedMenu/ThemedMenuItem 令牌,和条目右键菜单明显不是一套;
//   2) **字体偏大** —— 用应用字体(14px)与 Basic 默认行高,而本项目紧凑
//      菜单的基准是 12px(与托盘菜单同档);
//   3) **标签是英文** —— 它用的是 QtQuick.Controls.impl 的 *Action
//      (Cut/Copy/Paste/…),而本应用 windeployqt 带 --no-translations,
//      qsTr 不会被翻译,于是中文界面里冒出英文菜单项。
//
// 实现要点:
//   · 用 ContextMenu 附加属性整体换掉默认菜单(见 TranslateView 的用法);
//   · 菜单项**每次展开前重建**(与 Main.qml 条目右键菜单同一套路):
//     enabled 依赖"有无选中 / 可否撤销 / 剪贴板有无内容",这些只在该点
//     右键那一刻才算数;
//   · 只读控件(译文输出框)只给「复制 / 全选」,不摆一排置灰项。
//
// ⚠️ 挂用法必须是 `ContextMenu.menu: TextEditMenu { target: xxx }`,
//    且 Menu 不能带 id(Qt 文档:会破坏 ContextMenu 的懒加载优化)。
//    本组件内部有 id 是允许的 —— 和 Basic 自带的 TextEditingContextMenu 同构。
//
// 只 import QtQuick 即可:ThemedMenu / ThemedMenuItem / ThemedMenuSeparator 是
// 同目录组件,Component 来自 QtQml(默认可用);无需 QtQuick.Controls.Basic。
import QtQuick
import Nextboard.panel

ThemedMenu {
    id: root

    /// 目标文本控件(TextArea / TextField),由使用方按 id 注入。
    required property var target

    /// 菜单宽度:给一个确定值,不跟随内容。条目是 2 字中文标签
    /// (剪切/复制/全选…),紧凑项已把宽度下限放到 0(见 ThemedMenuItem.compact),
    /// 若不给确定宽度,菜单会被压到 ~58px 而显得局促。
    /// 100px 与托盘菜单(118px)同一量级,比条目右键菜单(200px)明显紧凑。
    minWidth: 100

    Component { id: itemComp; ThemedMenuItem {} }
    Component { id: sepComp; ThemedMenuSeparator {} }

    /// ⚠️ 函数名不能叫 `addItem` —— Menu 自身有 C++ 方法 `addItem(item)`,
    ///    同名会互相遮蔽:函数体里再调 `addItem(...)` 就变成递归调用自己,
    ///    表现为 createObject 拿到的对象异常、报
    ///    "Could not set initial property text" / "target is not a function"。
    ///    Main.qml 的条目右键菜单同样用 `addAction` / `addSep` 命名,理由相同。
    function addAction(label, enabled, fn) {
        const it = itemComp.createObject(root, { text: label, enabled: enabled, compact: true })
        it.triggered.connect(fn)
        root.addItem(it)
    }

    function addSep() {
        root.addItem(sepComp.createObject(root))
    }

    onAboutToShow: rebuild()

    function rebuild() {
        while (root.count > 0) root.takeItem(0).destroy()

        const t = root.target
        if (!t) return

        const editable = !t.readOnly
        const hasSelection = t.selectedText.length > 0
        const hasText = t.length > 0

        if (editable) {
            addAction("撤销", t.canUndo === true, function () { t.undo() })
            addAction("重做", t.canRedo === true, function () { t.redo() })
            addSep()
            addAction("剪切", hasSelection, function () { t.cut() })
            addAction("复制", hasSelection, function () { t.copy() })
            // canPaste 由 TextEdit 维护(监听剪贴板 dataChanged),比"永远置亮"准确
            addAction("粘贴", t.canPaste === true, function () { t.paste() })
            addAction("删除", hasSelection,
                      function () { t.remove(t.selectionStart, t.selectionEnd) })
            addSep()
        } else {
            // 只读(译文输出框):只保留读取类操作,不摆置灰的编辑项
            addAction("复制", hasSelection, function () { t.copy() })
            addSep()
        }
        addAction("全选", hasText, function () { t.selectAll() })
    }
}
