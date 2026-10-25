# Nextboard

Windows 桌面一体化效率板:**剪贴板历史 + 截图(框选/全屏/编辑/贴图/滚动长截图)+ 待办(含桌面便签)+ OCR + 翻译**。
下一代 board,继承 clipboard-plus 的功能体验与核心代码资产,根治其 WebView2
多窗口内存问题(300–500MB → 目标 ~100–140MB)。

## 技术栈(已定稿)

| 层 | 技术 |
| --- | --- |
| 业务核心 | Rust → `nbcore.dll`(cdylib,JSON 门面 FFI,复用 clipboard-plus 业务代码与单测) |
| 宿主/窗口 | Qt 6 Widgets(MSVC 工具链,单进程事件循环) |
| 主面板 UI | **Qt Quick / QML**(QQuickWidget 嵌入无边框窗口;v0.2 起取代 Sciter) |
| 会话窗口 | 遮罩/贴图/编辑器 = Qt 原生窗口(QPainter / QGraphicsScene) |

分层一句话:**有窗口的归 C++,有逻辑的归 Rust,有界面的归 QML/Qt**。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/01-architecture.md](./docs/01-architecture.md) | 总体架构:技术栈定案、进程/窗口模型与内存预算、FFI 契约、数据模型、风险登记、与第一代决策对照 |
| [docs/02-migration-core.md](./docs/02-migration-core.md) | nbcore 改写路线:逐文件映射表、耦合点改造方案、Cargo 配置要点、FFI 方法表、测试策略、M1–M3 实施顺序 |
| [docs/03-roadmap.md](./docs/03-roadmap.md) | 开发路线图:S0–S6 里程碑、验收门、依赖与并行、回退预案、backlog |
| [docs/04-build-package-release.md](./docs/04-build-package-release.md) | 构建/打包/安装/发布规程:发布构建清单、运行期依赖与体积预算、NSIS 安装器规格、发布流程与检查单(§8 若干决策仍待用户拍板) |
| [docs/05-todo.md](./docs/05-todo.md) | 待办:数据模型与交互(复选框/截止/提醒/分组/气泡) |
| [docs/06-memory-optimization.md](./docs/06-memory-optimization.md) | 内存优化:量化预算、实测读数、逐项落地表与未做项(翻译页/QML 地板/遮罩多屏) |
| [docs/07-auto-update.md](./docs/07-auto-update.md) | 自动更新:版本比较、检查流程、六模式验收(**渠道未定,功能空置**) |
| [docs/08-rapidocr-engine.md](./docs/08-rapidocr-engine.md) · [08a](./docs/08a-rapidocr-implementation-roadmap.md) · [08b](./docs/08b-rapid-ocr-rs-analysis.md) | RapidOCR 引擎接入:选型、实现路线、上游 crate 分析 |
| [docs/09-ocr-optimal-plan.md](./docs/09-ocr-optimal-plan.md) · [10](./docs/10-plan-b-implementation.md) · [11](./docs/11-rapidocr-rewrite-design.md) · [12](./docs/12-ocr-memory-analysis.md) · [13](./docs/13-ocr-enhancement-plan.md) | OCR 方案与内存收口:B 方案实施、引擎内化重写、内存实测、用户选项实测 |
| [docs/14-capture-toolbar-plan.md](./docs/14-capture-toolbar-plan.md) | 截图浮条:形态、按钮语义、与编辑器的分工 |
| [docs/17-editor-ui-unification.md](./docs/17-editor-ui-unification.md) | 编辑器与浮条 UI 统一:工具集、无障碍、设置项收敛(含 F1–F7 落地记录) |
| [docs/18-next-steps.md](./docs/18-next-steps.md) | 现状盘点与下一版 backlog(定期重写;以最新一次为准) |
| [docs/19-scrolling-capture-plan.md](./docs/19-scrolling-capture-plan.md) | 滚动长截图:设计与分阶段实现(S1–S6 全部落地) |
| [docs/20-todo-sticky-note.md](./docs/20-todo-sticky-note.md) | 待办贴屏(桌面便签):数据模型(v7)、FFI 契约、窗口与贴边/半隐藏设计、实现记录与踩坑 |
| [docs/21-todo-title-note.md](./docs/21-todo-title-note.md) | 待办「标题 + 备注」分离:数据模型(v8)、FTS 纳入备注、编辑卡片/列表/便签三处口径 |
| [docs/theme.md](./docs/theme.md) | 视觉规范:色板、尺度、组件语言(Theme.qml 的出处) |
| [docs/progress.md](./docs/progress.md) | 逐日开发记录与实测数据(踩坑与结论的唯一权威来源) |

