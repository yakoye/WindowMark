# 生成安装包：把 build\Release 里的产物和文档收进一个 zip，结构和发布包完全一样——
# 解压后双击 WindowMarkSetup.exe 安装，或者直接运行 WindowMark.exe。
#
#   .\make-package.ps1            测试包 -> dist\test\WindowMark-v<版本>-test-<构建时间>-<提交>-win64.zip
#   .\make-package.ps1 -Release   发布包 -> dist\WindowMark-v<版本>-win64.zip（发布前才用）
#
# 测试包的名字里带构建时间和提交号，永远不会和发布包同名：版本号没改之前，测试包要是也叫
# WindowMark-v0.5.3-win64.zip，就会把已经发布出去的那个 v0.5.3 覆盖掉。
# 发布包已经存在时拒绝覆盖，除非加 -Force。
#
# reinstall.ps1 装完会自动调用它（测试包），所以每次编译安装都会留下一个安装包。
param(
    [switch]$Release,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$build = Join-Path $PSScriptRoot 'build\Release'

# 和发布包同样的内容。要加文件就加在这里。
$binaries = @(
    'WindowMark.exe', 'WindowMarkSetup.exe', 'WindowMarkUninstall.exe',
    'WindowMarkDiag.exe', 'WindowMarkInspect.exe', 'ClipKeeper.exe'
)
$docs = @('README.md', 'CHANGELOG.md', 'LICENSE', 'measure_shadow_auto.bat', 'measure_shadow_inset.bat')
$tools = @(
    'auto-shadow-inset.py', 'diagnose-window.py', 'find-stray-lines.py', 'inspect-corner.py',
    'measure-gap.py', 'measure-shadow-inset.ps1', 'measure-stroke.py', 'watch-line-over.py'
)

function Fail($text) {
    Write-Host $text -ForegroundColor Red
    exit 1
}

# ---- 版本号：唯一来源是 AppIdentity.h 的 kProductVersion，和关于框、托盘菜单是同一个 ----
$identity = Get-Content (Join-Path $PSScriptRoot 'src\shared\AppIdentity.h') -Raw -Encoding UTF8
$m = [regex]::Match($identity, 'kProductVersion\[\]\s*=\s*L"([^"]+)"')
if (-not $m.Success) { Fail '读不出 src\shared\AppIdentity.h 里的 kProductVersion' }
$version = $m.Groups[1].Value

# ---- 要打进去的文件，一个都不能少 ----
$entries = New-Object System.Collections.Generic.List[object]
foreach ($name in $binaries) { $entries.Add(@{ Source = (Join-Path $build $name); Entry = $name }) }
foreach ($name in $docs) { $entries.Add(@{ Source = (Join-Path $PSScriptRoot $name); Entry = $name }) }
foreach ($name in $tools) {
    $entries.Add(@{ Source = (Join-Path (Join-Path $PSScriptRoot 'tools') $name); Entry = "tools/$name" })
}
$missing = @($entries | Where-Object { -not (Test-Path $_.Source) } | ForEach-Object { $_.Source })
if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Host "  缺少 $_" -ForegroundColor Red }
    Fail '有文件不存在，没有打包。先跑一次 reinstall.ps1（或 build.bat）完成构建。'
}

# ---- 构建时间：构建戳是构建自己写的，不会过期，也不会写错 ----
$stampTime = $null
$stampFile = Join-Path $PSScriptRoot 'build\generated\BuildStamp.h'
if (Test-Path $stampFile) {
    $s = [regex]::Match((Get-Content $stampFile -Raw), 'L"([^"]+)"')
    if ($s.Success) { $stampTime = $s.Groups[1].Value }
}
if (-not $stampTime) {
    $stampTime = (Get-Item (Join-Path $build 'WindowMark.exe')).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
}

# ---- 名字和位置 ----
if ($Release) {
    $outDir = Join-Path $PSScriptRoot 'dist'
    $zipName = "WindowMark-v$version-win64.zip"
} else {
    $commit = 'nogit'
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $head = & git rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $head) { $commit = "$head".Trim() }
        # 有没提交的源码改动时标出来：这个包未必对得上任何一个提交
        $dirty = & git status --porcelain --untracked-files=no 2>$null
        if ($dirty) { $commit = "$commit-dirty" }
    }
    $digits = $stampTime -replace '[^0-9]', ''          # 20260918111358
    $when = $digits.Substring(0, 8) + '-' + $digits.Substring(8, 4)
    $outDir = Join-Path $PSScriptRoot 'dist\test'
    $zipName = "WindowMark-v$version-test-$when-$commit-win64.zip"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$zip = Join-Path $outDir $zipName
if (Test-Path $zip) {
    if ($Release -and -not $Force) {
        Fail "$zip 已经存在。发布包不覆盖；确实要重打就加 -Force。"
    }
    Remove-Item -LiteralPath $zip -Force
}

# ---- 打包：直接写进 zip，文件平铺在根上，脚本在 tools 子目录里——和发布包一样 ----
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($e in $entries) {
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $e.Source, $e.Entry, [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}

# ---- 回读核对：包里每个文件和源文件逐字节一致，一个不多一个不少 ----
$sha = [System.Security.Cryptography.SHA256]::Create()
$problem = $null
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    if ($archive.Entries.Count -ne $entries.Count) {
        $problem = "包里有 $($archive.Entries.Count) 个文件，应该是 $($entries.Count) 个"
    } else {
        foreach ($e in $entries) {
            $entry = $archive.GetEntry($e.Entry)
            if (-not $entry) { $problem = "包里缺少 $($e.Entry)"; break }
            $stream = $entry.Open()
            try { $inZip = [BitConverter]::ToString($sha.ComputeHash($stream)) } finally { $stream.Dispose() }
            $onDisk = [BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($e.Source)))
            if ($inZip -ne $onDisk) { $problem = "包里的 $($e.Entry) 和源文件不一致"; break }
        }
    }
} finally {
    $archive.Dispose()
}
if ($problem) { Fail "安装包核对失败：$problem" }

# ---- 报告 ----
$info = Get-Item $zip
Write-Host ''
Write-Host '安装包已生成：' -ForegroundColor Green
Write-Host "  $($info.FullName)"
Write-Host ("  {0:N0} 字节，{1} 个文件，逐个核对过；构建时间 {2}" -f $info.Length, $entries.Count, $stampTime)
$installed = Join-Path $env:LOCALAPPDATA 'Programs\WindowMark\WindowMark.exe'
if (Test-Path $installed) {
    $same = (Get-FileHash $installed).Hash -eq (Get-FileHash (Join-Path $build 'WindowMark.exe')).Hash
    if ($same) {
        Write-Host '  包里的 WindowMark.exe 就是这台机器上已安装的那个'
    } else {
        Write-Host '  注意：包里的 WindowMark.exe 和这台机器上已安装的不是同一个文件' -ForegroundColor Yellow
    }
}
Write-Host '  用法：解压后双击 WindowMarkSetup.exe 安装，或者直接运行 WindowMark.exe'
exit 0
