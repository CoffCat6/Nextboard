; Nextboard NSIS 安装器(docs/04-build-package-release.md §5.3)
;
; 前置:先跑 tools\package.ps1 产出 dist\Nextboard-<版本>\(§5.2 白名单 + app-local CRT)
; 产物:dist\Nextboard-Setup-<版本>.exe
;
; 规格(§5.3,与文档决策一致):
;   - 安装位置:%LOCALAPPDATA%\Programs\Nextboard(单用户、免 UAC、数据目录可写;D1-a)
;   - RequestExecutionLevel user:不弹 UAC
;   - 快捷方式:开始菜单(默认装);桌面(可选,默认不勾)
;   - 开机自启:可选组件(默认不勾),Run 键值带 --hidden(D11,修开机弹面板)
;   - 数据在 **exe 同级 data/**(便携式布局):数据库/截图 blob/设置/日志同处一地。
;     故卸载必须**逐项删除并跳过 data** —— 直接 RMDir /r "$INSTDIR" 会连用户历史一起删。
;   - 卸载:删安装目录(保留 data)/快捷方式/Run 键;个人数据默认保留,
;     "同时删除个人数据"做成卸载器组件页勾选项(默认不勾,D4)
;   - 静默:安装 /S;卸载 /S(均跳过组件页,个人数据保留)
;
; ⚠️ 本文件必须带 BOM UTF-8 且由 Unicode NSIS 编译(含中文界面;无 BOM 会按本地
;    代码页解析导致乱码/编译失败)。

Unicode true
ManifestDPIAware true
XPStyle on

; LZMA 固体压缩:包体最小(58MB 数据 → 16MB)。
; ⚠️ 实测坑:makensis 在 %TEMP% 不可写的会话(沙箱/受限环境)会报
;    "Internal compiler error #12345: error creating mmap the size of ..." ——
;    与脚本无关,是 32 位 makensis 的内存映射拿不到临时文件。
;    规避:跑 makensis 前把 TMP/TEMP 重定向到可写目录(package.ps1 第 7 步已处理)。
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APP_NAME      "Nextboard"
!define APP_PUBLISHER "Nextboard"
!define EXE_NAME      "nextboard.exe"
!define REG_UNINST    "Software\Microsoft\Windows\CurrentVersion\Uninstall\Nextboard"
!define REG_RUN       "Software\Microsoft\Windows\CurrentVersion\Run"
; 数据目录 = exe 同级 data/(便携式布局,见 app/src/host/app_paths.hpp)
!define DATA_DIR      "$INSTDIR\data"
; 0.2.0 早期版本的默认位置(%LOCALAPPDATA%):仅用于"删除个人数据"时一并清理
!define LEGACY_DATA_DIR "$LOCALAPPDATA\Nextboard\Nextboard"

; ---- 版本号:单一来源 = CMakeLists(改版本只需改这一行) ----
!ifndef APP_VERSION
  !define APP_VERSION "0.2.0"
!endif

; ---- stage 源目录(package.ps1 的输出);/DSRC_DIR=... 可覆盖(验证/子集编译用) ----
!ifndef SRC_DIR
  !define SRC_DIR "..\dist\Nextboard-${APP_VERSION}"
!endif

; ---- 输出文件名;/DOUT_FILE=... 可覆盖(CI/验证用) ----
!ifndef OUT_FILE
  !define OUT_FILE "..\dist\Nextboard-Setup-${APP_VERSION}.exe"
!endif

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Nextboard"
InstallDirRegKey HKCU "${REG_UNINST}" "InstallLocation"
RequestExecutionLevel user

; 安装器图标与 exe 同源(§5.3)
!define MUI_ICON "..\app\resources\nextboard.ico"
!define MUI_UNICON "..\app\resources\nextboard.ico"
!define MUI_ABORTWARNING
; 完成页"立即运行"(默认勾选,§5.3)
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE_NAME}"
!define MUI_FINISHPAGE_RUN_CHECKED

; 页面顺序:位置 → 组件 → 安装 → 完成
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; 卸载页顺序:确认 → 组件(个人数据勾选)→ 执行
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

InstType "标准安装"
InstType "完整安装"

; ================= 安装 =================

Section "!${APP_NAME}(必需)" SecCore
  SectionIn RO
  ; 运行中检测(§5.3):结束旧实例,避免文件占用导致覆盖失败。
  ; 托盘程序无未保存状态,直接结束;taskkill 对未运行是空操作。
  nsExec::Exec 'taskkill /IM "${EXE_NAME}" /F'
  Sleep 300
  SetOutPath "$INSTDIR"
  ; ⚠️ 用 * 而非 *.*:qml 目录下有大量无扩展名的 qmldir 文件,
  ;    *.* 会漏掉它们 → 运行期 "module is not installed"。File /r 递归含子目录。
  File /r "${SRC_DIR}\*"
  WriteUninstaller "$INSTDIR\uninstaller.exe"
  ; ARP 卸载项(单用户安装,写 HKCU)
  WriteRegStr   HKCU "${REG_UNINST}" "DisplayName"     "${APP_NAME} ${APP_VERSION}"
  WriteRegStr   HKCU "${REG_UNINST}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr   HKCU "${REG_UNINST}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr   HKCU "${REG_UNINST}" "DisplayIcon"     "$INSTDIR\${EXE_NAME}"
  WriteRegStr   HKCU "${REG_UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${REG_UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${REG_UNINST}" "NoRepair" 1
  WriteRegStr   HKCU "${REG_UNINST}" "UninstallString" '"$INSTDIR\uninstaller.exe"'
  ; EstimatedSize 单位 KB(供 ARP 显示)
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${REG_UNINST}" "EstimatedSize" "$0"
SectionEnd

Section "开始菜单快捷方式" SecStartMenu
  SectionIn 1 2
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${EXE_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk" "$INSTDIR\uninstaller.exe"
SectionEnd

; 以下为可选项:组件页出现,默认不勾(.onInit 里显式清零,双保险)
Section "桌面快捷方式" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${EXE_NAME}"
SectionEnd

; D11:Run 键值带 --hidden(修开机弹面板,§2.6)
Section "开机自动启动(静默驻留托盘)" SecAutoStart
  WriteRegStr HKCU "${REG_RUN}" "Nextboard" '"$INSTDIR\${EXE_NAME}" --hidden'
SectionEnd

Function .onInit
  ; 默认 InstType = "标准安装"(核心 + 开始菜单);桌面/自启默认不勾
  SectionSetFlags ${SecDesktop}   0
  SectionSetFlags ${SecAutoStart} 0
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "在开始菜单创建程序入口(含卸载快捷方式)。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop}   "在桌面创建快捷方式。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutoStart} "开机自动启动,静默驻留托盘(不弹窗口)。"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ================= 卸载 =================

; ⚠️⚠️ 命名即语义(踩过坑,见 docs/progress.md):
;   NSIS 只把 **精确名 "Uninstall"** 或 **"un." 前缀** 的 Section 编进卸载器,
;   其余 Section 一律算"安装段",会在**安装时**执行。
;   这里曾把卸载段写成 `Section "!卸载 Nextboard"` —— 既不是精确名也没有 un. 前缀,于是:
;     ① 安装时就把刚铺好的文件、ARP 项、Run 键、快捷方式全删了(装完什么都不剩);
;     ② 生成的卸载器 **0 个 section**,运行它什么都不做。
;   自查点:makensis 汇总行的 `Uninstall: N sections` —— 卸载段数应 >0(本项目为 2)。
Section "Uninstall" SecUninstall
  SectionIn RO            ; 卸载器主段:必选(组件页置灰),静默卸载也必定执行
  ; 运行中检测
  nsExec::Exec 'taskkill /IM "${EXE_NAME}" /F'
  Sleep 300
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  DeleteRegValue HKCU "${REG_RUN}" "Nextboard"
  DeleteRegKey   HKCU "${REG_UNINST}"
  ; ⚠️ 数据目录($INSTDIR\data)默认保留 → 不能直接 RMDir /r "$INSTDIR"。
  ;    Windows 没有"排除删除",故逐项删除顶层条目并跳过 data;
  ;    以后新增目录/文件无需改这里(遍历天然覆盖)。
  FindFirst $0 $1 "$INSTDIR\*"
un_del_loop:
  StrCmp $1 "" un_del_done
  StrCmp $1 "." un_del_next
  StrCmp $1 ".." un_del_next
  StrCmp $1 "data" un_del_next            ; 个人数据:默认保留(D4)
  ; Delete 对目录无效、RMDir /r 对文件无效;两者都调用可同时覆盖文件/目录/空目录
  Delete "$INSTDIR\$1"
  RMDir /r "$INSTDIR\$1"
un_del_next:
  FindNext $0 $1
  Goto un_del_loop
un_del_done:
  FindClose $0
  Delete "$INSTDIR\uninstaller.exe"        ; 卸载器自删(进程退出后生效)
  RMDir "$INSTDIR"                         ; 有 data 时非空 → 失败并保留目录,符合预期
SectionEnd

; D4:个人数据默认保留;仅当用户在组件页勾选本节时才删
; (静默卸载跳过组件页且本节默认不选 → 数据必保留,防误删剪贴板历史;
;  已用行为探测实测:静默卸载时本节确实不执行)
; ⚠️ 必须有 "un." 前缀才归卸载器;否则会被当成安装段(见上方命名说明)。
Section /o "un.同时删除个人数据(历史记录/截图/设置)" SecData
  RMDir /r "${DATA_DIR}"
  ; 顺带清理 0.2.0 早期版本的 %LOCALAPPDATA% 数据目录(若存在)
  RMDir /r "${LEGACY_DATA_DIR}"
  RMDir "$INSTDIR"
SectionEnd

!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
  ; 注意:卸载器描述块内部用的同样是 MUI_DESCRIPTION_TEXT(无 UN 变体)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecData} "删除 ${DATA_DIR} 下的全部数据:剪贴板历史、截图 blobs、设置。默认不删除,建议保留以便重装后继续使用。"
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END
