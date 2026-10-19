# 18 · 现状盘点与下一版 backlog(2026-10-19)

> 触发:用户问「还有哪些功能可以推进或完善」。本文是对**当前仓库实况**的全量盘点,
> 不是路线图复述;所有条目都给出证据坐标,便于直接动手。
>
> ⚠️ 上一版(2026-10-17)的条目已有一半在随后两天内落地,本版**逐条重新核实**过:
> 已完成的移入 §1 并保留证据,只有仍未做的才进 §2–§4。排期请以本文为准。

---

## 0. 现状一句话

功能面已完整:剪贴板历史 / 搜索 / 标签 / 来源筛选 / 多选批量 / 待办(含桌面便签) /
截图四模式 / 标注编辑器 / 贴图 / 滚动长截图 / OCR 双后端 / 翻译五通道 /
备份·恢复·导入·导出(Markdown·JSON·CSV)。

**真正的缺口只剩三类:**

1. **发布收尾没签字** —— 渠道、签名、安装冒烟、性能留痕都空着,自动更新因此是个死按钮;
2. **少数看得见的断点** —— 文本条目不可编辑、删除不可撤销、搜索只有子串匹配;
3. **文档仍有残余偏差** —— 版本号二分、docs/04 状态、docs/13 残留字符。

---

## 1. 已落地(勿再排期,附证据)

| 项 | 证据 |
| --- | --- |
| 自启带 `--hidden` | `app/src/host/auto_start.cpp` 的 `kHiddenArg` + `refreshCommandLineIfStale` |
| 遮罩首帧 QPainter 守卫 | 提交 `0b083da`;`capture_session.cpp` 的 paintEvent 已有 `isActive()` 判断 |
| 滚动长截图全链路 | docs/19 S1–S6;`nb_stitch_append/finish/abort`(core_bridge.cpp:70-72) |
| 另存 JPEG / WebP | 提交 `aa72b98`;`app/src/host/image_save.{hpp,cpp}` 三处入口统一 |
| 截图入库免 base64 往返 | `nb_ingest_capture_rgba` 原生导出,宿主 `ingestImageAndCopy` 优先直入 |
| 待办桌面便签 | docs/20;`app/src/note/`;验收 `--note-flow` |
| 列表键盘导航 / 复制后自动粘贴 / 多选批量 | `Main.qml` 的 `kbIndex` 与设置 `copyAutoPaste` |
| `save_settings` 字段级合并 | `ffi.rs:595-615` `merge_settings`;docs/17 F1/F7 |
| 默认软件渲染 | `settings.rs:477` `software_render: true` |
| 热键冲突 toast | `hotkeys.cpp:170-175`(提交 `5ec3a19`):失败项汇总成一条 toast |
| 贴图软上限与字节预算淘汰 | `core_facade.cpp:494-606` + `logMemory` |
| OCR 后端 / 偏好 / 驻留选择器 | `SettingsDrawer.qml` 的 `ocrBackendCombo` / `ocrProfileCombo` / `ocrSessionCombo` |
| OCR 语言 / 放大倍数控件 | 本轮新增:`ocrLanguageCombo` / `ocrScaleSpin`,仅「系统内置」后端可见 |
| 单条文本上限控件 | 本轮新增:`maxTextKbSpin`(此前只有字段、没有入口) |
| 导出 JSON / CSV | 本轮新增:`export_json` / `export_csv` + 3 条单测 |
| 搜索命中高亮 | 本轮新增:`EntryCard.qml` 的 `query` 属性与 `highlightedRichText()` |
| 贴图方向键微调 | 本轮新增:`pin_window.cpp` 的 keyPressEvent(1px;Shift 10px) |
| 面板尺寸记忆 | 本轮新增:`settings.panelSize` + `PanelWindow::preferredPanelSize/savePanelSize` |
| 仓库清理 | `git rm --cached` 两个 smoke_host 二进制;`.gitignore` 的 28 行 clangd 条目合并为一条 |
| 待办贴屏多屏横跳 | 2026-10-19:`anchorFor` 改取屏内一端 + 新增 `screenForNote` + `adoptScreenUnderWindow` 收起态短路;`--note-flow` 新增 STEP3b(有 A/B 留痕)。见 docs/20 §13.11 |
| 贴屏上下边下线 | 2026-10-19 用户裁定;菜单只剩左右,核心 `normalized()` 把 top/bottom 归一成 none。见 docs/20 §6.2 修订 |
| 贴屏交接处不自动收起 | 2026-10-19 用户拍板(docs/20 §11 Q7):交界处收起几何不成立(推出去=推到邻屏上 → enter/expand 弹回 → 闪烁死循环)。`edgeOutsideIsAnotherScreen()` 拦截 + `leaveEvent` 加 `isCursorOverWindow()` 守卫。见 docs/20 §13.12 |

---
## 2. P0 · 发布收尾(挡住「能发」,不挡「能用」)

