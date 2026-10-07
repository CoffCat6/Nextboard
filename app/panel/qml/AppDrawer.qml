// 右侧抽屉容器:设置 / 翻译共用,带滑入动画。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Drawer {
    id: root
    edge: Qt.RightEdge
    width: parent.width * 0.46
    height: parent.height
    modal: false
    interactive: false
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.colors.surface
        border.color: Theme.colors.border
        border.width: Theme.metrics.borderWidth
    }

    enter: Transition {
        NumberAnimation { property: "position"; to: 1.0; duration: Theme.animation.normal; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "position"; to: 0.0; duration: Theme.animation.fast; easing.type: Easing.InCubic }
    }
}
