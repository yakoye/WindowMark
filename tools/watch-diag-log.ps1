# 打开诊断记录并实时跟着看。
#
# 日志在哪儿由**正在运行的那个 WindowMark.exe** 决定，不是写死的 %LOCALAPPDATA%：
# 绿色版的配置和诊断文件都在 exe 旁边。所以这里先找进程，再按和程序一样的规则定位。
#
#   .\tools\watch-diag-log.ps1              跟着看全部
#   .\tools\watch-diag-log.ps1 -Filter 边框  只看含「边框」的行
#   .\tools\watch-diag-log.ps1 -Off          关掉记录（删 diag.on）并退出
param(
    [string]$Filter = '',
    [switch]$Off
)

$ErrorActionPreference = 'Stop'

function Resolve-DiagDir {
    # 规则和程序里的三层查找一致，取前两层能看见的那部分：
    # exe 同目录有 settings.conf 就是它，否则 %LOCALAPPDATA%\WindowMark。
    $proc = Get-Process WindowMark -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($proc -and $proc.Path) {
        $beside = Split-Path $proc.Path
        if (Test-Path (Join-Path $beside 'settings.conf')) { return $beside }
    }
    return Join-Path $env:LOCALAPPDATA 'WindowMark'
}

$dir = Resolve-DiagDir
$marker = Join-Path $dir 'diag.on'
$log = Join-Path $dir 'diag.log'

if ($Off) {
    if (Test-Path $marker) { Remove-Item $marker -Force; Write-Host "已关掉记录：删了 $marker" }
    else { Write-Host '记录本来就没开' }
    exit 0
}

if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
if (-not (Test-Path $marker)) { New-Item -ItemType File -Path $marker | Out-Null }

$proc = Get-Process WindowMark -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Host ''
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host '  WindowMark 诊断记录' -ForegroundColor Cyan
Write-Host '============================================================' -ForegroundColor Cyan
Write-Host "  正在运行  : $(if ($proc) { "pid $($proc.Id)  $($proc.Path)" } else { '没找到进程（日志位置按默认位置猜的）' })"
Write-Host "  记录已打开: $marker"
Write-Host "  日志      : $log"
if ($Filter) { Write-Host "  只看含「$Filter」的行" }
Write-Host ''
Write-Host '  开关是即时的，不用重启 WindowMark。复现一次问题，下面就会有东西。'
Write-Host '  看完按 Ctrl+C；想关掉记录再跑一次本脚本加 -Off。'
Write-Host '------------------------------------------------------------'

while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 500 }
if ($Filter) {
    Get-Content -LiteralPath $log -Wait -Tail 0 -Encoding UTF8 |
        Where-Object { $_ -match [regex]::Escape($Filter) }
} else {
    Get-Content -LiteralPath $log -Wait -Tail 0 -Encoding UTF8
}
