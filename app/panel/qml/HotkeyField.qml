// 快捷键录制控件:点击进入录制态,按下组合键即捕获。
// 产出形如 "Ctrl+Shift+V" 的字符串;合法性(至少一个修饰键、各动作两两不重复、
// 不可为空)由 core 的 settings 校验负责 —— 这里不做客户端校验,直接展示后端错误。
import QtQuick
import Nextboard.panel

Item {
    id: hk

    property string value: ""
    property bool recording: false
    /// 录制完成、值真的变了才发(用于触发自动保存与热键重注册)。
    signal valueModified()
    implicitHeight: Theme.metrics.controlHeight

    function keyName(key) {
        if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(key)
        if (key >= Qt.Key_0 && key <= Qt.Key_9) return String.fromCharCode(key)
        if (key >= Qt.Key_F1 && key <= Qt.Key_F24) return "F" + (key - Qt.Key_F1 + 1)
        return ""
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.metrics.radiusInput
        color: Theme.colors.surfaceSecondary
        border.width: Theme.metrics.borderWidth
        border.color: hk.recording ? Theme.colors.accent : Theme.colors.border
        Behavior on border.color { ColorAnimation { duration: Theme.animation.fast } }
        Text {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            // 技术字段:"Ctrl+Shift+V" 这类组合键在等宽下各段严格对齐,
            // 不同动作之间横向可比(一眼看出哪个键位冲突)
            font.family: Theme.fontMono
            font.pixelSize: Theme.metrics.fontCaption
            color: hk.recording ? Theme.colors.accent
                 : hk.value === "" ? Theme.colors.textSecondary : Theme.colors.textPrimary
            text: hk.recording ? "按下组合键…(Esc 取消)" : (hk.value || "未设置")
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            hk.recording = true
            hk.forceActiveFocus()
        }
    }

    Keys.onPressed: function(event) {
        if (!hk.recording) return
        if (event.key === Qt.Key_Escape) {
            hk.recording = false
            event.accepted = true
            return
        }
        // Delete / Backspace = 清空绑定(允许"不设置快捷键",该动作就不注册)
        if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            hk.recording = false
            if (hk.value !== "") {
                hk.value = ""
                hk.valueModified()
            }
            event.accepted = true
            return
        }
        // 只按修饰键时继续等待主键
        if (event.key === Qt.Key_Control || event.key === Qt.Key_Shift
                || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta) {
            event.accepted = true
            return
        }
        const name = hk.keyName(event.key)
        if (name === "") {
            event.accepted = true
            return
        }
        const parts = []
        if (event.modifiers & Qt.ControlModifier) parts.push("Ctrl")
        if (event.modifiers & Qt.ShiftModifier) parts.push("Shift")
        if (event.modifiers & Qt.AltModifier) parts.push("Alt")
        if (event.modifiers & Qt.MetaModifier) parts.push("Win")
        parts.push(name)
        const next = parts.join("+")
        const changed = next !== hk.value
        hk.value = next
        hk.recording = false
        if (changed) hk.valueModified()
        event.accepted = true
    }
}
