# Nextboard 构建、打包、安装与发布

| 项 | 内容 |
| --- | --- |
| 版本 | v1.1 |
| 日期 | 2026-10-09 |
| 状态 | 打包与安装器**已实现并实测**(`tools/package.ps1` + `tools/installer.nsi`,见 §10);§8 决策按推荐项落地,待用户最终确认 |
| 关联 | [03-roadmap.md](./03-roadmap.md) S6.4/S6.7 · [01-architecture.md](./01-architecture.md) · [progress.md](./progress.md) |

---

## 1. 目的与范围

把路线图 S6.4(打包)、S6.7(发布检查单)从"事项"落成**可执行规程**。覆盖四段:

```
源码 ──构建──▶ 可运行产物 ──打包──▶ 分发包 ──安装──▶ 用户机器 ──发布──▶ 渠道/更新
```

| 章节 | 内容 |
| --- | --- |
| §2–3 | 构建:现状盘点 + 发布构建附加规程(与日常构建的差异、已知坑) |
| §4–5 | 打包:运行期依赖白名单、VC++ 运行库、体积预算、产物形态、安装器规格 |
| §6 | 安装:安装器行为、升级、卸载与数据保留 |
| §7–9 | 发布:版本规范、流程清单、更新机制、发布检查单、合规、待拍板事项 |

**不在范围**:自动更新服务端、多语言安装器、MSI/WiX、应用商店上架、代码签名证书采购流程(仅列为前置项)。

---

## 2. 当前基线(事实盘点)

> 本章全部为实测/代码核对结论,是后续所有设计的地基;数字来自 2026-10-09 的 `app/build`。

### 2.1 构建链路(build.ps1 四步)

| 步骤 | 命令 | 产物 |
| --- | --- | --- |
| 1. core | `cargo build --release`(core/ 目录) | `core/target/release/nbcore.dll`(实测 **3.75MB**,strip+lto)+ 导入库 `nbcore.dll.lib` |
| 2. app | CMake(`-G Ninja -DCMAKE_BUILD_TYPE=Release`)+ ninja,经 vcvars64 | `app/build/nextboard.exe`(实测 **1.54MB**);POST_BUILD 自动拷贝 nbcore.dll |
| 3. Qt 部署 | `windeployqt --no-translations --no-system-d3d-compiler --no-opengl-sw` | Qt6 DLL + `platforms/qwindows.dll` + `qml/` + `imageformats/` 等 |
| 4. 完整性修复 | `icacls ... /setintegritylevel Medium` | 托盘图标可见的前提(沙箱继承 Low 标签的规避) |

- 单测:`cargo test` **184 项**(progress.md 2026-10-09 OCR 轮数据)。
- QML 经 `qt_add_qml_module` 打进 qrc,**无运行期资源目录依赖**;应用图标双通道(qrc + rc 编入 PE)。
- 手动分步等价命令见 [README.md](../README.md) §快速开始。

### 2.2 版本号(单一来源)

| 组件 | 位置 | 当前值 | 去向 |
| --- | --- | --- | --- |
| app | `app/CMakeLists.txt` `project(Nextboard VERSION 0.2.0)` | **0.2.0** | `NB_APP_VERSION` 宏 → 设置页「关于」`appVersion` |
| core | `core/Cargo.toml` `workspace.package version` | **0.1.0** | `nb_version()` → 设置页「关于」`coreVersion` |

打包/安装器/发布说明里的版本号必须与这两处一致(§7.1 定义改动点)。

### 2.3 运行期依赖(实测,`app/build`)

必选(windeployqt 部署集):

| 文件/目录 | 实测大小 | 说明 |
| --- | --- | --- |
| `nextboard.exe` | 1.54MB | 含 PE 图标 + PerMonitorV2 manifest(见 [app.manifest](../app/app.manifest)) |
| `nbcore.dll` | 3.75MB | 显式加载(LoadLibrary),ABI v1 |
| `Qt6Core/Gui/Widgets/Qml/QmlModels/Quick/QuickTemplates2/QuickControls2*/QuickLayouts/QuickShapes/QuickWidgets/Network/OpenGL/Svg/...dll` | 合计 ~48MB | windeployqt 按依赖决定,不手列 |
| `platforms/qwindows.dll` | ~0.9MB | 缺它启动即报平台插件缺失 |
| `qml/`(QtQuick、QtQuick/Controls/Basic、QtQuick/Layouts) | 1.5MB | QML 面板运行库 |
| `imageformats/qico.dll` | ~0.05MB | 应用图标是 `.ico`,缺它窗口/托盘图标静默变空白 |
| `styles/ iconengines/ tls/ networkinformation/ platforminputcontexts/ generic/` | 合计 ~0.8MB | windeployqt 部署的辅助插件,保留 |