## 上游项目

clipboard-plus:`D:/3project/rust/clipboard-plus`(只读参考基线;其设计文档
docs/design.md、phase5–11、memory-optimization 中的真机踩坑结论全部视为本项目
的既定约束)。

## 状态

- [x] 技术选型与架构定稿
- [x] 改写路线与开发路线文档
- [x] S0:工具链验证(Rust msvc / VS2022 MSVC 14.44 / Qt 6.9.0 msvc2022_64 / CMake 4.0.1)、
      基线 tag `nextboard-baseline-20261005`
- [x] S1:nbcore.dll 完成(含 C++ 冒烟宿主链路)
- [x] S2:Qt 宿主骨架 + 遮罩/贴图 PoC(可运行:热键 / 托盘 / 单实例 / 日志 / 开机自启)
- [x] S3:主面板数据驱动(桥 / 列表 / 分类计数 / 设置抽屉 / 翻译视图;搜索逻辑由 core 单测覆盖)
- [x] S3 收尾:滚动转发(卡片显式转交滚轮给列表)、右键菜单(复制 / 翻译 / 识别文字 / 固定 / 删除)、
      loadMore(分页 total 收敛);面板交互闭环补齐(ESC 收起、复制后收起、Ctrl+1..9 序号复制、
      toast / 译文信号接通、分类计数随数据刷新)
- [x] S4:编辑器(QGraphicsScene)+ 截图 / 贴图闭环
      (4.1 编辑器、4.2 两种框选确认模式与直通路径、4.3 全屏与一键贴图、
       4.4 截图翻译 / 取字;2026-10-25 补构图操作:90° 旋转 + 交互式裁剪,
       标注与撤销栈全程跟随 —— docs/17 §18.14)
- [x] 主面板 UI 换代:v0.2 起 **Sciter → Qt Quick / QML**(QQuickWidget 嵌入无边框窗口),
      单一 QML 引擎同时承载历史列表、设置抽屉与翻译页 —— 内存收口的主要来源
- [x] S5 一体化素材库:翻译原文 / 译文「存为条目」(source=translate)参与搜索、
      标签与按标签过滤、数据导入(并入式 import:hash 去重 + id 冲突重铸)
- [x] 待办(docs/05 · 20 · 21):v7 `todo_note` 表 + v8「标题 + 备注」分离;
      复选框 / 截止 / 提醒 / 分组 / 今日待办气泡;桌面便签贴边吸附 + 半隐藏拉手 + 几何落库重启复原
- [x] 滚动长截图(docs/19):S1–S6 全部落地(核心拼接算法 / FFI 会话 / 遮罩长截图态 /
      真实采集与滚动 / 浮条入口与入库 / 设置与热键);验收 `--stitch-flow`、`--stitch-settings-flow`、
      `--stitch-live-flow` 全通过
- [x] OCR:RapidOCR 引擎内化(自带模型、中英混排)+ 系统内置双后端可选;
      截图取字 / 条目识别 / 截图翻译三入口;引擎档位与驻留可配
- [x] 数据备份 / 恢复 / 导入(zip)+ 导出(Markdown / JSON / CSV)
- [x] S6 内存收口:隐藏态稳态约 130MB(第一代 300–500MB),默认软件渲染;详见 docs/06
- [ ] S6 收尾:发布渠道与签名(自动更新按钮据此空置)、安装冒烟与性能验收留痕(docs/03 S6.5–S6.7)
- [ ] 二维码识别:尚未实现

## 快速开始

**一键构建**(推荐,自动完成 core → app → 部署):

