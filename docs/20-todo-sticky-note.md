# 20 · 待办贴屏(桌面便签):可行性与设计方案

| 项 | 内容 |
| --- | --- |
| 版本 | v1(设计,未实现) |
| 日期 | 2026-10-18 |
| 状态 | **N1–N5 已实现**(2026-10-18);§11 的 6 项按"我的建议"拍板并落地,偏离在 §13 回写 |
| 关联 | [05-todo.md](./05-todo.md)(待办数据模型/调度)· [06-memory-optimization.md](./06-memory-optimization.md) §4(贴图实测开销)· [theme.md](./theme.md)(视觉语言) |
| 关联补充 | 待办已拆成「标题 + 备注」([21](./21-todo-title-note.md)):便签**标题条 = 标题**、**正文区 = 备注** |
| 一句话 | 把一条**待办**变成一个常驻桌面的**活窗口**:可编辑、可勾完成、贴边半隐藏、位置状态落库、重启复原 —— 与现有「贴图」同构,但语义是**持久对象**而非一次性位图。 |

---

## 1. 需求与定位

### 1.1 用户原话

> "接下来开始考虑进行待办贴屏(todo 小便签功能),本质上类似。先设计方案,贴屏后
> 可以贴靠屏幕边缘可以半隐藏等,先规划可行性和设计方案"。

### 1.2 它和「贴图」不是一回事(必须先说清)

仓库里已经有两个容易混淆的能力,新功能是**第三个**:

| 能力 | 入口 | 内容 | 生命周期 | 可编辑 | 与库同步 |
| --- | --- | --- | --- | --- | --- |
| **贴图** pin | 热键 `Ctrl+Shift+P` / 条目右键「贴到屏幕」(`Main.qml:2345`) | 图片条目原图;**文本条目渲染成只读位图**(`PinWindow::renderText`) | 一次性;10 张软上限 + 96MB 字节预算,超限**淘汰最旧**(core_facade.cpp:494/505) | 只能走编辑器改像素 | 否(贴完即脱钩) |
| **便签** note(本设计) | 待办卡片右键 / 热键 / 托盘 | 待办正文(可编辑)+ 截止/提醒状态 | **持久**:位置/尺寸/折叠态落库,重启复原;不被淘汰 | 是(内联编辑) | 是(双向:改正文 = 改待办) |
| 待办列表 | 面板「待办」页(`Main.qml` viewId=todo) | 全部待办 | 库 | 走编辑卡片 | 是 |

> 结论:便签是「待办的第二个视图」,不是「贴图的第二种内容」。这一点决定了后面
> 所有取舍 —— 尤其是**不能复用贴图的淘汰注册表**(静默关掉一条便签 = 用户数据丢
> 了一样,而贴图被淘汰是设计内行为)。

### 1.3 为什么现在做(时机判据)

| 传统难点 | 本项目现状 | 结论 |
| --- | --- | --- |
| 无边框置顶窗口 + 拖动/缩放/透明度 + 屏间 DPR 换算 | `PinWindow` 已踩平(`pin_window.cpp:127` flags、`:380 memoryBytes`、`:432` DPI change 延迟重算) | ✅ 现成 |
| 边缘缩放命中(无边框窗口没有非客户区) | `window_effects.hpp:41 framelessResizeHitTest`,面板已在用(`panel_window.cpp:435`) | ✅ 现成 |
| 自绘圆角/阴影/工具条(透明窗口下 QSS 画自身背景失效) | `ToolbarPanel` + `paintEvent` 阴影三层叠加(`pin_window.cpp:72` 注释记录了坑) | ✅ 现成 |
| 待办数据 + 状态 + 到点提醒 | `store\todo.rs` + `todo_meta` + 调度线程(`core.rs:22`),列表/搜索/标签全通 | ✅ 现成 |
| 跨窗口刷新链路(改一处、两处视图同步) | `WM_APP_CLIP_CHANGED` → `ClipEntryModel::refresh()` | ✅ 现成 |
| 贴边/半隐藏 | **本项目没有先例**,是本设计的主要新增面(§6) | ⚠️ 需自研 |

---

## 2. 目标与非目标

### 2.1 目标(v1)

| # | 目标 |
| --- | --- |
| G1 | 从待办进入「贴到桌面」:每个待办**至多一个**便签,窗口内编辑正文即时写回待办 |
| G2 | 便签可拖动、8 向缩放、勾选完成、显示截止/逾期/提醒、关闭 |
| G3 | **贴边吸附**:拖动松手时距屏幕可用区边缘 ≤ 12px 自动对齐到该边 |
| G4 | **半隐藏**:贴边后可自动滑出屏幕,只留一条 ≥8px 的「拉手」;鼠标移上去滑回 |
| G5 | 位置/尺寸/吸附边/半隐藏态/透明度/置顶 **落库**,重启与切换屏幕后复原 |
| G6 | 与面板**双向同步**:便签改完面板立刻刷新;面板删除/清空 → 便签关闭 |
| G7 | 内存与数量有界:便签不参与贴图淘汰,但数量设上限并**明确拒绝**(不静默关) |

### 2.2 非目标(明确不做)

| # | 不做 | 理由 |
| --- | --- | --- |
| N1 | 富文本 / Markdown 渲染 / 图片嵌便签 | 数据模型是纯文本(`text_content`);扩富文本要改 schema + 编辑引擎,与本功能价值不匹配 |
| N2 | 便签之间互相吸附、成组移动 | 布局持久化在路线图里本就是 backlog;先单窗行为稳 |
| N3 | 便签透明度/配色做成每张独立风格面板 | 只给 `opacity`;`fontSize` 给一个全局默认即可,配色跟随主题 |
| N4 | 便签内新建/删除待办(只编辑当前这条) | 便签是"某条待办的视图",不是另一个待办管理器 |
| N5 | 云同步 / 多机布局同步 | 无此基础设施 |
| N6 | 便签上截图(粘贴图片进便签) | 属于 N1 的延伸 |

---

## 3. 数据模型(v7 迁移)

### 3.1 决策:新建 `todo_note` 扩展表,复用 `kind='todo'` 条目

与 `todo_meta`(`v6.sql:73`)同一手法:条目本体不动,只把**窗口状态**放进扩展表。
好处是列表/搜索/标签/计数/删除全部沿用既有链路,且**不需要重建 `clip_entries`**
(v6 那次重建是为改 CHECK 约束;这次 kind 无需新增,只加表 → 迁移风险低一个数量级)。

```sql
-- v7:待办贴屏(桌面便签)窗口状态(docs/20 §3)
-- 与 todo_meta 同口径:与条目 1:1,**无外键**,一致性由应用层维护
-- (删条目/清空/批量删除/导入替换时同步删本表,见 §3.3)。
CREATE TABLE IF NOT EXISTS todo_note (
  id         TEXT PRIMARY KEY,               -- = clip_entries.id(kind 必须为 'todo')
  x          INTEGER NOT NULL,               -- 逻辑像素(窗口左上,**不含**阴影留白)
  y          INTEGER NOT NULL,
  w          INTEGER NOT NULL,               -- 逻辑像素(内容区,≥ 180×120)
  h          INTEGER NOT NULL,
  screen     TEXT NOT NULL DEFAULT '',       -- QScreen::name(),恢复时优先匹配该屏
  edge       TEXT NOT NULL DEFAULT 'none',   -- none|left|right|top|bottom(吸附边)
  auto_hide  INTEGER NOT NULL DEFAULT 0,     -- 贴边后是否自动半隐藏
  collapsed  INTEGER NOT NULL DEFAULT 0,     -- 当前是否处于半隐藏(滑出)态
  top_most   INTEGER NOT NULL DEFAULT 1,     -- 是否置顶(关闭后跟随普通窗口层级)
  visible    INTEGER NOT NULL DEFAULT 1,     -- 0 = 用户已关闭这张便签(保留几何,下次重贴原位出现)
  opacity    INTEGER NOT NULL DEFAULT 100,   -- 30..100
  font_size  INTEGER NOT NULL DEFAULT 0,     -- 0 = 用全局默认(主题字号)
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_todo_note_screen ON todo_note (screen);
```

