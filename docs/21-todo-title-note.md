# 21 · 待办:标题与备注分离

| 项 | 内容 |
| --- | --- |
| 版本 | v1(已实现) |
| 日期 | 2026-10-18 |
| 状态 | **已落地并验收**(v8 迁移 + 面板双字段 + 便签正文=备注 + FTS 纳入备注) |
| 关联 | [05-todo.md](./05-todo.md)(待办数据模型)· [20-todo-sticky-note.md](./20-todo-sticky-note.md)(桌面便签) |
| 一句话 | 待办从"一段文字"拆成"一句话标题 + 可折叠的长备注",并让**便签、列表、搜索**三处口径一致。 |

---

## 1. 需求与现状

用户原话:

> "接下来开始丰富待办,标题和备注,目前只有标题,然后贴屏后内容一致了"。

### 1.1 之前是什么

待办只有 `clip_entries.text_content` 一个正文字段,它同时承担三件事:

| 用途 | 表现 |
| --- | --- |
| 列表卡片的正文 | 多行全文,超出折叠 4 行 |
| 便签正文区(可编辑) | 直接编辑 `text_content` |
| FTS 检索 | `clip_fts.text_content` |

结果是"标题"和"正文"只能二选一:写一句话就装不下细节,塞满细节就没有可扫读的标题。

### 1.2 现在是什么

| 概念 | 字段 | 语义 | 界面位置 |
| --- | --- | --- | --- |
| **标题** | `clip_entries.text_content` | 一句话,**必填**(空则拒绝保存) | 列表卡片主文本 / 便签标题条 / 托盘子菜单 |
| **备注** | `todo_meta.note_text` | 可多行,**可空** | 编辑卡片折叠区 / 列表卡片 2 行摘要 / 便签正文区 |

> 之所以标题继续用 `text_content`:列表、搜索、预览、剪贴板复用、托盘全都在读它,
> 换字段等于把这些链路全改一遍;而备注是**待办专属**的补充信息,放 `todo_meta`
> 与 `due_at`/`remind_at` 同层最自然,也避免给每条剪贴板记录多背一列。

---

## 2. 数据模型(v8)

```sql
ALTER TABLE todo_meta ADD COLUMN note_text TEXT;
```

**只 ALTER 加列,不重建 `clip_entries`**(与 v5 同一手法):`kind` 无需新增,
v6 那种"重建表 → 必须补回索引与 FTS 触发器"的高风险动作完全不必要。

### 2.1 但 FTS 必须重建

备注要可搜(中文用户把细节都写在备注里,不索引等于没写)。而 `clip_fts` 是**普通
FTS5 表**(非外部内容表,见 v4 的注释),SQLite 不能给已存在的虚拟表加列 —— 只能重建:

```sql
DROP TRIGGER clip_fts_insert / clip_fts_delete / clip_fts_update;
CREATE VIRTUAL TABLE clip_fts_new (... ocr_text, note_text);
INSERT INTO clip_fts_new SELECT ..., NULL FROM clip_fts;
DROP TABLE clip_fts;
ALTER TABLE clip_fts_new RENAME TO clip_fts;
-- 再重建 3 条旧触发器 + 新增 2 条 todo_meta 触发器
```

### 2.2 ⚠️ 语句顺序不能换(实测踩到)

正确顺序是 **先 `DROP TRIGGER`、再 `DROP TABLE`、最后 `RENAME`**。

反过来(先 DROP TABLE 再补 DROP TRIGGER)会在这条 `ALTER TABLE ... RENAME` 上炸:

```
error in trigger clip_fts_insert: no such table: main.clip_fts
```

原因:`RENAME` 会**校验现有触发器**,而那时触发器仍指向已被删掉的表。
这是 v6 那条教训的镜像 —— v6 是"重建被触发器引用的**主表**,所以重建后必须补回触发器";
这里是"先卸掉引用,才能动表"。

### 2.3 为什么需要两条 `todo_meta` 触发器

备注在 `todo_meta` 上、不在 `clip_entries` 上,所以 `clip_entries` 的触发器管不到它:

