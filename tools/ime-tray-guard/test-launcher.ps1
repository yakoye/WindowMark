# Launcher integration tests. All fixtures are isolated under a new temp directory.
param([Parameter(Mandatory=$true)][string]$ProbeExe, [Parameter(Mandatory=$true)][string]$FixtureExe)
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('WindowMark-IME-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'capture.ps1') -Destination $testRoot
$runner = Join-Path $testRoot 'capture.ps1'
$target = Join-Path $testRoot 'WindowMarkImeTrayProbe.exe'
function Run-Case([string]$Label, [int]$Expected) {
    $args = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Label {1} -Samples 2 -DelayMs 0 -IntervalMs 250 -StallTimeoutMs 1000' -f $runner,$Label
    $p = Start-Process powershell.exe -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $testRoot ($Label + '.out.log')) -RedirectStandardError (Join-Path $testRoot ($Label + '.err.log'))
    $null = $p.Handle
    if (-not $p.WaitForExit(15000)) { $p.Kill(); throw "Launcher test timeout: $Label" }
    $p.Refresh()
    if ($p.ExitCode -ne $Expected) {
        Get-Content -LiteralPath (Join-Path $testRoot ($Label + '.out.log'))
        Get-Content -LiteralPath (Join-Path $testRoot ($Label + '.err.log'))
        throw "${Label}: expected $Expected, got $($p.ExitCode); evidence: $testRoot"
    }
    $records = @(Get-ChildItem -LiteralPath (Join-Path $testRoot 'results') -Filter ($Label + '*.run.json'))
    if ($records.Count -ne 1) { throw "Missing/duplicate run record: $Label" }
    $record = Get-Content -LiteralPath $records[0].FullName -Raw | ConvertFrom-Json
    if ($record.exit_code -ne $Expected) { throw "Wrong recorded exit: $Label" }
    if ($Label -eq 'fixture-hang' -and $record.outcome -ne 'timeout') { throw 'Not reported as timeout' }
    Write-Output "PASS $Label exit=$Expected"
}
Copy-Item -LiteralPath $ProbeExe -Destination $target
Run-Case 'real-probe' 0
Copy-Item -LiteralPath $FixtureExe -Destination $target -Force
Run-Case 'fixture-error' 7
Run-Case 'fixture-hang' 124
if (Get-Process -Name WindowMarkImeTrayProbe -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $target }) {
    throw 'Test child leaked'
}
Write-Output "PASS no leaked child; test evidence: $testRoot"
