# 22 设置页:从抽屉到整窗页面

> 2026-10-25 实施。把设置从「主面板右侧的 Popup 抽屉」重构为「主面板窗口内的
> 第三种页面形态」(与翻译页同一模式),解决三件事:
> ① 设置项渐多后,单一长滚动表单里**跳转到不同类设置不便**;
> ② 抽屉(Popup)整表单一次实例化约 **6.6MB 私有 / 11MB 工作集**且常驻不释放
>    (progress.md 2026-10-09 记录),表现为「打开设置抽屉内存突然小量增多」;
> ③ Popup 挂在 overlay 层,与整窗页面形态(翻译页)互相干扰(浮在翻译页上
>    且 Esc 收不到,2026-10-09 修过一例)。

## 1. 形态决策:页面,不是独立窗口

用户提出「能否像翻译界面那样独立出来」。事实澄清:**翻译界面并不是独立窗口**,
而是主面板内的整窗页面(docs/06 内存收口把独立翻译窗口并入了主面板,省掉第二个
QQmlEngine 与渲染栈,隐藏态 231MB → 130MB)。设置页沿用同一模式:

- 同一 `QQuickWidget`、同一 `QQmlEngine`,与列表/翻译页**互斥显示**;
- `PanelWindow::Mode` 增加 `Settings`,`mode` 属性暴露 `"settings"`;
- 进入时窗口切到 **860×640**(下限 700×520;侧栏 176 + 内容表单的舒适尺寸),
  回列表时恢复用户记住的 `panelSize`(既有守卫:仅 List 形态记尺寸);
- `m_holdOpen` 对 Settings 也置真(填 API Key / 测代理时切走焦点不消失)。

独立顶层窗口方案被否:第二个 QML 引擎 + 场景图/渲染栈约 +50MB 起,与内存收口方向相反。

## 2. 布局与分组

```
┌──────────────────────────────────────┐
│ ←  设置                          ✕   │ 48px 标题栏(可拖动;←/✕ 均为退出设置回列表)
├──────────┬───────────────────────────┤
│ ▎通用     │  当前分类表单(Flickable)    │
│  截图     │  …                        │
│  …(9 类) │                           │
├──────────┴───────────────────────────┤
│  已保存 · 数据目录将在下次启动生效      │ 跨分类共享状态行
└──────────────────────────────────────┘
```

九个分类(把旧表单顶部无标题的杂项区拆开):

| 分类 | 收纳内容 | 页文件 |
| --- | --- | --- |
| 通用 | 主题、剪贴板监听、自动粘贴、历史/图片/文本上限、软件渲染、开机自启、数据目录 | SettingsPageGeneral.qml |
| 截图 | 框选后行为、右键取消、延时、遮罩超时、长截图×4 | SettingsPageCapture.qml |
| 贴图 | 不透明度、双击关闭、图片优先、文本贴图×5(含色板) | SettingsPagePin.qml |
| 快捷键 | 9 个动作录制 | SettingsPageHotkeys.qml |
| 文字识别 | 后端、语言、放大、偏好、驻留 | SettingsPageOcr.qml |
| 翻译服务 | 通道、各通道字段、超时、测试连接 | SettingsPageTranslate.qml |
| 网络与代理 | 代理模式、手动字段、测试链路 | SettingsPageNetwork.qml |
| 数据 | 备份、导出×3、恢复/导入(确认卡)、危险操作(清空确认卡) | SettingsPageData.qml |
| 关于 | 版本、检查更新 | SettingsPageAbout.qml |

侧栏选中态与列表页分类标签同一视觉语言(§15:AccentSoft 底 + Accent 字 +
左 3px 指示条)。图标全部走 HeaderIcon 词汇表,新增 4 个 kind:
`capture`(四角取景框)/ `keyboard`(键位阵列)/ `network`(三节点网状)/
`info`(圆 + i);其余复用既有(settings/pin/text/translate/file)。

## 3. 内存策略(解决「打开设置突增」)

- **壳懒构建 + 常驻**:`Main.qml` 的 `settingsPage` Loader 首次进设置才建壳
  (标题栏 + 侧栏,几百 KB 级),之后常驻(保住导航位置)。
