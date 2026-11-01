# 24 · 快捷功能(快捷指令):类型体系与可扩展设计方案

| 项 | 内容 |
| --- | --- |
| 版本 | v1(设计,未实现) |
| 日期 | 2026-11-01 |
| 状态 | 设计稿,待评审 |
| 关联 | [23-smart-capture-plan.md](./23-smart-capture-plan.md)(验收风格)· [22-settings-page.md](./22-settings-page.md)(设置页协议)· [theme.md](./theme.md)(§38 图标词汇) |
| 前置已交付 | QuickRail 界面壳(✨ feat(panel) a21ce35:52px 窄栏 + 占位钮)· 设置页「快捷功能」分类与「显示快捷栏」开关(quick_rail,STEP16 验收过) |
| 一句话 | 把面板右侧那条栏从"占位壳"变成**可配置的指令条**:每颗按钮 = 一条快捷指令,能打开应用、直达系统设置页、触发软件内部功能;类型体系按"开放注册"设计,为远期插件入口留位。 |

---

## 1. 需求与定位

### 1.1 用户原话

> "接下来开始定义快捷功能,本质上是创建一个快捷指令,指令可以快速打开一些应用,
> 或者系统设置以及系统应用如(环境变量配置,服务,网络等相关),还有就是软件内部
> 的功能。先初步设定这几项,开发的路线要保留后期的可扩展性,这个地方后期也可以
> 作为插件入口,先分析和给出建议,并完善后续的开发文档。"

### 1.2 与仓库内相邻概念的区别

| 概念 | 入口 | 语义 | 与本设计的区别 |
| --- | --- | --- | --- |
| 全局热键(hotkeys) | 键盘触发动作 | 动作集**固定枚举**(hotkeys.cpp `Action`),可换键不可增删 | 指令是**用户可增删**的按钮,数量与内容开放 |
| 托盘菜单 | 托盘右键 | 固定菜单树 | 指令条是面板内的、用户可配置的一排按钮 |
| 快捷指令(本设计) | 面板右侧 QuickRail | `{类型, 目标, 参数}` 的用户配置,点击即执行 | — |

结论:热键管"键盘侧的高频动作"(固定),快捷指令管"鼠标侧的个性化直达"(开放)。
两者不互相替代;同一动作既可以有热键也可以是指令(如智能截屏)。

### 1.3 为什么落在这条栏

- 呼出即用的面板里,唯一"常驻、可见、不占正文空间"的位置就是 QuickRail(52px 窄栏,界面壳已交付);
- 头部按钮是产品定义的命令(动作型),侧栏按钮是用户定义的命令(配置型)——一动一静,职责清晰。

---

## 2. 目标与非目标

### 2.1 目标(v1)

| # | 目标 |
| --- | --- |
| G1 | 指令统一抽象:`{ id, type, label, icon, payload }`,`type` 首批三档:`app` / `system` / `internal` |
| G2 | `app`:打开应用程序(.exe/.lnk)或文件夹;支持启动参数与工作目录 |
| G3 | `system`:直达系统设置页/系统工具;内置一份**精选目录**(ms-settings + cpl/msc,见 §5),也可手填 |
| G4 | `internal`:软件内部动作,经**动作注册表**(§4)分发;首批 ≥4 个真实动作 |
| G5 | 设置页「快捷功能」:命令的添加/编辑/删除/排序/启用;QuickRail 按配置渲染 |
| G6 | 类型体系开放注册:执行器按 type 分发,新增类型 = 注册一个 handler(插件入口的技术底座) |
| G7 | 配置落库 + 校验(schema/重复 id/类型白名单),损坏配置回退默认不阻塞启动 |

### 2.2 非目标(v1 明确不做)

