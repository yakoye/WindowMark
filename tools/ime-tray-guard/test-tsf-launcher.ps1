# Failure-path integration tests; fixture files are isolated and preserved.
param(
    [Parameter(Mandatory=$true)][string]$TrialExe,
    [Parameter(Mandatory=$true)][string]$ProbeExe,
    [Parameter(Mandatory=$true)][string]$FixtureExe
)
$ErrorActionPreference='Stop'
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('WindowMark-TSF-launcher-test-'+[Guid]::NewGuid().ToString('N'))
foreach ($case in @('trial-error','probe-error')) {
    $dir=Join-Path $testRoot $case
    $null=New-Item -ItemType Directory -Path $dir -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-tsf-trial.ps1') -Destination $dir
    $trialSource=if ($case -eq 'trial-error') {$FixtureExe} else {$TrialExe}
    $probeSource=if ($case -eq 'probe-error') {$FixtureExe} else {$ProbeExe}
    Copy-Item -LiteralPath $trialSource -Destination (Join-Path $dir 'WindowMarkTsfSlotTrial.exe')
    Copy-Item -LiteralPath $probeSource -Destination (Join-Path $dir 'WindowMarkImeTrayProbe.exe')
    $args='-NoProfile -ExecutionPolicy Bypass -File "{0}" -Seconds 5 -BaselineSeconds 2' -f (Join-Path $dir 'run-tsf-trial.ps1')
    $p=Start-Process powershell.exe -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dir 'stdout.log') -RedirectStandardError (Join-Path $dir 'stderr.log')
    $null=$p.Handle
    if (-not $p.WaitForExit(25000)) { $p.Kill(); throw "Test timeout: $case; evidence $dir" }
    if ($p.ExitCode -ne 1) { throw "Wrong launcher result: $case $($p.ExitCode)" }
    $records=@(Get-ChildItem -LiteralPath (Join-Path $dir 'results') -Filter '*.run.json')
    if ($records.Count -ne 1) { throw 'Expected exactly one run record' }
    $record=Get-Content -LiteralPath $records[0].FullName -Raw | ConvertFrom-Json
    if ($record.outcome -ne 'error' -or $record.protection_active -or $record.visibility_verified) { throw 'False success' }
    if ($case -eq 'trial-error' -and $record.trial_exit_code -ne 7) { throw 'Trial failure code lost' }
    if ($case -eq 'probe-error' -and $record.probe_exit_code -ne 7) { throw 'Probe failure code lost' }
    if ($record.trial_forced_termination) { throw 'Expected cooperative stop, not kill' }
    $leaked=@(Get-Process -Name WindowMarkTsfSlotTrial,WindowMarkImeTrayProbe -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dir,[StringComparison]::OrdinalIgnoreCase) })
    if ($leaked.Count) { throw 'Own child leaked' }
    Write-Output "PASS $case; no false success, no own child leaked"
}
Write-Output "Evidence: $testRoot"
