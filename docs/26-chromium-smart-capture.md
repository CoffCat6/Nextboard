# 26 · 智能框选对 Chromium 系的支持缺陷:根因与修复计划

| 项 | 内容 |
| --- | --- |
| 版本 | v2(2026-11-02 分析 + 同日完成 P0/P1 修复) |
| 状态 | **P0(S1/S2/S3)+ P1(S4) 已实施并验收**;P2(S5 层级链 / S6 设置项)未做 |
| 关联 | [23-smart-capture-plan.md](./23-smart-capture-plan.md)(设计与既有结论,§7.4 结论**需更正**) |
| 现象 | 在 Chrome / Edge / Electron(VSCodium、MarkText)上,智能框选**永远只框住整个窗口**,元素级(S3)等于没做 |

---

## 1. 一句话结论

**这不是「Chromium 惰性无障碍」,而是我们自己的下沉算法与诊断工具犯了同一个错:**
每层只认**一个**子节点(主算法取"面积最小",诊断工具取"第 0 个")。

Chromium 的顶层恰好是**两个同尺寸 Pane**:`[0]` 是空壳(0 子节点),`[1]` 才是 Web 内容
(control 视图 34~49 个节点、深 9~11 层)。选了 `[0]` 就再也下不去 —— 症状恰好长成
"元素级永远等于窗口",于是被写进 docs/23 当成 Chromium 的限制。

---

## 2. 实测证据(本机,2026-11-02)

### 2.1 复现环境

- Chrome 156.0.8078.79 / Edge 156.0.4314.58 / Windows 11 26H2,三屏混合 DPI(1.75 / 1 / 1)
- 探针页 `%TEMP%\nb_uia_probe.html`:固定布局的 header / sidebar / article(h1+p+p+红块)/ footer
- 探针脚本为一次性 PowerShell(UIAutomationClient + `AccessibleObjectFromWindow`),
  跑完已删除,不进版本库;复现方法见 §2.5 与 §9 的排障入口

### 2.2 主算法在 Chrome 上的实际走向

把 `probeElementAt` 的贪心逻辑逐层打印(control view,光标落在正文区):

```
L0 节点 Window 'NB Probe Page - Google Chrome' 子节点数=2
   [0] * Pane ''    w=2077 h=1388      ← 空壳:子节点数 = 0
   [1] * Pane 'NB Probe Page - Go...'  w=2078 h=1390   ← Web 内容:子树 34 节点
   -> 贪心选中:'[0]'(面积 2077×1388 < 2078×1390,取最小)
L1 节点 Pane '' 子节点数=0               ← 到此结束
```

**结果:best 停在 depth=1,即"与窗口等大"** —— `sameSizeStreak` 那套"同尺寸继续下沉"
的补丁根本没机会运行,因为 `if (!next) break` 先触发了。

改成"同层所有命中兄弟都入栈、DFS 找最深"后,同一位置:

| 视图 | 深度 | 命中矩形 | 耗时 | 遍历节点 |
| --- | --- | --- | --- | --- |
| ControlView | 9 | `1422×1225` Group(正文区) | 21~30 ms | 23 |
| RawView | 11 | `735×386` 或 `1338×38` Group(红块 / 单行文本) | 41~56 ms | 56~62 |

### 2.3 真实页面复测(不是探针页)

用户现开的 Telegram Web(3388×1918)与探针页同口径对比:

| 窗口 | 现状(min 面积贪心) | ControlView DFS | RawView DFS |
| --- | --- | --- | --- |
| Telegram Web | depth=1,**整窗** | depth=11,`1218×702` | depth=20,`315×702` |
| 探针页(Chrome) | depth=1,**整窗** | depth=9,`1422×1225` | depth=11,`735×386` |
| 探针页(Edge) | depth=1,**整窗** | depth=9~12 | depth=12~14 |

三款 Chromium 一致:现状是 `depth=1`,而树上**本来就有** 9~20 层的可用节点。

### 2.4 排除"惰性无障碍"这个假设

- 冷启动 Chrome(全新 profile,先只问 UIA、绝不碰 MSAA)连测 5 次:后代数 **49**、
  raw 深度 **11**,与"先 MSAA 激活再问"完全一致 —— **不需要 `--force-renderer-accessibility`**;
