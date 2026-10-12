# 05 · 待办(Todo)功能设计

> 状态:**已实现**(v6)。本文档是实现依据;实现过程中的偏离在文末「实现记录」回写。
> 需求原话:"在翻译按钮的左边添加一个新的按钮,可以在主界面列表中新添加一项,
> 属于新的类型(主要为文本,可以设计何时截至,何时提醒,何时通知)"。
>
> 已拍板的开放决策:
> ① 待办由用户显式创建,不走剪贴板监听、不参与去重;
> ② 完成的待办**归档**:留在列表里、划线展示;
> ③ 通知以**托盘气泡**先行,Windows 原生 Toast 列为后续增强;
> ④ 提醒/通知在 UI 上**合并为一个时刻**(到点同时"面板高亮 + 托盘气泡"),
>    core 的两通道字段保留以兼容数据与后续拆分。

---

## 1. 需求与范围

### 1.1 要做什么

主面板头部新增「待办」入口(翻译左侧),点击后新建一条**待办条目** ——
它与 text/image/file 并列的第四种条目类型 `todo`,主体是文本,并携带可选时刻:

| 概念 | 定义 |
| --- | --- |
| 截止 due | 条目**属性**:什么时候到期。到而未做 → 列表标"逾期",只改变展示,不主动打扰 |
| 提醒 remind | **面板内提醒**:到点后面板内呈现(列表刷新 + 头部按钮转 primary);不打扰桌面 |
| 通知 notify | **系统级提醒**:到点弹托盘气泡(点击气泡呼出面板并切到待办视图) |

`due` 是数据,`remind`/`notify` 是两种**提醒通道**。条目可以只设 due(纯展示),
也可以设提醒时刻。

### 1.2 范围裁剪

**做**:创建/编辑/完成(归档划线)/删除;due/remind/notify 三字段;调度到点通知;
导航"待办"视图与计数;搜索命中待办正文;标签筛选。
**不做**(后续增强):重复周期、子任务、项目分组、待办转剪贴板、Windows 原生 Toast、
声音提示、多提醒时刻。

### 1.3 与剪贴板监听的关系(已拍板)

- 待办**不经剪贴板监听入库**,由用户在 UI 显式创建;
- **不参与 blake3 去重**、不参与 `touch` 置顶(复制同样文本不影响待办);
- **不参与超限淘汰**,也不计入历史上限 —— 否则待办会被静默吞掉、上限被永久占用;
- **不参与「清空历史」** —— 清空剪贴板不应误删用户任务。

---

## 2. 数据模型与迁移

### 2.1 决策:待办即条目(kind = 'todo')+ 独立扩展表

`clip_entries.kind` 增 `'todo'`,待办专属**时刻字段**放扩展表 `todo_meta(id = 条目 id)`。
理由:列表/搜索/标签/固定/删除/计数全部复用既有链路,只有时刻字段进扩展表 ——
避免每行 list_entries 都背时刻载荷,也避免两套序列化分叉。

复用的是 `clip_entries.tags` 列(不另建待办标签表),所以标签筛选/统计天然可用。

### 2.2 v6 迁移

`migrations/v6.sql`,要点:

1. **重建 clip_entries**(SQLite 不能改 CHECK):列清单取 v5 之后的最终形态
   (`id/kind/text_content/blob_path/file_paths/width/height/hash/byte_size/preview/pinned/created_at/last_used_at/source/tags/ocr_text`),
   索引全部重建。
2. ⚠️ **FTS 触发器必须重建**:触发器挂在旧表上,`DROP TABLE` 会连它一起删掉。
   (v3 当年没这问题是因为 FTS 是 v4 才引入的。)漏了这步搜索会静默失效,单测会红。
3. 新建 `todo_meta`:

```sql
CREATE TABLE IF NOT EXISTS todo_meta (
  id           TEXT PRIMARY KEY,              -- = clip_entries.id
  due_at       INTEGER,                       -- 截止时刻,unix 毫秒;NULL = 无截止
  remind_at    INTEGER,                       -- 面板内提醒时刻;NULL = 不提醒
  notify_at    INTEGER,                       -- 系统通知时刻;NULL = 不通知
  done_at      INTEGER,                       -- 完成时刻;NULL = 未完成(归档判定)
  reminded     INTEGER NOT NULL DEFAULT 0,    -- remind 已触发(调度去重)
  notified     INTEGER NOT NULL DEFAULT 0,    -- notify 已触发
  bubble_shown INTEGER NOT NULL DEFAULT 0     -- 气泡已播报(宿主确认制去重)
);
```