**运行期子集合计:56.1MB / 147 个文件**(exe + nbcore + 顶层 Qt6 DLL + 上述目录),满足路线图 S6.4「包体 <60MB」目标。

已核实的事实:

- **PNG 缩略图不依赖 imageformats 插件**:部署集里没有 `qpng.dll` 而 `.thumb640.png` 正常显示 → PNG 已内置 Qt6Gui;ico 才需要插件。
- `qmltooling/`(1.0MB)是 QML 调试工具,**运行期不需要**。
- VC++ 运行库不在部署集里(windeployqt 不管 CRT),见 §4.2 —— 这是打包的**第一必答项**。

### 2.4 数据与配置位置(便携式布局,2026-10-09 起)

**数据根目录 = `<nextboard.exe 同级>\data\`**(`app/src/host/app_paths.hpp` 是唯一来源)。

| 路径(相对 exe) | 内容 |
| --- | --- |
| `data\` | 数据根目录(exe 同级;`app_paths.cpp appDataDir()`) |
| `data\app_config.json` | 全部设置(core `settings.rs` `CONFIG_FILE`) |
| `data\clipboard.db` + `-wal` + `-shm` | SQLite(v5 schema) |
| `data\blobs\<YYYY-MM>\*.png`、`*.thumb640.png` | 截图/图片 blob 与缩略图 |
| `data\nextboard.log` | 宿主侧文件日志 + 崩溃兜底日志(`main.cpp`) |
| `data\nbcore.log` | Rust 侧文件日志(core `init_logging`) |
| `data\data-move-pending.txt` | 自定义数据目录迁移标记(重启时搬迁,源目录保留) |

- **便携性**:整个安装目录可直接拷贝迁移(数据跟着走);便携版 zip 解压即用。
- **前提**:exe 所在目录必须**可写** → 安装位置默认 `%LOCALAPPDATA%\Programs\Nextboard`
  (单用户免 UAC);装到 Program Files 等只读位置会失败,启动时给出可读错误(见 §2.5)。
- 自定义数据目录:设置页仍可改绝对路径、**重启生效**、复制式迁移(源保留可回退)。
- **迁移提示**:从旧默认位置(`%LOCALAPPDATA%\Nextboard\Nextboard`)升级的用户,
  历史数据不会自动搬移 —— 手工把该目录内容拷进新的 `data\` 即可(卸载器"删除个人数据"
  会一并清理旧位置)。卸载策略见 §5.3。

### 2.5 数据/日志目录可写性 —— 安装位置决策的关键约束

宿主日志与数据**同在 exe 同级 `data\`**:`data\nextboard.log`(含崩溃兜底日志)、
`data\nbcore.log`。

推论:**exe 目录不可写 → 日志静默全丢、数据目录创建失败**(安装目录是只读位置时)。
因此打包链路做两件事:

1. 安装器默认装到 `%LOCALAPPDATA%\Programs\Nextboard`(单用户,可写;§5.3 D1-a);
2. `core_bridge` 在 `nb_init` 前显式建目录,**失败立即返回可读错误**
   (`数据目录不可写:<路径>(安装目录需可写)`),不再以"核心初始化失败"含混暴露。

### 2.6 自启与单实例

| 项 | 现状 | 备注 |
| --- | --- | --- |
| 开机自启 | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 值 `Nextboard` = `"<exe 路径>" --hidden`(`auto_start.cpp` `commandLine()`) | 应用内设置开关与安装器(`installer.nsi` SecAutoStart)**写同一形态**(D11,2026-10-17 统一);开机静默驻留托盘、不弹面板 |
| 单实例 | 命名互斥量 `NextboardSingleInstance`(`main.cpp:88`) | 第二实例激活既有窗口后退出 |

---

## 3. 构建(Build)

### 3.1 环境要求(本机实测版本)

| 工具 | 版本/路径 | 说明 |
| --- | --- | --- |
| Rust | 1.98.1 `x86_64-pc-windows-msvc` | core 构建 |
| MSVC | VS2022 Community,MSVC 14.44 | app 构建;`vcvars64.bat` 在 VS 默认路径 |
| Qt | 6.9.0 `msvc2022_64`(`D:\Qt\6.9.0\msvc2022_64`) | 禁 MinGW 套件(架构 R4) |
| CMake / Ninja | Qt 自带(`D:\Qt\Tools\...`) | build.ps1 头部路径 |
| Windows SDK | **`D:\Windows Kits\10`(非默认路径)** | 沙箱内 `vcvars64` 依赖 `reg.exe` 读 `KitsRoot10` 被拦 → 手工补 INCLUDE/LIB/PATH(progress.md 已记录) |

### 3.2 发布构建规程(与日常构建的差异)

日常 `build.ps1` 即可增量构建;**发布必须**执行:

| # | 动作 | 通过标准 |
| --- | --- | --- |
| 1 | git 工作区干净,先提交 | `git status` 空 |
| 2 | `build.ps1 -Clean -Test` | 干净配置 + **184 单测全绿** |
| 3 | 核对版本号(app/core 两处,§7.1) | 与计划发布号一致 |
| 4 | 确认无 `nextboard.exe` 残留进程 | 无 LNK1104 |
| 5 | 构建产物验收 | §4.1 必选项逐项存在;`nb_abi_version()=1`;exe 内版本串 = 计划版本;构建零警告 |
| 6 | 记录构建环境快照(工具链四件套版本) | 写入发布说明 |

### 3.3 已知坑与规避(构建期)

| 坑 | 现象 | 规避 |
| --- | --- | --- |
| Windows SDK 非默认路径 + 沙箱拦 `reg.exe` | `vcvars64` 定位 SDK 失败,`C1083 stddef.h` | 发布机手工设 `INCLUDE/LIB/PATH` 指向 MSVC 14.44 + `D:\Windows Kits\10`(progress.md 已有先例) |
| LNK1104 | `nextboard.exe` 被旧进程占用 | 构建前 `Stop-Process -Force nextboard` |
| `build.ps1` 需 **BOM UTF-8** | PowerShell 5.1 解析中文注释崩 | 编辑时保留 BOM |
| 构建目录继承 Low 完整性标签 | 托盘图标被 UIPI 拒,静默不显示 | `icacls ... Medium`(build.ps1 第 4 步已含;**打包 stage 拷贝后建议复核一次**) |
| 沙箱不能启动 GUI | 运行期验收受限 | 所有运行期结论以真机/干净环境为准(本文档所有"需真机确认"同理) |

---

## 4. 运行期依赖与体积预算(打包的事实基础)

### 4.1 打包文件清单(三类)

| 类别 | 内容 | 处置 |
| --- | --- | --- |
| **必选** | exe、nbcore.dll、windeployqt 部署的 Qt6 DLL、`platforms/`、`qml/`、`imageformats/qico.dll`、`styles/ iconengines/ tls/ networkinformation/ platforminputcontexts/ generic/` | 全量进包 |
| **条件** | 其余 `imageformats/*` 格式插件(gif/jpeg/webp/tiff/…) | 当前 UI 只用 ico + PNG(内置);**默认只留 `qico.dll`**,其余列为可选开关(裁剪前需真机确认无回归) |
| **可删** | `qmltooling/`(1.0MB 调试工具)、`nextboard.log`、`*.lib/*.exp`、`.rcc/ CMakeFiles/ nextboard_autogen/ meta_types/ qmltypes/ generic` 构建残留 | 不进包 |

### 4.2 VC++ 运行库(必须决策,当前**未解决**)

**事实**:MSVC 默认动态链接 CRT。`nextboard.exe` 与 `nbcore.dll`(Rust `*-pc-windows-msvc` 目标)都依赖 `vcruntime140.dll` / `msvcp140.dll`(新版 CRT 可能还需要 `vcruntime140_1.dll`);`windeployqt` 不部署 CRT;本机 `app/build` 里也没有。**目标用户机(未装 VS/VC 运行库)缺这些 DLL → 启动即失败。**

| 方案 | 做法 | 体积 | 需要 UAC | 评价 |
| --- | --- | --- | --- | --- |
| **A. app-local CRT(推荐)** | 打包时把 3 个 CRT DLL 拷进安装目录(源:VS 的 `VC\Redist\MSVC\<ver>\...` 或已装 CRT 的 System32) | +0.7MB | 否 | 微软许可允许应用本地分发 CRT;最可靠、零外部依赖 |
| B. 静默 vcredist | 安装器内嵌 `vcredist_x64.exe`, `/install /quiet /norestart` | +25MB 或联网下载 | 取决于安装位置 | 体积/网络依赖;与 A 二选一即可 |
| C. 静态 CRT(`/MT`) | CMake 对 app 目标加 `/MT` 重编 | 0 | 否 | **只解决一半**:Rust cdylib 无法静态 MSVC CRT,治标不治本 |

**发布前必须复核**(固化进打包脚本校验):扫描 exe/dll 导入表,列出 CRT 依赖的精确集合(建议 `dumpbin /dependents`;无 dumpbin 时用 PowerShell 读 PE 导入表),缺一即报错,防止"开发机恰好有 CRT"造成的假通过。

### 4.3 体积预算

- 实测运行期子集 **56.1MB / 147 文件** ≤ 路线图 60MB 目标(未压缩);加 app-local CRT 后 ~56.8MB,仍达标。
- 可选瘦身(按 收益/风险 排序):`qmltooling/` −1.0MB(零风险);`imageformats` 裁剪至 qico −1.5MB(需真机确认);Qt6VirtualKeyboard/OpenGL/Svg 等"看着没用"的 DLL —— windeployqt 是按依赖部署的,**先验证再删**,不默认裁。
- NSIS(LZMA 固体压缩)安装包预计 20MB 量级;便携 zip 约 55MB(zip 对已压缩的 DLL 收益有限)。

---

## 5. 打包(Package)

### 5.1 产物形态

| 产物 | 文件名 | 用途 |
| --- | --- | --- |
| 安装包(主) | `Nextboard-Setup-<版本>.exe` | 普通用户默认渠道 |
| 便携版(辅) | `Nextboard-<版本>-portable.zip` | 免安装/绿色版;解压即用。因数据在 exe 同级 `data\`(§2.4),便携版**自包含**:解压到 U 盘/任意目录,数据与安装版互不干扰,整目录拷走即完成迁移 |

### 5.2 打包流程(已实现:`tools/package.ps1`)

```
1. 前置检查:app/build 来自 §3.2 发布构建(版本串/ABI/单测记录)
2. 建 stage 目录 dist/Nextboard-<版本>/
3. 按 §4.1 白名单拷贝(必选 + qico;imageformats 裁剪为开关)
4. 补齐 app-local CRT(§4.2 方案 A)
5. 瘦身:白名单天然排除 qmltooling / *.lib / *.exp / log
6. 校验:禁入项断言 + 必选项存在性 + 体积预算(<60MB)
7. 产出:portable.zip;调 makensis 编 Setup.exe
8. **产物完整性标签**:dist 目录 + zip/setup 显式置 Medium 并复核(否则双击安装报
   "Error writing temporary file" —— 见 §11)
9. 输出 dist/ 摘要(体积、文件数、SHA256 清单),供发布说明引用
```

用法:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package.ps1                  # 版本从 CMakeLists 解析
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.2.0   # 显式指定版本
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -KeepAllImageFormats  # 保留全部图片格式插件
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -SkipInstaller   # 只出便携版
```

**实测踩坑(已固化进脚本,2026-10-09)**:

| 坑 | 现象 | 规避 |
| --- | --- | --- |
| `%TEMP%` 不可写(沙箱/受限会话) | makensis 报 `Internal compiler error #12345: error creating mmap`,与脚本无关 | 调 makensis 前把 `TMP/TEMP` 重定向到 `dist\tmp`(用完清理) |
| makensis 相对路径按 **CWD** 解析 | nsi 内 `..\dist\...` 从仓库根运行会找不到 stage | 调用前 `Push-Location tools\`(脚本内已处理) |
| icacls 修完整性标签被拦 | 沙箱拦 WRITE_DAC,`$ErrorActionPreference='Stop'` 把 stderr 升级成终止错误 | `cmd /c` 包裹 + 失败降级为告警(真机与 build.ps1 同样成功) |

路径配置(NsisBin / CrtSource)在两个脚本各自定义,暂未提取 `tools/paths.ps1`;后续合并时以本节为准。

### 5.3 安装器规格(已实现:`tools/installer.nsi`,NSIS MUI2)

**安装**

| 项 | 规格 |
| --- | --- |
| 安装位置 | 默认 `%LOCALAPPDATA%\Programs\Nextboard`(**单用户、免 UAC、数据/日志目录可写**,呼应 §2.5);允许自定义 |
| 运行中检测 | 安装/升级前 `taskkill /F` 结束旧实例(托盘程序无未保存状态,静默结束;未运行时空操作) |
| 快捷方式 | 开始菜单(默认装,含卸载入口);桌面(可选,**默认不勾**) |
| 开机自启 | 可选组件(**默认不勾**);写入 HKCU Run 时**带 `--hidden`**,修 §2.6 的开机弹面板问题 |
| 文件关联 | 不做 |
| 卸载项(ARP) | 注册显示图标/显示名/显示版本/发布者/EstimatedSize,指向卸载程序 |
| 图标 | 安装器图标用 `app/resources/nextboard.ico`(与 exe 同源) |

**卸载**

- 删安装目录**除 `data\` 外的全部内容**、开始菜单/桌面快捷方式、HKCU Run 值(若存在)。
  ⚠️ 数据在 `$INSTDIR\data`,不能 `RMDir /r "$INSTDIR"`:卸载器**逐项遍历顶层条目并跳过 `data`**
  (Windows 无"排除删除";遍历写法对以后新增的目录/文件自动覆盖)。
- **个人数据默认保留**(`<安装目录>\data\`:历史、截图 blobs、设置、日志)。
  "同时删除个人数据"是**卸载器组件页的一个可选 Section**(`Section /o`,默认不选):
  交互卸载时用户可勾选;静默卸载(`/S`)跳过组件页且该节不选 → **数据必保留**,天然防误删。
  勾选时同时清理旧默认位置 `%LOCALAPPDATA%\Nextboard\Nextboard`(0.2.0 早期版本遗留)。

> ⚠️ **Section 命名即语义(踩过坑,后果严重)**:NSIS 只把**精确名 `Uninstall`** 或
> **`un.` 前缀**的 Section 编进卸载器,其余一律算"安装段"、会在**安装时**执行。
> 曾经把卸载段写成 `Section "!卸载 Nextboard"` → 安装时自我删除(装完什么都不剩),
> 且生成的卸载器 **0 个 section**(什么都不做)。
> **自查点**:makensis 汇总行的 `Uninstall: N sections` 必须 >0(本项目为 2)。
> 卸载器主段另加 `SectionIn RO`(必选、组件页置灰、静默卸载也必跑)。

**静默/升级**

- 静默安装:`Nextboard-Setup.exe /S`;自定义目录:`/D=<路径>`(NSIS 规范,`/D` 须在末尾且不加引号)。
- 升级:同版本可覆盖安装(文件覆盖 + ARP 信息刷新);保留数据与设置。

---

## 6. 安装(Install)

用户流程(安装器默认选项):下一步 → 选位置(可跳过)→ 勾快捷方式/自启 → 安装 → 完成(可选"立即运行")。

**安装后冒烟清单(真机/干净环境,发布前必跑)**:

| # | 检查 | 通过标准 |
| --- | --- | --- |
| 1 | 启动 | 托盘图标出现(完整性标签 Medium);`nextboard.log` 无错误 |
| 2 | **数据落点** | 安装目录下出现 `data\`:`clipboard.db`、`app_config.json`、`nextboard.log`、`nbcore.log`(截图后另有 `blobs\`) |
| 3 | 热键 | Ctrl+Shift+V/A/Q/P 四主键全部响应 |
| 4 | 截图闭环 | 框选 → 入库 → 列表出现条目 → 可粘贴 |
| 5 | 关于页 | 版本号 = 发布版本(app 与 core 两行) |
| 6 | 只读位置防御 | 手工把安装目录设为不可写后启动 → 应弹**可读错误**(数据目录不可写),而非静默无日志 |
| 7 | 自启(若勾选) | HKCU Run 存在;重启后托盘出现且**不弹面板** |
| 8 | 卸载(默认) | 安装目录只剩 `data\`;快捷方式/Run 键/ARP 项清除;历史仍在 |
| 9 | 卸载(勾选删数据) | `data\` 被删除;旧 `%LOCALAPPDATA%\Nextboard\Nextboard`(若有)一并清理 |
| 10 | 重装 | 装回后历史数据仍在(验证"卸载保留数据"策略) |

> **2026-10-10 会话内实测**:第 **1、2、8** 项通过(启动无错误日志;数据落于安装目录
> `data\`;默认卸载后仅剩 `data\` 且用户数据完好),第 **3** 项的热键注册为 6/6。
> 第 **4**(截图闭环)、**5**(关于页版本)、**6**(只读位置防御)、**7**(自启重启后不弹面板 ——
> 注册表**形态**已由 `--autostart-flow` 覆盖,重启观感仍须真机)、
> **9**(勾选删数据的交互路径)、**10**(重装)仍须真机确认 —— 明细见 §10.1。

---

## 7. 发布(Release)

### 7.1 版本与 tag

- 语义化版本;当前 app **0.2.0** / core **0.1.0**。
- 版本改动点(单一来源):`app/CMakeLists.txt` 的 `project(... VERSION)`;core 按需动 `core/Cargo.toml`;git tag `vX.Y.Z`。
- 下一发布版号(0.3.0 vs 0.2.1 vs 1.0.0-rc)见 §8 D10。

### 7.2 发布流程清单

| # | 步骤 | 产出/留痕 |
| --- | --- | --- |
| 1 | §3.2 发布构建规程全过 | 构建日志/单测数 |
| 2 | 改版本号;同步 README 状态、docs(架构文档仍有 Sciter 表述,顺手修订,见 §7.5) | commit |
| 3 | commit(Conventional Commits) | commit hash |
| 4 | `git tag vX.Y.Z && git push --tags` | tag |
| 5 | `tools/package.ps1` | `dist/`(Setup + zip + SHA256) |
| 6 | 干净环境冒烟(§6 清单) | 冒烟记录 |
| 7 | 代码签名(§8 D6,未定则跳过并记录风险) | 签名状态 |
| 8 | `gh release create vX.Y.Z`(本机已装 GitHub CLI)上传 Setup + zip + 校验和 | Release 页 |
| 9 | 发布说明:变更摘要/已知问题/系统要求/构建环境 | Release notes |
| 10 | 更新检查源就绪(§7.3) | 可被「检查更新」发现 |

### 7.3 更新机制（v1 已实施;细节见 [07-auto-update.md](./07-auto-update.md)）

- **v1 做**:设置页「关于」区「检查更新」→ 语义化版本比较 → 有新版则提示,并用
  `QDesktopServices::openUrl` 打开下载页。⚠️ **实现落点是 nbcore 的 `check_update`,
  不是原先写的宿主 `QNetworkAccessManager`**(2026-10-15 改判,理由见 07 §3 与本文 §8 D12:
  宿主侧要重复实现一遍应用内代理三态才谈得上一致,而版本比较放 Rust 才有表驱动单测)。
- **v1 不做**:静默下载、差量升级、应用内安装;启动后台自动检查;托盘入口。
- **检查地址仍是空置**:`app/CMakeLists.txt` 的 `NB_UPDATE_CHECK_URL` 留空 = 功能禁用
  (入口可见但置灰并说明原因),渠道定了改这一行重编即启用 —— 07 §5。
- 后续(v2 设想):下载到临时目录 + SHA256 校验 + 引导用户运行安装包;前置是 D6 代码签名。
- 注意:Qt6Network.dll 已在部署集内(v1 走 core 的 WinHTTP,不额外依赖它);失败(断网/代理/
  限流/404)一律给可读中文错误。

### 7.4 发布检查单(路线图 6.7 展开)

| 组 | 检查项 |
| --- | --- |
| 核心功能 | 面板(列表/搜索/分类计数/标签)/截图(框选两模式、全屏、延时、编辑器)/贴图(上限、工具条、跨屏 DPI)/翻译(默认微软通道、自定义、历史)/OCR 三条手动链 |
| 环境 | 三屏不同缩放;中英文混排;明暗主题;代理;开机自启 |
| 安装专项 | §6 冒烟清单 7 项 |
| 性能 | 冷启动→托盘 <1s;热键→遮罩 <500ms;直通截图 <500ms(日志埋点实测) |
| 内存 | `mem[startup]` 等埋点与上一版对比(硬件/软件渲染两档) |

### 7.5 合规

| 项 | 要求 |
| --- | --- |
| Qt 6(LGPLv3) | 当前为动态链接,合规;**关于页/发布说明注明 "Qt 6.9.0(LGPLv3)"** 并给出 qt.io 许可链接;安装包不修改 Qt DLL |
| 第三方组件 | nbcore 侧:rusqlite(bundled SQLite)、blake3、ureq、windows/windows-rs、image 等,以 `core/Cargo.lock` 固化;发布说明附清单 |
| Sciter | UI 已迁 QML、**Sciter 依赖与 EULA 署名义务随之消失**;但 `docs/01-architecture.md`、README 仍有 Sciter 表述,发布前一并修订(避免文档与实物不符) |

---

## 8. 待拍板事项(审查重点)

| # | 事项 | 选项 | 推荐 | 影响 |
| --- | --- | --- | --- | --- |
| D1 | 安装位置 | a) `%LOCALAPPDATA%\Programs\Nextboard` 单用户免 UAC;b) Program Files 需管理员 | **a(已落地)** | 数据与日志都在 exe 同级 `data\`,故目录必须可写:b 需额外做只读回退,否则数据/日志双双失效 |
| D2 | VC++ 运行库 | a) app-local CRT 拷贝;b) 静默 vcredist;c) /MT 静态 | **a(已落地)** | 见 §4.2;c 不彻底 |
| D3 | 便携版 zip | 提供 / 不提供 | **提供(已落地)** | 便携性因"数据跟着 exe 走"而完整:解压即用、整目录可搬 |
| D4 | 卸载是否删数据 | 默认保留(勾选删)/ 默认删 | **默认保留(已落地)** | 数据在安装目录内,卸载**必须显式跳过 `data\`**(§5.3) |
| D5 | 安装器 | NSIS(已装)/ Inno Setup(需另装)/ WiX(需另装) | **NSIS(已落地)** | 本机零额外安装;脚本可入版本库 |
| D6 | 代码签名 | 有证书 / 暂不签 | **待用户确认** | 未签名 → SmartScreen 警告,影响安装转化;需要则补充 signtool 与时间戳服务器 |
| D7 | 发布渠道 | GitHub Releases(gh 已装)/ 其他 | **GitHub Releases** | 与 §7.3 更新检查直接配套 |
| D8 | 日志策略 | a) 保留文件日志,与数据同目录;b) 放 exe 根目录;c) 发布版移除 | **a(已落地)** | 日志落 `data\nextboard.log`(含崩溃兜底);与数据同处,便于整包取证;c 会丢失崩溃现场 |
| D9 | 更新机制范围 | v1 仅"检查更新+打开下载页"(§7.3) | **是(已落地 2026-10-15)** | 与路线图 6.4 一致;实现与实测见 [07-auto-update.md](./07-auto-update.md) |
| D12 | 更新检查的实现落点 | a) 宿主 `QNetworkAccessManager`(本文原 §7.3 写法);b) nbcore 新增 `check_update`,复用 `translate::http` | **b(已落地,2026-10-15 改判)** | 选 a 要在 C++ 重写一遍 system/manual/SOCKS5 的代理翻译与接线才能与翻译链路行为一致,而本仓无 C++ 测试设施;版本比较与响应解析放 Rust 才能表驱动单测 |
| D10 | 下一发布版号 | 0.2.1 / 0.3.0 / 1.0.0-rc | 待用户定 | 影响 tag、安装器显示、更新比较基准 |
| D11 | 自启是否带 `--hidden` | 安装器写 Run 时带 / 不带 | **带,且两处统一(已落地 2026-10-17)** | 修开机弹面板(§2.6)。应用内开关同步改:`auto_start.cpp` 的 `commandLine()` 固定追加 `--hidden`;老版本(≤0.2.0)留下的无 `--hidden` 值由启动时 `refreshCommandLineIfStale()` 就地升级(用户不必关了再开)。验收 `--autostart-flow` |

---

## 9. 验收标准(DoD)

1. 发布构建可按 §3.2 规程复现,产物通过 §4.1 清单校验(含 CRT 复核)。
2. `tools/package.ps1` 一条命令产出 Setup + portable.zip + 摘要(体积/文件数/SHA256)。
3. 安装包在**干净 Windows 10/11 x64**(无 VS、无 Qt)安装后,§6 冒烟 7 项全过。
4. 卸载/覆盖升级/数据保留行为符合 §5.3;卸载后重装数据仍在。
5. 包体(未压缩运行期子集)≤ 60MB(路线图 6.4)。
6. §7.2 发布清单可执行且每步有留痕(版本/tag/Release/校验和)。
7. §8 决策全部有结论并回填本文档(状态改"已定稿")。

---

## 附录 A:NSIS 脚本(已落地:`tools/installer.nsi`)

正式脚本即 `tools/installer.nsi`(MUI2 + LZMA 固体压缩 + SimpChinese),以下为实现要点
摘录,完整内容以文件为准:

```nsis
Unicode true
ManifestDPIAware true
XPStyle on
SetCompressor /SOLID lzma
RequestExecutionLevel user                       ; D1-a:单用户安装,不弹 UAC
InstallDir "$LOCALAPPDATA\Programs\Nextboard"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS                  ; 桌面/自启勾选
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS                ; 个人数据删除勾选
!insertmacro MUI_UNPAGE_INSTFILES

Section "!${APP_NAME}(必需)" SecCore
  SectionIn RO
  nsExec::Exec 'taskkill /IM "nextboard.exe" /F'  ; 运行中检测
  SetOutPath "$INSTDIR"
  ; ⚠️ 必须用 * 不能用 *.*:qml 目录下有 12 个无扩展名 qmldir 文件,
  ;    *.* 会漏掉 → 运行期 "module is not installed"(已实测验证,见 §10)
  File /r "${SRC_DIR}\*"
  WriteUninstaller "$INSTDIR\uninstaller.exe"
  WriteRegStr HKCU "${REG_UNINST}" "DisplayName" "${APP_NAME} ${APP_VERSION}"
  ; ...DisplayVersion / Publisher / DisplayIcon / InstallLocation / UninstallString
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2          ; EstimatedSize(KB)供 ARP 显示
  WriteRegDWORD HKCU "${REG_UNINST}" "EstimatedSize" "$0"
SectionEnd

Section "开始菜单快捷方式" SecStartMenu   ; SectionIn 1 2(默认装)
Section "桌面快捷方式"       SecDesktop    ; .onInit 里置 0:默认不勾
Section "开机自动启动"       SecAutoStart  ; Run 键值带 --hidden(D11);默认不勾

Section "!卸载 ${APP_NAME}" SecUninstall
  ...删快捷方式/Run 值/卸载项 + RMDir /r "$INSTDIR"
SectionEnd

Section /o "同时删除个人数据(历史记录/截图/设置)" SecData   ; D4:默认不选,数据保留
  RMDir /r "$LOCALAPPDATA\Nextboard\Nextboard"
SectionEnd
```

覆盖/命名宏:`APP_VERSION`、`SRC_DIR`(stage 目录)、`OUT_FILE` 均可由 `makensis /D...`
覆盖(CI 与验证用)。

## 附录 B:打包校验命令

```powershell
# CRT 依赖复核(§4.2):当前策略 = 四个 CRT 全部拷贝(System32 存在性已在前置检查断言);
# 更严格的做法是扫描 stage 全部 PE 的导入表,缺任一即失败(发布机有 dumpbin 时:
#   dumpbin /dependents nextboard.exe)

# 文件清单比对(等价于 §10 的 /V4 日志法,可入 CI):
#   makensis /V4 tools\installer.nsi > compile.log
#   日志 "File: "<name>" <size> bytes" 行与 stage 文件数应一致(143)
```

---

## 10. 打包实现与实测记录(2026-10-09)

**工具链**:NSIS 3.x(Unicode,makensis 32 位)+ MUI2 + LZMA;`package.ps1` 全流程自动。

| 项 | 实测值 |
| --- | --- |
| stage(运行期子集) | 143 文件 / 55.2 MB(预算 60MB 内) |
| `Nextboard-0.2.0-portable.zip` | 22.1 MB |
| `Nextboard-Setup-0.2.0.exe` | 15.9 MB(58.3MB 数据 LZMA → 16.3MB,28.5%) |
| SHA256 清单 | `dist\Nextboard-0.2.0-SHA256.txt`(zip + setup 双录) |

**安装器文件清单验证**(`makensis /V4` 日志 vs stage 逐一比对):

- 日志 `File:` 条目 **143 = stage 143**,多重集完全一致;
- 无扩展名的 `qmldir` **12 = 12**(证实 `File /r ...\*` 必须用 `*`;`*.*` 会漏);
- 关键文件全部在内:`nextboard.exe` / `nbcore.dll` / `qwindows.dll` / `qico.dll` /
  `vcruntime140.dll` / `vcruntime140_1.dll` / `msvcp140.dll` / `msvcp140_1.dll`。

**当时未完成的验证 —— 已于 2026-10-10 补齐(见下)**:

- ~~静默安装挂起无产出~~ → 根因是**产物带 Low 完整性标签**(§11),改标签后安装器可正常运行;
- ~~icacls 被沙箱拦~~ → 新策略下可写,`package.ps1` 的标签步骤已稳定生效。

### 10.1 安装/卸载端到端实测(2026-10-10,补齐 §6 冒烟)

| 步骤 | 结果 |
| --- | --- |
| 静默安装最终产物 | exit 0;**144 文件**;**qmldir 12**;`vcruntime140.dll` 在;ARP/开始菜单/uninstaller 齐 |
| 安装不误删已有文件 | 预置 `keepme.txt` 安装后仍在 ✔ |
| 装好的副本可运行 | `--screenshot` 出图;日志 `data dir = ...\Programs\Nextboard\data` |
| 静默卸载 | exit 0;**仅剩 `data\`**;`clipboard.db`/用户标记保留;exe/Qt/uninstaller/ARP/开始菜单/Run 全清 |
| 编译汇总行 | `Install: 4 sections (1 required)` / `Uninstall: 2 sections (1 required)` |

> 修复前的汇总行是 `Uninstall: 0 sections` —— 卸载段因命名不合格被归入安装段,
> 导致"安装时自我删除、卸载器什么都不做"。详见 §5.3 的命名说明与 progress.md。

---

## 11. 常见问题(打包 / 安装)

### 11.1 双击安装包报 `Error writing temporary file. Make sure your temp folder is valid.`

**不是安装包损坏,而是"安装包文件自身的完整性标签"问题。**

- **机制**:Windows 按**映像文件自身的完整性标签**决定新进程的完整性级别。若
  `Nextboard-Setup-*.exe` 带 **Low** 标签(在本工作区里打包就会如此 —— 沙箱把工作区标成 Low,
  `dist` 下新建文件继承),安装器就会以 **Low 完整性**启动,而 `%TEMP%` 属于 Medium →
  "禁止向上写" → NSIS 建不出 `$PLUGINSDIR` → 报这句话。便携版/构建产物同理:
  **任何从工作区内启动的 exe 都可能被降级运行**。
- **判定**:`icacls "dist\Nextboard-Setup-0.2.0.exe"` 看 `Mandatory Label` 是否为 `Low`。
- **处置(任选)**:
  1. 重新打包 —— `package.ps1` 结尾会自动修复并复核,输出
     `[OK] 产物完整性级别 = Medium(双击可直接运行)`;
  2. 手工:`icacls "Nextboard-Setup-0.2.0.exe" /setintegritylevel Medium`;
  3. 应急:把安装包**复制到工作区之外**的普通目录(桌面/下载)再运行 ——
     副本继承目标目录的标签,即为 Medium。
- **预防**:凡"构建/打包产出可执行文件"的流程,结尾都应复核标签(本项目已固化在
  `build.ps1` 第 4 步与 `package.ps1` 第 8 步)。

### 11.2 安装"成功"却什么都没装 / 卸载不生效

先看 makensis 汇总行的 **`Uninstall: N sections`**:若为 `0`,说明卸载段命名不合格
(必须精确名 `Uninstall` 或 `un.` 前缀),它们已被当成安装段 —— 详见 §5.3。