| # | 项 | 证据 | 影响 | 量级 |
| --- | --- | --- | --- | --- |
| **2.1** | **发布渠道 + 第一个 Release** | `app/CMakeLists.txt:145` 的 `NB_UPDATE_CHECK_URL` 默认空串 → 设置页「检查更新」置灰 | `check_update` FFI + 版本比较 + 13 例单测 + 六模式验收全都已实现,但渠道没定,功能等于不存在 | **小**(定渠道填一行 + 发一次) |
| **2.2** | **代码签名证书** | docs/04 §8 D6 | 不签 → 每次发布都要在说明里写 SmartScreen 警告;且堵死 v2 的「应用内静默升级」 | **需采购**(用户决策) |
| **2.3** | **版本号口径:app 0.2.0 vs core 0.1.0** | `app/CMakeLists.txt:3` vs `core/Cargo.toml:7`;`tools/installer.nsi:46` 也写 0.2.0 | 自动更新的比较基准、安装器 DisplayVersion、git tag 三者要对齐 | **小**(定口径 + 写进 docs/04) |
| **2.4** | **S6.6 性能四项读数** | 埋点已齐(日志里 `mem[...]` / `capture-*`),从未填表 | 路线图的量化验收门没有一条有留痕结论 | **小**(纯读日志) |
| **2.5** | **S6.5 内存组合场景** | 隐藏态约 130MB 已有读数,但「10 张贴图 + 50 条含 4K 图列表」没测过 | 唯一的架构级验收门 | **小–中** |
| **2.6** | **安装冒烟 7 项** | docs/04 §6 | 静默安装在本环境无法验证,只能真机 | **小**(真机) |
| **2.7** | **手感验收(软件渲染)** | docs/06 §11.5;默认后端已是软件 | 这是默认渲染后端变更的唯一放行条件,至今未确认 | **小** |

---

## 3. P1 · 功能补全(有价值、当前确实没有)

| # | 功能 | 为什么值得做 | 现状证据 | 量级 |
| --- | --- | --- | --- | --- |
| **3.1** | **文本条目内联编辑** | 复制来的文本有笔误或多条空行时只能删掉重抄;S5「素材库」语义下这是明显缺口 | `core_facade.cpp:723` 直接 `emit toastMessage("只有图片条目可以编辑")`;core 只有 `save_text_entry`(新增)无 update;FTS 触发器已覆盖 `AFTER UPDATE OF text_content`(v8.sql) | **中** |
| **3.2** | **删除可撤销** | 误删即永久丢失;确认文案自己写着「此操作不可恢复」 | `delete_entry` / `delete_entries_bulk` 真删;`SettingsDrawer.qml` 与 `Main.qml` 的确认弹窗文案 | **中** |
| **3.3** | **搜索增强(正则 / 仅标题 / 大小写)** | 现在只有 FTS + 中文 LIKE 兜底;命中高亮已做,匹配能力没做 | `store/entry.rs` 的 `list(conn, query, kind, pinned, tag, source, limit, offset)` 是唯一入口 | **中** |
| **3.4** | **翻译历史持久化** | 历史是进程内内存队列,上限 50,退出即清零 | `translate/mod.rs:109` `static HISTORY: Mutex<Vec<HistoryItem>>` | **小**(可复用 `clip_entries` + source=translate) |
| **3.5** | **截图自动 OCR 入库(默认关)** | 路线图 S5.4 的原意:截图即可搜到图里的字 | `ocr_text` 列 + FTS 触发器已备,app 侧无开关 | **小** |
| **3.6** | **二维码识别** | 截图 → 直接出链接或文本;可复用现成 RGBA 管线 | 全域零实现;方案评估已完成,需新增解码依赖(首次构建要联网) | **小–中** |
| **3.7** | **敏感内容保护:应用黑名单 + 疑似密钥不落库** | 隐私是剪贴板工具的天然软肋(密码管理器、网银) | 路线图 backlog 已列;app 与 core 两侧 grep 零命中 | **中** |
| **3.8** | **剪贴板来源应用追踪** | 有了 owner 才能做黑名单,也才能在卡片上标「来自 Chrome」 | `clip.rs` 无 `GetClipboardOwner`;有 `CF_HDROP`,无 `CF_HTML` | **中** |
| **3.9** | **日志轮转** | 日志只 append + flush,长期运行无限增长 | `app/src/main.cpp:29` 的 `QFile log(...)`;S2 注释写着「发布期移除」 | **小** |
| **3.10** | **磁盘占用统计 / VACUUM** | 用户看不出数据库与 blob 占了多大,也没有回收手段 | 无任何入口 | **小** |
| **3.11** | **导出诊断包** | 用户报障时拿不到现场(日志 + 设置 + 版本 + 屏幕信息) | 只有手工找文件 | **小** |
| **3.12** | **自动 / 定时备份** | 手动 zip 三件套已完整,但没人会记得按 | 无定时器 | **小–中** |
| **3.13** | **编辑器裁剪 / 旋转** | 标注工具已有 10 种(含马赛克、取色、序号),构图类操作缺失 | `annotation_engine` 的 `EditorTool` 枚举 | **中** |
| **3.14** | **i18n** | 界面全中文硬编码 | `tr(` / `QTranslator` / `qsTr(` 零命中 | **大**(不建议 v1 做) |