与 v1~v5 一致:**无外键**,两表一致性由应用层维护(删条目时显式删扩展行)。

### 2.3 条目载荷形状(宿主契约)

`list_entries` 与 `todo_list` 返回的条目**形状一致**:扁平 ClipEntry 字段 +
（仅待办）一个 `todo` 徽标键。这是宿主 `ClipEntryModel::applyPage` 单一解析路径的前提。

```json
{
  "id": "...", "kind": "todo", "textContent": "买牛奶", "tags": "[\"生活\"]",
  "dueAt": 1791808378619, "remindAt": 1791804778619,
  "todo": { "dueAt": 1791808378619, "done": false, "overdue": false }
}
```

- `todo` 键**只在 todo 条目上出现**(`skip_serializing_if`),与 v5 的 tags 同一兼容手法;
- 徽标只含卡片正面所需的 `{dueAt?, done, overdue}`;`remindAt/notifyAt` 为编辑预填
  下发在**顶层**(见 §5 的 todo_get);
- ⚠️ `TodoItem` 是**手写 Serialize** 把 ClipEntry 摊平到顶层,不用 `{entry: {...}}`
  嵌套 —— 嵌套形状会让宿主按扁平解析出整页空行(实测过)。
- `overdue` 需要 now,`from_row` 拿不到 → 由 `entry::list` 返回前统一补算
  (`stamp_todo_overdue`),不为一行数据把 now 透传进 SQL。
- `hash` 列不下发(既有口径)。

---

## 3. 三组时间的语义

| 字段 | 语义 | 触发的行为 | 允许值 |
| --- | --- | --- | --- |
| `due_at` | 截止。**纯数据**,到点不推送 | 列表展示:未完成且过期 → error 色"已逾期";今天到期 → "今天 HH:mm 截止";待办视图逾期区置顶 | NULL 或毫秒;可早于当前(创建即逾期,合法) |
| `remind_at` | 面板内提醒 | 待办视图置顶、列表刷新、头部「待办」按钮转 primary | NULL 或毫秒 |
| `notify_at` | 系统通知 | 托盘 `showMessage` 气泡;点击 → 面板弹出并切"待办"视图 | NULL 或毫秒 |

补全的语义规则:

- **一个条目每类时刻至多一个**;
- **时间粒度 = 分钟**(UI 按分钟选,DB 存毫秒);
- **时区一律本地时区**:DB 存绝对毫秒,展示按本地时区格式化;
- **错过补发(missed)**:应用未运行期间到点的 notify,启动后首轮扫描触发,
  气泡文案前缀"错过的提醒:"(`pending_bubbles` 用 `startup_ms()` 判定);
- **完成语义**:done 后 due 不再参与逾期展示,remind/notify 取消(已过点也不补发);
  **取消完成**后已过期的提醒**不再补发**(标志位不复位),避免取消瞬间轰炸;
- **编辑已触发过的提醒**:改到未来 → `reminded` 复位(允许再次提醒);
  改到过去 → 视为"立即提醒",下一轮调度触发一次。

---

## 4. 调度与通知

### 4.1 调度线程(`todo.rs`)

- `nb_init` 时 spawn,名字 `todo-scheduler`;
- **轮询 15s**:到点精度是分钟级,15s 最坏延迟远小于粒度;精确 sleep 需要处理
  "提醒被改后重算失效"的复杂度,不划算;
- 每轮:remind / notify 各取一批"未完成、未触发、时刻 <= now"(LIMIT 8,防轰炸),
  **命中即写标志位**再通知宿主 —— 通知丢失(宿主未起)宁可少一次气泡,不重复轰炸;
- 命中则 `notify_host(WM_APP_TODO_DUE)`;
- 退出:`SHUTDOWN` 检查,最长 15s。

### 4.2 通知通道与双层去重

- `WM_APP_TODO_DUE = 0x8000 + 4`(+3 为 CAPTURE_READY 预留,+10 单实例占用);
- **载荷问题**:WM_APP 不带 payload(与 CLIP_CHANGED 同哲学,不往 PostMessage
  塞指针/字符串)。宿主收到信号后 `call("todo_due_pending")` 拉取待播报列表;
