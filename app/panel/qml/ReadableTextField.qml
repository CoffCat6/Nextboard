// 主题化单行文本框(数据目录 / 接口地址 / API Key / Token / 模型名 /
// 响应取值路径 / 代理主机与账号 / 十六进制色值…)。
//
// 全部是**技术字段**,所以统一用等宽字体:URL 与 JSON 里的字符
// (0/O、1/l/I、{}[])在等宽下才对得齐、肉眼可核对。中文回退由 C++ 侧
// app_font.hpp 的替换表钉到雅黑,不会掉宋体。
import QtQuick
import QtQuick.Controls.Basic
import Nextboard.panel

TextField {
    id: control

    color: Theme.colors.textPrimary
    font.family: Theme.fontMono
    placeholderTextColor: Theme.colors.textTertiary
    selectionColor: Theme.colors.accent
    selectedTextColor: Theme.colors.accentText
    background: Rectangle {
        color: Theme.colors.surface
        border.color: control.activeFocus ? Theme.colors.accent : Theme.colors.border
        border.width: Theme.metrics.borderWidth
        radius: Theme.metrics.radiusInput
    }
}
