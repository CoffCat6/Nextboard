// 右侧抽屉容器:设置 / 翻译共用,带滑入动画。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Drawer {
    id: root
    edge: Qt.RightEdge
    // 设置项较多(通用/数据/贴图/快捷键/翻译服务/代理),占比过小会让字段行频繁折行
    width: parent.width * 0.52
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
