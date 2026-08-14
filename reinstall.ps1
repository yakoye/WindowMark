# 一键：编译 -> 卸载旧版 -> 安装新版。改完界面参数直接跑这个看效果。
#
#   .\reinstall.ps1           编译、卸载、安装（保留 settings.conf）
#   .\reinstall.ps1 -Fresh    同上，但连同 settings.conf 一起删掉
#   .\reinstall.ps1 -NoBuild  跳过编译，直接重装现有产物
#
# -Fresh 的用处：改代码里的**默认值**（例如 border.width、drawer.bottom_active_thickness）
# 时，已存在的 settings.conf 会覆盖掉新默认值，看不到效果。加 -Fresh 才能验证默认值。
param(
    [switch]$Fresh,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$release = Join-Path $PSScriptRoot 'build\Release'
$setup = Join-Path $release 'WindowMarkSetup.exe'
$installedUninstaller = Join-Path $env:LOCALAPPDATA 'Programs\WindowMark\WindowMarkUninstall.exe'
$settings = Join-Path $env:LOCALAPPDATA 'WindowMark\settings.conf'

function Step($text) {
    Write-Host ''
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

# cmake 通常随 Visual Studio 一起装，但不保证在 PATH 里（取决于装了哪些组件），
# 所以找不到就去 VS 的安装目录里翻，翻不到再给一句能照做的提示。
function Find-CMake {
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $root = & $vswhere -latest -products * -property installationPath 2>$null
        if ($root) {
            $p = Join-Path $root 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $p) { return $p }
        }
    }
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        $hit = Get-ChildItem -Path (Join-Path $base 'Microsoft Visual Studio') -Filter cmake.exe `
            -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

# ---- 1. 编译 ----
if (-not $NoBuild) {
    Step '编译'
    $cmake = Find-CMake
    if (-not $cmake) {
        Write-Host '找不到 cmake。请安装 Visual Studio 并勾选「使用 C++ 的桌面开发」工作负载，' -ForegroundColor Red
        Write-Host '或者单独装 CMake 并加入 PATH。' -ForegroundColor Red
        exit 1
    }
    Write-Host "cmake: $cmake"
    Set-Alias cmake $cmake -Scope Script
    cmake -S . -B build | Out-Null
    # 只把 error/warning 打出来，成功时保持安静
    $log = cmake --build build --config Release --parallel 2>&1
    $problems = $log | Select-String -Pattern ' error |: error|: warning| warning '
    if ($problems) {
        $problems | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host '编译失败，没有安装。' -ForegroundColor Red
        exit 1
    }
    Write-Host '编译通过（零错误零警告）' -ForegroundColor Green

    Step '单元测试'
    & (Join-Path $release 'windowmark_core_tests.exe')
    if ($LASTEXITCODE -ne 0) {
        Write-Host '测试失败，没有安装。' -ForegroundColor Red
        exit 1
    }
}

if (-not (Test-Path $setup)) {
    Write-Host "找不到 $setup，先跑一次不带 -NoBuild 的。" -ForegroundColor Red
    exit 1
}

# ---- 2. 卸载旧版 ----
Step '卸载旧版'
if (Test-Path $installedUninstaller) {
    # /Purge 连用户数据一起删；不加则保留 settings.conf
    $args = if ($Fresh) { @('/S', '/Purge') } else { @('/S') }
    $p = Start-Process -FilePath $installedUninstaller -ArgumentList $args -PassThru
    if (-not $p.WaitForExit(60000)) {
        Write-Host '卸载超时。' -ForegroundColor Red
        exit 1
    }
    $kept = if ($Fresh) { '配置已清除' } else { '配置已保留' }
    switch ($p.ExitCode) {
        0 { Write-Host "卸载完成，$kept" }
        6 { Write-Host "卸载完成（有残留项，通常是程序还被占用），$kept" -ForegroundColor Yellow }
        default {
            Write-Host "卸载失败，退出码 $($p.ExitCode)" -ForegroundColor Red
            exit 1
        }
    }
} else {
    Write-Host '没有已安装的版本，跳过'
    if ($Fresh -and (Test-Path $settings)) {
        Remove-Item $settings -Force
        Write-Host '已删除 settings.conf'
    }
}

# 等进程真正消失。卸载返回后进程可能还在退出中，而它退干净之前单例互斥量没释放，
# 新装的实例会以为「已经在跑了」直接退出，安装器就报启动失败（退出码 7）。
for ($i = 0; $i -lt 40; $i++) {
    if (-not (Get-Process -Name WindowMark -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 1200   # 互斥量释放比进程消失还要晚一点

# ---- 3. 安装新版 ----
Step '安装新版'
$p = Start-Process -FilePath $setup -ArgumentList '/S' -PassThru
if (-not $p.WaitForExit(60000)) {
    Write-Host '安装超时。' -ForegroundColor Red
    exit 1
}
if ($p.ExitCode -eq 7) {
    # 7 = 文件都装好了，只是没能拉起进程。自己拉一次就行。
    Write-Host '文件已安装，进程未自动启动，正在手动拉起...' -ForegroundColor Yellow
    Start-Process (Join-Path $env:LOCALAPPDATA 'Programs\WindowMark\WindowMark.exe')
} elseif ($p.ExitCode -ne 0) {
    Write-Host "安装失败，退出码 $($p.ExitCode)" -ForegroundColor Red
    exit 1
}

Start-Sleep -Milliseconds 2000
$proc = Get-Process -Name WindowMark -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "已安装并启动，pid $($proc.Id)" -ForegroundColor Green
} else {
    Write-Host '安装完成，但进程没起来。' -ForegroundColor Yellow
}

# 「我装的到底是不是刚编的那个」—— 直接把答案打出来，不用去开关于框
$installed = Join-Path $env:LOCALAPPDATA 'Programs\WindowMark\WindowMark.exe'
$stampFile = Join-Path $PSScriptRoot 'build\generated\BuildStamp.h'
if (Test-Path $stampFile) {
    $m = Select-String -Path $stampFile -Pattern 'L"([^"]+)"'
    if ($m) { Write-Host ("构建时间: {0}" -f $m.Matches[0].Groups[1].Value) }
}
if ((Test-Path $installed) -and (Test-Path (Join-Path $release 'WindowMark.exe'))) {
    $same = (Get-FileHash $installed).Hash -eq (Get-FileHash (Join-Path $release 'WindowMark.exe')).Hash
    if ($same) {
        Write-Host "已安装的就是刚构建的那个（哈希一致）" -ForegroundColor Green
    } else {
        Write-Host "警告：已安装的和刚构建的不是同一个文件" -ForegroundColor Red
    }
}

if (Test-Path $settings) {
    Write-Host ''
    Write-Host '当前生效的配置：'
    Get-Content $settings | Where-Object { $_ -match '^\s*[a-z]' } | ForEach-Object { Write-Host "  $_" }
}
