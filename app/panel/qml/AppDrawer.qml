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

    // ---- 抽屉交互(三条 closePolicy 路径全部打通,modal 拦截穿透) ----
    // ⚠️ modal:true 是**必须的**:modal:false 时抽屉只画在 overlay 层却不拦输入,
    //    点在抽屉外的列表区域会直接落到列表条目上(误触复制/误删,用户实测反馈)。
    //    modal:true 后,QQuickDrawerPrivate::blockInput 会把抽屉外的按下吞掉。
    modal: true
    // ⚠️ interactive 不能关!它不只管拖拽手势 —— Qt 把**所有** closePolicy 路径
    //    都挂在它后面(qquickpopup.cpp:tryClose 首行 `if (!interactive) return false`,
    //    keyPressEvent 里 Esc 也是 `if (interactive) closeOrReject()`)。
    //    此前 interactive:false → Esc 被吞掉但不关、点外面也永不关,只剩 ✕ 一条路,
    //    而 ✕ 还在滚动内容顶部,必须滑回顶才点得到(用户实测反馈)。
    interactive: true
    // 拖拽手势:关闭方向(抽屉已打开 + 横向拖动)保留,当作"甩走"的快捷;
    // 打开方向(dragMargin 边缘拖出)归零 —— 桌面端从面板右缘误拖出抽屉是纯干扰。
    dragMargin: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // 打开即拿焦点:Esc 不必先点进抽屉内某个输入框才生效(否则刚打开时
    // 键盘焦点还在主面板上,Esc 会被 modal 层直接吞掉、到不了抽屉)。
    focus: true

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