| 触发器 | 时机 | 为什么必须 |
| --- | --- | --- |
| `todo_meta_fts_update` | `AFTER UPDATE OF note_text` | 改备注要更新索引,否则搜到的还是旧词 |
| `todo_meta_fts_insert` | `AFTER INSERT` | **新建待办时 meta 行在条目之后插入** —— `clip_fts_insert` 执行那一刻 meta 还不存在,子查询取到 NULL。不补的话"刚建的待办按备注搜不到",要等下次编辑正文才偶然进索引 |

两条都采用"**整行重建**(DELETE + INSERT)"而不是 UPDATE:只改备注时若走 UPDATE,
`text_content` 列会丢(那是另一个触发器写进去的)。

### 2.4 一处容易漏的地方:LIKE 回退也要覆盖备注

`entry::list` 的搜索是 **FTS 优先、LIKE 兜底**:

- FTS5 的 unicode61 分词器按空格/标点切分,`季度 OKR 复盘材料` 的 token 是
  `季度` / `OKR` / `复盘材料` —— 搜 `复盘` **MATCH 不到**;
- 于是回退到 LIKE,而原来的 LIKE 只查 `text_content` / `file_paths` / `ocr_text`。

**漏了备注 → 中文备注搜不到**。修法:两条 WHERE(列表查询与 COUNT 计数)各补一句

```sql
OR (e.kind = 'todo' AND e.id IN
    (SELECT id FROM todo_meta WHERE note_text LIKE ?1 ESCAPE '\'))
```

`todo::list`(待办页)同理,让两端口径保持一致。

---

## 3. 契约

### 3.1 FFI

| 方法 | 参数 | 语义 |
| --- | --- | --- |
| `todo_create` | `noteText?` | 可选,空串等同不设 |
| `todo_update` | `noteText?` | **三态**:键缺失 = 不改;`null` / 空串 = 清空;字符串 = 设置 |
| `todo_get` / `todo_list` / `todo_note_list` | — | 响应**顶层**加 `noteText`(非空才出现,`skip_serializing_if`) |

⚠️ `noteText` 放在**顶层**、不放进 `todo` 徽标:`todo` 徽标是"卡片正面必需的最小集"
(`dueAt`/`done`/`overdue`),备注不属于那一层;而且 `todo_note_list` 的 `note` 键
已被**窗口状态**占用(`x/y/w/edge/...`),重名会直接撞车。

### 3.2 空值归一化

备注 `trim` 后为空一律存 **NULL**(不是空串),与 `tags` 的"空数组 = 无标签"同一口径。
否则卡片会多渲染一行空白,搜索里也多一条空 token。

---

## 4. 界面

### 4.1 编辑卡片(Main.qml)

> ⚠️ **顺序是硬约束:标题在上、备注在下**(2026-10-18 修)。
> 初版把两个块整体写反了 —— 打开卡片第一行是"备注"、第二行才是"标题",
> 两块各自都能用,只有肉眼才看得出来。现已加验收 `--todo-layout-flow` 钉住:
> 按 `objectName` 取两个输入框,比较它们在**场景坐标**里的 y
> (两者父级不同,直接比各自的 y 是在比两个坐标系下的值)。


标题下方新增**可折叠**的备注区:

```
[标题                        ]  ← 单行 Enter 提交
备注 (3 行)            [收起]  ← 有内容或有焦点时自动展开
[补充说明、清单、链接…      ]  ← Enter 换行,不提交
```

- **折叠**是为了不让卡片变高:常驻一个多行框会把截止/提醒/标签挤出视野;
- **有内容或获得焦点时自动展开** —— 编辑既有待办时不该把备注藏起来;
- ⚠️ 备注框的 Enter 必须是**换行**,不能沿用标题框的"Enter 提交"(语义相反)。

### 4.2 列表卡片(EntryCard.qml)

标题之下、正文之上插一行备注摘要:`maximumLineCount: 2` + 省略号,完成后随标题一起划线弱化。

无备注时**整行不渲染**(不给卡片留空位)。

### 4.3 便签(NoteWindow)

这是本需求"贴屏后内容一致"的落点:

