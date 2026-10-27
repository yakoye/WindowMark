# Produce an immutable test package, not a formal WindowMark release.
param(
    [Parameter(Mandatory=$true)][string]$ProbeExe,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists; choose a new package directory.' }
if (-not (Test-Path -LiteralPath $ProbeExe -PathType Leaf)) { throw 'Probe EXE missing.' }
$null = New-Item -ItemType Directory -Path $Destination
Copy-Item -LiteralPath $ProbeExe -Destination (Join-Path $Destination 'WindowMarkImeTrayProbe.exe')
$files = @('capture.ps1','capture.bat','capture-rdp.bat','capture-vnc.bat','capture-mobaxterm.bat','README.md','analyze.py')
$files += @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.md' | Where-Object { $_.Name -ne 'README.md' } | ForEach-Object { $_.Name })
foreach ($name in $files) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $Destination }
foreach ($bat in Get-ChildItem -LiteralPath $Destination -Filter '*.bat') {
    if (@([IO.File]::ReadAllBytes($bat.FullName) | Where-Object { $_ -gt 127 }).Count -gt 0) { throw 'BAT must be ASCII.' }
}
$manifest = @(Get-ChildItem -LiteralPath $Destination -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ file=$_.Name; length=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
})
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.json') -Encoding UTF8
Write-Output "Test package created: $Destination"
