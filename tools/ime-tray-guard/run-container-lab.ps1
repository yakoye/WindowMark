# Safe isolated lab only. No Explorer attachment; target is our own fixture.
$ErrorActionPreference='Stop'
$evidence=Join-Path $PSScriptRoot ('results/lab-'+(Get-Date -Format yyyyMMdd-HHmmss-fff)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$null=New-Item -ItemType Directory -Path $evidence
& (Join-Path $PSScriptRoot 'WindowMarkXamlSlotLab.exe') 2>&1 | Tee-Object -FilePath (Join-Path $evidence 'layout.log')
if($LASTEXITCODE -ne 0){throw 'Layout transaction checks failed'}
& (Join-Path $PSScriptRoot 'test-container-owner-exit.ps1') -BuildDirectory $PSScriptRoot -EvidenceDirectory (Join-Path $evidence 'normal') -NormalExit
& (Join-Path $PSScriptRoot 'test-container-owner-exit.ps1') -BuildDirectory $PSScriptRoot -EvidenceDirectory (Join-Path $evidence 'owner-exit')
Write-Output "PASS isolated lab. Native guard NOT validated. Evidence: $evidence"