**`visible` 的用途(关窗 ≠ 忘位置)**:用户点 ✕ 关闭便签 = "从桌面拿走",不是"清空偏好"。
保留行并置 `visible = 0`:重启不再自动出现在桌面上,但下次「贴到桌面」时**回到上次的位置/尺寸**。
若关窗即删行,重贴一定回到默认位置 —— 用户摆好的布局每关一次就丢,体验上是退步。
行本身的生命周期跟随条目(条目删了才连带删),不存在"关掉的便签变孤儿"。

**为什么坐标/尺寸一律存逻辑像素**:逻辑像素是 Qt 的窗口坐标,跨屏时 Qt 会自动按
新屏 DPR 缩放窗口物理尺寸;存物理像素会在"用户在 125% 屏上摆好、换 100% 屏"时
错位。原始 `screen` 名(`QScreen::name()`,`\\.\DISPLAYn`)用于恢复时优先落回原屏,
找不到再夹回主屏 —— 与 `PinWindow` 的 DPR 口径同源(`pin_window.cpp:390`)。

**`collapsed` 与 `edge` 分开存**:`edge` 是"贴在哪",`collapsed` 是"现在是否收起"。
只存一个字段会出现"用户拖离边缘后仍以为在收起态"的脏状态。

### 3.2 版本推进

`store\mod.rs:45` 的 `LATEST_VERSION` 6 → 7,追加 `MIGRATION_V7` 分支。
⚠️ 该常量同时是**备份版本闸门**(`backup_export::stage_backup` 拒收更高版本):
升到 7 之后,新 exe 打出的备份**旧 exe 会拒绝恢复** —— 这是正确行为(旧 exe 读不懂
`todo_note`),但发布说明要写一句。

### 3.3 级联清理:四处**必须**同步改(与 `todo_meta` 完全同构)

这是本设计最容易漏、且漏了会留下永久孤儿行的地方。`todo_meta` 当年踩过一遍,
逐点列清:

| 位置 | 现状 | 需补 |
| --- | --- | --- |
| `ffi.rs:898` `delete_entry` | 先 `todo::delete_meta` 再 `entry::delete` | 同样删 `todo_note` |
| `store\entry.rs:585` `delete_bulk` | 事务内逐 id `DELETE FROM todo_meta`(:612) | 同事务删 `todo_note` |
| `store\entry.rs:538` `clear` | 同事务 `DELETE FROM clip_entries` + `todo_meta` | 加 `todo_note` |
| `store\todo.rs:419/430` `delete`/`delete_meta` | 只删 meta | 一并删 note(单一收口点) |

`backup_export.rs:766-785` 的**导入**路径也要扩(见 §3.4);`clear_done` 走
`todo::delete` → 自动覆盖。测试里加一条与 `todo.rs:878` 同形的断言:
`SELECT COUNT(*) FROM todo_note` 必须为 0。

### 3.4 备份 / 恢复 / 导入

- **备份**:整库 zip(`backup_snapshot`),新表**自动**随包,无需改代码;
- **恢复**:整库替换 + 启动时 `migrate`,新表自动建,无需改代码;
- **导入(并入)**:`read_staged_rows` 只读 `clip_entries`,`todo_meta` 靠
  `read_todo_meta` 逐行搬(且**换新 id**)。必须加一个 `read_todo_note` 同构函数,
  把窗口状态按新 id 搬过去 —— 否则"导入备份后待办有了,便签全没了"。
  这是 v6 待办导入时踩过的同一处,注释已写在 `backup_export.rs:766`。
- `tools/check_db_integrity.py` 增加:`todo_note` 表存在 + `idx_todo_note_screen` 存在。

---

## 4. FFI 契约(仍不新增导出函数)

沿用 JSON 门面(`ffi.rs` 的 `match method`),三个新方法:

| 方法 | 请求 | 响应 data | 错误 |
| --- | --- | --- | --- |
| `todo_note_list` | `{}` | `{items: [扁平条目 + note 徽标]}`(含 `visible=0` 的行,由宿主决定是否建窗) | — |
| `todo_note_save` | `{id, x?, y?, w?, h?, screen?, edge?, autoHide?, collapsed?, topMost?, opacity?, fontSize?, visible?}` | Null | 条目不存在/非 todo → InvalidInput;`edge` 枚举非法 → InvalidInput(数值类夹紧不报错,见下) |
| `todo_note_delete` | `{id}` | Null | —(幂等) |

**形状约定(必须与既有端点一致)**:`todo_note_list` 的每一项 = `todo_list` 的扁平
形状(`id/kind/textContent/...`)+ `todo` 徽标 + `note` 对象:

```json
{ "id": "…", "kind": "todo", "textContent": "买牛奶",
  "todo": { "dueAt": 1791808378619, "done": false, "overdue": false },
  "note": { "x": 1200, "y": 160, "w": 280, "h": 240, "screen": "\\.\DISPLAY1",
            "edge": "right", "autoHide": true, "collapsed": false,
            "topMost": true, "opacity": 100, "fontSize": 0 } }
```

理由:`TodoItem` 那条手写 `Serialize`(`store\todo.rs:36`)存在的唯一目的是让宿主
走**单一扁平解析路径**;便签再引入 `{entry:{...}}` 嵌套会重复"整页解析成空行"的
事故(docs/05 §2.3 的实测记录)。`note` 键只在便签端点出现(`skip_serializing_if`)。

**save 是 upsert**:行在**首次 `save` 时创建**(贴屏那一刻)。首次调用**必须**带齐
`x/y/w/h`(宿主建窗即有几何,拿不到说明调用方写错了 → InvalidInput);其余字段取
SQL DEFAULT。这样"用户从没贴过"与"贴过又关了"由 `visible` 区分,而不会给每条待办
预建一行(预建会让 200 条待办凭空多 200 行)。

**参数三态**(与 `todo_update` 同语义,见 docs/05 §5):`save` 中**键缺失 = 不改**,
显式值 = 设置。`edge` 非法值显式报错,不静默回退 `none`。

**写入前夹紧(与"显式报错"并存,不是替代)**:数值类字段走**夹紧**而非拒绝 ——
拖窗口时宿主每帧都可能写一次几何,一次越界(比如屏幕刚被拔掉,`x = 3200`)就报错会让
便签卡在"位置永远存不进去"的状态。规则:`w ∈ [180,1200]`、`h ∈ [120,900]`、
`opacity ∈ [30,100]`、`font_size ∈ [0,96]`(0 = 用全局默认)。`edge` 是**枚举**,
非法值仍然报错(枚举不存在"差一点"的合法近似,静默回退会掩盖宿主 bug)。

**`todo_note_list` 顺带做孤儿清理**:`JOIN clip_entries WHERE kind='todo'` 缺席的行
直接 `DELETE`(低风险自愈,与 `list` 的 JOIN 口径一致)。

---

## 5. 窗口实现:为什么**新写** `NoteWindow`,不继承 `PinWindow`

`PinWindow` 的骨架(QWidget 无边框 + 透明底 + 自绘阴影 + 顶层弹层工具条 + DPR
重算)正是便签需要的,但它中心是一个 `QImage m_image`,并且:

- `paintEvent` 只画位图(`pin_window.cpp:400`),便签要画**可编辑文本控件**;
- 内置旋转(`rotate90cw`)、编辑回写、文本位图渲染 —— 对便签全是死代码;
- 它的淘汰注册表语义见 §1.2,不能共用。

**窗口标志**:

```cpp
Qt::FramelessWindowHint | Qt::Tool | Qt::NoDropShadowWindowHint
// 置顶与否按 top_most 动态加 WindowStaysOnTopHint(见下)
setAttribute(Qt::WA_TranslucentBackground);
```

- `Qt::Tool` 而不是普通窗口:便签不该占任务栏、不该进 Alt+Tab(屏幕上有 5 张便签却多出
  5 个任务栏按钮是不可接受的)。**代价**就是 §7.2 说的"没有入口找回来" —— 所以托盘
  子菜单是硬要求,不是可选项;
- 置顶用 `SetWindowPos(HWND_TOPMOST/HWND_NOTOPMOST)` 动态切(与 `panel_window.cpp:246`
  同一写法),不用重建窗口标志 —— `setWindowFlag()` 会**销毁并重建原生窗口**,窗口
  位置/焦点/编辑状态全丢,对正在输入的便签是灾难;