- **两层去重**:调度层保证"到点只产生一次事件"(`reminded/notified`);
  宿主层保证"气泡只弹一次"(`bubble_shown`,播报后回写 `todo_due_mark_shown`)。

### 4.3 宿主接线

```
todo 调度线程 → notify_host(WM_APP_TODO_DUE)
  → PanelWindow::nativeEvent → emit todoDue()
      ├─ Tray::onTodoDue()         拉 todo_due_pending → 逐条 showMessage
      │                             → todo_due_mark_shown
      └─ ClipEntryModel::refreshNow()  列表/角标立即刷新(绕开 200ms 去抖)
气泡点击 → QSystemTrayIcon::messageClicked → CoreFacade::todosRequested
  → PanelWindow::showCenteredOnTodos()  → showCentered + QML showTodos()
```

⚠️ `messageClicked` 在部分 Win10/11 环境不触发(通知中心设置/免打扰);那时气泡
本身已起提示作用,用户从托盘图标也能打开面板看到计数 —— 属可接受的降级路径。

---

## 5. FFI 契约(JSON 门面,不新增导出函数)

| 方法 | 请求 | 响应 data | 错误 |
| --- | --- | --- | --- |
| `todo_create` | `{text, dueAt?, remindAt?, notifyAt?, tags?}` | `{id}` | 正文空 → InvalidInput |
| `todo_update` | `{id, text?, dueAt?, remindAt?, notifyAt?, tags?}` | Null | id 不存在/非 todo → InvalidInput |
| `todo_list` | `{done?: "open"\|"done"\|"all", query?, tag?, due?: "today", limit, offset}` | `{items, total}` | done/due 非法值 → InvalidInput |
| `todo_get` | `{id}` | 扁平条目 + todo 徽标 + 三时刻 | — (不存在 → null) |
| `todo_set_done` | `{id, done}` | Null | id 不存在 → InvalidInput |
| `todo_delete` | `{id}` | Null | 同上;同时删 todo_meta |
| `todo_counts` | `{}` | 同 `entry_counts` | — |
| `todo_due_pending` | `{}` | `[{id, text, at, missed}]` | — |
| `todo_due_mark_shown` | `{ids}` | Null | — |
| `todo_clear_done` | `{}` | Null | — |

时间参数的三态语义(**关键**)：

- `create`:键缺失与 null 等价(不设);
- `update`:**键缺失 = 不改**;`null` = 明确清空;整数 = 设置。
  宿主据此让"清空时刻"与"没动这个字段"可区分。

语义细则:

- `todo_update` 的 `tags` 同样三态:**缺失 = 不改**、**空数组 = 清空**。
  ⚠️ 宿主编辑时必须**总是**下发 tags(哪怕是空数组),否则"在编辑器里删光标签
  再保存"会静默无效(实测过)。
- 空数组与 NULL 同义(都表示"无标签"),与 `entry::set_tags` 同一口径 ——
  避免 `list_tags` 里冒出一个空标签行。
- `list_entries` 的 kind 过滤、`entry_counts` 的 todoOpen/todoDone 计数;`evict`/`clear`
  排除 todo;
- **列表排序**:`list_entries` 保持 `pinned DESC, last_used_at DESC`(主列表里待办
  与其它类型混排,创建/编辑会 touch last_used_at 自然上浮);`todo_list` 单独排序:
  未完成在前(逾期 > 今日到期 > 未来 > 无期限,组内 due_at 升序),归档按 done_at
  降序垫底。
- `todo_list` 支持 `query`(正文 LIKE,通配符转义)与 `tag`(json_each 精确匹配),
  与 `list_entries` **同语义** —— 宿主对两个端点下发同一组筛选参数,否则
  "标签筛选/搜索 + 待办页"组合会与主列表给出不一致结果集。

---

## 6. UI 设计

### 6.1 头部入口

翻译按钮**左侧**插入:`iconName: "todo"` + 文案「待办」;有未完成待办时按钮转
`primary` 变体(面板内"有提醒"的常驻线索)。点击 = 打开新建卡片。

### 6.2 新建/编辑:居中卡片

复用 `tagEditorCard` 的"屏障 + 居中卡片"模式(Esc/点外部关闭、Enter 提交)。字段:

