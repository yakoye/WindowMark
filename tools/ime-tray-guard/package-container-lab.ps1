# Fixture-only package. Native Explorer commands are disabled in EXE and DLL.
param([Parameter(Mandatory=$true)][string]$BuildDirectory,[Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Destination){throw 'Destination exists'}
$binaries=@('WindowMarkXamlContainerTrial.exe','WindowMarkXamlContainerTap.dll','WindowMarkXamlContainerFixture.exe','WindowMarkXamlSlotLab.exe')
foreach($name in $binaries){if(-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name) -PathType Leaf)){throw "Missing $name"}}
$help=& (Join-Path $BuildDirectory 'WindowMarkXamlContainerTrial.exe') --help
if($LASTEXITCODE -ne 0 -or ($help -join ' ') -notmatch 'FIXTURE ONLY'){throw 'Not a fixture-only build'}
$null=New-Item -ItemType Directory -Path $Destination
foreach($name in $binaries){Copy-Item -LiteralPath (Join-Path $BuildDirectory $name) -Destination $Destination}
foreach($name in @('run-container-lab.ps1','run-container-lab.bat','test-container-owner-exit.ps1')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $Destination}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CONTAINER-README.md') -Destination (Join-Path $Destination 'README.md')
foreach($bat in Get-ChildItem -LiteralPath $Destination -Filter '*.bat'){
 if(@([IO.File]::ReadAllBytes($bat.FullName)|Where-Object{$_ -gt 127}).Count){throw 'BAT must be ASCII'}
}
$manifest=@(Get-ChildItem -LiteralPath $Destination -File | Sort-Object Name | ForEach-Object{[ordered]@{file=$_.Name;length=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.json') -Encoding UTF8
Write-Output "Created fixture-only package: $Destination"
