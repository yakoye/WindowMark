# ASCII launcher; TSF registration experiment, NOT read-only and NOT a guard.
[CmdletBinding()]
param(
    [ValidateRange(5,90)][int]$Seconds = 45,
    [ValidateRange(2,15)][int]$BaselineSeconds = 10
)
$ErrorActionPreference = 'Stop'
$trialExe = Join-Path $PSScriptRoot 'WindowMarkTsfSlotTrial.exe'
$probeExe = Join-Path $PSScriptRoot 'WindowMarkImeTrayProbe.exe'
foreach ($exe in @($trialExe,$probeExe)) {
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing EXE: $exe" }
}
$results = Join-Path $PSScriptRoot 'results'
$null = New-Item -ItemType Directory -Path $results -Force
$runId = 'tsf-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$stem = Join-Path $results $runId
$trial = $null
$probe = $null
$outcome = 'not-started'
$forced = $false
$trialCode = $null
$probeCode = $null
$startedUtc = [DateTime]::UtcNow.ToString('o')
$hashes = @{}
foreach ($exe in @($trialExe,$probeExe)) {
    $stream = [IO.File]::OpenRead($exe)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hashes[[IO.Path]::GetFileName($exe)] = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
Write-Host 'TSF experiment: temporary private empty items. NO input method activation.'
Write-Host 'NOT a working guard. See README.md for timing and manual checks.'
Write-Host "Baseline: $BaselineSeconds s; registered: $Seconds s; auto-remove then capture recovery."
Write-Host 'To stop registration early, double-click stop-tsf-trial.bat. Do not close this window.'
try {
    $samples = $BaselineSeconds + $Seconds + 12
    $probeArgs = '--out "{0}.taskbar.jsonl" --label tsf-trial --samples {1} --interval-ms 1000' -f $stem,$samples
    $probe = Start-Process -FilePath $probeExe -ArgumentList $probeArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput ($stem+'.probe.stdout.log') -RedirectStandardError ($stem+'.probe.stderr.log')
    $null = $probe.Handle
    $trialArgs = '--run-trial --out "{0}.trial.jsonl" --seconds {1} --baseline-seconds {2}' -f $stem,$Seconds,$BaselineSeconds
    $trial = Start-Process -FilePath $trialExe -ArgumentList $trialArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput ($stem+'.trial.stdout.log') -RedirectStandardError ($stem+'.trial.stderr.log')
    $null = $trial.Handle
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (-not $trial.WaitForExit(250)) {
        if ($watch.Elapsed.TotalSeconds -gt ($BaselineSeconds+$Seconds+20)) { throw 'Trial timeout; requesting normal cleanup.' }
        if ($probe.HasExited) { throw 'Probe exited before trial; requesting normal cleanup.' }
    }
    $trial.WaitForExit()
    $trialCode = $trial.ExitCode
    if ($null -eq $trialCode -or $trialCode -ne 0) { throw "Trial returned $trialCode; inspect evidence, no success assumed." }
    $lastLength = 0
    $lastGrowth = $watch.Elapsed.TotalSeconds
    while (-not $probe.WaitForExit(250)) {
        $length = if (Test-Path -LiteralPath ($stem+'.taskbar.jsonl')) { (Get-Item -LiteralPath ($stem+'.taskbar.jsonl')).Length } else { 0 }
        if ($length -gt $lastLength) { $lastLength = $length; $lastGrowth = $watch.Elapsed.TotalSeconds }
        if (($watch.Elapsed.TotalSeconds-$lastGrowth -gt 25) -or ($watch.Elapsed.TotalSeconds -gt ($samples*2+30))) { throw 'Read-only probe timeout.' }
    }
    $probe.WaitForExit()
    $probeCode = $probe.ExitCode
    if ($null -eq $probeCode -or $probeCode -ne 0) { throw "Probe returned $probeCode." }
    $outcome = 'collected-not-validated'
} catch {
    $outcome = 'error'
    Write-Warning $_
} finally {
    if ($null -ne $trial -and -not $trial.HasExited) {
        $stopper = Start-Process -FilePath $trialExe -ArgumentList '--stop' -WindowStyle Hidden -PassThru
        $null = $stopper.Handle
        if (-not $stopper.WaitForExit(3000)) { $stopper.Kill() }
        if (-not $trial.WaitForExit(10000)) { $trial.Kill(); $forced=$true; $null=$trial.WaitForExit(5000) }
        $trialCode = $trial.ExitCode
    }
    if ($null -ne $probe -and -not $probe.HasExited) { $probe.Kill(); $null=$probe.WaitForExit(5000); $probeCode=$probe.ExitCode }
    if ($null -ne $trial -and $trial.HasExited) { $trial.WaitForExit(); $trialCode=$trial.ExitCode }
    if ($null -ne $probe -and $probe.HasExited) { $probe.WaitForExit(); $probeCode=$probe.ExitCode }
    [ordered]@{
        started_utc=$startedUtc; ended_utc=[DateTime]::UtcNow.ToString('o'); outcome=$outcome
        trial_exit_code=$trialCode; probe_exit_code=$probeCode; trial_forced_termination=$forced
        protection_active=$false; visibility_verified=$false; baseline_seconds=$BaselineSeconds
        hold_seconds=$Seconds; executable_sha256=$hashes
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath ($stem+'.run.json') -Encoding UTF8
}
Write-Host "Result: $outcome. Files: $stem.*"
Write-Host 'Registration/collection success is NOT proof of reserved space.'
if ($outcome -ne 'collected-not-validated') { exit 1 }
exit 0