- 加 `--force-renderer-accessibility` / `+ --enable-features=UiaProvider` 后子树规模、
  可达深度**无任何变化**(18 nodes → 18 nodes);
- UIA 全局事件订阅(`AutomationFocusChanged` + 桌面级 `StructureChanged`)**不改变**子树。

所以 docs/23 §7.4 的观测(`L0` 只有与窗口等大的容器、`L1 n=0`)是**真的**,
但归因错了:那不是 Chromium 不给树,是**诊断工具自己只看第 0 个兄弟**。

### 2.5 诊断工具也栽在同一个坑

`app/src/capture/smart_capture.cpp:493`(`dumpElementTree`):

```cpp
if (i == 0) {           // 只沿第 0 个孩子继续下沉
    release(node);
    node = child;
}
```

Chrome 的第 0 个孩子正是空壳 Pane → `L1 n=0`。`--smart-uia-flow` 把这个结论原样
打进日志并**软放行**(`app/src/host/smart_capture_flow.cpp:791-800`:`strictlySmaller`
为假时只 `qInfo` 不 `check(false)`),于是四条 flow 全绿、缺陷存活 6 天。

**流程根因**:验收对"元素级 == 窗口级"没有硬断言 —— 而这正是本功能**唯一**的失效形态。

---

## 3. 其余发现(按影响排序)

| # | 发现 | 影响 | 证据 |
| --- | --- | --- | --- |
| F1 | 每层只取一个兄弟(面积最小) | **致命**,Chromium 上 100% 失效 | §2.2 |
| F2 | 诊断工具只沿第 0 个孩子 | 把 F1 误诊成"目标应用不给树",误导了 6 天的判断 | `smart_capture.cpp:493` |
| F3 | 验收对"元素==窗口"软放行 | 缺陷可长期存在且全绿 | `smart_capture_flow.cpp:791` |
| F4 | 探测结果不合格时不再升级 | 用户停住不动就一直停在整窗;动一下又只是重投同样的整窗(白耗 COM) | `capture_session.cpp:1086`、`:1100` |
| F5 | 无负缓存 | 同一窗口每次悬停都重探一次(Chrome 上 ~20ms,真实页面 100~140ms) | `capture_session.cpp:1027` |
| F6 | 只用 ControlView | Chrome 上 RawView 比 ControlView **再细 2~9 层**(能到单行文本/单列),现有实现够不到 | §2.2 |
| F7 | 粒度不可调 | "框住整页正文"与"框住这一行"只能二选一,用户无指针路径切换 | 现状设计自然结果 |
| F8 | Chromium 外壳与网页同在树里 | 标签栏/地址栏/书签栏都可被单独框住;功能上**不算错**(有时确实想截地址栏),但默认粒度要挑"内容区" | §2.2 的 `toolbar` 分支 |

### 3.1 两条已确认**不成立**的怀疑

- **"遮罩会挡住 UIA 命中测试"** —— 对 `ElementFromPoint` 成立(实测遮罩全屏置顶后返回
  `ControlType.Window 3072x1920`,即遮罩自己,与 docs/23 §5 一致);对**自遍历**不成立。
  所以 docs/23 §5 的"不用 `ElementFromPoint`、自行遍历子树"这个决定是对的,继续保留。
- **"可以退到 MSAA(`IAccessible` + `accHitTest`)绕开"** —— **走不通**。实测 Chromium 的
  renderer HWND 上 `accChildCount == 0`、`accHitTest` 在无遮罩时也返回 `null`
  (虽有 `--force-renderer-accessibility` 也一样)。UIA 自遍历是唯一可行路径。

---

## 4. 修复方案

### S1(P0)把"贪心单支下沉"换成"受预算约束的 DFS"

`app/src/capture/smart_capture.cpp:347-430` 整段替换:

```
从窗口根出发,显式栈 DFS:
  弹出 (node, depth)
  depth > kMaxDepth(20) → 跳过
  对 node 的每个子元素:
     取 BoundingRectangle;无效 / 面积 ≤0 → 跳过
     不包含光标 → 跳过;边长 < minSize → 跳过
     入栈 (child, depth+1)
     若 depth+1 > bestDepth(或同深度但面积更小)→ 记为 best
  每轮检查 elapsed > budgetMs 或 visited > maxNodes → 停
```

