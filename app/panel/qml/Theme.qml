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

        // ---- 标签调色板(S5.2:自定义标签按名字取色) ----
        // 与来源徽标同口径:**彩色前景 + 中性底**,暗色用亮柔彩、浅色用深饱和。
        //
        // 8 个色值不是手挑的,是按下面三条约束搜出来的(工具与实测见
        // docs/progress.md「标签配色」):
        //   ① 文字对比度:浅色值在 surfaceSecondary(#F8FAFC)上 ≥ 4.7:1,
        //      暗色值在 #1A2028 上 ≥ 6.5:1(11px 小字按正文当量对待,留余量);
        //   ② 两两可区分:同主题内任意两色的 CIEDE2000 ΔE ≥ 17.5;
        //   ③ 不与"固定"徽标撞色:与 warning 的 ΔE ≥ 19(暗) / 29(浅)。
        // ③ 是硬要求而非锦上添花:"固定"是**状态**标记且以实心色块出现,
        //    而标签也是彩色胶囊 —— 一次误读就会把"有标签"看成"已固定"。
        //    故色相 45°~100°(实测 warning 落在 58°/84°)整段不用。
        //
        // ⚠️ 两个主题**共用同一套色相**(第 i 档在亮/暗下是同一色族):
        //    否则同一个标签切换主题会换色相,颜色就不再是标签的记忆锚点。
        readonly property color tagRed:     dark ? "#FF8882" : "#CE0124"   // H24
        readonly property color tagOlive:   dark ? "#BCCE02" : "#6B7600"   // H116
        readonly property color tagGreen:   dark ? "#3DC964" : "#008235"   // H149
        readonly property color tagTeal:    dark ? "#03EED9" : "#087E73"   // H184
        readonly property color tagAzure:   dark ? "#01C9FD" : "#067A9B"   // H225
        readonly property color tagBlue:    dark ? "#7BAAFE" : "#1165FA"   // H262
        readonly property color tagPurple:  dark ? "#C08EFF" : "#9742E7"   // H303
        readonly property color tagMagenta: dark ? "#FA74C3" : "#D11695"   // H346

        /// 取色用的调色板。⚠️ 顺序即哈希色位:**增删中间项会改变所有既有标签的颜色**
        /// (末尾追加除外),要改请连同 progress.md 的说明一起改。
        readonly property var tagPalette: [tagRed, tagOlive, tagGreen, tagTeal,
                                           tagAzure, tagBlue, tagPurple, tagMagenta]
    }

    // ---- 标签取色(纯函数,无状态) ----

    /// 标签名 → 颜色。**只依赖名字**:同名恒定、跨重启恒定、不落库、不依赖条目集合,
    /// 因此可以直接写在绑定里(委托复用/回收都不会"串味")。
    ///
    /// ⚠️ 代价是不同标签可能撞色:8 色下 32 个真实标签实测同色 57 对(随机期望 62 对),
    /// 即"同屏 3 个标签里有一对同色"约 3 成概率。要严格不撞色就得把"标签名→颜色"
    /// 持久化(含分配顺序),那是 core 侧改动,不在本轮范围。
    function tagColor(name) {
        const s = (name === undefined || name === null) ? "" : ("" + name)
        if (s.length === 0) return colors.textSecondary
        return colors.tagPalette[tagHash(s) % colors.tagPalette.length]
    }

    /// 字符串 → 32 位无符号哈希(FNV-1a 变体 + murmur3 finalizer 的雪崩)。
    ///
    /// 为什么不能沿用常见的 djb2(`h = h * 33 + c`):33 ≡ 1 (mod 8),于是
    /// `h mod 8` 退化成 `(5381 + Σord(c)) mod 8` —— **与字符顺序无关**。
    /// 实测后果:任意换序的标签必然同色(`重要`/`重...`、`ab`/`ba`),且分布按
    /// "码和同余"聚集,`重要` 与 `学习` 稳定撞在同一个色位。
    ///
    /// ⚠️ 三个乘数都 < 2^22,配合 < 2^31 的余数,保证中间值 < 2^53(QML 的 JS 是
    /// 双精度,超过 2^53 会丢低位、不同平台可能不一致)。
    /// ⚠️ 位运算必须用 `>>> 0` 转无符号:`& 0xffffffff` 在 JS 里是**有符号**的
    /// (0xffffffff 按 int32 是 -1),结果变负数会让后续取模与乘法全错。
    function tagHash(s) {
        let h = 2166136261 % 2147483647
        for (let i = 0; i < s.length; ++i)
            h = (h * 131 + s.charCodeAt(i)) % 2147483647
        h = (h ^ (h >>> 13)) >>> 0
        h = (h * 65599 + 7) % 2147483647
        h = (h ^ (h >>> 11)) >>> 0
        h = (h * 2654435 + 11) % 2147483647
        h = (h ^ (h >>> 15)) >>> 0
        return h
    }

    /// 标签行的富文本:每个标签用自己的颜色,分隔符弱化。
    /// ⚠️ 必须是**一个 Text(RichText)**,不能改成一串 TagChip 委托 ——
    /// EntryCard 的元信息行是 RowLayout,动态增删委托会让它的隐式尺寸失效
    /// (清空标签后卡片会塌陷),详见 EntryCard.qml 的注释。
    function tagRichText(names) {
        if (!names || names.length === 0) return ""
        const parts = []
        for (let i = 0; i < names.length; ++i) {
            const n = "" + names[i]
            // 颜色值参与字符串拼接时由其 toString()(#rrggbb)承担
            parts.push('<span style="color:' + tagColor(n) + '">' + escapeHtml(n) + '</span>')
        }
        return parts.join('<span style="color:' + colors.textTertiary + '"> · </span>')
    }

    /// 标签名是用户输入,进 RichText 前必须转义 —— 否则 `<b>` 之类会被当标记渲染
    function escapeHtml(s) {
        return ("" + s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
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