| 字段 | 控件 | 说明 |
| --- | --- | --- |
| 正文 | `TextEdit`(多行) | Enter 提交、Shift+Enter 换行;空则拒绝提交 |
| 截止 | 时间字段 + 时间选择器浮层 | 滚轮选年月日/时分 + 快捷钮(今天 18:00 / 明天 09:00 / +1 小时 / 清空) |
| 提醒 | 同上 | 设了截止且提醒未设 → 默认联动"截止前 10 分钟";**用户手动设置过提醒(浮层确定/✕清空)后,改截止不再自动覆盖** |
| 标签 | 逗号分隔输入 | 可空;解析/去重与标签编辑器一致 |

**时间选择器交互契约(§6.2 修订,修掉首版三个缺陷)**:

1. **热区声明顺序即命中优先级**:字段内"点击打开浮层"的 `MouseArea` 必须声明在
   内容 `RowLayout` **之前** —— 首版声明在后,铺满整行盖住了 ✕ 清空钮,
   于是"点 ✕ 变成重新打开浮层",用户以为设好了时间、提交时字段其实仍是 0,
   表现就是"设了时间却保存不上"。
2. **关闭策略不含 `CloseOnPressOutside`**:它会把"点保存"那一下吃掉用于关浮层
   (点击不穿透到按钮),用户就会感觉"必须点别处才能关掉轮子,否则保存没反应"。
   改为浮层内「确定/取消」+ 屏障统一处理外部点击。
3. **滚轮改动实时写回字段**(第二轮修订):曾改成"点确定才写回",结果真实点击
   「确定」无效(见第 5 条),用户的时刻怎么都设不上。实时写回后,滚轮一动
   值就进字段 —— **时间必然被设定**,按钮退化为"关闭浮层"的辅助动作;
   「取消」恢复打开前的值再关闭。
4. **空值打开时滚轮归位到"现在"**:否则停在默认下标(3 年前),用户接下来拨的
   两下多半是在纠年份。
5. **「确定/取消」按钮必须放在 `TodoDatePicker` 内部**(与滚轮同一子树),
   不能作为 Popup `contentItem` 的兄弟项 —— 首版放平级,合成点击探针显示
   按钮位置的最顶层项是 Popup 的默认 background Rectangle 而非按钮,
   点击无效(实测)。
6. **月份换算口径只有一条**:列 values 一律是**数值本身**(月 = 1..12、日 = 1..n),
   与 JS 的 0 基月份只在"进/出 `new Date`"两处换算。首版三处口径互相打架
   ("预填偏一个月 + 选完偏一天"),是时间设定错乱的根源。
7. **预填全程包在 `syncing` 里**:`setCurrent` 会逐个触发
   `onCurrentIndexChanged → rebuildFromTumblers`,若只包住 `selectedMs` 赋值,
   "年已定月未定"的中间组合会一路 `changed()` 出去,把字段写成中间值。
8. 保存失败时**卡片与输入保留**(错误原文显示在错误行),不关卡片。
9. **合成点击验收的注入目标**(测试基建,第二轮踩坑):给按钮投真实鼠标事件时,
   必须发给 **QQuickWidget**(QWidget 层,它会完整转发进 QML 场景),
   不能直接 `sendEvent` 给 `QQuickWindow` —— 后者绕过窗口系统鼠标状态,
   事件被 Overlay 的 popup 容器拦下,**所有按钮都点不着**(实测连卡片上的
   「保存」也一样),探针结果全是假象。另:查找浮层内按钮必须从窗口的
   `contentItem` 递归(Popup 内容挂在 Overlay 层,不在 QML rootObject 子树)。

### 6.3 列表呈现

- **导航标签页**增「待办」(`viewId: "todo"`,icon `todo`),计数 = `todoOpen`(未完成数);
  点切 `Entries.setView("todo")` → 模型请求 `todo_list`,且**固定传 `done: "all"`**
  —— 完成是归档不是删除,从待办页消失会像"数据丢了"。
- **EntryCard 的 todo 形态**:
  - 元信息行最左放**复选框**(点击 = `todo_set_done` 切换);完成后正文划线
    (`font.strikeout`)+ 透明度 0.55 + 待办视图垫底;
  - due 展示:未完成且过期 → error 色 + "已逾期"(不显示时长);
    今天到期 → "今天 HH:mm 截止";其余 → "截止 MM-dd HH:mm";
  - 已设提醒且未完成 → 一枚铃铛徽标(`bell` 图标,提醒/通知已合并为一个时刻);
  - 卡片右下角「编辑待办」按钮,与右键菜单「编辑待办…」同语义;
  - **单击卡片不做任何事**(完成只走复选框/右键)—— 首版单击切换完成态,
    "点一下卡片看正文"就把任务勾掉了,误触率过高;
  - 主列表(all)混排时同样展示上述元素。