保留的既有语义:
- best **必须明显小于窗口**才算数(防"把整窗报成单个元素"),判据用
  `w < winW - kMinShrink` 而非现在的 `!sameAsWindow`(后者在 Chrome 上恒为真,
  等于没判);
- 失败一律返回空表(降级契约不变,docs/23 §"降级契约")。

删除 `sameSizeStreak`(`:338`、`:418-427`)—— 它是为"单支下沉"打的补丁,
DFS 下自然消解;留着只会让"同尺寸容器"被误判为死循环。

**预算调整**(实测依据):Chrome/Telegram 上 ControlView 21~136 ms、RawView 41~108 ms。

| 参数 | 现值 | 建议 | 依据 |
| --- | --- | --- | --- |
| `budgetMs` | 350 | **500** | 真实页面 DFS 实测峰值 136ms,留 3.5x 余量容纳复杂站(购物/文档站) |
| `maxNodes` | 3000 | **4000** | 复杂页面 raw 视图节点数上限;配合 `visited` 每节点计数(现在按"每层子节点数"累加,量纲不对) |
| `maxDepth` | 32 | **20** | 实测最深 20(Telegram raw),再深是文本 run,框选无意义 |

### S2(P0)视图策略:两级探测

```
第一发:ControlView + budget 500ms   → 通常已到 depth 9~18,够用就返回
第二发:ControlView 结果 depth ≤ 2
        → RawView + 剩余预算,再深挖一次
```

理由:ControlView 快且语义干净(元素/文档/工具栏),RawView 细但节点多。两级探测让
"常见情况快、复杂情况细",而不是全局都用 RawView 拖慢所有窗口。

> **实现更正(2026-11-02 实施期)**:S2 第一版写错了 —— 用 `FindAll(TreeScope_Children)`
> 冒充"两级视图",而 FindAll 拿到的**始终是同一棵树**,换条件不换视图。真正的两级
> 需要 `IUIAutomation::get_ControlViewWalker` / `get_RawViewWalker` 两个 walker 对象
> 分发,并用 `GetFirstChildElement` / `GetNextSiblingElement` 遍历。
> 另注意这两个方法的**参数是"前一个兄弟"而非父节点**,写成父节点会编译错。

### S3(P0)修 `dumpElementTree`

`app/src/capture/smart_capture.cpp:467-504`:

- 每层**遍历全部**子元素(受 `maxPerLevel` 限),而不是只下沉 `i==0`;
- 输出补上 `depth`、`controlType`、`子节点数` 与"是否为空壳",让诊断**一眼能看出**
  "同名同尺寸的两个兄弟里哪个有子树";
- ⚠️ 这个函数是 `--smart-uia-flow` 的唯一留痕手段,它错了等于没有诊断。

### S4(P1)探测调度:允许升级 + 负缓存

`app/src/capture/capture_session.cpp:1025-1111`:

| 改动 | 现状问题 |
| --- | --- |
| 探测结果**等于**当前窗口级预选时,不再"丢弃并等下一次鼠标移动",而是**标记该 (windowId, 光标格) 已试过**并允许重投一次(RawView 那一发) | 现状是静默丢弃;用户不动鼠标就永远停在整窗(F4) |
| 按 `windowId` 记住"这个窗口探不到更细的",同一会话内不再重投 | 每次悬停都白花一次 COM(F5) |
| 探测成功时缓存 `windowId → 元素层级链`(见 S5) | 一次探测只值一个矩形,浪费 |

### S5(P2)层级链 + 滚轮切粒度

现在滚轮只在**重叠窗口**之间切换(`capture_session.cpp:1686-1712`)。建议扩成两段:
同一个窗口内,滚轮在"窗口 → 区块 → 段落 → 行"这条**祖先链**上移动。

- 探测返回**整条链**(从粗到细 3~5 个候选),而不是单个 best;
- 命中栈排序键已有 `(属主窗口 z 序, -level)`(docs/23 §7.5),链内切换可直接复用;
- 空态文案改为"滚轮切换粒度",与既有"滚轮切换重叠窗口"合并成一句;
- 默认落到链上**"面积 ≤ 窗口 60% 且 ≥ 24×24"的第一个**,避免默认框住整窗。

### S6(P2)设置项(可选,默认值先说清)

