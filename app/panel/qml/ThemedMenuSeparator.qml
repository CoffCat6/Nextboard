// 主题化菜单分隔线:Basic 的 MenuSeparator 颜色取 palette.mid(未注入),
// 这里固定走 Theme.colors.border。
//
// ⚠️ 不设 implicitWidth:QQuickMenuPrivate::resizeItem() 会把每个条目
//    拉伸到菜单内容宽(qquickmenu.cpp:912 `item->setWidth(contentItem->width())`),
//    分隔线自然铺满整行。原先写死 160 会让任何窄菜单(如紧凑的文本编辑菜单)
//    被强行撑到 160px —— 那是无意的宽度地板,不是设计值。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

MenuSeparator {
    id: control

    padding: 0
    verticalPadding: 4

    contentItem: Rectangle {
        implicitHeight: 1
        color: Theme.colors.border
    }

    background: Item {}
}
