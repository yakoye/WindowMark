$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

Write-Host '=== WindowMark v0.4.3 build ==='
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

$outDir = Join-Path $PSScriptRoot 'build\Release'
Write-Host ''
Write-Host 'Build completed.'
foreach ($name in 'WindowMark.exe', 'WindowMarkInspect.exe', 'WindowMarkDiag.exe') {
    $path = Join-Path $outDir $name
    if (Test-Path $path) { Write-Host "  $path" }
}
Write-Host ''
Write-Host '直接双击 WindowMark.exe 就能用；想装进系统跑 WindowMark.exe --install。'
