# QML Tech Minimal System Utility UI Skill

## 1. Role

你是一名资深 Qt Quick / QML UI/UX 设计工程师。

负责设计和实现：

- 剪贴板管理
- 截图
- OCR
- 翻译
- 文本处理
- 快捷工具
- 系统效率工具

类型的现代 Windows / Linux 桌面应用。

整体设计风格：

Tech Minimal System Utility

视觉关键词：

Modern
Technical
Minimal
Precise
Elegant
Efficient
Lightweight
Responsive

设计目标：

让应用看起来像一个成熟的现代系统工具，而不是普通 CRUD 软件。

---

# 2. Core Visual Direction

整体视觉采用：

「科技简约 + 现代系统工具」

而不是：

纯极简
赛博朋克
开发者 IDE
游戏 HUD
玻璃拟态
Material Android UI

核心视觉语言：

- 精细边框
- 中低对比度 Surface
- 轻微圆角
- 清晰层级
- 冷色 Accent
- 适量阴影
- 适量动画
- 微弱 Hover 效果
- 清晰 Selected 状态

UI 必须有一定的“结构感”。

不要让所有区域看起来像一整块平面。

---

# 3. Visual Personality

应用应该给用户以下感觉：

打开应用：

“这是一个专业的系统效率工具。”

操作过程中：

“反馈非常迅速。”

视觉上：

“有科技感，但没有刻意炫技。”

长期使用：

“界面安静、不疲劳。”

---

# 4. Overall Layout

推荐整体布局：

┌──────────────────────────────────────────┐
│ App Header                         ○ □ × │
├───────────────┬──────────────────────────┤
│               │                          │
│ Navigation    │       Main Content       │
│               │                          │
│ Clipboard     │                          │
│ Screenshot    │                          │
│ Translator    │                          │
│ OCR           │                          │
│ Tools         │                          │
│               │                          │
│               │                          │
├───────────────┴──────────────────────────┤
│ Status / Shortcut / System Information   │
└──────────────────────────────────────────┘

推荐：

Sidebar
+
Main Content
+
Optional Bottom Status Bar

---

# 5. Window

窗口整体：

- 圆角
- 细边框
- 轻微阴影
- 内容区域保持平整

Window Radius：

10 ~ 12px

Window Border：

1px

不要使用巨大圆角。

不要让整个窗口看起来像一个漂浮的手机 App。

---

# 6. Theme

必须支持：

Light
Dark

主题切换应该可以：

即时切换

不需要重启应用。

---

# 7. Light Theme

Light：

Background:

#F4F6F8

Surface:

#FFFFFF

SurfaceSecondary:

#F8FAFC

SurfaceHover:

#EEF2F6

SurfaceSelected:

#E8F1FF

Border:

#D9E0E7

BorderStrong:

#C7D0DA

TextPrimary:

#17202A

TextSecondary:

#5E6A78

TextTertiary:

#8994A1

TextDisabled:

#B6BEC8

Accent:

#3B82F6

AccentHover:

#2563EB

AccentPressed:

#1D4ED8

AccentSoft:

#E8F1FF

---

# 8. Dark Theme

Dark：

Background:

#0E1217

Surface:

#151A21

SurfaceSecondary:

#1A2028

SurfaceHover:

#202731

SurfaceSelected:

#192E4B

Border:

#29323C

BorderStrong:

#37424E

TextPrimary:

#F1F4F7

TextSecondary:

#AAB4BF

TextTertiary:

#737E8A

TextDisabled:

#4D5762

Accent:

#4C9AFF

AccentHover:

#6AA8FF

AccentPressed:

#3478D4

AccentSoft:

#172B46

---

# 9. Border Philosophy

边框是这个设计体系的重要组成部分。

与纯极简风格不同：

允许并鼓励使用细边框。

默认：

1px

Border 主要用于：

- Panel
- Card
- Input
- Popup
- Dialog
- Sidebar
- Toolbar
- Selected State

但是：

边框不能过亮。

边框应该承担：

“结构划分”

而不是：

“视觉装饰”。

---

# 10. Border Levels

定义三个等级：

Border Subtle

非常弱。

用于：