| # | 不做 | 理由 |
| --- | --- | --- |
| N1 | 脚本解释器/命令包装(如 `cmd /c` 自由拼) | 安全面;`app` 类型已覆盖"启动一个程序带参数"的需求,拼 shell 串是恶意软件的分发面 |
| N2 | 插件运行时(动态库加载/子进程协议) | 远期方向,本期只做**数据模型与执行器的可扩展**(§7.3),运行时形态待调研 |
| N3 | 指令的键盘快捷键绑定 | 热键系统已有固定动作表;给用户指令再绑热键是双配置面,v1 不做 |
| N4 | 子菜单/文件夹分组 | 52px 窄栏容不下层级交互;数量上限(§6.3)先行控制 |
| N5 | 图标自定义上传 | 用内置图标词汇 + 首字母兜底;自定义图片引入资源管理问题 |
| N6 | 指令的导入/导出/分享 | 备份恢复已覆盖配置文件;跨用户分享待真实需求 |

---

## 3. 数据模型

### 3.1 决策:配置放 settings 的 JSON 数组字段,不建独立表

```rust
// settings.rs
/// 主面板快捷指令条(QuickRail)的用户配置。数组顺序即显示顺序。
pub quick_rail_commands: Vec<QuickCommand>,

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct QuickCommand {
    /// 稳定 id(编辑不改 id,排序/启用按 id 定位)。生成规则见 §3.3。
    pub id: String,
    /// 指令类型:内置 "app" | "system" | "internal";开放字符串(§7.2)。
    #[serde(rename = "type")]
    pub kind: String,
    /// 显示名(按钮 tooltip;≤ 24 字符,v1 单行)。
    pub label: String,
    /// 图标:HeaderIcon 词汇;空 = 按类型给默认(app 可给首字母,§6.3)。
    pub icon: String,
    /// 类型相关载荷(见下表)。校验规则按 kind 分发(§3.2)。
    pub payload: QuickPayload,
    /// 启用开关(false = 保留配置但不渲染)。
    pub enabled: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct QuickPayload {
    /// app:可执行文件路径(.exe/.lnk)或文件夹;system:目录键或 ms-settings URI;
    /// internal:动作注册表 id(§4)。
    pub target: String,
    /// app 专有:启动参数(原样传给 ShellExecute,不做 shell 展开)。
    pub args: String,
    /// app 专有:工作目录(空 = 继承 target 所在目录)。
    pub workdir: String,
}
```

| 决策 | 取舍 |
| --- | --- |
| 放 settings 不建表 | 指令是**配置**不是数据:无搜索/FTS/淘汰语义;设置页读写与备份恢复全自动跟随 |
| `kind` 是开放字符串 + 校验白名单 | 反序列化不限制未来类型;`validate()` 只要求"内置三类型必过各自校验,未知类型放行但宿主不渲染不执行"(前向兼容:新版配置回滚旧版不会因新类型炸掉校验) |
| payload 平铺而非嵌套对象 | app 的 args/workdir 之外其它类型不用它们;平铺让 QML 编辑卡片直接映射字段,不用按类型建子结构 |

### 3.2 校验规则(validate 追加段)

| 类型 | 校验 |
| --- | --- |
| 通用 | id 非空且**列表内唯一**;label 非空 ≤24 字符;列表长度 ≤ 12(§6.3) |
| app | target 非空;扩展名 ∈ {.exe, .lnk, .bat, .cmd} 或为已存在目录;args ≤ 512 字符 |
| system | target 必须命中 §5 目录的键或匹配 `ms-settings:` 前缀(防手填任意 URI 的面) |
| internal | target 非空;**是否为真实动作 id 由宿主校验**(§4.2,core 不知道宿主动作) |

> core 与宿主的校验分工沿用既有原则:core 校验**语法**(格式/长度/唯一性),
> 宿主校验**语义**(动作 id 是否存在、路径是否可执行)。core 校验过的字段宿主可信任。

### 3.3 id 生成

时间戳 + 随机后缀:`qc-<unix_ms>-<4位hex>`,宿主生成(core 不参与)。
不使用递增序号:排序配置在数组顺序里,id 与顺序解耦后编辑/排序互不干扰。

### 3.4 现有 quick_rail: bool 字段保持不动

它管"整条栏显不显示";`quick_rail_commands` 管"栏里有什么"。两个正交开关,
设置页布局上相邻(已交付的开关不动,新增管理区在其下方)。

---

## 4. internal 动作注册表

