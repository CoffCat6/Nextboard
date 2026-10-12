# Nextboard 打包脚本(docs/04-build-package-release.md §5.2)
#
# 用法(在仓库根目录):
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1                  # 版本从 CMakeLists 解析
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.2.0   # 显式指定版本号
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -KeepAllImageFormats  # 保留全部图片格式插件
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -SkipInstaller   # 跳过 NSIS 安装包
#
# 前置:app/build 必须来自发布构建(build.ps1 -Clean -Test,见文档 §3.2)。
# 产物(仓库根 dist/ 下):
#   Nextboard-<版本>-portable.zip   便携版
#   Nextboard-Setup-<版本>.exe      安装包(需 tools/installer.nsi + NSIS;缺任一则跳过并提示)
#
# ⚠️ 本脚本需带 BOM UTF-8(PowerShell 5.1 解析中文注释的前提,见 progress.md 踩坑记录)。
param(
    [string]$Version,
    [switch]$KeepAllImageFormats,   # 默认只保留 imageformats/qico.dll(UI 仅用 ico;PNG 内置 Qt6Gui)
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'

# ---- 路径配置(与 build.ps1 同源;NSIS/CRT 源按需修改) ----
$Root      = Split-Path $PSScriptRoot -Parent
$BuildDir  = Join-Path $Root 'app\build'
$DistDir   = Join-Path $Root 'dist'
$NsiFile   = Join-Path $Root 'tools\installer.nsi'
$NsisBin   = 'D:\Program Files (x86)\NSIS\Bin\makensis.exe'
$CrtSource = Join-Path $env:SystemRoot 'System32'
# app-local CRT(文档 §4.2 方案 A):微软许可允许应用本地分发
$CrtDlls   = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll')

# windeployqt 部署的运行期辅助插件目录(文档 §4.1 必选)
$RuntimeDirs = @('platforms', 'qml', 'styles', 'iconengines', 'tls',
                 'networkinformation', 'platforminputcontexts', 'generic')
# 顶层 Qt6 DLL 由 -Filter 'Qt6*.dll' 统一收录

function Step($msg) { Write-Host "`n==== $msg ====" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  [OK] $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  [WARN] $msg" -ForegroundColor Yellow }
function Die($msg)  { Write-Host "  [FAIL] $msg" -ForegroundColor Red; exit 1 }

function DirSizeMb($path) {
    $s = (Get-ChildItem $path -Recurse -File | Measure-Object Length -Sum).Sum
    return [math]::Round($s / 1MB, 1)
}

# ---- 第 1 步:前置检查 ----
Step '前置检查'
if (-not (Test-Path (Join-Path $BuildDir 'nextboard.exe'))) { Die "缺 $BuildDir\nextboard.exe — 先跑 build.ps1" }
if (-not (Test-Path (Join-Path $BuildDir 'nbcore.dll')))    { Die "缺 $BuildDir\nbcore.dll — 先跑 build.ps1" }
if (-not (Test-Path (Join-Path $BuildDir 'platforms\qwindows.dll'))) {
    Die '缺 platforms\qwindows.dll — Qt 运行库未部署,先跑 build.ps1(-Deploy)'
}
$makensis = $null
if (-not $SkipInstaller) {
    if ((Test-Path $NsiFile) -and (Test-Path $NsisBin)) { $makensis = $NsisBin }
    else {
        Warn "跳过安装包:需要 $NsiFile 与 NSIS($NsisBin)同时就位(当前缺其中之一)"
        Warn '便携版 zip 仍会正常产出;补齐后重跑即可'
    }
}
foreach ($dll in $CrtDlls) {
    if (-not (Test-Path (Join-Path $CrtSource $dll))) { Die "System32 缺 $dll — 改脚本头部 CrtSource 指向 VC Redist 目录" }
}
Ok 'nextboard.exe / nbcore.dll / 平台插件 / CRT 源就绪'

# ---- 第 2 步:版本号(参数优先,否则从 CMakeLists 单一来源解析) ----
Step '版本号'
if (-not $Version) {
    # ⚠️ 行内容形如 "project(Nextboard VERSION 0.2.0 LANGUAGES CXX)" —
    #    版本号后面是 LANGUAGES 而非紧跟右括号,正则不得断言 "\)"
    $cmake = Get-Content (Join-Path $Root 'app\CMakeLists.txt') -Encoding UTF8
    $hit = $cmake | Select-String 'project\(Nextboard VERSION ([0-9][0-9.]*)'
    if (-not $hit) { Die '无法从 app\CMakeLists.txt 解析 project(VERSION) — 请用 -Version 显式指定' }
    $Version = $hit.Matches[0].Groups[1].Value
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { Die "版本号非法:`"$Version`"(期望 X.Y.Z)" }
Ok "版本 $Version"

# ---- 第 3 步:stage 目录(每次重建,防残留) ----
Step "stage 目录 dist\Nextboard-$Version"
$Stage = Join-Path $DistDir "Nextboard-$Version"
if (Test-Path $Stage) {
    # 必须删干净再拷:残留的旧文件会把"不在白名单里"的东西带进发布包。
    # 删除失败要给出可读原因(常见:上一轮产物被占用/只读,或受限会话的完整性冲突),
    # 而不是抛原始 PowerShell 异常。
    try {
        Remove-Item $Stage -Recurse -Force -ErrorAction Stop
    } catch {
        Die "无法清理旧 stage: $Stage —— $($_.Exception.Message)`n        请关闭占用该目录的程序(或手动删除)后重跑"
    }
}
New-Item -ItemType Directory -Force $Stage | Out-Null
if (-not (Test-Path $DistDir)) { New-Item -ItemType Directory -Force $DistDir | Out-Null }
Ok $Stage

# ---- 第 4 步:按白名单拷贝(文档 §4.1) ----
Step '按白名单拷贝运行期文件'
Copy-Item (Join-Path $BuildDir 'nextboard.exe') $Stage
Copy-Item (Join-Path $BuildDir 'nbcore.dll')    $Stage
$dlls = Get-ChildItem $BuildDir -Filter 'Qt6*.dll' -File
if (-not $dlls) { Die 'app/build 下没有 Qt6*.dll — windeployqt 未执行' }
$dlls | ForEach-Object { Copy-Item $_.FullName $Stage }
Ok "exe + nbcore + $($dlls.Count) 个 Qt6 DLL"

foreach ($dir in $RuntimeDirs) {
    $src = Join-Path $BuildDir $dir
    if (Test-Path $src) { Copy-Item $src (Join-Path $Stage $dir) -Recurse }
}
# imageformats:UI 只用 ico(PNG 内置 Qt6Gui);其余格式插件默认裁剪,-KeepAllImageFormats 保留
$imgSrc = Join-Path $BuildDir 'imageformats'
if (Test-Path $imgSrc) {
    New-Item -ItemType Directory -Force (Join-Path $Stage 'imageformats') | Out-Null
    if ($KeepAllImageFormats) {
        Copy-Item "$imgSrc\*" (Join-Path $Stage 'imageformats') -Recurse
        Ok 'imageformats 全量保留'
    } else {
        Copy-Item (Join-Path $imgSrc 'qico.dll') (Join-Path $Stage 'imageformats')
        Ok 'imageformats 只保留 qico.dll(全量保留用 -KeepAllImageFormats)'
    }
}

# ---- 第 5 步:app-local CRT + 完整性标签 ----
Step '补齐 app-local CRT(§4.2 方案 A)'
# 沙箱会给新建目录/文件打 Low 完整性标签 → 低完整性进程调 Shell_NotifyIcon 被 UIPI 拒,
# 托盘图标静默不显示(文档 §3.3 / build.ps1 第 4 步同款问题)。
# ⚠️ 顺序必须"先目录(OI)(CI) 继承、后文件兜底":先拷贝再逐文件修,新文件继承的是
#    stage 目录当时的 Low 标签,icacls 单文件修复在受限会话里会被"拒绝访问"。
# 完整性修复:与 build.ps1 同款。沙箱/受限会话可能拦截 icacls(WRITE_DAC),此时降级为
# 告警——真实环境跑本脚本时与 build.ps1 一样会成功;安装器解压出的新文件默认继承 Medium。
# ⚠️ 用 cmd /c 包裹:$ErrorActionPreference='Stop' 会把 icacls 的 stderr 升级成终止错误。
cmd /c "icacls `"$Stage`" /setintegritylevel (OI)(CI)Medium >nul 2>&1"
foreach ($dll in $CrtDlls) {
    Copy-Item (Join-Path $CrtSource $dll) $Stage
}
Ok ("CRT x" + $CrtDlls.Count + " 就位")
# 兜底:显式修复 exe;失败只告警(目录继承通常已覆盖)
$nbExe = Join-Path $Stage 'nextboard.exe'
cmd /c "icacls `"$nbExe`" /setintegritylevel Medium >nul 2>&1"
$label = (cmd /c "icacls `"$nbExe`"" | Select-String 'Mandatory Label') -join '; '
if ($label -match 'Low Mandatory') { Warn "nextboard.exe 完整性仍为 Low,托盘图标可能不显示(本会话若为沙箱属环境限制,真机重跑即可):$label" }
else { Ok 'stage 目录与 nextboard.exe 完整性级别 = Medium' }

# ---- 第 6 步:校验 ----
Step '校验 stage'
# 6.1 构建残留/调试文件不得混入(白名单拷贝的兜底断言)
$forbidden = Get-ChildItem $Stage -Recurse -File |
    Where-Object { $_.Name -match '\.(lib|exp|pdb|log)$' -or $_.Name -eq 'nextboard.log' }
if ($forbidden) { $forbidden | ForEach-Object { Die "stage 混入非运行期文件: $($_.FullName)" } }
if (Test-Path (Join-Path $Stage 'qmltooling')) { Die 'stage 不应包含 qmltooling(调试工具)' }
# 6.2 必选项存在性
$must = @('nextboard.exe', 'nbcore.dll', 'platforms\qwindows.dll',
          'imageformats\qico.dll', 'qml\QtQuick', 'qml\QtQuick\Controls')
foreach ($m in $must) {
    if (-not (Test-Path (Join-Path $Stage $m))) { Die "stage 缺必选项: $m" }
}
$fileCount = (Get-ChildItem $Stage -Recurse -File).Count
$sizeMb = DirSizeMb $Stage
Ok "文件数 $fileCount / 体积 $sizeMb MB(路线图 6.4 目标 <60MB)"
if ($sizeMb -ge 60) { Warn "运行期子集 $sizeMb MB 超过 60MB 预算,检查是否混入多余文件" }

# ---- 第 7 步:产出 portable.zip 与安装包 ----
Step '产出便携版 zip'
$zip = Join-Path $DistDir "Nextboard-$Version-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# 压缩 stage 的内容(解压后直接得 Nextboard-<版本>/ 目录)
Compress-Archive -Path $Stage -DestinationPath $zip
Ok "$zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"

if ($makensis) {
    Step '编译 NSIS 安装包'
    # 坑 1(实测):%TEMP% 不可写时(沙箱/受限会话)makensis 报
    #   "Internal compiler error #12345: error creating mmap"(32 位进程的内存映射失败),
    #   与脚本无关。把 TMP/TEMP 重定向到仓库内 dist\tmp 即可。
    # 坑 2:nsi 内用 ..\dist\ 相对路径引用 stage,makensis 按 CWD 解析 →
    #   必须在 tools\ 目录下运行(从仓库根直接调会去找 ..\..\dist 而失败)。
    $tmpDir = Join-Path $DistDir 'tmp'
    New-Item -ItemType Directory -Force $tmpDir | Out-Null
    $env:TMP = $tmpDir
    $env:TEMP = $tmpDir
    Push-Location $PSScriptRoot
    try {
        cmd /c "`"$makensis`" `"$NsiFile`""
    } finally {
        Pop-Location
    }
    if ($LASTEXITCODE -ne 0) { Die 'makensis 失败(细节见上方输出)' }
    $setup = Join-Path $DistDir "Nextboard-Setup-$Version.exe"
    if (-not (Test-Path $setup)) { Die "makensis 已通过但未找到 $setup — 检查 installer.nsi 的 OutFile" }
    Ok "$setup"
}

# ---- 第 7.5 步:产物完整性标签(可运行性前提!) ----
# ⚠️ 本工作区会被沙箱打上 Low 完整性标签,dist 下**新建**文件会继承它。
#    而 Windows 会按"映像文件自身的标签"决定进程完整性:
#      Low 标签的安装包 → 以 Low 完整性启动 → 写不了 Medium 的 %TEMP%
#      → NSIS 建不出 $PLUGINSDIR → 报
#        "Error writing temporary file. Make sure your temp folder is valid."
#    (便携版 zip 解压出的 exe 同理:解压到工作区内会继承 Low,双击同样跑不起来。)
#    故对 dist 目录与全部产物显式设为 Medium;并复核,异常时给出可读处置办法。
Step '产物完整性标签(可运行性前提)'
cmd /c "icacls `"$DistDir`" /setintegritylevel (OI)(CI)Medium >nul 2>&1" | Out-Null
$artifacts = @($zip) + @(Get-ChildItem $DistDir -File -Filter '*.exe' | ForEach-Object { $_.FullName })
foreach ($art in $artifacts) {
    if (Test-Path $art) { cmd /c "icacls `"$art`" /setintegritylevel Medium >nul 2>&1" | Out-Null }
}
$stillLow = @()
foreach ($art in $artifacts) {
    if (Test-Path $art) {
        $label = (cmd /c "icacls `"$art`"") -join ' '
        if ($label -match 'Low Mandatory') { $stillLow += $art }
    }
}
if ($stillLow) {
    Warn '以下产物完整性仍为 Low,双击运行可能报 "Error writing temporary file":'
    $stillLow | ForEach-Object { Warn "  $_" }
    Warn '处置:把文件复制到工作区之外的普通目录(桌面/下载)再运行,或手动执行 icacls <文件> /setintegritylevel Medium'
} else {
    Ok '产物完整性级别 = Medium(双击可直接运行)'
}

# ---- 第 8 步:摘要(供发布说明引用) ----
Step 'dist 摘要'
$summary = Get-ChildItem $DistDir -File | ForEach-Object {
    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
    "{0,-40} {1,8} MB  SHA256 {2}" -f $_.Name, [math]::Round($_.Length / 1MB, 1), $hash.Substring(0, 16) + '…'
}
$summary | ForEach-Object { Write-Host "  $_" }
$hashFile = Join-Path $DistDir "Nextboard-$Version-SHA256.txt"
# ⚠️ 匹配 Nextboard-* 而非 Nextboard-<版本>-*:安装包名是 Nextboard-Setup-<版本>.exe,
#    用版本号打头会漏掉它(zip 与 setup 都要进校验和清单)
Get-ChildItem $DistDir -File |
    Where-Object { $_.Name -like 'Nextboard-*' -and $_.Extension -ne '.txt' } |
    ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name } |
    Set-Content -Path $hashFile -Encoding ascii
Ok "校验和已写入 $hashFile"
# makensis 临时目录(第 7 步 TMP/TEMP 重定向的落点),清理避免混进 dist
$tmpDir = Join-Path $DistDir 'tmp'
if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force }

Write-Host "`n==== 打包完成 ====" -ForegroundColor Cyan
Write-Host "  stage : $Stage($fileCount 个文件,$sizeMb MB)"
Write-Host "  便携版: $zip"
if ($makensis) { Write-Host "  安装包: $(Join-Path $DistDir "Nextboard-Setup-$Version.exe")" }
else { Write-Host "  安装包: 跳过(缺 tools\installer.nsi 或 NSIS)" }
Write-Host '  下一步: 按 docs/04-build-package-release.md §6 在真机/干净环境跑安装冒烟清单'