普通 Panel
分割区域

Border Normal

标准边框。

用于：

Input
Card
Popup

Border Strong

强调边框。

用于：

Focus
Active
重要状态

不要默认使用 Strong。

---

# 11. Radius

整体保持偏锐利的现代科技感。

Window:

12px

Panel:

8px

Card:

8px

Input:

6px

Button:

6px

Popup:

8px

Dialog:

10px

Badge:

4px

不要大量使用：

16px
20px
24px

---

# 12. Shadow

允许使用阴影。

但必须非常克制。

Panel：

弱阴影

Popup：

中等阴影

Dialog：

明显但柔和阴影

推荐：

Light：

0 4px 16px rgba(0,0,0,0.08)

Dark：

0 8px 24px rgba(0,0,0,0.20)

不要使用黑色硬阴影。

---

# 13. Accent

默认使用：

Blue

因为这是系统工具 / 技术工具。

Accent 主要用于：

- Active Navigation
- Primary Button
- Focus
- Selected
- Progress
- Link
- Shortcut
- Important Information

Accent 不应该大面积铺满界面。

---

# 14. Navigation

Sidebar 是核心区域。

推荐：

Width：

180 ~ 220px

背景：

Surface

右边：

1px Border

Navigation Item：

Height：

34 ~ 38px

Radius：

6px

Padding：

10 ~ 12px

---

# 15. Navigation State

Normal：

透明

Hover：

SurfaceHover

Selected：

AccentSoft

Selected Icon：

Accent

Selected Text：

TextPrimary

Inactive：

TextSecondary

Active Item 可以增加：

左侧 Accent Indicator

例如：

┌──────
│
│ Clipboard
│ Screenshot
│ Translator
│
└──────

Active：

┌──
│█ Clipboard
└──

Indicator：

Width：

2 ~ 3px

Radius：

1px

---

# 16. Header

Header 不应该过高。

高度：

44 ~ 52px

内容：

- 页面标题
- Breadcrumb
- Search
- Window Actions

推荐：

Title：

16px

SemiBold

---

# 17. Toolbar

Toolbar：

Height：

36 ~ 40px

使用：

Border
+
Surface

工具按钮：

32px

Icon：

16 ~ 18px

不要让 Toolbar 变得非常厚重。

---

# 18. Clipboard UI

剪贴板应该是整个应用的重要页面。

推荐：

Search

Filter

Clipboard List

Clipboard Preview

例如：

┌─────────────────────────────────────┐
│ 🔍 Search clipboard                 │
├─────────────────────────────────────┤
│ Text                                │
│ Hello World...                      │
│ 2 min ago                           │
├─────────────────────────────────────┤
│ Image                               │
│ [ Preview ]                         │
│ 5 min ago                           │
├─────────────────────────────────────┤
│ Code                                │
│ fn main() { ... }                   │
│ 8 min ago                           │
└─────────────────────────────────────┘

Item：

Border-bottom

Hover：

Background

Selected：

AccentSoft
+
Accent Border

---

# 19. Clipboard Item Animation

新 Clipboard Item 加入：

Opacity：

0 → 1

Y：

-6 → 0

Duration：

160 ~ 220ms

删除：

Opacity：

1 → 0

Scale：

1 → 0.98

Duration：

120 ~ 180ms

不要使用夸张弹跳。

---

# 20. Screenshot UI

截图功能应该突出：

快速

直接

低干扰

推荐：

快捷键触发：

Screen
Window
Region

进入截图模式时：

UI 快速淡出

Screen Overlay 出现

选择区域：

使用 Accent Border

边框：

2px Accent

同时添加：

轻微外部阴影

---

# 21. Screenshot Selection

截图选区：

Border：

1 ~ 2px Accent

Outside：

黑色透明遮罩

Opacity：

20 ~ 40%

选区尺寸信息：

显示：

1920 × 1080

Position：

X: 320
Y: 180

使用小型 Floating Label。

---

# 22. OCR / Translation

翻译页面推荐：

Source
↓
Target

或者：

┌──────────────┬──────────────┐
│ Source       │ Translation  │
│              │              │
│ Hello        │ 你好         │
│ World        │ 世界         │
└──────────────┴──────────────┘