### 4.1 决策:注册表放 C++(CoreFacade),不放 QML 也不放 Rust

| 候选 | 否决/采纳理由 |
| --- | --- |
| Rust(nbcore) | ✗ 动作大多要动宿主窗口(收面板再截图、切形态)——core 是无窗口逻辑层,倒挂了 |
| QML 单例 | ✗ QML 侧可分发的只有纯视图动作;截图链路(hidePanelThenCapture)、贴图(pinLatest)的实现在 C++,QML 反调多一层且验收直调困难 |
| **C++ CoreFacade(Q_INVOKABLE)** | ✓ 所有可执行路径的汇聚点;目录可序列化成 QVariantList 给 QML 渲染,验收可直调 |

### 4.2 接口(新增于 CoreFacade)

```cpp
/// 动作目录(QML 渲染 + 设置页挑选 + 校验):[{id, label, iconKind, available}]
Q_INVOKABLE QVariantList quickActionCatalog() const;
/// 执行内部动作。未知 id 返回 false 并 toast(配置可能来自更高版本)。
Q_INVOKABLE bool runQuickAction(const QString& id);
```

内部实现:`QHash<QString, ActionEntry>` 静态注册表,`ActionEntry { label, iconKind, handler }`。
**新增动作 = 在注册表文件加一行** —— 这是"动作池"的唯一落点,设置页挑选列表从
catalog() 来,永不手抄两份。

### 4.3 首批动作(全部复用现有链路,零新逻辑)

| id | label | 接到的现有实现 |
| --- | --- | --- |
| `capture.region` | 智能截屏 | `hidePanelThenCapture(m_panel, CaptureIntent::Region)`(hotkeys.cpp:86 同源) |
| `pin.latest` | 贴图(最近内容) | `CoreFacade::pinLatest()`(hotkeys.cpp:93 同源) |
| `monitor.toggle` | 暂停/恢复监听 | 托盘 pauseAction 同一条 get→改→save 链路(tray.cpp:177);label 按状态动态 |
| `theme.toggle` | 深浅色切换 | 取反 theme 并 save_settings + applyTheme(设置页 save() 已有幂等应用) |
| `todo.new` | 新建待办 | panel.openTodoComposerForAcceptance() 已有入口 |
| `settings.open` | 打开设置 | panel.showSettings() |

> `available` 字段为插件时代预留:某动作在当前环境不可用时目录里标记,UI 置灰。v1 恒 true。

---

## 5. system 类型:精选目录 + URI 直填

### 5.1 内置目录(设置页下拉挑选;target 存**键名**而非命令串)

| 键 | 显示名 | 执行 |
| --- | --- | --- |
| `env-vars` | 环境变量 | `rundll32.exe sysdm.cpl,EditEnvironmentVariables` |
| `services` | 服务 | `services.msc` |
| `network` | 网络连接 | `ncpa.cpl` |
| `firewall` | 防火墙 | `wf.msc` |
| `devmgmt` | 设备管理器 | `devmgmt.msc` |
| `diskmgmt` | 磁盘管理 | `diskmgmt.msc` |
| `taskmgr` | 任务管理器 | `taskmgr.exe` |
| `control` | 控制面板 | `control.exe` |
| `display` | 显示设置 | `ms-settings:display` |
| `sound` | 声音设置 | `mmsys.cpl` |
| `power` | 电源/睡眠 | `ms-settings:powersleep` |
| `windows-update` | Windows 更新 | `ms-settings:windowsupdate` |

要点:

- **目录数据是表不是硬编码 if**:`QVector<{key, label, exec}>`,执行器统一走
  `ShellExecuteW`;新增系统页 = 表里加一行。
- `.msc/.cpl` 直接 ShellExecute(系统关联打开);`ms-settings:` URI 由 ShellExecute
  直接吃(实测均通)。
- 手填仅允许 `ms-settings:*` 前缀(校验见 §3.2),不开放任意 URI(protocol handler
  是注册表面,放开等于把启动器交给第三方协议)。

### 5.2 app 类型执行

