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
# 判定"部署完整"的标准不是 Qt6Widgets.dll 存在,而是**平台插件** qwindows.dll——
# 缺它时 exe 启动即弹 "could not find the Qt platform plugin" 或静默无窗口。
$needDeploy = $Deploy -or
    -not (Test-Path (Join-Path $BuildDir 'Qt6Widgets.dll')) -or
    -not (Test-Path (Join-Path $BuildDir 'platforms\qwindows.dll'))
if ($needDeploy) {
    Step '部署 Qt 运行库 (windeployqt)'
    & (Join-Path $QtBin 'windeployqt.exe') --no-translations --no-system-d3d-compiler `
        --no-opengl-sw (Join-Path $BuildDir 'nextboard.exe')
    if ($LASTEXITCODE -ne 0) { Die 'windeployqt 失败' }
    if (-not (Test-Path (Join-Path $BuildDir 'platforms\qwindows.dll'))) {
        Die 'windeployqt 后仍缺 platforms\qwindows.dll,检查 Qt 安装'
    }
    Ok 'Qt DLL + 平台插件 + Quick/QML 运行库已部署'
} else {
    Write-Host '  Qt 运行库已部署,跳过(强制重跑用 -Deploy)'
}
# nbcore.dll 由 CMake POST_BUILD 自动拷贝;QML 经 qrc 打进 exe,无需部署资源目录

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