- ⚠️ `WA_TranslucentBackground` 下 QSS 给控件**自身**画背景不生效(透明窗口的已知
  坑,`pin_window.cpp:72` 有原样记录)。便签底板因此也走 `paintEvent` 自绘,只给
  子控件(QTextEdit 等)用 QSS。

**决定**:新增 `app/src/note/note_window.{hpp,cpp}`(独立类,~400 行),
把可复用的**机制**抽出来复用,而不是复用类:

| 机制 | 复用方式 |
| --- | --- |
| 阴影留白 + 自绘三层阴影 | 抄 `pin_window.cpp:400-430` 的画法(`kNoteShadowMargin = 8`) |
| DPI 变化的延迟重算 | 抄 `pin_window.cpp:432` 的 `QTimer::singleShot(0, …)` 套路 |
| 8 向缩放命中 | 直接调 `nb::framelessResizeHitTest`(`window_effects.hpp:41`) |
| 右键工具条(顶层弹层) | 抄 `ToolbarPanel` + `showToolbarAt` 的"夹进屏幕可用区"逻辑 |

**窗体组成(纯 Widgets,不用 QML)**:

```
┌──────────────────────────────┐  ← 阴影留白 8px(窗口比内容大一圈)
│ ▣ 买牛奶              ⧉ ✕ ⋯ │  ← 标题条:复选框 + 首行摘要 + 折叠/菜单/关闭
├──────────────────────────────┤
│ 正文 …(QTextEdit,可编辑)   │
│                              │
├──────────────────────────────┤
│ 截止 10-20 18:00   ⏰ 提醒    │  ← 页脚:截止/逾期(红)/提醒徽标
└──────────────────────────────┘
```

**为什么不用 `QQuickWidget`(与主面板不同)**:主面板是全应用唯一的功能界面,值得
一份 QML 树(docs/06 实测 QML 树地板 71.6MB);便签是**可能开好几张的轻窗口**,
每张再挂一个场景图/RHI 上下文的代价与收益不匹配。纯 Widgets 的 `QTextEdit` 还有
一个隐性优势:**IME(中文输入法)行为是最稳的**,而 QML `TextEdit` 的预编辑串在
多窗口置顶场景历史上更容易出问题。视觉一致性靠 `CoreFacade` 已有的
`buildPalette(dark)`(`core_facade.cpp:114`)导出的亮/暗两套色 + 少量 QSS 保证。

**文本编辑与写回**:

- `QTextEdit` 的 `textChanged` 做 **800ms 去抖**写库(`todo_update {id, text}`),
  避免每个按键一次事务;
- 失焦、`Ctrl+S`、关闭窗口时**立即 flush**;
- 空正文(trim 后为空)拒绝写入并恢复上一个非空值(与 `todo::update` 的
  `InvalidInput` 同口径,`store\todo.rs:218`)—— 不能让"全删再退出"静默清空待办;
- 写库成功 → core 内部已 `notify_host(WM_APP_CLIP_CHANGED)`(`ffi.rs:1176`),
  面板列表与角标自动刷新。**便签侧不要自己再发一次**,否则一轮编辑两次全量刷新。

---

## 6. 贴边与半隐藏(本设计的主要新增面)

### 6.1 状态机

```
                    ┌──────────── drag 中 ────────────┐
                    │                                 │
  free ◄──(拖离边缘 >12px)── snapped ──(开 autoHide)──► 半隐藏
   │                            │                        │
   │                            └──(自动收起计时到)──────┘
   │                                                     │
   └────────────────────(鼠标进入拉手,滑回)─────────────┘
```

字段映射:`edge != none` ⇒ snapped;`edge != none && auto_hide && collapsed` ⇒ 半隐藏。
`collapsed` 单独落库是为了"收起状态下退出程序,重启还在原位收起"。

### 6.2 吸附规则(确定性的,不靠手感)

1. 归属屏取 **`screenForNote()`**:展开态看窗口中心落在哪块屏,收起态**看记忆的屏名**；
   再取该屏的 `availableGeometry()` —— **用可用区而不是 `geometry()`**，否则便签会贴在
   任务栏底下。
2. 只判**左右**两条边：分别算窗口左/右边到可用区左/右边的距离，取最小者；
   ≥ `kNoteSnapPx = 12`（逻辑像素）则不吸附（设 `edge = none`）。
3. 吸附时把**纵向夹回可用区**（避免「贴了右边却有一半在屏幕外」）。
4. 吸附完成后立刻 `todo_note_save`，不靠退出时统一写。

> **修订（2026-10-19，用户实测裁定）**：上下边从产品面**下线**，只保留左右。
> 理由：上边是标题栏/全屏区、下边是任务栏，两者在实际使用中都无法稳定触发
> （松手位置稍偏就把窗口顶出可用区），而用户实际使用「以左右为主」。
> 枚举里仍保留 `top`/`bottom` 只为读得懂历史数据；宿主读入即归一成 `none`，
> 核心写入时也归一 —— 库里不会再新增这两个值。

> **边缘的定义 = 当前屏幕的边缘，不是所有显示器拼起来的外轮廓。**
> 这一条原先没写清，是「贴到一边后自动收起来回横跳」缺陷的温床之一：
> 收起是**程序**把窗口推出屏外的动作，如果此时用几何去认屏，锚点必然落到
> 相邻的那块屏上，下一次展开就按邻屏的边算，于是两块屏之间来回振荡。
> 正确做法：**收起态不重新认屏**（`adoptScreenUnderWindow()` 在收起态直接返回），
> 归属屏只在「用户拖动/缩放结束」与「屏幕布局变化」两个时刻更新。

### 6.3 半隐藏的几何

以右边吸附为例(左边镜像 —— 只有左右两种方向):

```
可见拉手宽度  kSliver = 8 (逻辑像素)
窗口 x_collapsed = avail.right() - (kSliver + kNoteShadowMargin) + 1
窗口 x_expanded  = avail.right() - 窗口宽 + kNoteShadowMargin + 1
```

即:收起时窗口**右边缘与屏幕右边缘对齐**,只露出 `8 + 阴影留白` 的宽度;露出的
那一条就是拉手(自绘一个窄条 + 一个小箭头,颜色取 `Theme.colors.borderStrong`,
未读/逾期时取 `error`)。

滑动用 `QPropertyAnimation(pos)`,120–160ms `OutCubic`(theme.md §31 micro/normal
档),**不用 QML 动画**。

### 6.4 触发与防抖动(三个必须处理的坑)

| 坑 | 现象 | 对策 |
| --- | --- | --- |
| 滑入后鼠标仍在窗口内 → 立刻又滑出 | 窗口来回抽动 | 状态机:滑入/滑出动画期间 `m_sliding = true`,**动画结束才允许反向**;收起动作要求"鼠标离开 ≥ 700ms" |
| 用户正在打字,鼠标移开窗口 → 便签跑了 | 无法输入 | `QTextEdit` 有焦点(`hasFocus()`)**不自动收起**;正在拖动/缩放同理 |
| 只有 8px 可见时鼠标进不来(在屏幕外那部分收不到事件) | 永远收不回来 | 拉手**必须在窗口可见区内**:收起几何按 §6.3 保证 `kSliver ≥ 8` 落在屏幕内 |

补充保护:

- **自动收起只在便签不活跃时生效**:前台窗口是便签(`isActiveWindow()`)时不收起;
- 提供**每张便签**的「钉住(不自动隐藏)」开关(`auto_hide = 0`),以及托盘
  「全部展开」兜底(见 §7.3);
- 多屏/分辨率变化(`QGuiApplication::screenRemoved` / `screenGeometryChanged`)
  时**全部便签重新夹回其所在屏的可用区并重算收起几何** —— 否则拔掉副屏后,
  便签永远停在坐标 (3000, 200) 的虚空中。

### 6.5 拖拽实现选择:自绘拖动,不用 `startSystemMove`

面板的无边框拖动走 `startSystemMove()`(`panel_window.cpp:225`,含 `WM_NCLBUTTONDOWN`
兜底),但那套进入的是**系统模态移动循环**,程序拿不到"松手"这一刻,也就无法做吸附。
便签改为:标题条上 `mousePressEvent` 记 offset → `mouseMoveEvent` 跟随 → `mouseReleaseEvent`
做 §6.2 吸附。代价是拖动不如系统移动跟手一点点,收益是吸附可确定、可验收。

---

## 7. 宿主接线与入口