`ShellExecuteW(NULL, L"open", target, args, workdir, SW_SHOWNORMAL)`:
比 `QProcess::startDetached` 多覆盖 .lnk 与"以默认方式打开文件夹"语义;
工作目录空的兜底 target 所在目录。启动失败(路径不存在/被删)→ toast,不重试不排队。

---

## 6. UI 设计

### 6.1 QuickRail(改造已交付的壳)

```
actions 数组(静态占位)→ 三段拼装:
  [quick_rail_commands 中 enabled 的项(顺序即数组序)]
+ [固定的「添加/管理」钮(plus 图标,原占位钮转正)]
```

- 按钮渲染复用现有 delegate(icon/tooltip/hover);internal 的动态 label
  (monitor.toggle)由目录信号驱动刷新;
- 「添加」钮点击 → 打开设置页快捷功能分类(panelWindow.showSettings 链路);
  这取代现在的 toast 占位;
- 超出栏高的按钮**不滚动、不折叠**:数量上限 12 兜底(§6.3),实际建议 6~8 颗。

### 6.2 设置页「快捷功能」分类(扩展现有页)

```
[✓] 在主面板右侧显示快捷功能栏        (已交付,不动)
── 栏内按钮管理 ──
┌──────────────────────────────────────────┐
│ ≡ 智能截屏        [internal]  [开] [↑][↓][✕] │
│ ≡ 打开服务        [system]    [开] [↑][↓][✕] │
│ ≡ VS Code         [app]       [开] [↑][↓][✕] │
│ + 添加指令                              │
└──────────────────────────────────────────┘
[ 添加指令 ] → 弹编辑卡片(复用面板居中卡片模式:屏障+卡片):
    类型下拉(app/system/internal) → 按类型切换表单:
      app:     路径(必填) / 参数 / 工作目录 / 名称
      system:  目录下拉(§5.1)或 ms-settings 手填 / 名称
      internal: 动作目录下拉(§4.2 catalog)/ 名称
    [取消] [保存]
编辑 = 同卡片预填;删除走行内确认(指令删除可重建,单条确认即可)。
```

- 排序 ↑↓ 直接改数组顺序并 scheduleSave(不做拖拽:列表短,按钮排序比 DnD 稳且可验收);
- 每行「开/关」= enabled,关闭的行置灰保留(配置宁可灰不可丢)。

### 6.3 数量与尺寸约束

| 约束 | 值 | 理由 |
| --- | --- | --- |
| 上限 | 12 | 40px 钮 + 4px 间距,380px 最小高面板放 8 颗,600px 下 12 颗;再多说明需要分组(远期) |
| label | ≤ 24 字符 | tooltip 单行不截断 |
| icon | HeaderIcon 词汇或空 | app 类型 v1 给通用 "app" 图标(.lnk 内嵌图标提取 Q2 评估) |

---

## 7. 可扩展性设计(插件入口的底座)

### 7.1 三层分离

```
配置层  settings.quick_rail_commands(JSON,core 只做语法校验)
分发层  C++ CommandDispatcher:type → handler 注册表(QHash)
执行层  handler(ShellExecute / 动作分发 / ——未来:插件协议)
```

新增一种指令类型的改动面 = 分发层注册一个 handler + 设置页编辑卡片加一个类型分支 +
core 校验白名单追加。三层任何一层都不需要知道其它类型的细节。

### 7.2 指令 schema 的版本与演进

- `type` 开放字符串 + "未知类型不渲染不执行不报错"是**前向兼容契约**:
  装过带插件指令的配置回滚到旧版,旧版静默跳过,不炸校验、不丢配置(数据还在,
  升回来即可用)——与 settings 容器级 default 的哲学一致;
- 若未来 payload 结构需要破坏性演进,在 QuickCommand 顶层加 `v` 字段(现在不占用,
  避免 v1 全量写 1 的噪音)。

### 7.3 插件入口(远期方向,v1 只留位不实现)

| 候选形态 | 评估 |
| --- | --- |
| 动态库插件 | ✗ ABI 锁死、崩溃带崩主进程、签名/分发复杂 |
| 子进程 + stdout JSON 协议(类 LSP) | ✓ 进程隔离;插件 = 一个 exe,清单(manifest)声明 `type` 与调用方式;指令点击 → 宿主拉起/复用插件进程 → JSON 往返 |
| 单文件指令包(.nbcmd,本质是分享出去的 JSON) | ✓✓ 折中:导入 = 校验后并入配置,零运行时,最早可落地 |

