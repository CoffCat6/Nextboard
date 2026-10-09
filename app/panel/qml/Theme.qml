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

    // ---- 字体族(与 C++ 侧 nb::uiFontFamilies() 同一份来源) ----
    //
    // ⚠️ 这里只能写**单个族名**,不能写 "A, B" 形式的回退链:
    //    QML 的 font.family 底层是 QFont::setFamily,它是
    //    `setFamilies(QStringList(family))` —— 单元素,会把应用字体的回退链
    //    整条覆盖掉;而逗号串会被当成一个完整族名去匹配,匹配不到就由 Qt
    //    的 font matching 随便挑一个(实测落到 Tahoma),中英混排直接错乱。
    //    (Qt.font({families: [...]}) 也不行:QML 的 font 值类型没有 families
    //     属性,该键被静默忽略。)
    //    所以回退链只在 C++ 侧 app_font.hpp 表达一次,QML 只放主族名。
    //
    // 中文为什么不掉宋体:app_font.hpp 装了 QFont::insertSubstitution
    // (Segoe UI / 等宽族 → 微软雅黑),缺字回退被钉死了。改族名前先看那边。
    readonly property string fontFamily: "Microsoft YaHei UI"

    /// 技术字段(URL / API Key / JSON / 请求头 / 路径 / 语种代码 / 快捷键)的
    /// 等宽字体。Cascadia Mono 是 Windows Terminal 默认字体(Win11 自带),
    /// Consolas 全 Windows 版本都有,中文经 C++ 替换表回退雅黑。
    readonly property string fontMono: "Cascadia Mono"

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