- **类型徽标文案中文化**:text/image/file/todo → 文本/图片/文件/待办;
  来源徽标对 todo 不显示(类型徽标已表达,重复即噪音)。
- **标签**:卡片上的标签行 = 色点 + 名字(与筛选条同一色函数),让标签有可认的
  "图标",不再是一串与正文混在一起的彩色文字。

### 6.4 交互细节

- 新建待办后面板**不关闭**(与"复制后收起"相反,用户可能连续建几条),Toast "已添加待办";
- 删除走现有 `deleteEntryWithToast`,core 侧连带删 todo_meta;
- 搜索框命中待办正文(FTS),结果里 todo 卡片同样显示复选框与截止。

### 6.5 面板未打开时

气泡点击才弹面板(不自动弹,避免打扰)。托盘 tooltip 保持产品名,不显示计数
(角标方案同理不做 —— Windows 托盘改图标有闪烁成本)。

### 6.6 托盘「今日待办」二级菜单

不打开面板就能看到今天还有什么(§1.2 裁剪项的落地)。

- **一级项**:「今日待办 (N)」,插在「打开主面板」之后;N = 今日关注数,菜单
  打开时刷新。具体待办条目**只放二级菜单**——一级菜单放不下(既定决策)。
- **口径**:未完成 且 (已逾期 或 今天到期),等价 `due_at ≤ 今天 23:59:59.999`
  (本地时区,复用 `end_of_today_ms` 单点边界)。经 `todo_list` 的可选参数
  `due: "today"` 由 **core 过滤** —— "…共 N 条"需要 SQL 层精确 COUNT,
  宿主过滤拿不到总数。
- **二级菜单条目**:`状态 · 正文首行`(逾期 → "已逾期",今天 → "今天 HH:mm";
  正文首行截 20 字 + 多行加省略号),排序沿用 §5;**上限 8 条** + 禁用态
  "…共 N 条";空态 = 禁用态"今天没有待办";FFI 失败 = 禁用态"暂不可用"。
- **交互**:点条目 → 弹面板切待办视图并**定位高亮**该条
  (`PanelWindow::showCenteredOnTodo(id)` → QML `showTodos(id)` →
  `indexOfId` + `positionViewAtIndex` + 高亮 1.8s);「查看全部」→ 只切视图。
- **刷新时机**:一级菜单 `aboutToShow` 钩子(与「延时截图」动态文案同点)重建
  子菜单,**不做后台轮询**;与 §4 的气泡双层去重互不相干。