```powershell
cd D:\3project\rust\Nextboard
powershell -ExecutionPolicy Bypass -File build.ps1            # 常规构建
powershell -ExecutionPolicy Bypass -File build.ps1 -Test      # 顺带跑 core 单测
powershell -ExecutionPolicy Bypass -File build.ps1 -Run       # 构建后直接启动
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean     # 清缓存重新配置
```

**打包发布产物**(构建后执行,产出便携 zip + NSIS 安装包):

```powershell
powershell -ExecutionPolicy Bypass -File tools\package.ps1    # dist\Nextboard-Setup-0.2.0.exe / -portable.zip
```

产物:`app\build\nextboard.exe`(nbcore.dll / Qt 运行库 / OCR 模型与 onnxruntime.dll
由脚本与 CMake 自动部署;QML 已打进 exe)。
热键(均可在设置里改):`Ctrl+Shift+V` 面板 / `Ctrl+Shift+A` 框选 / `Ctrl+Shift+Q` 全屏 /
`Ctrl+Shift+P` 贴图 / `Ctrl+Shift+L` 滚动长截图 / 另有截图取字、截图翻译、延时截图。
数据/日志/配置:`app\build\data\`(便携式布局,exe 同级 `data` 目录 —— 数据库、截图 blob、
`app_config.json`、`nextboard.log`、`nbcore.log` 都在这里)。
打包/安装/发布规程见 [docs/04-build-package-release.md](./docs/04-build-package-release.md)。

### 验收 CLI(全部为真机交互脚本,不是单测)

`nextboard.exe` 支持一组以 `--xxx-flow` 命名的自检入口,跑完打印断言行并退出非零码。
它们走**真实点击/真实按键**(经 QQuickWidget 投递),能抓到只有交互才暴露的缺陷:

| 入口 | 验什么 |
| --- | --- |
| `--todo-flow` / `--todo-groups` / `--todo-layout-flow` | 待办新建卡片、分组分界、字段顺序 |
| `--note-flow` | 桌面便签贴边吸附 / 半隐藏 / 几何复原 |
| `--capture-toolbar-flow` | 截图浮条按几何真点、标注笔画落点、长截图面板 |
| `--editor-flow` | 编辑器裁剪/旋转:引擎级变换对位 + 真窗口键鼠端到端 |
| `--stitch-flow` / `--stitch-settings-flow` / `--stitch-live-flow` | 长截图会话、设置读写比、真机滚动拼接 |
| `--settings-flow` / `--restore-flow` / `--clear-done-flow` / `--clear-all-flow` | 设置抽屉、备份恢复/导入、批量删除确认 |
| `--update-flow` / `--autostart-flow` | 更新检查(配 `--update-url`)、开机自启注册表 |
| `--screenshot <png>` / `--query <词>` / `--translate <文本>` / `--settings` | 自截面板到指定 PNG,便于人工比对 |
| `--dump-monitors` / `--hidden` / `--software` / `--hardware` / `--data-dir <dir>` | 环境诊断与运行期开关 |

### 手动分步(与脚本等价)

**core(nbcore.dll,Rust)**:

```powershell
cd core
cargo test              # 350 通过 / 1 既有失败(translate::http 的 socks5 用例,需外网)/ 5 ignored
cargo build --release   # 产出 target\release\nbcore.dll(约 4.9MB)+ 导入库 nbcore.dll.lib
```

**app(nextboard.exe,C++/Qt,需 MSVC 环境——开始菜单的 "x64 Native Tools Command Prompt for VS 2022")**:

```bat
cd /d D:\3project\rust\Nextboard\app\build
D:\Qt\Tools\CMake_64\bin\cmake.exe -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ..
D:\Qt\Tools\Ninja\ninja.exe
D:\Qt\6.9.0\msvc2022_64\bin\windeployqt.exe --no-translations nextboard.exe   :: 仅首次
nextboard.exe
```

**core 冒烟宿主(可选,FFI 链路验收)**:把 `core\target\release\nbcore.dll` 拷到
`tools\smoke_host\` 后,在该目录执行
`cl /nologo /std:c++20 /EHsc /utf-8 smoke_host.cpp /link /LIBPATH:..\..\core\target\release nbcore.dll.lib && smoke_host.exe`。
