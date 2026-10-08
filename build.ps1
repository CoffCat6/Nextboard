# Nextboard 一键构建脚本
#
# 用法(在仓库根目录):
#   powershell -ExecutionPolicy Bypass -File build.ps1            # 构建 core + app
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Test      # 顺带跑 core 单测
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Run       # 构建后启动
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean     # 清理后重新配置构建
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Deploy    # 强制重跑 windeployqt
#
# 前置(本机默认路径,可用参数覆盖):Rust(msvc 目标)、VS2022、Qt 6.9.0 msvc2022_64
param(
    [switch]$Run,
    [switch]$Test,
    [switch]$Clean,
    [switch]$Deploy    # 强制重跑 windeployqt(默认:缺 Qt DLL 或平台插件时自动执行)
)

$ErrorActionPreference = 'Stop'

# ---- 路径配置(按需修改) ----
$Root       = $PSScriptRoot
$CoreDir    = Join-Path $Root 'core'
$AppDir     = Join-Path $Root 'app'
$BuildDir   = Join-Path $AppDir 'build'
$QtBin      = 'D:\Qt\6.9.0\msvc2022_64\bin'
$QtCMake    = 'D:\Qt\Tools\CMake_64\bin\cmake.exe'
$QtNinja    = 'D:\Qt\Tools\Ninja\ninja.exe'
$VcVars64   = 'D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'

function Step($msg)  { Write-Host "`n==== $msg ====" -ForegroundColor Cyan }
function Ok($msg)    { Write-Host "  [OK] $msg" -ForegroundColor Green }
function Die($msg)   { Write-Host "  [FAIL] $msg" -ForegroundColor Red; exit 1 }

# ---- 前置检查 ----
Step '检查环境'
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { Die 'cargo 不在 PATH,先安装 Rust(msvc 工具链)' }
if (-not (Test-Path $QtCMake))  { Die "找不到 CMake: $QtCMake" }
if (-not (Test-Path $QtNinja))  { Die "找不到 Ninja: $QtNinja" }
if (-not (Test-Path $VcVars64)) { Die "找不到 vcvars64: $VcVars64(请改脚本头部的路径)" }
Ok 'cargo / CMake / Ninja / vcvars64 全部就绪'

# ---- 第 1 步:core(nbcore.dll)----
Step '构建 nbcore.dll (cargo build --release)'
Push-Location $CoreDir
try {
    if ($Test) {
        cargo test
        if ($LASTEXITCODE -ne 0) { Die 'core 单测失败' }
        Ok 'core 单测通过'
    }
    cargo build --release
    if ($LASTEXITCODE -ne 0) { Die 'cargo build 失败' }
} finally { Pop-Location }
$dll = Join-Path $CoreDir 'target\release\nbcore.dll'
if (-not (Test-Path $dll)) { Die "未产出 $dll" }
Ok "nbcore.dll 就绪 ($([math]::Round((Get-Item $dll).Length/1MB,1)) MB)"