### 7.1 注册表(与贴图表**刻意不同**)

`app/src/note/note_registry.{hpp,cpp}`:

- `QHash<QString id, QPointer<NoteWindow>>`,`id → 窗口`;
- 与 `pinRegistry()`(`core_facade.cpp:509`)的区别:**没有淘汰**。超上限(建议 20 张)时
  `showToast("便签已达上限 20 张,请先关闭一些")` 并**拒绝新建**,不静默关旧窗;
- 提供 `hideAllForCapture()` / `restoreAfterCapture()`(§7.4)、
  `closeForEntry(id)`(条目被删时)、`reclampAll()`(屏幕变化时)。

### 7.2 入口

| 入口 | 位置 | 行为 |
| --- | --- | --- |
| 待办右键「贴到桌面」 | `Main.qml:2309` 的 todo 组 0,插在「编辑待办…」之后 | `Core.openTodoNote(id)` |
| 待办卡片按钮 | `EntryCard.qml:501-510` 的 CardAction 组,「编辑待办」旁加「贴到桌面」 | 同上 |
| 托盘「便签」子菜单 | `tray.cpp:98` 附近,与「今日待办」并列 | 列出**已打开的便签**(`关于` `显示/收起`),底部「全部展开」「全部收起」 |
| 热键 | `HotkeyMap` 增 `toggle_notes`(默认空 = 不绑定)。⚠️ 一处新增连带五处:枚举 `HotkeyAction`、`ALL` 数组长度(`settings.rs:364` 现为 9)、`Settings::default`(`:401`)、`SettingsDrawer` 快捷键行、`hotkeys.cpp` 的 `Action` 映射 | 有便签则全部收起/展开,无便签则打开"最近未完成待办"的便签 |

⚠️ 托盘子菜单是**必需的兜底**,不是锦上添花:便签是 `Qt::Tool` 窗口,不占任务栏、
不进 Alt+Tab,一旦被别的置顶窗口盖住就**再也点不到**。`core_facade.cpp:622-626` 为此
写过一段"漏一次用户就再也找不回来"的教训,便签必须从设计上避开它。

### 7.3 面板 → 便签 的单向同步

`NoteRegistry` 订阅 `CoreFacade::clipChanged`(已由 `WM_APP_CLIP_CHANGED` 驱动,
`qml_bootstrap.cpp:177` 同一条链路),做两件事:

1. **存活检查**:对每个打开中的便签 id 调 `todo_get`;返回 null → 关闭该便签
   (条目被删/被清空/被导入替换);
2. **刷新内容**:返回非 null → 若正文/完成态/截止与窗口当前值不一致则更新
   (避免自己刚写完又回灌、光标跳动)。

去抖 200ms,与 `ClipEntryModel` 同一节奏;便签数量是数十量级,`todo_get` 是主键查询,
成本可忽略。

### 7.4 截图期间隐藏便签

先把事实说清:`xcap 0.9.8` 的 Windows 后端是 `GetWindowDC` + `BitBlt`
(`xcap-0.9.8/src/windows/gdi.rs:108/117`,**只有 `SRCCOPY`,没有 `CAPTUREBLT`**);
而行内已知:不带 `CAPTUREBLT` 的 BitBlt **不包含分层窗口**(`WS_EX_LAYERED`)。
便签与贴图都是 `WA_TranslucentBackground` 窗口,Windows 上通常即分层窗口 ——
**所以它们很可能本来就不会被拍进截图**(需在目标机实测确认,见 §9 R3)。

即便如此仍然**要隐藏**,理由不是"技术上做不到排除",而是产品语义:

- 用户按截图热键的意思是"我要截屏幕上那个东西",而不是"我要把桌面便签也带走";
- 现有实现里**贴图不会被隐藏**,这本身是个待议的一致性问题(见 Q3);
- 一旦将来换捕获后端(DXGI/WGC,**会**包含分层窗口),不统一隐藏就会立刻翻车 ——
  这类"当前后端恰好没暴露"的隐性依赖,不应写进设计。

> 结论:隐藏是**产品约定**,不是对当前后端的补偿;实现代价只是两处调用。

接线点(两处,缺一不可 —— `startCaptureSession` 有**两条返回路径**):

