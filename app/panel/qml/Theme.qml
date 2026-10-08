// 全局主题(docs/theme.md §44-46):colors / metrics / animation 三组令牌,
// 由 Core.darkTheme 驱动亮/暗即时切换。
//
// settings.theme("system"/"light"/"dark")由 C++ 侧解析(含系统配色探测)并写入
// Core.darkTheme;Basic 风格控件的配色走 QPalette,由 CoreFacade::applyTheme
// 注入(值与 colors 对齐),只改这里是不够的。
pragma Singleton
import QtQuick
import Nextboard.Core 1.0

QtObject {
    readonly property bool dark: Core.darkTheme

    /// 全局字体族(中文由 Qt 的回退链自动落到 Microsoft YaHei UI)。
    /// 与 C++ 侧 QApplication::setFont 保持一致;QML 里显式声明族名是为了
    /// 不依赖"应用字体是否已被设置",避免不同引擎实例表现不一致。
    readonly property string fontFamily: "Segoe UI"

    // ---- 色板(theme.md §7/§8) ----
    readonly property QtObject colors: QtObject {
        readonly property color background:       dark ? "#0E1217" : "#F4F6F8"
        readonly property color surface:          dark ? "#151A21" : "#FFFFFF"
        readonly property color surfaceSecondary: dark ? "#1A2028" : "#F8FAFC"
        readonly property color surfaceHover:     dark ? "#202731" : "#EEF2F6"
        readonly property color surfaceSelected:  dark ? "#192E4B" : "#E8F1FF"

        readonly property color border:       dark ? "#29323C" : "#D9E0E7"
        readonly property color borderStrong: dark ? "#37424E" : "#C7D0DA"

        readonly property color textPrimary:   dark ? "#F1F4F7" : "#17202A"
        readonly property color textSecondary: dark ? "#C2CBD4" : "#5E6A78"
        // Tertiary 只用于装饰性弱文本(占位/图标),暗色较文档原值提亮一档保可读
        readonly property color textTertiary:  dark ? "#8D99A6" : "#8994A1"
        readonly property color textDisabled:  dark ? "#4D5762" : "#B6BEC8"

        readonly property color accent:        dark ? "#4C9AFF" : "#3B82F6"
        readonly property color accentHover:   dark ? "#6AA8FF" : "#2563EB"
        readonly property color accentPressed: dark ? "#3478D4" : "#1D4ED8"
        readonly property color accentSoft:    dark ? "#172B46" : "#E8F1FF"
        /// 强调色之上的文字(按钮/选中徽标)。
        readonly property color accentText:    "#FFFFFF"

        readonly property color success: dark ? "#4ADE80" : "#16A34A"
        readonly property color warning: dark ? "#FBBF24" : "#D97706"
        readonly property color error:   dark ? "#F87171" : "#DC2626"
        /// 警告色之上的文字(暗色琥珀配深字,浅色琥珀配白字)。
        readonly property color onWarning: dark ? "#1A1405" : "#FFFFFF"

        /// 来源徽标(capture / translate);浅底需加深保证对比度。
        readonly property color sourceCapture:   dark ? "#7EC2FF" : "#1A6FC4"
        readonly property color sourceTranslate: dark ? "#C9A7FF" : "#6F42C1"
        readonly property color sourceOcr:       dark ? "#7FD8B4" : "#1B7A55"
    }

    // ---- 尺度(theme.md §11/14/16/24/26/39) ----
    readonly property QtObject metrics: QtObject {
        readonly property int radiusWindow: 12
        readonly property int radiusPanel:  8
        readonly property int radiusCard:   8
        readonly property int radiusInput:  6
        readonly property int radiusButton: 6
        readonly property int radiusPopup:  8
        readonly property int radiusBadge:  4

        readonly property int spacingXs: 4
        readonly property int spacingSm: 8
        readonly property int spacingMd: 12
        readonly property int spacingLg: 16

        readonly property int headerHeight:  48
        readonly property int searchHeight:  34
        readonly property int navItemHeight: 30
        readonly property int controlHeightSm: 28
        readonly property int controlHeight:   32
        readonly property int controlHeightLg: 36
        readonly property int borderWidth: 1

        // ---- 字号(theme.md §39:PageTitle 20 / Section 16 / Body 14 / Secondary 13 / Caption 12) ----
        // 早先整体偏小一档(body 13 / micro 10),中文在 1.75x 高 DPI 下显得局促;
        // 这里按规范对齐,并新增 Secondary 档给卡片的次级正文使用。
        readonly property int fontPageTitle: 18
        readonly property int fontSection:   15
        readonly property int fontBody:      14
        readonly property int fontSecondary: 13
        readonly property int fontCaption:   12
        readonly property int fontMicro:     11
    }

    // ---- 动画(theme.md §30/31):fast 反馈,normal 状态/面板,slow 大过渡 ----
    readonly property QtObject animation: QtObject {
        readonly property int fast:   120
        readonly property int normal: 180
        readonly property int slow:   240
    }

    // ---- 兼容别名:存量 QML(尤其 SettingsDrawer)仍引用旧扁平名,迁移完成后移除 ----
    readonly property color bg:      colors.background
    readonly property color surface: colors.surface
    // 存量把"次级面 / 悬停面"混用 surface2,统一映射到悬停面(卡片/按钮悬停一致)
    readonly property color surface2: colors.surfaceHover
    readonly property color border:  colors.border
    readonly property color text:    colors.textPrimary
    readonly property color muted:   colors.textSecondary
    readonly property color accent:    colors.accent
    readonly property color accentDim: colors.accentPressed
    readonly property color danger:  colors.error
    readonly property color pin:     colors.warning
    readonly property color accentText: colors.accentText
    readonly property color pinText:    colors.onWarning
    readonly property color sourceCapture:   colors.sourceCapture
    readonly property color sourceTranslate: colors.sourceTranslate
    readonly property color sourceOcr:       colors.sourceOcr

    readonly property font baseFont: Qt.font({
        family: fontFamily,
        pixelSize: metrics.fontBody
    })
}
