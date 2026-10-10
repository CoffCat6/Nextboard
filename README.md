# Nextboard

Windows 桌面一体化效率板:剪贴板历史 + 截图(框选/全屏/编辑/贴图)+ 翻译。
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
| [docs/04-build-package-release.md](./docs/04-build-package-release.md) | 构建/打包/安装/发布规程:发布构建清单、运行期依赖与体积预算、NSIS 安装器规格、发布流程与检查单(**草案,待审查**) |

## 上游项目

clipboard-plus:`D:/3project/rust/clipboard-plus`(只读参考基线;其设计文档
docs/design.md、phase5–11、memory-optimization 中的真机踩坑结论全部视为本项目
的既定约束)。

## 状态

- [x] 技术选型与架构定稿
- [x] 改写路线与开发路线文档
- [x] S0:工具链验证(Rust 1.98 msvc / VS2022 MSVC 14.44 / Qt 6.9.0 msvc2022_64 / CMake 4.0.1)、
      基线 tag `nextboard-baseline-20261005`、Sciter SDK 就位(D:/4Lib/sciter-js-sdk-main)
- [x] S1:nbcore.dll 完成(160 单测全绿 + C++ 冒烟宿主 11/11 PASS)
- [x] S2:Qt 宿主骨架 + Sciter 嵌入/遮罩/贴图 PoC(可运行,热键/托盘/单实例/日志齐)
- [x] S3:主面板数据驱动(桥/列表/分类计数/设置抽屉/翻译抽屉;搜索逻辑由 core 单测覆盖)
- [x] S3 收尾:滚动转发(卡片显式转交滚轮给列表)、右键菜单(复制/翻译/识别文字/固定/删除)、
      loadMore(分页 total 收敛);面板交互闭环补齐(ESC 收起、复制后收起、Ctrl+1..9 序号复制、
      toast/译文信号接通、分类计数随数据刷新)
- [x] S4:编辑器(QGraphicsScene)+ 截图/贴图闭环 —— 实现完成,待真机回归
      (4.1 编辑器、4.2 两种框选确认模式与直通路径、4.3 全屏与一键贴图、
       4.4 截图翻译/取字)
- [ ] S5–S6:见路线图(一体化素材库 → 内存收口 → 发布)

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

产物:`app\build\nextboard.exe`(nbcore.dll / Qt 运行库由脚本与 CMake 自动部署;QML 已打进 exe)。
热键:`Ctrl+Shift+V` 面板 / `Ctrl+Shift+A` 框选 / `Ctrl+Shift+Q` 全屏 / `Ctrl+Shift+P` 贴图。
数据/日志/配置:`app\build\data\`(便携式布局,exe 同级 `data` 目录 —— 数据库、截图 blob、
`app_config.json`、`nextboard.log`、`nbcore.log` 都在这里)。
打包/安装/发布规程见 [docs/04-build-package-release.md](./docs/04-build-package-release.md)。

### 手动分步(与脚本等价)

**core(nbcore.dll,Rust)**:

```powershell
cd core
cargo test              # 160 个单测
cargo build --release   # 产出 target\release\nbcore.dll(约 3.6MB)+ 导入库 nbcore.dll.lib
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
