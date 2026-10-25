// 主题化数值框:Basic 控件深色可读性覆盖。
//
// ⚠️ 手输提交的关键约束(踩过的坑,勿删):这里**不要**写
//    `onEditingFinished: parent.value = valueFromText(...)`。
//    手输提交由 QQuickSpinBox 原生路径负责(回车键释放 / 失焦 → updateValue),
//    它以 modified=true 提交并发射 valueModified,自动保存才被触发;
//    QML 侧的程序化赋值会抢先写入同一个值,原生路径随后认为"值没变"而不再
//    发射 valueModified —— 保存被静默吞掉(qquickspinbox.cpp:setValue 的
//    modified 标志仅在 value != correctedValue 时才发 valueModified)。
//    实测症状:手输数值关掉抽屉再打开又变回去,只有 +/- 加减能保存。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

SpinBox {
    id: control

    contentItem: TextInput {
        // 用 parent.* 而非 id 访问:与原 SettingsDrawer 内联版同一写法(生产验证过),
        // 且避免 qmllint 对 `control.textFromValue` 报 use-proper-function
        // (textFromValue 是 QJSValue 属性,qmllint 静态解析 id 时会挑刺,parent 路径不触发)。
        text: parent.textFromValue(parent.value, parent.locale)
        validator: parent.validator
        font: parent.font
        color: Theme.colors.textPrimary
        selectionColor: Theme.colors.accent
        selectedTextColor: Theme.colors.accentText
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        readOnly: !parent.editable
        selectByMouse: true
    }
    background: Rectangle {
        color: Theme.colors.surface
        border.color: control.activeFocus ? Theme.colors.accent : Theme.colors.border
        border.width: Theme.metrics.borderWidth
        radius: Theme.metrics.radiusInput
    }
}
