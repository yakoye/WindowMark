# Immutable experimental package, not a stable release.
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Destination) { throw 'Destination exists; select a new package directory.' }
foreach ($name in @('WindowMarkTsfSlotTrial.exe','WindowMarkImeTrayProbe.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name) -PathType Leaf)) { throw "Missing: $name" }
}
$null=New-Item -ItemType Directory -Path $Destination
foreach ($name in @('WindowMarkTsfSlotTrial.exe','WindowMarkImeTrayProbe.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDirectory $name) -Destination $Destination
}
foreach ($name in @('run-tsf-trial.ps1','run-tsf-trial.bat','stop-tsf-trial.bat','analyze.py','transitions.py')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $Destination
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'TSF-README.md') -Destination (Join-Path $Destination 'README.md')
foreach ($bat in Get-ChildItem -LiteralPath $Destination -Filter '*.bat') {
    if (@([IO.File]::ReadAllBytes($bat.FullName) | Where-Object { $_ -gt 127 }).Count) { throw 'BAT must be ASCII.' }
}
$manifest = @(Get-ChildItem -LiteralPath $Destination -File | Sort-Object Name | ForEach-Object {
    [ordered]@{file=$_.Name;length=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.json') -Encoding UTF8
Write-Output "Created experimental TSF package: $Destination"