---
## 4. P2 · 体验与工程债

| # | 项 | 证据 | 说明 |
| --- | --- | --- | --- |
| 4.1 | 列表排序切换(时间 / 大小 / 类型) | 查询固定 `ORDER BY e.pinned DESC, e.last_used_at DESC` | 固定优先不变,其余可选 |
| 4.2 | 遮罩 backing store 与多屏懒捕获 | docs/06 §3/§10,约 53MB / 3 屏 | 需重写遮罩渲染路径 + UX 评审,成本高 |
| 4.3 | 翻译页首次打开 +49MB | docs/06 §10/§13 | 已改冻结;TranslateView 内部瘦身未定位 |
| 4.4 | QML 树地板 71.6MB | docs/06 §10 | 「长期隐藏卸载整棵 QML」被判定交互代价过大 |
| 4.5 | 贴图字节预算收到 32MB / 降采样 | docs/06 §9 第 12 项 | 待产品决策 |
| 4.6 | 12px 小字识别差异取舍 | docs/10 §14.6 T-B2.9';`Theme.qml:191` `fontCaption = 12` | 低优先,需产品决定 |
| 4.7 | CI(单测 + 验收 CLI 全手动) | 仓库无 `.github/` | 需先解决「验收 CLI 要真机桌面」这一前提 |
| 4.8 | ~~发布文档仍写「待用户最终确认」~~ → ✅ **已改**(2026-10-19):docs/04 头部改为「§8 的代码签名(D6)与发布渠道(D10)仍待用户拍板,其余按推荐项落地」 | 剩下的只是 2.1 / 2.2 两项决策本身 |
| 4.9 | ~~docs/13 开头有 `undefined` 与 `---` 拼接残留~~ → ✅ **已清**(2026-10-19) | — | — |
| 4.10 | 无内存 / 性能验收 flow | 现有 23 个 flow 全是交互链路 | 可加 `--mem-flow` 读 `mem[...]` 埋点并断言阈值 |
| 4.11 | **`Main.qml` 台账:`ReferenceError: model is not defined`** | `Main.qml` 的 `todoNote: model.todoNote ? ...`(委托根已声明显式 `required property`,应改用它而非 `model`);**改动前就存在**,每行渲染刷一条 WARN | 纯噪声,但会淹没真正的新告警 |

---

## 5. 需要用户拍板(挡住后续工作的)

| # | 问题 | 为什么现在必须定 |
| --- | --- | --- |
| 1 | **发布渠道 + 是否发第一个 Release** | 决定 2.1(自动更新)能否从「死按钮」变成可用 |
| 2 | **代码签名证书** | 不签 → 每次发布都要写 SmartScreen 警告;且堵死应用内静默升级 |
| 3 | **版本号口径**:core 跟随 app,还是独立演进? | 决定 2.3 与打包/更新比较基准 |
| 4 | **默认渲染后端**:维持软件(省约 101MB,手感待验)还是换回硬件? | 决定 2.7 的走向 |
| 5 | **下一步主线**:发布收尾 / 文本编辑 + 撤销 / 搜索增强 / 敏感内容保护 —— 四选一 | 工作量与收益差异大,不宜并行铺开 |

---

## 6. 建议的执行顺序

```
第 1 批(半天、零风险、纯文档/仓库)
  2.3 版本号口径  ·  4.8 docs/04 状态  ·  4.9 docs/13 清理  ·  仓库卫生复查

第 2 批(1 天,验收留痕)
  2.4 性能四项读数  ·  2.7 手感验收(定默认后端)  ·  2.5 内存组合场景

第 3 批(按拍板结果)
  2.1 + 2.2 发布打通(渠道 + 签名 + 第一个 Release)
  或 3.1 + 3.2(文本编辑与删除撤销同一套「条目写路径」,一起做省一半)
  或 3.7 + 3.8(敏感内容保护,须先做来源应用追踪)
```

---

## 附:本次盘点核实的坐标

- 设置字段全集:`core/nbcore/src/settings.rs:45-190`(含热键 8 动作 `HotkeyMap`)
- 设置校验:`settings.rs:501+`(`validate`)、`:663+`(`validate_translate`)
- FFI 方法分发:`core/nbcore/src/ffi.rs:716` 起的 `match method`
- 宿主对 QML 暴露的 API:`app/src/panel/{core_facade,panel_window,clip_entry_model}.hpp`
- 验收 CLI:`app/src/main.cpp` 的参数解析段(23 个入口)
- 版本号:`app/CMakeLists.txt:3`(0.2.0)、`core/Cargo.toml:7`(0.1.0)、`tools/installer.nsi:46`(0.2.0)
- 打包:`tools/package.ps1`、`tools/installer.nsi`;更新地址:`app/CMakeLists.txt:145`
- 测试规模:`cargo test -p nbcore --release` → 356 项(350 passed / 1 既有失败 / 5 ignored)