- **内容页只装当前分类**:壳内 `Loader` 的 `sourceComponent` 绑定当前分类,
  切分类 = 销毁旧页、构建新页 —— 任何时刻只有**一个分类页**活着(旧抽屉是
  40+ 字段全量常驻)。
- **离开设置页即卸载**:`contentActive` 由 modeChanged/panelHidden 信号驱动,
  离开时**先落盘再卸载**(顺序由 SettingsView 的 Connections 保证,避免
  「卸载先跑、pending 的编辑被销毁」的竞态)。

### 3.1 实测读数(--settings-flow,acceptance-data 隔离目录)

| 时点 | 工作集 | 私有 |
| --- | --- | --- |
| startup | 149.7 | 91.7 |
| 首次 mode-settings(切形态+壳+通用页) | 159.6 | 101.4 |
| ✕ 退出(通用页交互过) → mode-list | 175.1 | 116.6 |
| 再进 settings | 175.2 | 117.0 |
| Esc 退出 → mode-list | 175.4 | 115.6 |
| 再进(切 OCR 分类) | 175.5 | 115.6 |

要点:
- **反复进出设置页零累积**(175.1 → 175.4 → 175.5,噪声级)—— 这是相对旧抽屉
  的结构性收益(旧版整表单常驻,退出不释放任何东西);
- 首开增量(含 800×600→860×640 窗口后备缓冲切换)约 +4~10MB,与旧抽屉同量级,
  但旧版这 6.6MB 是**永久常驻**,新版离开即销毁(进程私有字节因分配器持有堆
  不会立即回落,但对象树已释放、后续分配复用);
- 从不进设置的用户零成本(懒构建,与旧版持平)。

## 4. 页面协议(所有 SettingsPage*.qml 一致)

```qml
property var view            // 壳(SettingsView),由 Loader onLoaded 注入
function load(s)             // 从 get_settings 结果回填;兜底默认值与 core Default 同口径
function collect(s)          // 把本页字段写回设置对象(读-改-写,未实例化的页不参与)
```

保存仍是 400ms 去抖自动落盘;`save()` 先 `get_settings` 取当前值、只让**已实例化**
的分类页写入自己的字段,再 `save_settings` —— 未打开的分类天然不碰。
新增:**切分类 / 离开设置页前 `flushPendingSave()`**,修掉抽屉时代「去抖窗口内
关掉抽屉丢最后一笔编辑」的隐患。

异步结果(翻译/代理测试、备份/导出/恢复/导入、更新检查)的 `Connections` 挂在
**壳根**、结果写根属性、各页 label 绑定 —— 用户切走分类再切回来,结果还在
(挂在分类页上会随页面销毁丢失)。脏标志(hotkeysDirty/dataDirDirty/renderDirty)
由各页置位、壳层 save 成功后统一消费,applyTheme/reloadHotkeys/refreshNow 语义不变。

## 5. 共享表单控件文件化

原 SettingsDrawer.qml 的内联 `component` 抽成模块级文件(与 ReadableComboBox.qml
同一做法),供各分类页复用:`ReadableCheckBox/ReadableSpinBox/ReadableTextField/
HotkeyField/ColorSwatchGrid/SectionDivider/SectionTitle/FieldLabel/HintLabel.qml`。

⚠️ 抽文件的坑(本轮实抓):内联组件住在 SettingsDrawer 里时靠**宿主的 import**
解析 `Label`;独立成文件后必须自带 `import QtQuick.Controls.Basic`,漏了就是
「Type SectionTitle unavailable → 整个设置页加载失败 → 主面板白屏」。
qmlcachegen 不做跨文件类型解析,**构建不报错、运行时才炸** —— 新增 QML 文件
必须跑一次 `--screenshot --settings` 自截确认。

## 6. 交互语义