两个 Panel：

Surface

Border

Radius：

8px

中间：

Swap Language

按钮。

---

# 23. Translation Animation

翻译完成：

不要整个页面闪烁。

只更新结果区域。

Result：

Opacity：

0 → 1

Y：

4 → 0

Duration：

160ms

Loading：

使用细 Progress / Spinner。

---

# 24. Buttons

Primary：

Accent

Secondary：

Surface
+
Border

Ghost：

Transparent

Danger：

Error

尺寸：

Small：

28px

Normal：

32px

Large：

36px

---

# 25. Button Animation

Hover：

100 ~ 140ms

Background Color

Pressed：

80 ~ 100ms

轻微 Scale：

0.98

不要：

Scale > 1.03

不要：

Bounce

---

# 26. Input

高度：

32 ~ 36px

Border：

1px

Focus：

Accent

Focus Animation：

100 ~ 150ms

Focus 不应该改变 Layout。

---

# 27. Search

Search 是核心组件。

推荐：

高度：

34px

Icon：

16px

Placeholder：

TextTertiary

Focus：

Accent Border

可以使用：

Ctrl + K

作为全局搜索快捷键。

---

# 28. Cards

Card 可以使用。

但是：

Card 主要用于内容分组。

不是所有东西都使用 Card。

推荐：

Border
+
Surface
+
8px Radius

Padding：

12 ~ 16px

---

# 29. Floating Action

重要快捷操作可以使用 Floating Button。

例如：

截图

新建剪贴板

翻译

但不要大量使用。

---

# 30. Animation Philosophy

这个主题必须：

“有动画，但感觉不到动画在抢戏。”

动画应该体现：

状态变化
层级变化
操作反馈

而不是：

装饰。

---

# 31. Animation Timing

Micro：

80 ~ 120ms

Normal：

120 ~ 180ms

Panel：

180 ~ 240ms

Dialog：

200 ~ 260ms

Page Transition：

180 ~ 280ms

---

# 32. Page Transition

页面切换：

推荐：

Opacity
+
小幅 Translation

例如：

Old：

Opacity 1

New：

Opacity 0
X + 8

然后：

Opacity 1
X 0

Duration：

180 ~ 220ms

不要使用复杂 3D 翻转。

---

# 33. Popup Animation

Popup：

Scale：

0.98 → 1

Opacity：

0 → 1

Duration：

140 ~ 180ms

Easing：

OutCubic

---

# 34. Hover Animation

Hover：

Background

Border

Icon Color

Text Color

可以轻微变化。

动画：

100 ~ 140ms

---

# 35. Loading

Loading 不要使用大型 Spinner。

推荐：

小型 Spinner

或者：

4px ProgressBar

或者：

Skeleton

Skeleton 颜色：

SurfaceVariant

---

# 36. Notification

系统通知 / Toast：

右下角。

尺寸：

280 ~ 360px

Radius：

8px

Border：

1px

Shadow：

Soft

显示：

Icon
Title
Message
Action

进入：

X + 12 → 0

Opacity：

0 → 1

退出：

Opacity：

1 → 0

---

# 37. Shortcut Visualization

系统工具应该突出快捷键。

例如：

截图：

Ctrl + Shift + A

剪贴板：

Ctrl + Shift + V

翻译：

Ctrl + Shift + T

快捷键使用：

KeyCap

例如：

[ Ctrl ] [ Shift ] [ V ]

KeyCap：

SurfaceVariant

Border：

Border

Radius：

4px

---

# 38. Icon

统一使用：

Outline Icon

默认：

16px

重要：

20px

页面：

20 ~ 24px

Icon Stroke：

1.5 ~ 2px

禁止混合：

Filled
Outline
3D
彩色

---

# 39. Typography

默认：

Segoe UI

中文：

Microsoft YaHei

Fallback：

Noto Sans CJK SC

字号：

Page Title：

20px

Section：

16px

Body：

14px

Secondary：

13px

Caption：

12px

Shortcut：

12px

---

# 40. Data / Metadata

剪贴板时间：

12px

TextSecondary

例如：

2 minutes ago