1. **隐藏**:`capture_session.cpp:322` 里 `pruneOverlays()` 的"忙碌忽略"早退**之后**、
   任何捕获之前调 `NoteRegistry::hideAllForCapture()`(早退位置之前调会让"截图进行中
   被忽略"的那次触发顺手隐藏便签,却没人为它恢复);
2. **恢复**——按路径分开:
   - **框选/长截图**路径:`closeAllOverlays`(`:101`)的 `destroyed` 计数归零处
     (`:115-120` 已有"等最后一块遮罩真正销毁"的机制),便签跟随它恢复;
   - **全屏直通**路径(`:370-383`)**根本不经过 `closeAllOverlays`**(它自己
     `logMemory("capture-end")` 后就 `return`),必须在 `:379 finishCapture` 之后
     单独恢复。这是最容易漏的一条 —— 漏了的表现是"按一次全屏截图热键,便签全部
     消失且再也回不来"。

不用逐出口补 `show()`:除上述两处,其余出口都汇流到 `closeAllOverlays`
(`:407/441/456/468/566` 五处调用点已核对)。参考 `unparkPinAfterEdit` 的注释
(`core_facade.cpp:544`)——"逐出口补 show() 必漏"是已经付过一次学费的结论。

### 7.5 启动恢复

`main.cpp` 里 `core.setNotifyHwnd(...)`(`:376`)之后、`exec()` 之前:
`NoteRegistry::restoreAll()` 读 `todo_note_list`,**只为 `visible = 1` 的行建窗**
(`visible = 0` 的行是几何记忆,等用户下次点「贴到桌面」时才出窗)。

**焦点**:全部使用 `WA_ShowWithoutActivating` 显示(与 `capture_session.cpp:153` 同法),
不得抢前台 —— 开机自启场景尤其致命。`--hidden` 静默启动时是否恢复便签见 Q2。

---

## 8. 分阶段实施(N1–N5)

| 阶段 | 内容 | 可独立验证的验收 | 量级 |
| --- | --- | --- | --- |
| **N1 core** | v7 迁移 + `store::todo_note` CRUD + 3 个 FFI 方法 + 四处级联 + 导入/备份 + `check_db_integrity.py` | `cargo test` 新例全绿;`tools/verify_todo_note.py` 走真实 DLL 契约 | 0.5 天 |
| **N2 只读窗口** | `NoteWindow` 骨架(画正文/复选框/截止)、拖动/缩放/关闭、`todo_note_save` 持久化、右键入口 + 托盘子菜单 | 建窗→拖动→关闭→重启复原;位置与库一致 | 1–1.5 天 |
| **N3 编辑与同步** | `QTextEdit` 内联编辑(去抖写回)、勾完成、逾期着色、外部改动/删除同步 | 便签改字 → 面板刷新;面板删条目 → 便签关闭 | 1 天 |
| **N4 贴边/半隐藏** | 吸附、`autoHide`、滑入滑出动画、拉手绘制、屏幕变化重夹、钉住开关 | `--note-flow` 真鼠标驱动(见 §10.2) | 1–1.5 天 |
| **N5 收尾** | 截图期隐藏、数量上限提示、内存读数、文档回写(05/README 状态) | 截图里看不到便签;内存读数与 §9 R1 预估一致 | 0.5 天 |

总计约 **4–5 天**(单人),N1 与 N2 可并行开工(N2 先接假数据占位)。

---

## 9. 风险与对策

| # | 风险 | 影响 | 对策 |
| --- | --- | --- | --- |
| R1 | 多张便签的内存开销 | 每张 `~260×220` 逻辑像素的 backing store:DPR1 = 260×220×4 ≈ 0.22MB,DPR1.5 = 390×330×4 ≈ 0.52MB,加文本引擎按 1MB/张 估;20 张 ≈ 20MB 量级 | 数量上限 20;开窗/关窗打 `logMemory("note-open"/"note-close")`,与贴图同一读数习惯 |
| R2 | **`Qt::Tool` 窗口丢失入口** | 用户关不掉/找不回便签 | 托盘「便签」子菜单(§7.2)+ 全部展开兜底;**不留无入口的状态** |
| R3 | 截图把便签拍进画面 | 产物脏 | 当前后端(xcap GDI,`SRCCOPY` 无 `CAPTUREBLT`)大概率不含分层窗口 —— **须实测**;不论结果都按 §7.4 隐藏,换 DXGI/WGC 后端时该约定自动兜住 |
| R4 | 多屏/混合 DPI 下几何错位 | 便签跑到屏幕外 | 存逻辑像素 + `screen` 名;恢复与屏幕变化时一律夹回可用区;DPR 变化按 `pin_window.cpp:432` 延迟重算 |
| R5 | 编辑去抖与外部刷新互撞 | 光标跳到行首/输入被覆盖 | 写库时不回灌同 id;`clipChanged` 刷新前比对"规范化后正文"(trim + 换行统一) |
| R6 | 半隐藏时收不回来(拉手在屏幕外) | 便签"消失" | §6.3 几何保证 `kSliver` 在可用区内;R2 的托盘兜底 |
| R7 | v7 迁移引入备份兼容问题 | 旧 exe 拒收新备份 | 故意如此(版本闸门);发布说明写一句 |
| R8 | 用户把便签贴在任务栏侧并开自动隐藏 | 抢鼠标 | 下边吸附禁止 `autoHide`(§6.2 第 4 条) |

---

## 10. 测试与验收

### 10.1 core 单测 + 契约脚本

| 用例 | 断言 |
| --- | --- |
| v7 迁移 | 新表 + 索引存在;v6 库升级后 `todo_meta` 数据逐字段不变;`integrity_check ok` |
| CRUD | save 三态(缺失不改/显式改)、首次 save 建行(upsert)、`visible` 往返、非法 `edge` 拒绝 |
| 形状 | `todo_note_list` 条目**扁平** + `todo` + `note`,无 `entry` 嵌套、无 `note` 键串到 `todo_list` |
| 级联 | delete / delete_bulk / clear / clear_done 后 `todo_note` 无孤儿行 |
| 导入 | 备份 zip 导入后便签窗口状态随新 id 保留;重复导入不重复建窗 |
| 夹紧 | `save` 的 w/h/opacity/fontSize 越界被**夹紧**(不报错);`edge` 非法值**报错**(§4) |

脚本:`tools/verify_todo_note.py`,风格照 `tools/verify_todo.py`(临时数据目录、
按自身位置推导仓库根、覆盖契约与级联)。

### 10.2 交互验收(`nextboard.exe --note-flow`)

自带鼠标驱动的验收在本仓库是**唯一抓出过"按钮点不着"的手段**(见 `panel_window.hpp:100`
一段说明)。便签是原生 QWidget,注入方式比 QML 简单:

| 步骤 | 断言 |
| --- | --- |
| 建待办 → 开便签 | `todo_note_list` 有行且 `visible=1`;窗口几何 == 库中几何 |
| 关便签 → 重启 | 不建窗,行仍在(`visible=0`);再点「贴到桌面」回到**上次几何** |
| 拖标题条到右边缘 5px 处松手 | `edge` == `right`;窗口 `x + w` 对齐可用区右边 |
| 开 autoHide → 鼠标移开 900ms | `collapsed` == 1;可见宽度 == `kSliver + 阴影留白` |
| 鼠标移到拉手上 | 滑回,`collapsed` == 0,几何恢复展开值 |
| 改正文 + 失焦 | `todo_get` 的 `textContent` 一致;面板行文本刷新 |
| 面板删该条目 | 便签窗口在 400ms 内关闭;`todo_note` 无孤儿 |

⚠️ **Enter/Leave 必须用真实光标驱动**(`QCursor::setPos` + 等待),并在 `--data-dir`
隔离目录里跑(与 `--stitch-live-flow` 同一门禁:会移动鼠标)。

---

## 11. 待拍板(实现前需要回答)

| # | 问题 | 拍板结果(2026-10-18 按建议) | 影响面 |
| --- | --- | --- | --- |
| ~~**Q1**~~ | 「半隐藏」的语义 | ✅ **已拍板:贴边自动收起(留 8px 拉手)**;卷起留 v1.1;透明度另给了右键菜单三档(60/85/100) | §6 全部 |
| ~~**Q2**~~ | `--hidden` 静默启动(开机自启)时是否恢复便签窗口? | **恢复**,但 `WA_ShowWithoutActivating`;便签是桌面陈设,不恢复等于每次开机都丢布局 | §7.5 |
| ~~**Q3**~~ | 截图期间是否隐藏便签?是否**顺带**把贴图也一起隐藏(现状不隐藏)? | 便签**隐藏**;贴图维持现状(本次不动,单独记一条 backlog) | §7.4、docs/18 P2 |
| ~~**Q4**~~ | 便签数量上限与超限行为 | 上限 **20**,**拒绝新建 + toast**;绝不淘汰(淘汰 = 用户布局被静默销毁) | §7.1 |
| ~~**Q5**~~ | 置顶是默认行为还是开关 | 每张便签有 `top_most` 开关,**默认置顶**;关闭后跟随普通窗口层 | §3.1 |
| ~~**Q6**~~ | 一条待办能否开多张便签 | **不可**(1:1,主键即条目 id);想多贴就复制一条待办 | §3.1 |
| ~~**Q7**~~ | 屏幕**交接处**(这一侧紧接着另一块屏)是否做自动收起? | ✅ **已拍板(2026-10-19 用户):交界处不做自动收起**(方案 A)。几何上不成立 —— 要「藏起来」就得移出当前屏,而那个方向正是邻屏,没有「屏外」可用;强行收会变成「挪到邻屏、大半可见」,随即 enter/expand 弹回,形成闪烁死循环。取舍是**把不确定的行为换成确定的缺失**,并在 UI 上说明原因 | §6.2、§13.12 |

---

## 12. 与既有约定的对照(checklist)

- [ ] 数据层:迁移只追加、无外键、同事务级联(§3.3 四处)
- [ ] FFI:不新增导出函数、扁平载荷、`skip_serializing_if` 可选键、非法值显式报错
- [ ] 窗口:无边框 + 透明底 + 自绘阴影 + `framelessResizeHitTest` + DPR 延迟重算
- [ ] 入口:面板右键 / 卡片按钮 / 托盘子菜单 / 热键(默认空)四选齐,且**托盘兜底**
- [ ] 刷新:写库后不重复 `notify`;`clipChanged` 去抖 200ms
- [ ] 截图:统一隐藏/恢复(单一恢复点)
- [ ] 内存:`logMemory("note-open"/"note-close")`,数量上限
- [ ] 验收:core 单测 + `verify_todo_note.py` + `--note-flow` 三段齐
- [ ] 文档:实现后回写本文 §状态、`docs/05-todo.md` §1.2(范围)、README 状态
---

## 13. 实现记录(与初稿的偏离与踩坑)

### 13.1 六项拍板的结果(全部按 §11 的建议)

| # | 结论 |
| --- | --- |
| Q1 | 贴边后**自动滑出**,屏内只留 8px 拉手 + 8px 阴影留白;鼠标进入拉手滑回。右键菜单另给透明度三档(60/85/100),不额外做"半透明"设置项 |
| Q2 | `--hidden` 静默启动**恢复**便签,全部 `WA_ShowWithoutActivating`(不抢前台) |
| Q3 | 截图期间**隐藏便签**,会话结束恢复;贴图维持现状不隐藏(单列 backlog) |
| Q4 | 上限 **20 张**,超限 **toast 拒绝新建**,绝不淘汰 |
| Q5 | 每张便签有 `top_most` 开关,**默认置顶**;切换用 `SetWindowPos`,**不动窗口标志** |
| Q6 | 1:1(主键即条目 id),已开着则展开前置,不新建 |

### 13.2 实现中发现的、初稿没写到的坑

1. **不能继承 `PinWindow`** —— 落地时确认了这一点:便签的中心是子控件(复选框/正文/页脚),
   贴图的所有核心逻辑(位图旋转、编辑回写、文本渲染)在便签上都是死代码。最终独立成
   `app/src/note/note_window.{hpp,cpp}`,只复用机制(阴影画法/缩放命中/DPR 延迟重算)。

2. **`nativeEvent` 里绝不能调 `winId()`** —— 这是本次最耗时的一个坑:窗口创建期间
   (`WM_NCCREATE`)调 `winId()` 会递归触发 `CreateWindowEx` → 创建失败,日志刷满
   `Failed to create platform window` 而窗口**永远出不来**。修法与 `panel_window.cpp:430`
   一致:缓存 `m_cachedHwnd`,`WinIdChange` 时更新,别处一律用缓存。修完日志里该告警=0。

3. **自动收起不能看 `isActiveWindow()`**(初稿 §6.4 写错了)—— 鼠标移开时窗口**仍持有
   焦点**(没有别的窗口被激活),于是"鼠标早就离开、便签却永远收不起来"。验收 STEP3
   直接把这条抓了出来。最终判据是**鼠标位置**(`rect().contains(mapFromGlobal(QCursor::pos()))`)
   + **按键空闲**(3 秒内有过 `KeyPress` 才视为"正在打字")。`hasFocus()` 单独不够:
   光标移开时窗口照样有焦点。

4. **`setAutoHide(true)` 要主动起计时** —— 用户通常是从右键菜单勾的,那一刻鼠标本来
   就在窗口外,**不会再有 `leaveEvent`**;不起表的表现是"勾了自动收起但毫无反应"。

5. **拖动时必须夹进可用区** —— 不夹的话能拖到屏幕外一半,松手后既看不到也点不到,
   而且吸附判定永远够不到边缘(边界恰好是"贴住"的那一格)。夹的时候要允许溢出
   `kNoteShadowMargin` 那一圈,否则边框贴不到屏幕边。

6. **强制收起要绕过 `autoHide`** —— 托盘「全部收起」是显式指令;用户点了却毫无反应,
   比不支持收起更费解。改为 `collapse(animate, force)`。

7. **托盘「便签」子菜单是硬要求** —— 便签是 `Qt::Tool`(不占任务栏、不进 Alt+Tab),
   被别的置顶窗口盖住就再也点不到。二级菜单列出每张便签 + 「全部展开/收起」。
   ⚠️ 它复用 `QMenu#nbTrayTodoMenu` 的 QSS 选择器(否则深色下子菜单退回原生灰底)。

8. **`--hidden` 恢复便签的焦点** —— 必须 `WA_ShowWithoutActivating`;开机时一串便签
   抢前台是灾难。

### 13.3 落地清单

| 层 | 文件 |
| --- | --- |
| 迁移 | `core/nbcore/src/store/migrations/v7.sql`(`LATEST_VERSION` 6→7) |
| 数据访问 | `core/nbcore/src/store/todo_note.rs`(NoteEdge/NoteState/NotePatch/NoteItem/CRUD/自愈) |
| 级联 | `entry::clear` / `entry::delete_bulk` / `todo::delete` / `todo::delete_meta` / `todo::clear_done` |
| 备份导入 | `backup_export::read_todo_note` + 导入时按新 id 搬(旧备份无该表 → 宽容返回 None) |
| FFI | `todo_note_list` / `todo_note_save` / `todo_note_delete` + `note_patch()` |
| 窗口 | `app/src/note/note_window.{hpp,cpp}` |
| 注册表 | `app/src/note/note_registry.{hpp,cpp}`(开/关/恢复/同步/截图隐藏/上限) |
| 入口 | Main.qml 待办右键「贴到桌面（便签）」、托盘「桌面便签」子菜单 |
| 启动 | `main.cpp` 的 `restoreAll()` + `clipChanged → syncFromCore` + 屏幕变化 `reclampAll` |
| 截图 | `capture_session.cpp` 的隐藏(忙碌早退之后)+ **两条**恢复路径(框选走 `closeAllOverlays`、全屏直通单独补) |

### 13.4 验证

| 手段 | 结果 |
| --- | --- |
| `cargo test -p nbcore` | **345 passed / 1 failed**;唯一失败是 `translate::http::tests::send_dispatches_socks5_through_ureq`(域名解析 os error 11001),**改动前就红**(已用 `git stash` 在干净树上复现),与本次无关 |
| `tools/verify_todo_note.py`(真实 DLL 契约) | **全部断言通过**(v7 迁移/upsert/三态/夹紧/edge 报错/下边禁 autoHide/扁平形状/visible 往返/级联五路/备份导入换 id) |
| `tools/verify_todo.py`(既有待办回归) | **全部断言通过**(版本号断言改为读 core 源码,不再写死 6) |
| `nextboard.exe --note-flow`(真鼠标驱动) | **全部通过**(连跑 3 次均全绿):建窗→位置==库→拖到右边缘吸附→开自动收起→指针移开→收起(屏内仅 16px)→指针回拉手→滑回→**正文右键出便签菜单**→**菜单项真实触发**(贴左边/收起态取消贴边/不透明度落库)→改正文写回→删条目无孤儿。日志 `Failed to create platform window` = **0** |
| `nextboard.exe --todo-flow`(既有交互回归) | STEP1–STEP5 全部符合预期,未受 v7/便签改动影响 |
| `tools/check_db_integrity.py` | 新增 `todo_note` 表/索引/孤儿行三项体检;对**升级前**的旧库正确报出"v7 未执行" |
| `build.ps1` 全量构建 | 通过,零新增 warning |

### 13.5 右键菜单(2026-10-18 第二轮,用户反馈"存在 bug")

初版菜单只有 6 个平铺条目,复查发现**四个真缺陷**:

| # | 缺陷 | 现象 | 修法 |
| --- | --- | --- | --- |
| ① | `QTextEdit` 自带英文右键菜单并**吃掉** `contextMenuEvent` | 正文区右键只有 Copy/Paste/Select All,便签菜单**出不来**;只有标题条与边距上右键才有效(用户感知为"右键菜单坏了") | `setContextMenuPolicy(Qt::CustomContextMenu)` + `customContextMenuRequested → showToolbar` |
| ② | 给 `QMenu` 加了 `FramelessWindowHint \| NoDropShadowWindowHint` | 打乱 Qt::Popup 的抓取/关闭语义:点外部不关、或菜单闪一下就消失 | 不动 `windowFlags`,只用 QSS(与 `tray.cpp:83` 同一做法);并补 `::indicator` 规则 —— 有可勾选项时不给列宽,勾选标记会被裁掉 |
| ③ | 收起态点「取消贴边」→ `collapsed` 留在 1 | 窗口停在**按边算出的屏外位置**,鼠标永远进不来 —— 用户视角是"便签凭空消失" | 三层同时纠正:`setEdge` 先退出收起态;库侧 `normalized()` 强制 `edge=none ⇒ collapsed=0`;单测 `detaching_edge_clears_collapsed` 钉住 |
| ④ | 两条 `QPropertyAnimation` 同时驱动 `pos` | 前一条按自己的曲线继续写到结束 → "滑到一半又弹回去",且时快时慢(取决于两次触发的时间差) | `startSlide` 持有**唯一**动画实例:先 `disconnect` 再 `stop` 再重建(`stop()` 会 emit finished,不断开会污染 `m_sliding`) |

菜单结构也重排为分组 + 子菜单(状态全部反映当前值):

```
编辑待办…            组 0:内容(改待办本身)
标记完成 / 取消完成
复制正文              ← 走便签自绘的瞬时提示(主面板常隐藏,应用级 toast 看不见)
───
☑ 置顶               组 1:窗口行为
□ 贴边自动收起        ← 未贴边时禁用并给 tooltip("先贴到屏幕边缘")
  贴到屏幕边缘 ▸      ← 左/右/上/下 四选一(带勾选态)+ 取消贴边
  不透明度 ▸          ← 100/85/70/60
───
关闭便签             组 2:危险项置底(与列表右键菜单同一排布)
```

**为什么加显式子菜单**:只靠"拖到边 12px 内吸附"这一个入口,在小屏/高缩放下很容易
拖过头而没吸附,用户会判定"贴边功能是坏的"。显式入口把这条路径变成可发现的。

**菜单定位**:贴着屏幕右/下角右键时菜单会跑出屏幕,故先 `sizeHint()` 再夹进
`availableRect()`;弹出前 `activateWindow()` —— 便签是 `WA_ShowWithoutActivating`,
不先激活会让菜单刚出来就被自己判成外部点击。

### 13.6 验收补强:菜单不只是"看得到",还要"点得动"

`--note-flow` 新增两段:

- **STEP4.5**:正文区真投 `QContextMenuEvent` → 断言菜单弹出、含 7 类条目、
  「置顶」勾选态 == 实际状态;
- **STEP4.6**:菜单项**真实 `trigger()`** → 断言 `edge`/窗口位置/「取消贴边」清收起态/
  不透明度落库。这一段直接抓出了缺陷 ③。

同时修掉两个**验收自身**的假象(不改产品代码也得说清,否则后人会把它们当产品 bug):

1. **多屏：avail 只能算一次**。收起后窗口中心已在屏幕外,`screenAt()` 会退化到主屏 ——
   在 1755×1097 副屏上,"屏内可见宽"被算成 0,把正确实现判成缺陷。改为 STEP2 算一次、
   后续全程复用。
2. **真实光标不可靠**。`QCursor::setPos(1916,235)` 后立刻读回得到 **683,767** ——
   本机有其它进程在动鼠标。而且 Qt 只在**它自己观察到**指针跨越窗口边界时才发
   Enter/Leave,而拖动全程用的是合成事件、真实光标从未进入过窗口 → "把光标移走"
   不会产生 Leave,autoHide 计时根本不启动(STEP3 曾恒失败)。
   最终 `movePointerTo()` **两条路都走**:先真实移动光标(贴近用户路径),再显式投递
   Enter/Leave(保证确定性)。两者驱动同一段产品代码,重复投递幂等。

### 13.7 多屏与吸附边修复(2026-10-18 第三轮,用户实测反馈)

用户反馈两件事:"**只能贴到右边**"与"**无法移动到其他屏幕**"。查下来是**三处代码缺陷**
叠加,而同一条根因(把"记忆的屏名"当成了权威)。

| # | 缺陷 | 根因 | 修法 |
| --- | --- | --- | --- |
| ① | 拖不到别的屏 | `dragTo()` 用 `availableRect()` 夹取,而它**优先按 `m_screen`(库里的屏名)找屏** —— 便签一旦在某屏建起来,那块屏名就一直有效,夹取范围永远是那一块屏 | 引入 `virtualBounds()`(所有屏可用区的外接矩形)作为**拖动夹取**范围 |
| ② | 只能贴右边 | 同一条:`snapAfterMove()` 拿的是**旧屏**的边。窗口拖到副屏后,"左边/上边/下边"根本不在那个可用区内,四边里只有恰好与旧屏重合的那条(右侧)能命中 | `availableRect()` 改为**位置优先**:窗口现在在哪块屏上就以那块屏为准,`m_screen` 降级为"位置不可用时的兜底";拖动过程中 `adoptScreenUnderWindow()` 实时更新归属屏 |
| ③ | 位置记不住/重启乱跳 | `endDrag()` 写的是 `if (!snapped) saveGeometry();` —— **吸附成功那条路径只写了 `edge`,没写 x/y**。于是"拖到左边吸附 → 重启"会回到上一次存过几何的位置,而 `edge=left` 还在 → 窗口自己瞬移 | 吸附后**无条件** `saveGeometry()`;`setEdge()`(菜单贴边)也补一次;几何与 `screen` 一起落库 |

**判断依据**:位置是权威、屏名只是记忆。人在多屏间拖窗口时,唯一可靠的信号是
"窗口现在在哪";屏名只在"位置暂时不可用"(例如收起态大部分在屏外)时才有意义 ——
而那种情形 `anchorFor()` 取的可见锚点依然落在正确的屏上。

### 13.8 验收补强(多屏)

`--note-flow` 新增 **STEP2a**(跨屏)与 **STEP2b**(逐边):

- STEP2a:把窗口拖到另一块屏 → 断言①中心落在目标屏 ②库里 `screen` 已切 ③位置已落库
  ④**在副屏上贴左边**(直接回归"只能贴右边")。单屏机器打印 `SKIP` 而非 PASS ——
  假通过比没测更糟。
- STEP2b:左/上/下三条边逐个拖,断言 `edge` 落库 + 对应边贴住。

⚠️ 这一步同时修掉了**验收自身**的两个假象(不改产品代码也得记清):

1. **拖动靶点错了**:原 `dragTitleBar()` 把"光标要去的坐标"当靶,而窗口是
   `跟着按下点走` 的(`m_dragOffset = 按下点 − 窗口左上`)—— "移光标到 X"只会把窗口
   搬到 X − offset。单屏内 offset 很小,12px 的吸附阈值恰好吸收掉,看起来是对的;
   **跨屏后 offset 上百 px**,窗口落在离目标老远的地方,把正确实现报成缺陷。
   改为 `dragWindowTo()`(按**窗口位置**移动)与 `dragEdgeTo()`(把某条边搬到可用区)。
2. **靶子没摆干净**:窗口已在左边时只改 y 拖到上边,落点是**角落**,`left` 与 `top`
   距离都是 8px → 产品按固定顺序判 `left`(确定性行为,没错),但测试期望 `top` 就红了。
   `dragEdgeTo()` 改为**先把另一根轴搬到中间**,再走最后一段。

### 13.9 便签不跟随面板编辑(2026-10-18 第四轮,用户实测反馈)

**症状**:在面板里改完待办,已贴出的便签不变;**关掉重贴**才更新。

**根因:接错了信号。** 项目里有两条容易混淆的 `clipChanged`:

| 信号 | 发射源 | 覆盖的写入 |
| --- | --- | --- |
| `PanelWindow::clipChanged` | `nativeEvent` 收到 core 的 `WM_APP_CLIP_CHANGED` | **所有** sqlite 写入:剪贴板、`todo_update`、`todo_set_done`、`todo_delete`…… |
| `CoreFacade::clipChanged` | 宿主 C++ **主动**调 `notifyClipChanged()` | 只有两处:截图入库、编辑器提交 |

注册表当初连的是后者,而面板里的待办编辑根本不经过它 —— 于是便签永远停在打开
那一刻的内容。**改成 `PanelWindow::clipChanged`** 即可(同一信号也已经驱动着主列表
的 200ms 去抖刷新,口径一致)。

**顺手补的一处**:便签**自己**在打字时(800ms 去抖窗口内还没落库),同一信号也会
触发同步;此时若按库里的旧值回灌,会把用户刚敲的几个字直接冲掉。加了一条保护:
`m_bodyTimer->isActive()` 时不回灌,让本地改动到期落库(最后写入者获胜)。

**验收:先证伪,再修。** 新增 `--note-flow` 的 **STEP5c**:
用**面板编辑卡片的真实入口**(`todoSave`,不是裸调 `todo_update`)改标题与备注,
等一个事件循环后断言便签的标题条与正文区都已更新。

- 把信号临时改回旧写法:`STEP5c` 两行**都红**(正文 `""`、标题仍是旧值);
- 改回修复版:全绿,连跑 2 次稳定。

这条的价值在于:它断言的是"用户点保存之后便签会变",而不是"信号连对了没有"。

**教训**:一个工程里出现两个同名信号,靠名字猜必然接错。接线时该看"这个信号在
谁的 `emit` 里被发出",而不是"它的名字听起来像是对的"。

### 13.12 屏幕**连接处**的闪烁与收不回去(2026-10-19 第六轮,用户实测反馈)

用户原话:继续修复还是不行,在两块屏幕的连接处,无法正常贴屏,会出现闪烁且没有正常收回。

⚠️ 这一轮修的**不是** §13.11 那个缺陷。§13.11 修的是「归属屏翻转」(锚点取错端),
用户现在报的是**两块屏接缝处**的几何失效 —— 两者机理不同,单修前者不能解决后者。

#### 13.12.1 根因一:收起公式在连接处不成立

`collapsedPosition()` 的语义是「把窗口往贴边的那一侧推出屏幕,只留一条拉手」:

```
贴右收起 x = avail.right() - (kNoteSliver + kNoteShadowMargin) + 1
贴左收起 x = avail.left() - 窗口宽 + (kNoteSliver + kNoteShadowMargin)
```

这默认「该边外侧是空的」。屏幕连接处这个前提不成立。本机 `S24 PRO`(-1080..-1)与
`Mi Monitor`(0..1919)在 x=0 相接,窗口宽 196、拉手可见 16:

| 位置 | 收起 x | 窗口占位 | 落在邻屏的宽度 |
| --- | --- | --- | --- |
| S24 PRO 贴右 | -16 | [-16, 179] | **180px 在 Mi Monitor 上** |
| Mi Monitor 贴左 | -180 | [-180, 15] | **180px 在 S24 PRO 上** |

于是「收起」变成了「挪到邻屏上、而且几乎全可见」。死循环由此闭合:

```
收起 → 窗口挪到邻屏(仍占 180px 可见)
     → 鼠标本来就在那块区域 → enterEvent → expand() 弹回原屏
     → 鼠标离开 → leaveEvent → 700ms 计时 → collapse() 又挪过去
     → ……(无限循环 = 闪烁,且永远停在半收半展)
```

#### 13.12.2 根因二:leaveEvent 无条件起表,动画抖振被当成「用户离开」

滑入/滑出是 140ms 的 `QPropertyAnimation`,**窗口在鼠标底下移动**。Windows 于是连发
leave→enter→leave…;每次 leave 都起一遍收起计时器,动画一结束就收起、刚收起又展开。
即使不在连接处,这条也会让便签在边界条件下抖。

#### 13.12.3 修法

1. 新增 `edgeOutsideIsAnotherScreen(screen, edge)`:在贴边线外 2px、纵向屏中点取样,
   用 `QGuiApplication::screenAt()` 判断外侧是否另一块屏(走系统口径,不自比矩形,
   避免混合 DPI 下错位);
2. `collapse()` 在交界处**直接拒绝**,首次给一次 `showTransientHint`;
3. `setAutoHide(true)` 在交界处拒绝并提示;
4. `snapAfterMove()` / `setEdge()` 吸附或贴到交界边时**主动清掉 autoHide**,避免
   「勾着却永不生效」的假状态;
5. 钉住按钮与右键菜单项在交界处禁用,tooltip 说明原因;
6. `applyState()` 恢复旧数据时若发现正收在交界处,直接展开并落库;
7. `leaveEvent()` 增加 `isCursorOverWindow()` 守卫 —— 光标真在窗口外才起表。

#### 13.12.4 为什么要「拒绝」而不是想办法硬收

在连接处做半隐藏在**几何上不成立**:要「藏起来」就必须把窗口移出当前屏,而
移出去的方向正是邻屏 —— 没有「屏外」可用。可选的两条路:

| 方案 | 代价 |
| --- | --- |
| **A. 交界处不做自动收起**(本轮采用) | 交界处少一个便利功能,但行为确定、不会抖 |
| B. 交界处把「收起」改成「窗口缩成窄条」 | 需要 resize 而非 move,且要存/恢复尺寸;语义从「半隐藏」变成「缩小」 |

✅ **用户已拍板(2026-10-19):采用 A** —— 交界处不做自动收起(见 §11 Q7)。
它把**不确定的行为换成了确定的缺失**,并且在 UI 上说清楚了原因(tooltip + 一次性提示)。

B 不实现,但记入 §13.10 已知未做:「交界处若要收起,只能改成缩成窄条(resize 语义)」——
那是另一种形态,需要独立评估,不要顺手改。

#### 13.12.5 验收

交界处拒绝收起是**新的正确行为**,原来「已收起」的断言在那里必然失败。
`--note-flow` 相应改造:

- `collapsibleEdgeFor(QScreen*)`:按两侧邻屏情况挑一条非交界边;
- `edgeOutsideIsAnotherScreenForTest(window, edge)`:测试侧同口径复算;
- STEP3b / STEP4.6 按 `atJunction` 分流:交界处断言「未收起 + autoHide 已关」,
  非交界处断言「已收起」。

结果:**67 PASS / 0 FAIL**。STEP3b 日志同时给出「该边是否屏幕交界: true」与
三轮全部通过,证明这条路径被真实覆盖(而不是被跳过)。

### 13.11 多屏相邻边界的来回横跳修复(2026-10-19 第五轮,用户实测反馈)

用户原话:贴屏到一块屏幕的一边、而这边还连着另一块屏幕时,自动收起会在两块屏之间
来回切换;并且要求**取消贴上边和贴下边**(实际使用中无法触发),以左右为主。

**根因是两层叠在一起:**

| # | 缺陷 | 机理 |
| --- | --- | --- |
| ① | `anchorFor()` 取的是**贴边那一端** | 左贴时锚点取 `geo.left()+4`,而收起后窗口是**往左推出屏幕**的 —— 这个点落在左邻屏上,`screenAt()` 于是认到邻屏 |
| ② | `adoptScreenUnderWindow()` 收起态也按几何认屏 | 收起是**程序**把窗口推出屏外的动作,不代表用户意图。①认出的邻屏被写进 `m_screen`,下次展开按邻屏的边算 → 落回原屏 → 再认回邻屏,**来回振荡** |

两处都不是「再微调一下阈值」能解决的:它们都在语义层面把「收起后的位置」当成了
「用户想把便签放在哪块屏」的证据。

**修法:**

1. `anchorFor()` 改为取**屏内那一端**(贴左边取 `geo.right()`,贴右边取 `geo.left()`);
2. 新增 `NoteWindow::screenForNote()` 统一归属判定:**展开态看窗口中心,收起态看记忆的屏名**
   (记忆的屏没了才退到锚点/光标屏);
3. `adoptScreenUnderWindow()` **收起态直接返回** —— 归属屏只在「用户拖动/缩放结束」
   与「屏幕布局变化」两个时刻更新;
4. 上下边下线(见 §6.2 修订):菜单只剩左/右,`collapse()`/`setAutoHide()` 的 bottom 特判
   一并删除,`expandedPosition()`/`collapsedPosition()` 去掉纵向分支,拉手绘制恒为竖条;
5. 核心侧 `normalized()` 把 top/bottom 归一成 none(历史数据自愈),新增
   `NoteEdge::is_active()` 表达「产品面仍支持的边」。

**验收(`--note-flow` 新增 STEP3b):** 连续 3 轮 收起→展开,断言 ①每轮收起后库里的
`screen` 不变 ②三轮后 `screen` 仍等于起点 ③展开后位置不漂移(±2px)。

> STEP3b 必须贴**左边**才验得出来:本机主屏左侧接着 S24 PRO(x=-1080..0),
> 收起时窗口被推出左边界,旧实现的锚点正好落进邻屏。贴右边时锚点落在屏幕外的
> 虚空里,`screenAt()` 返回空 —— 缺陷反而复现不出来(第一版验证就踩了这个坑,
> A/B 跑出「旧代码也全绿」的假结论)。

**A/B 留痕**(同一台三屏机:Mi Monitor 0..1920、TL160MDMP03-0 在下方、S24 PRO -1080..0):

```
旧代码(临时回滚两处修复):
  STEP3b 起点 screen: "Mi Monitor" edge: "left"
  FAIL 第 1/2/3 轮收起后归属屏未变
  FAIL 三轮后归属屏未变      三轮后 screen: "S24 PRO"
  FAIL 展开后位置不漂移      geo: QRect(-1088,421 ...)   <- 跳到邻屏上
  失败项:5

新代码:
  STEP3b 起点 screen: "Mi Monitor" edge: "left"
  PASS 第 1/2/3 轮收起后归属屏未变
  PASS 三轮后归属屏未变      三轮后 screen: "Mi Monitor"
  PASS 展开后位置不漂移      geo: QRect(-8,421 ...)
  全部通过(66 PASS / 0 FAIL)
```

这一步的价值不在「修好了」,而在**它能在旧代码上变红** —— 没有 A/B,就无法排除
「新验收其实什么都没验」这种最常见的假绿。

### 13.10 已知未做(留 v1.1)

- **卷起**(折叠成标题条、原地不动)—— 与"自动滑出"是两套几何,不宜同批;
- 便签内改截止/提醒/标签:当前右键「编辑待办…」跳回面板编辑卡片(复杂控件只实现一份);
- 便签透明度没有可拖动的滑杆,只有右键四档(100/85/70/60);
- **贴图**仍不参与截图期隐藏(与便签口径不一致,单列 backlog);
- 布局持久化只覆盖便签,不动贴图(路线图 backlog 的「贴图布局持久化」仍开着);
- ~~上下边吸附~~ **已于 2026-10-19 从产品面下线**(用户裁定:实际使用无法触发),见 §6.2 修订与 §13.11。
- **屏幕交接处的收起**(方案 B:缩成窄条)—— 交界处按 §11 Q7 不做自动收起;若确实需要「交界处也能收」,那是 resize 语义的新形态,需独立设计;
