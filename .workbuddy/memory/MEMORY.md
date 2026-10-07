# Nextboard 项目长期记忆

## 构建(沙箱/无 vcvars 环境)

`build.ps1` **在沙箱里不可用且不可绕过**:它调用 `vcvars64.bat`,后者依赖 `reg.exe`
读注册表定位 Windows SDK,而 `reg.exe` 在程序黑名单中(即使关闭沙箱也被拦)。
沙箱内的等价做法是手工注入 MSVC + SDK 环境后直接跑 ninja(编译器/链接器在
build.ninja 里已是绝对路径,只需 INCLUDE / LIB):

```bash
MSVC="D:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207"
SDK="D:/Windows Kits/10"; SDKVER="10.0.26100.0"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKVER/ucrt;$SDK/Include/$SDKVER/um;$SDK/Include/$SDKVER/shared;$SDK/Include/$SDKVER/winrt"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKVER/ucrt/x64;$SDK/Lib/$SDKVER/um/x64"
cd D:/3project/rust/Nextboard/app/build && "D:/Qt/Tools/Ninja/ninja.exe"
```

- core:`cd core && cargo build --release`(cargo 可直接用)。
- 构建前若 `nextboard.exe` 被残留进程占用会报 `LNK1104`;沙箱里用 PowerShell 的
  `Stop-Process -Force` 清理(`taskkill` 被拒)。
- **GUI 进程在沙箱内一律无法启动**:`nextboard.exe`(GUI 子系统)与任何链接了
  Qt6Gui 的控制台程序都会在 exec 阶段直接返回 127。**只有纯 Qt6Qml +
  QCoreApplication 的控制台程序能跑** —— 需要验证 QML 引擎语义时,按这个形态写探针。

## 运行时排障入口

`app/build/nextboard.log` 是**追加式**的,含多次运行。定位当前问题:
先 `grep -an "Nextboard starting"` 找最后一次运行的行号,只看该行之后的记录。
QML 加载错误、绑定失败(`Unable to assign`)、JS 异常(`TypeError` / `ReferenceError`)
都会落在这里,是排查界面问题最快的一手证据(不要靠猜)。

## QML 静态校验(qmllint)

`Nextboard.Core` 由 C++ `qmlRegisterSingletonInstance` 注册,没有 `.qmltypes`,
qmllint 解析不了该模块就跳过语义检查 → 必须注入桩模块:

```
D:/tmp/nbqmlstub/Nextboard/Core/{qmldir,Core.qml,Entries.qml}
qmllint -I D:/tmp/nbqmlstub -I app/build -I D:/Qt/6.9.0/msvc2022_64/qml app/panel/qml/*.qml
```

遗留误报(可忽略):Theme.qml 的匿名 `QtObject` 成员导致 `[missing-property]` /
`[unqualified]` 大面积刷屏;`qmllint` **通过 ≠ 运行时可用**(它从 Qt 安装解析 import,
运行时用的是 `app/build/qml` 里已部署的模块)。每新增一个 Qt 模块 import,都要跑一次:

```bash
for m in $(grep -rhoE "^import Qt[A-Za-z.]*" app/panel/qml/*.qml | sed 's/^import //' | sort -u); do
  p="app/build/qml/$(echo "$m" | tr '.' '/')"; [ -f "$p/qmldir" ] && echo "OK $m" || echo "MISS $m"
done
```

## UI 约定

- **QML 单例必须跨引擎可用**:`Core` / `Entries` 用
  `qmlRegisterSingletonType` + 回调 + `QQmlEngine::setObjectOwnership(CppOwnership)` 注册。
  **不要用 `qmlRegisterSingletonInstance`** —— 它把单例绑死到单个 `QQmlEngine`,
  而本项目有多个并存引擎(主面板 + 独立翻译窗口),第二个引擎会取到 null
  (报错:"must only be accessed from one engine" / "callback returns a null pointer")。
  已验证:`qmlRegisterSingletonType` 在两引擎并存时均可用。
- **ComboBox 委托**(`ReadableComboBox.qml`):文案**一律用 `control.textAt(index)`**,
  声明 `required property int index` 即可。四层坑(全部踩过):
  1. `required property var modelData` —— 名字不存在;
  2. 声明了 required 属性后 `index` 必须**同样声明**,否则 `ReferenceError: index is not defined`;
  3. required 的 `model` 是 **`QQmlDMListAccessorData` 访问器对象**(不是模型项),
     直接赋给 `text` 会报 `Unable to assign QQmlDMListAccessorData to QString` → 下拉项空文字;
  4. 弹窗是独立窗口,必须显式 `palette: control.palette`。
  `textAt` 是 `Q_INVOKABLE`(见 `qml/QtQuick/Templates/plugins.qmltypes`),与关闭态的
  `displayText` 同一套逻辑 —— **关闭态能显示正确文本,下拉项就一定可以**。
  Qt 官方 QML 源码随 windeployqt 原样部署在 `app/build/qml/QtQuick/Controls/Basic/*.qml`,
  遇到行为不确定时**直接读官方源码**,比猜可靠。
- 弹窗(Popup / ComboBox 下拉)是**独立窗口**,不继承应用或父项调色板 ——
  必须显式 `palette: control.palette` 或逐个设置 palette 角色,否则浅色主题下白底白字。
- **主题令牌唯一来源是 `Theme.qml`**(colors / metrics / animation / fontFamily);
  QML 内**禁止硬编码颜色**与裸字号。
- **命令按钮统一用 `ThemedButton`**(primary/secondary/ghost/danger + sm/normal/lg +
  square)。不要直接用 Basic 的 `Button` / `ToolButton` —— 其配色走 QPalette,
  深色主题下对比度不可控。
- QML 里给数值属性赋 `undefined` 会让该属性失效(如 `implicitWidth` → 0,控件"消失"),
  条件表达式必须给出两个合法值。
- **QML 属性名禁用 `on*` 前缀**(`onAccent` 之类会被当成信号处理器 → 组件加载失败 →
  整片空白),用 `accentText` / `pinText` 这类命名。
- 抽屉/弹层内**不要嵌套 `Popup`**,也**不要 `QtQuick.Dialogs`**(windeployqt 不会部署它,
  运行时 "module is not installed" → 主面板空白);色板之类的就地展开。
- 窗口圆角走 **DWM**(`host/window_effects.hpp`),不要用"QML 圆角 + 透明窗口"。
- 翻译是**独立窗口**(`translate/translate_window.*` + `TranslateWindow.qml`),
  不是主面板抽屉;所有打开路径统一经 `CoreFacade::translatePrefill`。

## 踩坑速查

- Basic 控件深色可读性:QPalette 由 `CoreFacade::applyTheme` 注入亮/暗两套。
- 无边框窗口移动用 `QWindow::startSystemMove()`,不要逐帧 `move()`。
- 新 .cpp 需同时加进 `app/CMakeLists.txt` 的 `add_executable`;新 QML 需加进
  `qt_add_qml_module(... QML_FILES ...)`,否则不进 qrc。
