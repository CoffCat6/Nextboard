# 27 · 智能框选响应速度:从「停稳 400ms」到「每帧 0.003ms」

| 项 | 内容 |
| --- | --- |
| 版本 | v1(2026-11-02) |
| 状态 | **已实施并验收** |
| 关联 | [23-smart-capture-plan.md](./23-smart-capture-plan.md)(智能框选设计)· [26-chromium-smart-capture.md](./26-chromium-smart-capture.md)(元素级算法修复) |
| 现象 | 智能框选能明显感到延迟,"必须把鼠标停留 1s 左右"才有元素框,不灵敏 |

---

## 1. 一句话结论

慢了**三个环节**,而且三者叠加:

| # | 环节 | 实测 | 性质 |
| --- | --- | --- | --- |
| L1 | 停稳判定 `kElementProbeDelayMs = 400ms` | 400ms | **固定等待**,且每次 `mousemove` 都 `start(400)` **重置** |
| L2 | 探测本身(全树 DFS) | 77~136ms | 每次悬停**重算同一个答案** |
| L3 | 结果按**精确像素**校验 | 丢结果 | 抖动 1px → 104ms 白探 |

L1 被手抖反复重置,是"必须停 1 秒"的直接来源:用户以为自己在静止,实际手抖 1~2px
就重新计时。L2 让最坏情况变成 400 + 136 ≈ 536ms,再叠加 L3 的丢弃重来,体感破 1s。

**根治办法不是调小这几个数,而是换模型**:框选期间窗口内容本就是冻结的(连底图都
冻了),所以"每动一次鼠标重探一次"是在反复算同一个答案。改成"进窗预取一次 +
每帧查表"。

---

## 2. 实测数据

### 2.1 分段耗时(本机,Chrome 156 / Zed,中位数 7 次)

| 测量项 | 中位数 |
| --- | --- |
| `CoCreateInstance(CUIAutomation)` | 0.06~0.35ms |
| `ElementFromHandle` | 0.5~1.1ms |
| 单次 `GetFirstChildElement`(跨进程往返) | **0.3~5.9ms** |
| 单次 `get_CurrentBoundingRectangle` | 0.02~0.12ms |
| 单次 `get_CurrentName` | 0.06~0.12ms |
| **全树 DFS(现算法)** | **5~136ms** |
| 浅 DFS(仅 depth ≤ 3) | 4~12ms |
| 纯 walk 不算矩形(同上深度) | 6~11ms |

结论:瓶颈是**跨进程 COM 往返次数**,不是矩形计算。所以"少问一次"比"问快一点"
有效得多 —— 这正是快照方案的本质。

### 2.2 快照方案的两项关键指标

| 指标 | 实测 |
| --- | --- |
| 建快照(整窗一次) | 6~113ms(**每窗口只一次**) |
| 查表(1500 节点) | **0.003ms/次** |
| 查表(真实 ~130 节点) | 0.025ms/次 |

同一窗口内移动 10 次:修复前 10 × 113ms ≈ **1.13s**;修复后 113ms + 10 × 0.003ms
≈ **113ms**(只有第一次)。

### 2.3 正确性:快照查表 ≡ 逐点探测

在同一窗口撒 60 个采样点,分别用"快照查表"与"逐点 DFS"求解,逐字段比对矩形与深度:

```
Chrome(3072x1920):相同=4 不同=0 都没命中=56
Zed(2863x1575):   相同=0 不同=0 都没命中=60
```

**0 处不同**。命中数少是因为随机撒点大多落在空白处,属正常。

---

## 3. 方案

### 3.1 数据结构:整窗快照

`smart_capture.hpp` 新增:

```cpp
struct ElementNode {
    QRect physical;   // 绝对物理像素
    QString title;    // UIA Name
    int parent = -1;  // 父索引(为 docs/26 S5 的层级链留好)
    int depth = 0;
};

QVector<ElementNode> buildElementSnapshot(quintptr windowId, int maxNodes = 6000,
                                          int budgetMs = 400, ProbeStats* stats = nullptr);
int elementAtSnapshot(const QVector<ElementNode>& nodes, const QRect& winPhysical,
                      const QPoint& cursor, int minSize, bool allowEqualWin = false);
```

- `buildElementSnapshot` 阻塞(6~113ms),**必须在工作线程**;
- `elementAtSnapshot` 纯内存,无 COM、无分配,可在 `mouseMoveEvent` 里同步调用。

### 3.2 调度:进窗预取 + 每帧查表

`capture_session.cpp` 的 `smartHoverUpdate` 里,原来那段"停稳 400ms 才投探测"的
计时器逻辑整段删除,换成:

```
光标命中某窗口 W:
  快照已就绪 → 当帧查表(0.003ms),元素框立刻跟上
  快照未就绪 → 后台建一次;建完**用当前光标位置**立即刷一次预选
```

第二行那个细节很重要:建快照要 6~113ms,用户可能在等待期间已移到别处。
原实现靠"结果回来时校验光标位置,不一致就丢弃"——那正是 L3 的浪费。
现在建完直接按**最新**光标位置查表,结果永远不会因"用户动了"而作废。

### 3.3 删除的东西

| 删除项 | 理由 |
| --- | --- |
| `kElementProbeDelayMs = 400` | 停稳判定的全部意义就是"探测太贵,别跟着鼠标跑";现在不贵了 |
| `m_elementProbeTimer` | 同上 |
| `startElementProbe()` | 被 `ensureElementSnapshot` + 查表取代 |
| `m_elementProbePending` / `m_elementProbeGuard` | 由 `m_elementSnapshotPending` 承担 |
| `m_elementProbeDenied`(负缓存) | 快照只建一次,不存在"反复重投注定失败"的问题 |