新增 `smartCaptureGranularity`:`content`(默认,跳过外壳/工具栏)/ `element`(最细)/
`window`(等价关闭元素级)。**默认取 `content`**:Chrome 上地址栏/书签栏被单独框住
对多数人不是想要的;但要留出口给"就想截那条地址栏"的用户。

---

## 5. 实施计划

| 阶段 | 内容 | 交付 | 状态 |
| --- | --- | --- | --- |
| **P0-a** | S1 DFS 下沉 + 预算参数 | `probeElementAt` 重写 | ✅ 已完成 |
| **P0-b** | S2 两级探测(ControlView → RawView) | 同上 | ✅ 已完成 |
| **P0-c** | S3 诊断工具修正 | `dumpElementTree` | ✅ 已完成 |
| **P0-d** | 验收补强(硬断言 + Chromium 专项) | `--smart-uia-flow` | ✅ 已完成 |
| **P1** | S4 升级 + 负缓存 | `capture_session` 调度 | ✅ 已完成 |
| **P2-a** | S5 层级链 + 滚轮 | `capture_session` 交互 | ⬜ 未做 |
| **P2-b** | S6 设置项 | settings.rs + QML | ⬜ 未做 |
| **收尾** | 更正 docs/23、更新 progress.md | 文档 | ✅ docs/26 已写;docs/23 待改 |

### 5.1 实施期发现的自身缺陷(记录下来避免重犯)

| # | 缺陷 | 教训 |
| --- | --- | --- |
| I1 | S2 第一版用 `FindAll(TreeScope_Children)` 冒充两级视图 | FindAll **永远是同一棵树**,换条件不换视图;两级必须用两个 `TreeWalker` 对象 |
| I2 | `IUIAutomationTreeWalker::GetNextSiblingElement` 的参数是**前一个兄弟**,不是父节点 | 按"父节点 + 序号"的直觉写会编译错 |
| I3 | 第一版把子节点直接 append 进栈(转移所有权)后仍拿它取兄弟 | use-after-release。改成"本层先收齐 kids、本层统一释放、下沉时显式 AddRef",所有权规则只写一条 |
| I4 | 曾计划在 `--smart-capture-flow` 加 H 段用合成候选表验 Chromium 形状 | 候选表是**扁平**的,表达不了"同父同尺寸兄弟";该形状只能在真实 UIA 树上验 |
| I5 | 首轮统计 FAIL 数时 `Select-String 'FAIL'` 不区分大小写 | 把 `RESULT failures = 0` 也计成失败,误报 3 条 flow 有问题 |

---

## 6. 验收补强(与 P0 同批,否则等于没修)

### 6.1 落点更正:回归断言放在 `--smart-uia-flow`,不在 `--smart-capture-flow`

**原计划**是在 `--smart-capture-flow` 加 H 段,用合成候选表构造「两个同尺寸 Pane,
[0] 空壳」的形状。**实施后发现这个设计不成立**,原因值得记下来:

候选表是**扁平**的(`QVector<Candidate>`,窗口级 + 元素级混在一层),它无法表达
"同一父节点下两个同尺寸兄弟"——那纯粹是 UIA **树遍历层**的形状。在扁平表里放两条
同矩形窗口级候选,只会退化成"z 序第一个胜出",与 Chromium 的真实形态无关。
H 段实测因此失败(得 1754x1096 = 整个窗口,期望区块),但它考的是**我构造错了**,
不是被测代码有问题。该段已删除。

**真正的回归面**只有一处:`probeElementAt` 在真实 UIA 树上的下沉深度。因此断言
落在 `--smart-uia-flow`(它有真窗口、真 COM、真树),用 `ProbeStats`:

| 判据 | 说明 |
| --- | --- |
| 矩形明显小于窗口(差 ≥ 8px) | Chromium 外壳 Pane 只差 2px,不算细化 |
| **或** `depth >= 5` | 证明我们确实下沉过,不是停在根/第 1 层 |
| 两条都不成立 | **判失败**(原实现在这里只 `qInfo`,是缺陷存活 6 天的直接原因) |

### 6.2 Chromium 专项硬断言

`--smart-uia-flow` 末尾新增:凡类名为 `Chrome_WidgetWin_1` 的窗口单独计数,
**至少一个**必须探出元素级候选。用"至少一个"而非"全部",是因为个别 Electron 壳
确实不提供子树,全绿要求会在特殊机器上误红。