# ---- 第 2 步:app(nextboard.exe)----
Step '构建 nextboard.exe (CMake + Ninja, MSVC)'
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Force $BuildDir | Out-Null }
if ($Clean) {
    Step '清理构建缓存 (-Clean)'
    Remove-Item (Join-Path $BuildDir 'CMakeCache.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir 'CMakeFiles') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $BuildDir 'build.ninja') -Force -ErrorAction SilentlyContinue
    Ok '已清理 CMakeCache/CMakeFiles/build.ninja'
}
$needConfigure = -not (Test-Path (Join-Path $BuildDir 'build.ninja'))

# 在 MSVC 环境里执行 configure + build(cl 不在 PATH 时必须经 vcvars64)
$cmdChain = "`"$VcVars64`" >nul && cd /d `"$BuildDir`" && "
if ($needConfigure) {
    Write-Host '  首次配置(CMake generate)...'
    $cmdChain += "`"$QtCMake`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl .. && "
}
$cmdChain += "`"$QtNinja`""

cmd /c $cmdChain
if ($LASTEXITCODE -ne 0) { Die 'app 构建失败(细节见上方输出)' }
Ok 'nextboard.exe 构建完成'

# ---- 第 3 步:Qt 运行库部署 ----
# 判定"部署完整"的标准:
#  - platforms\qwindows.dll:缺它 exe 启动即弹 "could not find the Qt platform plugin" 或静默无窗口;
#  - Qt6Quick.dll / qml\ 插件目录:QML 面板(v0.2 起)的运行库,缺它面板空白;
#  - imageformats\qico.dll:应用图标/托盘图标用的是 .ico,QIcon 读它依赖该图片格式插件,
#    缺了不会崩,但窗口与托盘图标会静默变成空白。
$needDeploy = $Deploy -or
    -not (Test-Path (Join-Path $BuildDir 'Qt6Widgets.dll')) -or
    -not (Test-Path (Join-Path $BuildDir 'platforms\qwindows.dll')) -or
    -not (Test-Path (Join-Path $BuildDir 'Qt6Quick.dll')) -or
    -not (Test-Path (Join-Path $BuildDir 'qml')) -or
    -not (Test-Path (Join-Path $BuildDir 'imageformats\qico.dll'))
if ($needDeploy) {
    Step '部署 Qt 运行库 (windeployqt)'
    & (Join-Path $QtBin 'windeployqt.exe') --no-translations --no-system-d3d-compiler `
        --no-opengl-sw (Join-Path $BuildDir 'nextboard.exe')
    if ($LASTEXITCODE -ne 0) { Die 'windeployqt 失败' }
    if (-not (Test-Path (Join-Path $BuildDir 'platforms\qwindows.dll'))) {
        Die 'windeployqt 后仍缺 platforms\qwindows.dll,检查 Qt 安装'
    }
    if (-not (Test-Path (Join-Path $BuildDir 'Qt6Quick.dll'))) {
        Die 'windeployqt 后仍缺 Qt6Quick.dll:QML 面板无法启动,检查 Qt Quick 组件是否已安装'
    }
    if (-not (Test-Path (Join-Path $BuildDir 'imageformats\qico.dll'))) {
        Die 'windeployqt 后仍缺 imageformats\qico.dll:窗口/托盘图标会变空白,检查 Qt imageformats 插件'
    }
    Ok 'Qt DLL + 平台插件 + Quick/QML 运行库 + ico 图片插件已部署'
} else {
    Write-Host '  Qt 运行库已部署,跳过(强制重跑用 -Deploy)'
}
# nbcore.dll 由 CMake POST_BUILD 自动拷贝;QML 经 qrc 打进 exe,无需部署资源目录
# ---- 第 4 步:完整性标签修复(托盘图标可见的前提)----
# 背景:AI 沙箱类工具会给项目目录打上 "Low Mandatory Level" 完整性标签,目录下新建的
# 可执行文件会继承该标签;而 Windows 是按【可执行文件自身的标签】设定进程令牌的——
# 于是 nextboard.exe 以 Low(0x1000) 运行,而 Shell(explorer) 为 Medium(0x2000)。
# 低完整性进程调用 Shell_NotifyIcon 会被 UIPI 拒绝(ERROR_ACCESS_DENIED=5),Qt 又会
# 静默忽略该返回值 → 现象是:进程存活、热键全部正常,唯独系统托盘图标不显示。
# 这里把构建目录及其产物显式提升回 Medium,保证每次构建后托盘都能正常注册。
Step '修复完整性标签 (托盘图标可见的前提)'
$nbExe = Join-Path $BuildDir 'nextboard.exe'
icacls $BuildDir /setintegritylevel '(OI)(CI)Medium' > $null
icacls $nbExe /setintegritylevel Medium > $null
$nbLabel = (icacls $nbExe | Select-String 'Mandatory Label') -join '; '
if ($nbLabel -match 'Low Mandatory') {
    Write-Host "  [WARN] nextboard.exe 完整性仍为 Low,托盘图标可能不显示:$nbLabel" -ForegroundColor Yellow
} else {
    Ok '构建目录与 nextboard.exe 完整性级别 = Medium'
}

# ---- 完成 ----
$exe = Join-Path $BuildDir 'nextboard.exe'
Write-Host "`n==== 构建完成 ====" -ForegroundColor Cyan
Write-Host "  $exe"
Write-Host "  热键: Ctrl+Shift+V 面板 / Ctrl+Shift+A 框选 / Ctrl+Shift+Q 全屏 / Ctrl+Shift+P 贴图"
Write-Host "  日志: $($BuildDir)\nextboard.log"

if ($Run) {
    Step '启动'
    Start-Process $exe -WorkingDirectory $BuildDir
    Ok "已启动 nextboard.exe"
}