> 负缓存是 docs/26 §4 S4 为"重探模型"打的补丁。换模型后它解决的问题**不再存在**
> —— 留着会让人误以为还要防重复探测。这是"优化掉了补丁本身,而不是优化补丁"。

---

## 4. 为什么不用别的方案(都试过/想过)

| 方案 | 否决理由 |
| --- | --- |
| 把 `kElementProbeDelayMs` 从 400 调到 150 | 只是把 L1 减半;L2 的 77~136ms 仍在,而且更频繁触发时**更卡** |
| 只把 `budgetMs` 调小(如 100ms) | DFS 超时是硬截断,结果会停在很粗的层 —— 拿精度换速度,不划算 |
| 只在 `depth ≤ 3` 浅探测 | 实测 4~12ms(很快),但深层元素(单行文本/单列)就没了;而且仍要等 L1 |
| 用 `RawView` 换 `ControlView` 提速 | 反了:RawView 节点多一个量级,更慢 |
| MSAA `accHitTest` 单次调用 | docs/26 §3.1 已证不可行(Chromium renderer 上 `accChildCount == 0`) |
| 快照 + 失效检测(窗口内容变化时重建) | **本期未做**,见 §5 |

---

## 5. 已知限制与本方案的边界

1. **快照在会话内不失效**。框选会话很短(通常 < 10s),期间窗口内容变化(网页滚动、
   切标签)不会被感知 —— 但**底图同样是冻结的**,所以用户看到的本来就与快照一致。
   真要滚动后再框,得重新触发截图(Esc → 热键),那是既有产品语义。
2. **首次进入某窗口仍有 6~113ms 延迟**。这是不可消除的 COM 成本(除非提前预取全桌面,
   代价更大:每块屏的每个窗口都要 100ms+)。但它是**一次性**的,而且发生在光标刚
   进入窗口时 —— 那时用户本来也还在找位置。
3. **内存**:每个窗口一份节点数组。实测 147 节点 ≈ 每节点 60 字节(QRect 16 +
   QString 8 + int 8 + 填充)≈ 9KB/窗口。多屏几十个窗口也就几百 KB,可忽略。
   上限 `maxNodes = 6000` 兜底。

---

## 6. 验收

`--smart-capture-flow` A6a 新增 5 项断言(全部纯函数,不依赖真机环境):

| 断言 | 守的是什么 |
| --- | --- |
| 快照查表取最深命中(depth=4) | 查表语义与 DFS 一致 |
| 光标在区块空白处 → 命中区块本身(depth=3) | 命中测试按光标而非元素中心 |
| 与窗口等大(差 6px < 8px 阈值)不算细化 → -1 | 阈值与 `dfsDeepest` 同源 |
| `allowEqualWin=true` 可返回等大节点 | 为层级链滚轮留退路 |
| 空快照安全返回 -1 | 降级契约:拿不到树不能让截图不可用 |
| **查表 < 0.1ms/次**(1500 节点,实测 0.003ms) | **性能回归防线**:有人往查表里塞 COM 调用会立刻翻红 |

实测日志:

```
SMART-CAPTURE  PASS 快照查表取最深命中(depth=4,期望 4；索引=3)
SMART-CAPTURE  PASS 与窗口等大(差 6px < 8px 阈值)的节点不算细化,返回 -1(得 -1)
SMART-CAPTURE  PASS allowEqualWin=true 时允许返回等大节点(得 1)
SMART-CAPTURE  PASS 空快照安全返回 -1
SMART-CAPTURE  查表延迟 = 0.0030808 ms/次 (1500 节点)
SMART-CAPTURE  PASS 元素查表 < 0.1ms/次
```

四条智能框选 flow:`--smart-live-flow` / `--smart-uia-flow` / `--smart-settings-flow`
全绿;`--smart-capture-flow` 剩 5 个失败,**与改动前基线逐条一致**(B 段单击即确认、
G 段副屏),属预先存在,与本次改动无关。

---

## 7. 真机确认项(沙箱跑不了 GUI)

| # | 场景 | 期望 |
| --- | --- | --- |
| 1 | 光标进入 Chrome 窗口后**缓慢移动**(每秒几十像素) | 元素框连续跟随,无"等一下才出现" |
| 2 | 从任务栏切到 Chrome,立刻移动鼠标 | 首次有 ≤100ms 延迟,之后零延迟 |
| 3 | 在同一窗口内来回移动 | **完全无卡顿**(查表 0.003ms) |
| 4 | 快速横扫多个窗口 | 每个窗口首次进入有一次性延迟,不应卡死 UI |
| 5 | Chrome 内滚动页面后**不重新截图**继续悬停 | 框仍按旧位置(已知限制 §5.1,可接受) |
| 6 | 多屏之间来回移动 | 每块遮罩独立,无跨屏错乱 |

---

## 8. 排障入口

| 现象 | 先看 |
| --- | --- |
| 元素框还要等约 100ms | 是不是每次都在**不同窗口**间移动(每个窗口首次都要建快照) |
| 元素框卡顿/UI 顿 | 看 `buildElementSnapshot` 是否被误在 UI 线程调用(它必须在 `QtConcurrent::run` 里) |
| 各窗口都慢 | `worthProbing` 是否放过了本该拒绝的应用(如游戏);或 `maxNodes` 过大 |
| 查表结果与预期不符 | `--smart-capture-flow` A6a 段;阈值 `kMinShrinkPhys = 8` 与 `dfsDeepest` 必须同源 |