本机实测结果(2026-11-02 修复后):

```
探测 "35-主代理与子代理并行方案.md - ai-agent -"(VSCodium) → 2070x118  depth=18  63ms
探测 "最新福利羊毛话题 - LINUX DO - Google Chrome"         → 573x76    depth=14  71ms
探测 "新标题： — DeepSeek Harness"(Electron)              → 794x694   depth=11  58ms
PASS Chromium 系窗口能探出元素级候选(3/3 —— 外壳空壳必须被穿透)
```

对照修复前同一台机器:三者的 depth 全是 **1**、矩形全等于整窗。

### 6.3 真机手测清单(沙箱跑不了 GUI)

| # | 场景 | 期望 |
| --- | --- | --- |
| 1 | Chrome 打开新闻/购物页,鼠标在大图/标题/正文上分别停稳 | 框住的是**该区块**,不是整窗 |
| 2 | Chrome 页面缩放到 50% / 页面很长 | 仍框得住(预算够) |
| 3 | 在地址栏悬停 | 框住地址栏那条(default=content 时跳过) |
| 4 | Chrome 前台 + 另一个最大化窗口在下面 | 滚轮能在两者间切 |
| 5 | 同一窗口内滚轮 | 在 窗口→区块→段落 之间切粒度(P2) |
| 6 | 175% 屏上的 Chrome | 矩形与窗口边界差 < 2px(坐标口径不受影响) |
| 7 | Chrome 最小化 / 在别的虚拟桌面 | 不出现候选 |
| 8 | VS Code / VSCodium(Electron,同为 `Chrome_WidgetWin_1`) | 能框住编辑区,不是整窗 |

---

## 7. 风险与回退

| 风险 | 处置 |
| --- | --- |
| DFS 在极端页面超预算 | `budgetMs` 硬截断 + 返回当前 best(已是既有语义);超预算时退回 ControlView 结果 |
| RawView 在超大站(几万节点)卡住 | `maxNodes` 按**节点**计数(现值按"每层子节点数"累加,量纲错);超限立即返回 |
| 元素框过细导致"框得很碎" | 两处兜底:`minSize=24`(已有)+ §5 默认落到链上"≤ 窗口 60%"那一档 |
| 改动破坏非 Chromium 应用(微信/ZCode 现在能用) | `--smart-capture-flow` A~G 段全保留;线上加"元素不得超出窗口"断言(已有) |
| COM 调用量上升影响目标进程 | 仍只在"停稳 400ms"后投、仍走 `QtConcurrent::run`;新增负缓存(F5)反而**降低**总调用量 |

**回退开关**:`smartCaptureGranularity=window` 一行配置即可退回今日行为,不需回滚代码。

---

## 8. 需要更正的既有文档

| 文档 | 位置 | 更正 |
| --- | --- | --- |
| `docs/23-smart-capture-plan.md` | §7.4「真实覆盖率」 | `Chrome(ZCode / B 站) ⚠️ L1 n=0` **删除**;改为「现状实为算法只走第 0 个兄弟所致,见 docs/26」 |
| 同上 | §7.3「同尺寸容器必须穿透」 | 补一句:该补丁治不了"同层多兄弟"的情形,真正的修法是 DFS |
| 同上 | §11 已知限制 1 | 「Chrome/Chromium 系列元素级不生效」→ 删除,改为「(2026-11-02 已修,见 docs/26)」 |
| `docs/progress.md` | 2026-10-26 智能截图条目 | 追加"该结论于 2026-11-02 被推翻"的追记,不要篡改历史记录 |

---

## 9. 排障入口(新增)

| 现象 | 先看 |
| --- | --- |
| Chrome 上框住整窗 | 跑 `--smart-uia-flow`,看顶层**每个**兄弟的子节点数(§S3 改造后);若第一个是空壳而第二个有树 → 算法没走 DFS |
| 元素框比预期粗 | 是不是只跑了 ControlView(S2 第二发没投)? |
| 元素框比预期细/碎 | 调 S6 粒度或"≤ 窗口 60%"阈值 |
| 探测耗时异常 | 看 `visited` 计数(F5 的负缓存是否生效) |
