param(
    [Parameter(Mandatory=$true)][int]$MainPid,
    [Parameter(Mandatory=$true)][int]$ExplorerPid,
    [Parameter(Mandatory=$true)][string]$LeaseLog,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [ValidateRange(30,28800)][int]$Seconds = 900
)
# 只读进程/日志采样，不操作界面、不截图、不启停 Explorer。
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputDir) { throw 'Evidence directory already exists' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$encoding = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter((Join-Path $OutputDir 'samples.jsonl'), $false, $encoding)
$watch = [Diagnostics.Stopwatch]::StartNew()
$records = New-Object System.Collections.Generic.List[object]
try {
    do {
        # 布局/会话恢复会创建新租约；跟踪同一主进程，不能一直读已关闭的旧日志。
        $latestLease = Get-ChildItem -LiteralPath (Split-Path -Parent $LeaseLog) -Filter "$MainPid-*.jsonl" -File |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($latestLease) { $LeaseLog = $latestLease.FullName }
        $processes = @()
        foreach ($processId in @($MainPid, $ExplorerPid)) {
            $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
            if ($process) {
                $processes += @{pid=$processId; alive=$true; cpu_seconds=$process.CPU;
                    private_bytes=$process.PrivateMemorySize64; handles=$process.HandleCount;
                    threads=$process.Threads.Count; started_utc=$process.StartTime.ToUniversalTime().ToString('o')}
            } else { $processes += @{pid=$processId; alive=$false} }
        }
        $lease = $null
        if (Test-Path -LiteralPath $LeaseLog) {
            foreach ($line in @(Get-Content -LiteralPath $LeaseLog -Tail 4)) {
                try { $value = $line | ConvertFrom-Json; if ($null -ne $value.state) { $lease=$value } } catch {}
            }
        }
        $record = [pscustomobject]@{utc=[DateTime]::UtcNow.ToString('o'); elapsed_seconds=$watch.Elapsed.TotalSeconds;
            processes=$processes; lease_path=$LeaseLog; lease_state=$lease.state; lease_utc=$lease.utc;
            writes=$lease.writes; restores=$lease.restores; restore_failures=$lease.restore_failures;
            errors=$lease.errors; width=$lease.reserved_width}
        $writer.WriteLine(($record | ConvertTo-Json -Compress -Depth 6)); $writer.Flush()
        $records.Add($record)
        if ($watch.Elapsed.TotalSeconds -ge $Seconds) { break }
        Start-Sleep -Seconds 5
    } while ($true)
} finally { $writer.Dispose() }
$missing = @($records | Where-Object { @($_.processes | Where-Object { -not $_.alive }).Count -gt 0 }).Count
$inactive = @($records | Where-Object { $_.lease_state -ne 3 }).Count
$stale = @($records | Where-Object {
    -not $_.lease_utc -or (([datetime]$_.utc).ToUniversalTime() - ([datetime]$_.lease_utc).ToUniversalTime()).TotalSeconds -gt 12
}).Count
$failures = @($records | Where-Object { $_.errors -gt 0 -or $_.restore_failures -gt 0 }).Count
$summary = [ordered]@{duration_seconds=$watch.Elapsed.TotalSeconds; samples=$records.Count;
    missing_process_samples=$missing; inactive_samples=$inactive; stale_log_samples=$stale;
    error_samples=$failures; passed=($missing -eq 0 -and $inactive -eq 0 -and $stale -eq 0 -and $failures -eq 0)}
[IO.File]::WriteAllText((Join-Path $OutputDir 'summary.json'), ($summary | ConvertTo-Json), $encoding)
$summary | ConvertTo-Json
if (-not $summary.passed) { exit 2 }