| 动作 | 行为 |
| --- | --- |
| 头部「设置」按钮 / 托盘菜单「设置」 | 进入设置页(同一漏斗 `PanelWindow::showSettings()`) |
| **停留在设置页期间** | 失焦不隐藏(`m_holdOpen`)+ **窗口置顶**(`applyTopmost()`,与「固定」同一机制):点其它应用既不消失也不被盖住 —— "不主动关闭就不消失"。离开设置页时按固定状态还原置顶。翻译页**刻意不置顶**(既有决策不变) |
| Esc / ← / ✕ | 退出设置回列表页(Esc 分层:选择模式 → 筛选浮层 → 翻译页浮层 → **设置页退列表** → 收起面板) |
| ✕ ≠ 翻译页的 hidePanel | 设置改动即时自动保存,没有「到此为止」的收尾意图;实测 ✕ 若 hidePanel,面板隐藏后「调完继续用列表」的动线断掉(--settings-flow 抓出) |
| 热键呼出(showAtCursor) | 既有行为:一律切回列表页,设置内容不丢(壳常驻;需要回去走头部按钮或托盘「设置」) |
| 危险操作/恢复/导入确认卡 | 就地二次确认,语义不变(嵌套 Popup 的老坑随整窗页面消失) |

置顶的双重理由(代码注释同源):本窗口是 Qt::Tool(不占任务栏),一旦被其它窗口
盖住就没有入口找回;失焦不隐藏只保证"进程里还在",置顶才保证"看得见、回得来"。
2026-10-25 脚本实测(EnumWindows 轮询 + 激活桌面偷焦点):进设置页瞬间
`WS_EX_TOPMOST` 置位;偷焦点后窗口保持可见直到进程退出(旧逻辑会在 ~200ms
失焦定时器后隐藏)。

## 7. 验收更新

- `--settings-flow`:B2「点抽屉外被 modal 吞掉」改写为「设置页内普通点击不关页、
  不穿透」;D 段新增**真实点击侧栏 `settingsNavOcr` 切分类**再滚入目标
  (侧栏导航链路本身成为验收点);日志标签 `drawerOpen` → `settingsOpen`。
- `--clear-all-flow / --update-flow / --restore-flow`:无需改流程代码 ——
  `revealClearAll/revealUpdateCheck/openRestoreConfirm/openImportConfirm` 在
  SettingsView 内先切分类再转发。
- 全部 objectName 验收钩子原样保留(settingsClose/historyLimitSpin/ocrBackendCombo/
  clearAllBtn/restoreBackupButton/updateCheckBtn/…,侧栏新增 settingsNav*)。
- 2026-10-25 实测:四流程全绿;`--screenshot --settings` 自截确认渲染。

## 8. 文件清单

| 变更 | 文件 |
| --- | --- |
| 新增(壳+分类页+共享控件) | panel/qml/SettingsView.qml、SettingsPage{General,Capture,Pin,Hotkeys,Ocr,Translate,Network,Data,About}.qml、Readable{CheckBox,SpinBox,TextField}.qml、HotkeyField.qml、ColorSwatchGrid.qml、Section{Divider,Title}.qml、FieldLabel.qml、HintLabel.qml |
| 删除 | panel/qml/SettingsDrawer.qml、AppDrawer.qml(仅设置抽屉在用) |
| C++ | panel_window.{hpp,cpp}:Mode::Settings、860×640 尺寸、showSettings()/showList()、applyTopmost()(固定/设置页共用)、mode-settings 探针 |
| 接线 | Main.qml:settingsPage Loader、openSettingsPage()、Esc 分层、键盘守卫、acceptanceState/验收函数转发 |
| 图标 | HeaderIcon.qml:+capture/keyboard/network/info |
| 托盘 | tray.cpp:「设置」入口(打开翻译之后,直达 showSettings) |
| 验收 | acceptance.cpp:runSettingsFlow 的 B2/D 段与日志标签 |

## 9. 明确不做(后续增强,另行立项)

- 设置项搜索(需要字段元数据注册表);
- 托盘「设置」入口、设置专属热键;
- core 侧任何改动(settings 结构不变)。

## 10. 教训记录

- **qmllint 内存事故**:在循环里反复无参调用 qmllint(无 `-I` 导入路径),
  部分实例挂起不退出,多实例并存扫描把系统内存耗尽卡死(2026-10-25 实发)。
  对策:本轮起 QML 校验以**构建**(qmlcachegen 全量编译)+ `--screenshot` 自截
  为准;如需 lint 必须单文件 + 显式 import 路径。
  讽刺的是,事发前 qmllint 的 [unresolved-type] 告警正是 SectionTitle.qml 缺
  Controls 导入的真信号,被工具噪音淹没了 —— 告警要读,调用方式也要克制。