- **QSS**:托盘样式表选择器扩为 `QMenu#nbTrayMenu, QMenu#nbTrayTodoMenu`
  (#nbTrayMenu 只匹配一级本体,子菜单退回原生样式会与浅色主题割裂)。

---

## 7. 测试与验收

### 7.1 单测(nbcore `cargo test`,现 201 个全绿)

| 用例 | 断言 |
| --- | --- |
| v6 迁移 | 旧行逐字段保留(pinned/source/tags/ocr_text);'todo' 可插入、'html' 被拒;索引齐全;FTS 触发器仍生效;integrity_check ok |
| 排序 | 逾期 > 今日 > 未来 > 无期限,归档垫底;完成后不再标逾期 |
| 序列化形状 | todo_list 条目**扁平**(顶层 id/kind/textContent + todo 徽标),无 `entry` 嵌套、无 hash |
| list_entries 联查 | todo 条目带徽标(逾期已补算),非 todo 条目无该键 |
| 过滤 | query 命中正文且通配符转义;tag 精确匹配;两条件可叠加 |
| crud | create/get(三时刻回显)/update(清空时刻、改正文、空数组清标签)/set_done 往返 |
| 扫描 | 未到期不进、done 不进、已触发不进;两通道独立 |
| 气泡去重 | 未触发无待播报 → 触发后有 → 播报确认后不再弹 |
| 淘汰/清空排除 | 待办不被淘汰、不计入上限;clear_history 不动待办;clear_done 只清归档;删条目无孤儿 meta |

### 7.2 端到端(`tools/verify_todo.py`,经 nbcore.dll 真实契约)

运行(前置:`cargo build --release` 产出 nbcore.dll):

```
python tools/verify_todo.py
```

独立临时数据目录跑,覆盖:v6 建库 → CRUD → 形状契约 → 两端口径一致 →
时刻三态 → 排序 → 计数 → due=today 过滤 → **到点扫描 + 气泡去重**(等一个调度周期)
→ 清空/删除无孤儿。脚本路径按自身位置推导仓库根,不写死绝对路径。

### 7.3 交互验收(`nextboard.exe --todo-flow`)

驱动逻辑在 `app/src/host/acceptance.cpp`(单独成文件,避免淹没 main.cpp 的
产品入口)。分步执行"新建卡片 → 点截止字段开浮层 → 拨轮 → 点确定 → 输入正文
→ 保存",每步读回状态打日志:

| 步骤 | 断言 |
| --- | --- |
| STEP1 | 卡片与浮层均可见;浮层几何完整落在 overlay 可视区内(`insideX/insideY` = true) |
| STEP2 | 点确定后浮层关闭;字段值 == 目标毫秒(精确相等);提醒正确联动 |
| STEP4 | 库中条目 dueAt == 目标毫秒(端到端落库) |

⚠️ **验收写库的隔离**(必须):验收会真的建条目,故

- `--data-dir <path>` 覆盖数据目录(必须在任何 `appDataDir()` 调用**之前**解析);
- 指定 `--data-dir` 时**跳过单实例门禁**,否则第二次验收启动会被静默挡掉;
- `--todo-flow` 若**没**配 `--data-dir`,自动改道到 exe 同级 `acceptance-data/`
  并打 WARN —— 验收绝不能写用户真实数据。

### 7.4 库结构完好性(`tools/check_db_integrity.py`)

针对"迁移重建表"这一类高风险改动,对**任意库**做只读体检:
版本号、FTS 三个触发器是否都在(漏了搜索会静默失效)、索引齐全、
`clip_fts` 与主表行数一致、重建表列未丢,并用一次"插入→查索引→删除"的
**触发器探针**证明触发器真在工作(不是空壳),最后 `integrity_check`。

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| v6 重建表漏列/漏索引 | 老数据字段丢失,最严重 | 列清单写死;单测断言逐字段相等 + 索引齐全 + integrity_check |
| **漏重建 FTS 触发器** | 搜索/索引同步静默失效 | v6 内显式重建三个触发器;单测断言重建表后触发器仍生效 |
| from_db 漏 `Todo` | 老 exe 读新库整表炸 | `EntryKind` 三处匹配一次改全;同包分发;发布说明标注 |
| 调度线程与 UI 线程竞争 todo_meta | 偶发 SQLITE_BUSY | 写只在命中行(罕见);rusqlite 连接已有 Mutex |
| 气泡在 Win10/11 行为差异 | 通知点击无响应 | 降级:气泡已起提示;托盘图标可打开面板 |
| 提醒风暴(大量同分钟到期) | 气泡轰炸 | 调度单轮 LIMIT 8 |

---

## 9. 实现记录(与初稿的偏离)

1. **响应形状**:初稿示意 `{"todo": {dueAt, done, overdue, remindAt, notifyAt}}`,
   实现里徽标只含 `{dueAt, done, overdue}`(卡片正面所需最小集),
   `remindAt/notifyAt/doneAt` 由 `todo_get` 在**顶层**下发(编辑预填/推迟要用)。
2. **todo_list 的过滤参数**:初稿只列 `done/overdueFirst`;实现增加了 `query`/`tag`,
   与 `list_entries` 同语义(理由见 §5)。
3. **UI 提醒/通知合并**:初稿是三个独立字段,实现按决策 ④ 合并为一个「提醒」时刻,
   提交时写同值到 remind/notify 两字段。
4. **列表载荷的 overdue**:初稿描述为随行下发;实现改为行构造后再补算
   (`from_db` 无 now),对宿主透明。
5. **单击行为**:初稿 EntryCard 单击 = 切换完成;实现改为**无动作**(误触率问题),
   完成入口收敛到复选框与右键菜单。
6. **todo_list 的 done 参数**:实现由宿主固定传 `all`(归档可见并垫底),
   而不是默认 `open` —— 否则完成即"消失"。
