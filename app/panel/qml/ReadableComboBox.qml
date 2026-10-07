// 主题化下拉框:显式接管 popup / 委托 / 调色板三处。
//
// 为什么不能只改 contentItem:下拉选项渲染在 Popup(独立窗口)里,走 Basic
// 默认委托(ItemDelegate → palette.text),而弹窗内的调色板不保证跟随应用
// 主题 —— 实测浅色主题下白字、深色主题下深字,全部不可读。
// 这里三处一起钉死:palette 组整体对齐 Theme(兜住任何 palette 派生色)、
// 委托文字显式 textPrimary、弹窗底板显式 surface。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

ComboBox {
    id: control

    // 调色板组整体对齐主题:Basic 内部任何 palette 派生色都跟随
    palette.text: Theme.colors.textPrimary
    palette.windowText: Theme.colors.textPrimary
    palette.buttonText: Theme.colors.textPrimary
    palette.window: Theme.colors.surface
    palette.base: Theme.colors.surface
    palette.button: Theme.colors.surface
    palette.highlight: Theme.colors.accent
    palette.highlightedText: Theme.colors.accentText

    contentItem: Text {
        text: control.displayText
        font: control.font
        color: Theme.colors.textPrimary
        verticalAlignment: Text.AlignVCenter
        leftPadding: 10
        rightPadding: control.indicator ? control.indicator.width + control.spacing : 0
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: Theme.colors.surface
        border.color: control.activeFocus ? Theme.colors.accent : Theme.colors.border
        border.width: Theme.metrics.borderWidth
        radius: Theme.metrics.radiusInput
    }

    delegate: ItemDelegate {
        id: itemRoot
        required property var modelData
        width: control.width
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: itemRoot.modelData !== undefined ? String(itemRoot.modelData) : ""
            font: control.font
            color: Theme.colors.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: itemRoot.hovered || itemRoot.highlighted
                   ? Theme.colors.surfaceHover : Theme.colors.surface
        }
    }

    popup: Popup {
        y: control.height
        width: control.width
        padding: 1
        background: Rectangle {
            color: Theme.colors.surface
            border.color: Theme.colors.border
            border.width: Theme.metrics.borderWidth
            radius: Theme.metrics.radiusInput
        }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
    }
}