| 位置 | 之前 | 现在 |
| --- | --- | --- |
| 标题条 | `text_content` 的**首行** | **标题**全文(仍截 20 字) |
| 正文区 | 编辑 `text_content` | 编辑 **`noteText`** |
| 右键「复制正文」 | 复制 `text_content` | 改名「**复制内容**」,标题 + 空行 + 备注 |

**备注允许清空**(与标题相反):标题是必填项,空则恢复上一个非空值;备注是补充说明,
删光就是"没有备注"。

---

## 5. 备份 / 恢复 / 导入

- **备份 / 恢复**:整库 zip,新列自动随包,无需改代码;
- **导入(合并)**:`read_todo_meta` 的返回元组加一位 `note_text`,并在写 `todo_meta`
  时一并搬 —— 否则"导入后标题都在、备注全没了";
- 旧备份(v8 之前)没有这一列:`read_todo_meta` 先探测列是否存在,不存在则用 `NULL`
  占位,**不让整次导入失败**(与 `read_todo_note` 同一取舍)。

---

## 6. 验收

### 6.1 core 单测(4 条新增)

| 用例 | 断言 |
| --- | --- |
| `note_roundtrip_and_blank_normalizes_to_null` | 多行原样保留;全空白归一化成 NULL;三态清空 |
| `note_is_omitted_from_json_when_absent` | 空备注**整键省略**(与 tags 同手法);标题与备注互不串位 |
| `note_is_searchable_via_list` | 备注可搜;改备注后**旧词不再命中、新词命中** |
| `note_inserts_into_fts_for_new_todo` | 新建待办的备注**立刻**可搜(回归 `todo_meta_fts_insert`) |

### 6.2 契约脚本(`tools/verify_todo_note.py`)

新增:标题/备注分离、`noteText` 在顶层(不在 `note` 对象里)、三态、
FTS 与中文 LIKE 双路命中、备份导入后备注仍在。

### 6.3 交互验收(`nextboard.exe --note-flow`) —— 实测全绿

| 步骤 | 断言 |
| --- | --- |
| STEP1 | 备注写入 → 便签**正文区显示的就是备注**(不是标题) |
| STEP5 | 便签里改正文 → 写回 `noteText`,**标题未被覆盖**;多行原样 |
| STEP5b | 清空备注 → 真的清掉,且不影响标题 |

---

## 7. 踩坑记录

1. **`ALTER TABLE ... RENAME` 会校验现有触发器** —— 必须先 `DROP TRIGGER` 再动表,
   否则报 `no such table: main.clip_fts`。
2. **`QTextEdit` 默认的右键菜单会吃掉 `contextMenuEvent`**,导致便签正文区右键不出
   自己的菜单(见 docs/20 §13.5 ①)。
3. **字段顺序写反**:编辑卡片里两块 GridLayout 子项整体调换,标题跑到了备注下面。
   已在 `--todo-layout-flow` 里断言(先证伪再修:换回错误顺序时该断言确实报 FAIL)。
4. **`ReadableCheckBox` 只在 `SettingsDrawer.qml` 内部定义**,不是模块级类型。
   在 `Main.qml` 里引用会让**整份 Main.qml 加载失败**、面板一片空白 —— 正是本轮
   改动后 `--todo-flow` 报 `QML load errors: ReadableCheckBox is not a type` 的原因。
   改用主面板已有的 `ThemedButton`。
5. **字段名撞车**:`NoteData` 里已有 `QVariantMap note`(窗口状态),新增备注字段不能
   也叫 `note` → 用 `noteText`(C2371)。
6. **验收顺序**:备注要在 `open()` **之前**写。建窗那一刻就把 `noteText` 读进正文区,
   顺序写反的表现是"备注明明存了、便签里却是空的"。
7. **验收数据目录要清空**:`--todo-flow` 的 STEP4 断言"persisted items = 1",
   复用旧目录时会读到上一轮残留(实测 items=3 导致误报)。

---

## 8. 未做

- 备注不参与**托盘「今日待办」**的显示(那里只放标题,菜单本来就窄);
- 备注没有独立字数上限(沿用整条文本的大小约束);
- 便签不能编辑标题(仍走右键「编辑待办…」回面板);
- 标题/备注的富文本、清单勾选形态未做。
