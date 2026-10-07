// 主题化下拉框:显式接管 palette / contentItem / background / 委托 / 弹窗五处。
//
// 为什么需要这个组件:Basic 的 ComboBox 各处取色走 QPalette 派生,而**下拉弹窗是
// 独立窗口**,其调色板不继承应用/父项 —— 实测浅色主题下出现"白底白字"。
// 这里把调色板与每个可见元素都钉到 Theme 令牌上,并显式把调色板交给弹窗。
//
// 与 Qt 官方 ComboBox.qml 一致:委托里使用外层 id + required 属性时,
// 必须显式声明为 Bound 组件,否则依赖已废弃的"上下文捕获"
// (qmllint 对每条外层 id 引用都会给出 Info 级建议)。
pragma ComponentBehavior: Bound

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

    // 委托对齐 Qt 官方 Basic 实现(qml/QtQuick/Controls/Basic/ComboBox.qml)。
    // ComboBox 的 delegateModel 以 **required 属性**提供 `index`。
    //
    // ⚠️ 三个踩过的坑(均有运行时日志为证):
    // 1. 声明 `required property var modelData` —— 名字不存在;一旦声明了 required
    //    属性,`index` 就必须**同样声明**才注入,否则 "ReferenceError: index is not defined";
    // 2. 声明 `required property var model` 后直接用 `text: model` —— model 实际是
    //    **QQmlDMListAccessorData 访问器对象**而非模型项,赋给 QString 会失败:
    //    "Unable to assign QQmlDMListAccessorData to QString",表现为下拉项有底色但无文字;
    // 3. 因此文案不能靠拼 `model[...]`,改用 ComboBox 自带的取值 API
    //    `textAt(index)`(Q_INVOKABLE,与关闭态显示的 displayText 同一套逻辑,
    //    自动兼顾 textRole 与纯字符串 model)。
    delegate: ItemDelegate {
        id: itemRoot
        required property int index
        // 与官方一致:宽度取自所在 ListView(弹窗有 1px padding,用 control.width 会横向溢出)
        width: ListView.view ? ListView.view.width : control.width
        text: control.textAt(itemRoot.index)
        highlighted: control.highlightedIndex === itemRoot.index
        contentItem: Text {
            text: itemRoot.text
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
        // 弹层从下拉框正下方展开,高度收敛到窗口内 —— 否则长列表会跑出屏幕,底部选项点不到
        height: Math.min(contentItem.implicitHeight,
                         (control.Window ? control.Window.height : 600) - control.height - 16)
        padding: 1
        // 弹窗是**独立窗口**,不会自动继承应用/父项调色板 —— 官方实现同样是
        // 显式 `palette: control.palette`,否则浅色主题下会出现"白底白字"。
        palette: control.palette
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
