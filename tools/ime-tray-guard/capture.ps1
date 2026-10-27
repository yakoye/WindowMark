# ASCII launcher for Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param(
    [ValidatePattern('^[a-zA-Z0-9_-]{1,40}$')][string]$Label = 'console-switch',
    [ValidateRange(1,600)][int]$Samples = 60,
    [ValidateRange(250,10000)][int]$IntervalMs = 1000,
    [ValidateRange(0,30000)][int]$DelayMs = 8000,
    [ValidateRange(1000,60000)][int]$StallTimeoutMs = 25000
)
$ErrorActionPreference = 'Stop'
$probeExe = Join-Path $PSScriptRoot 'WindowMarkImeTrayProbe.exe'
if (-not (Test-Path -LiteralPath $probeExe -PathType Leaf)) { throw 'Probe EXE not found next to capture.ps1.' }
$hashStream = [IO.File]::OpenRead($probeExe)
$sha256 = [Security.Cryptography.SHA256]::Create()
try { $probeHash = [BitConverter]::ToString($sha256.ComputeHash($hashStream)).Replace('-','') }
finally { $hashStream.Dispose(); $sha256.Dispose() }
$results = Join-Path $PSScriptRoot 'results'
$null = New-Item -ItemType Directory -Path $results -Force
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$stem = Join-Path $results ($Label + '-' + $runId)
$output = $stem + '.jsonl'
$process = $null
$outcome = 'not-started'
$exitCode = -1
$startedUtc = [DateTime]::UtcNow.ToString('o')
Write-Host 'Read-only probe. No taskbar changes, injection, screenshots or keyboard capture.'
Write-Host "Focus the test app within $DelayMs ms. Samples: $Samples. Keep each state for 10 seconds."
try {
    $arguments = '--out "{0}" --label {1} --samples {2} --interval-ms {3} --delay-ms {4}' -f $output,$Label,$Samples,$IntervalMs,$DelayMs
    $process = Start-Process -FilePath $probeExe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput ($stem + '.stdout.log') -RedirectStandardError ($stem + '.stderr.log')
    # Retain the process handle before waiting: Windows PowerShell 5.1 otherwise
    # can lose ExitCode for a short-lived child returned by Start-Process.
    $null = $process.Handle
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $lastGrowthMs = 0
    $lastLength = 0
    $totalLimitMs = $DelayMs + $Samples * ($IntervalMs + 10000) + 30000
    while (-not $process.WaitForExit(250)) {
        $length = 0
        if (Test-Path -LiteralPath $output) { $length = (Get-Item -LiteralPath $output).Length }
        if ($length -gt $lastLength) { $lastLength = $length; $lastGrowthMs = $watch.ElapsedMilliseconds }
        $stallLimitMs = if ($lastLength -eq 0) { $DelayMs + $StallTimeoutMs } else { $IntervalMs + $StallTimeoutMs }
        if (($watch.ElapsedMilliseconds - $lastGrowthMs -gt $stallLimitMs) -or ($watch.ElapsedMilliseconds -gt $totalLimitMs)) {
            $outcome = 'timeout'
            $exitCode = 124
            break
        }
    }
    if ($outcome -ne 'timeout') {
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        if ($null -eq $exitCode) { throw 'Child exit code unavailable; refusing to report success.' }
        $outcome = if ($exitCode -eq 0) { 'completed' } else { 'probe-error' }
    }
} finally {
    # Only the exact process created by this launcher may be stopped.
    if ($null -ne $process -and -not $process.HasExited) {
        $process.Kill()
        $null = $process.WaitForExit(5000)
        if ($outcome -ne 'timeout') { $outcome = 'interrupted' }
    }
    [ordered]@{
        label=$Label; started_utc=$startedUtc; ended_utc=[DateTime]::UtcNow.ToString('o')
        outcome=$outcome; exit_code=$exitCode; samples_requested=$Samples
        interval_ms=$IntervalMs; delay_ms=$DelayMs; protection_active=$false
        probe_sha256=$probeHash
    } | ConvertTo-Json | Set-Content -LiteralPath ($stem + '.run.json') -Encoding UTF8
}
Write-Host "Result: $outcome; exit code: $exitCode"
Write-Host "Evidence: $output"
exit $exitCode