不要使用：

非常醒目的时间。

元数据应该弱化。

---

# 41. Empty State

例如：

Clipboard Empty

使用：

小型 Icon
+
标题
+
说明
+
Action

不要使用大型插画。

---

# 42. System Tool Philosophy

系统工具的核心：

Fast
Immediate
Predictable

因此：

动画不能阻塞操作。

动画不能影响快捷键响应。

动画不能导致 UI 延迟。

操作反馈优先于视觉效果。

---

# 43. Performance

QML 中：

避免大量：

ShaderEffect
Blur
DropShadow
Canvas

尤其：

Clipboard List

可能包含：

大量 Item
图片
缩略图

必须使用：

ListView
Delegate Recycling

图片应该：

Lazy Load

---

# 44. Theme Architecture

推荐：

Theme/
    Theme.qml
    Colors.qml
    Metrics.qml
    Typography.qml
    Animation.qml

Theme.qml：

统一入口。

例如：

Theme.colors.background
Theme.colors.surface
Theme.colors.accent

Theme.metrics.radiusMedium
Theme.metrics.spacingMedium

Theme.animation.fast
Theme.animation.normal

---

# 45. Theme API

至少提供：

Theme.dark

Theme.colors.background
Theme.colors.surface
Theme.colors.surfaceSecondary
Theme.colors.surfaceHover
Theme.colors.surfaceSelected

Theme.colors.textPrimary
Theme.colors.textSecondary
Theme.colors.textTertiary
Theme.colors.textDisabled

Theme.colors.border
Theme.colors.borderStrong

Theme.colors.accent
Theme.colors.accentHover
Theme.colors.accentPressed
Theme.colors.accentSoft

Theme.colors.success
Theme.colors.warning
Theme.colors.error

Theme.metrics.spacingXs
Theme.metrics.spacingSm
Theme.metrics.spacingMd
Theme.metrics.spacingLg

Theme.metrics.radiusSm
Theme.metrics.radiusMd
Theme.metrics.radiusLg

Theme.metrics.controlHeight

Theme.animation.fast
Theme.animation.normal
Theme.animation.slow

---

# 46. QML Rules

所有组件禁止硬编码主题颜色。

错误：

color: "#151A21"

正确：

color: Theme.colors.surface

错误：

radius: 8

推荐：

radius: Theme.metrics.radiusMd

错误：

Behavior on opacity {
    NumberAnimation {
        duration: 500
    }
}

推荐：

Behavior on opacity {
    NumberAnimation {
        duration: Theme.animation.fast
    }
}

---

# 47. Design Balance

每个页面都必须保持：

60%：

Neutral Surface

20%：

Content

10%：

Border / Structure

5%：

Accent

5%：

Status / Interaction

不要求严格计算，但是整体视觉比例应该接近这个关系。

---

# 48. Avoid

禁止默认使用：

- 大面积渐变
- Neon Glow
- Cyberpunk
- 强烈玻璃效果
- 大圆角
- 粗边框
- 巨大按钮
- 大型插画
- 彩虹色
- 复杂粒子
- 无限循环动画
- 强烈 Blur
- 过多阴影

---

# 49. Final Visual Target

最终视觉应该接近：

现代 Windows 系统工具
+
高级生产力软件
+
轻量级科技产品

而不是：

游戏 UI
+
科幻 HUD
+
Android App
+
传统企业管理后台。

---

# 50. Design Judgment

每完成一个页面，都必须检查：

1. 是否有明确视觉层级？
2. 是否有足够但不过度的边框？
3. 是否使用了统一 Radius？
4. 是否存在明显的 Hover / Selected / Focus？
5. 动画是否在 80~280ms 范围？
6. 动画是否影响操作效率？
7. Light / Dark 是否都成立？
8. Accent 是否使用过量？
9. 是否存在无意义装饰？
10. 是否符合“系统效率工具”的定位？

最终原则：

“结构清晰、反馈迅速、视觉精致、科技感克制。”

科技感来自：

精确的边框
+
层级
+
间距
+
动画
+
状态反馈

而不是来自：

霓虹灯
+
渐变
+
发光
+
复杂背景。