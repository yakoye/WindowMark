$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

Write-Host '=== WindowMark v0.2.0 build ==='
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

$outDir = Join-Path $PSScriptRoot 'build\Release'
Write-Host ''
Write-Host 'Build completed.'
foreach ($name in 'WindowMark.exe', 'WindowMarkSetup.exe', 'WindowMarkUninstall.exe') {
    $path = Join-Path $outDir $name
    if (Test-Path $path) { Write-Host "  $path" }
}
Write-Host ''
Write-Host 'Run WindowMarkSetup.exe to install. Installing over a running copy is fine.'