**v1 的留位**(已含在设计中,不额外付费):开放 type 字符串、dispatcher 注册表、
catalog 的 available 字段、§3.2 的白名单校验点。这四处是插件形态落地时的接缝,
其余(UI/配置/验收)不需要重写。

### 7.4 安全边界(从第一天钉死)

- **nbcore 永不执行进程**:所有启动动作在宿主 C++。core 保持"配置与数据"职责,
  不成为任意命令执行面(Rust 侧无 ShellExecute 依赖);
- `app` 不做 shell 包装:args 原样传 ShellExecute 的参数槽,不经过 `cmd`,
  `&&` `|` `>` 无效 —— 想跑脚本就建 .bat 再指向它(用户显式行为);
- `system` 类型只认 §5.1 目录键 + `ms-settings:` 前缀;
- 插件(远期)子进程以**用户权限**运行、无提权;manifest 必须声明触达的能力,
  设置页安装时明示。

---

## 8. 实施分期

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **Q1 主体** | 数据模型(§3)+ core 校验 + CommandDispatcher 与注册表(§4)+ app/system/internal 三 handler + QuickRail 渲染真实化 + 设置页列表管理(增删改排启,§6.2)+ 首批 6 动作(§4.3) | 新增 `--quick-commands-flow`(§10 全部 7 项) |
| **Q2 打磨** | system 目录补全 §5 全表 + app 图标推断(SHGetFileInfo 评估)+ rail 右键快捷管理(置顶/禁用/删除)+ 编辑卡片校验错误就地显示 | 手动清单;设置页相关 flow 回归 |
| **Q3 插件调研** | §7.3 子进程协议 PoC(单插件:返回两条指令并能执行) | 独立设计文档(届时再立 docs) |

依赖:Q1 无前置(QuickRail 壳与设置页分类已交付)。预估:Q1 约 2~3 天,Q2 约 1 天,Q3 单列。

---

## 9. 风险与既知取舍

| 风险/取舍 | 处置 |
| --- | --- |
| .lnk 图标取不到(v1) | 通用 "app" 图标 + 名称;Q2 评估 `SHGetFileInfo` 取图标缓存为 PNG 的成本 |
| monitor.toggle 的动态 label | v1 简化:rail 按钮图标固定,tooltip 现查状态(Q2 再评估目录动态 label 信号) |
| 用户配置了已卸载的 app | 执行时失败 → toast「无法启动:<label>」,指令保留不自动删 |
| 数组整体替换的并发写 | 设置页是唯一编辑入口(单窗口单页面),rail 只读;无并发面 |
| ms-settings 在旧版 Win10/企业精简版缺失 | 失败同 app 兜底 toast;目录标注适用版本(清单维护) |

---

## 10. 验收流(`--quick-commands-flow`)设计

进程内验收,风格对齐 docs/23(真实点击 + 断言读回):

1. **目录完备**:invoke quickActionCatalog() 含 §4.3 全部 id,available 全 true;
2. **internal 执行**:配置 theme.toggle → rail 真实点击 → get_settings 的 theme 翻转
   (+ Core.darkTheme 属性翻转)→ 复原;
3. **app 执行**:配置 target=`explorer.exe`、args=`/select,<已知存在文件>` → 点击 →
   断言执行成功标志(真机确认资源管理器窗口);
4. **system 映射**:配置 services → 断言 dispatcher 对该键解析出的执行串
   (暴露 Q_INVOKABLE quickSystemResolve(key) 供断言,不真开 msc);
5. **配置往返**:设置页卡片添加(类型分支各一条)→ 排序 → 禁用 → 删除 →
   每步断言 settings 快照;
6. **未知 type**:手工注入 type:`future-thing` → 断言 rail 不渲染、save 不报错、
   启动不炸;
7. **上限**:循环添加至 13 条 → 保存被拒且 toast。
