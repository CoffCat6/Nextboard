// 字段标签:与控件直接交互的引导文字,暗色下用 Secondary 的提亮值。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

Label {
    font.family: Theme.fontFamily
    color: Theme.colors.textSecondary
    font.pixelSize: Theme.metrics.fontCaption
}
