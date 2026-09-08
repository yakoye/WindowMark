# 打发布包：把构建产物、文档和几个用得上的工具拢到 dist\WindowMark-<版本>-win64\，
# 再压成同名的 zip。
#
# 版本号只有一个来源：src\shared\AppIdentity.h 里的 kProductVersion。这里读它，不重复
# 写一遍——CMake 也是读的同一个地方，三处版本号对不上的发布包比没有版本号更糟。
#
# 带进包的工具是「用户可能用得上的」那几个，不是开发时的全部。判据很直接：这个脚本
# 能不能帮用户自己解决一个具体症状。

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$identity = Get-Content (Join-Path $root 'src\shared\AppIdentity.h') -Raw
if ($identity -notmatch 'kProductVersion\[\]\s*=\s*L"([^"]+)"') {
    throw '读不出 AppIdentity.h 里的 kProductVersion'
}
$version = $Matches[1]
$name = "WindowMark-v$version-win64"
$stage = Join-Path $root "dist\$name"
$zip = Join-Path $root "dist\$name.zip"

Write-Host "打包 v$version"

$binaries = @(
    'WindowMark.exe',
    'WindowMarkInspect.exe',
    'WindowMarkSetup.exe',
    'WindowMarkUninstall.exe',
    'ClipKeeper.exe'
)
$release = Join-Path $root 'build\Release'
foreach ($exe in $binaries) {
    $path = Join-Path $release $exe
    if (-not (Test-Path $path)) {
        throw "缺 $exe —— 先跑一次 reinstall.ps1 把 Release 构建出来"
    }
}

# 用户可能用得上的工具。判据是「能不能帮他自己解决一个具体症状」，
# 开发时用的基准测试和自检脚本不进包。
$tools = @(
    'auto-shadow-inset.py',      # 量窗口矩形比可见部分大多少
    'measure-shadow-inset.ps1',  # 同上，鼠标悬停版
    'measure-stroke.py',         # 量边框实际线宽
    'measure-gap.py',            # 量边框缺口对不对得上遮挡
    'inspect-corner.py',         # 逐像素看圆角画成什么样
    'diagnose-window.py',        # 某个窗口为什么没边框 / 被谁盖了
    'find-stray-lines.py',       # 找孤立的边框线
    'watch-line-over.py'         # 守着抓「边框画到了上层窗口身上」
)
$batches = @(
    'measure_shadow_auto.bat',
    'measure_shadow_inset.bat'
)
$docs = @('README.md', 'CHANGELOG.md', 'LICENSE')

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'tools') | Out-Null

foreach ($exe in $binaries) {
    Copy-Item (Join-Path $release $exe) $stage
}
foreach ($doc in $docs) {
    Copy-Item (Join-Path $root $doc) $stage
}
foreach ($bat in $batches) {
    Copy-Item (Join-Path $root $bat) $stage
}
foreach ($tool in $tools) {
    $path = Join-Path $root "tools\$tool"
    if (-not (Test-Path $path)) { throw "缺工具 $tool" }
    Copy-Item $path (Join-Path $stage 'tools')
}

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip

$size = [math]::Round((Get-Item $zip).Length / 1KB)
Write-Host ''
Write-Host "好了：dist\$name.zip（$size KB）"
Get-ChildItem $stage -Recurse -File |
    ForEach-Object { '  ' + $_.FullName.Substring($stage.Length + 1) }
